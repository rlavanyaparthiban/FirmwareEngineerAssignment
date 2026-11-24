/* SPDX-License-Identifier: MIT */
#include "camera_source.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

#include "log.h"

using namespace libcamera;

namespace isp {

namespace {

struct RawFormat {
	PixelFormat format;
	Cfa cfa;
	unsigned bits;
};

/* Unpacked Bayer formats (one 16-bit word per sample) the ISP can read. */
const RawFormat kRawFormats[] = {
	{ formats::SRGGB10, Cfa::RGGB, 10 }, { formats::SGRBG10, Cfa::GRBG, 10 },
	{ formats::SGBRG10, Cfa::GBRG, 10 }, { formats::SBGGR10, Cfa::BGGR, 10 },
	{ formats::SRGGB12, Cfa::RGGB, 12 }, { formats::SGRBG12, Cfa::GRBG, 12 },
	{ formats::SGBRG12, Cfa::GBRG, 12 }, { formats::SBGGR12, Cfa::BGGR, 12 },
};

const RawFormat *findRawFormat(const PixelFormat &format)
{
	for (const RawFormat &f : kRawFormats)
		if (f.format == format)
			return &f;
	return nullptr;
}

/* Names what kind of memory is behind a frame buffer file descriptor. */
std::string describeBacking(int fd)
{
	char link[64], target[256];
	std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
	const ssize_t n = readlink(link, target, sizeof(target) - 1);
	if (n <= 0)
		return "unknown";
	target[n] = '\0';
	return target;
}

} /* namespace */

CameraSource::CameraSource()
	: pendingControls_(controls::controls)
{
}

CameraSource::~CameraSource()
{
	stop();

	for (const auto &[fd, mapping] : mappings_)
		munmap(mapping.address, mapping.length);
	mappings_.clear();
	planes_.clear();

	/* Requests reference buffers, buffers belong to the allocator, and all
	 * of them must be gone before the camera is released. */
	requests_.clear();
	allocator_.reset();
	config_.reset();
	if (camera_) {
		camera_->requestCompleted.disconnect(this);
		camera_->release();
		camera_.reset();
	}
	if (manager_)
		manager_->stop();
}

void CameraSource::logCameras() const
{
	LOG_INFO("libcamera %s, %zu camera(s) found", CameraManager::version().c_str(),
		 manager_->cameras().size());
	unsigned index = 0;
	for (const std::shared_ptr<Camera> &cam : manager_->cameras()) {
		const auto model = cam->properties().get(properties::Model);
		LOG_INFO("  camera %u: id '%s', model '%s'", index++, cam->id().c_str(),
			 model ? model->c_str() : "unknown");
	}
}

void CameraSource::logControls() const
{
	const ControlInfoMap &map = camera_->controls();
	LOG_INFO("camera exposes %zu control(s):", map.size());
	for (const auto &[id, info] : map)
		LOG_INFO("  %-22s %s", id->name().c_str(), info.toString().c_str());
}

bool CameraSource::supports(const ControlId &id) const
{
	return camera_ && camera_->controls().find(&id) != camera_->controls().end();
}

bool CameraSource::open(const Options &options)
{
	manager_ = std::make_unique<CameraManager>();
	int ret = manager_->start();
	if (ret) {
		LOG_ERROR("failed to start the camera manager: %s", std::strerror(-ret));
		return false;
	}

	logCameras();
	const auto cameras = manager_->cameras();
	if (cameras.empty()) {
		LOG_ERROR("no camera available");
		return false;
	}

	/* Select by id, by index, or take the first one. */
	if (options.camera.empty()) {
		camera_ = cameras.front();
	} else if (options.camera.find_first_not_of("0123456789") == std::string::npos) {
		const size_t index = std::stoul(options.camera);
		if (index < cameras.size())
			camera_ = cameras[index];
	} else {
		camera_ = manager_->get(options.camera);
	}
	if (!camera_) {
		LOG_ERROR("camera '%s' not found", options.camera.c_str());
		return false;
	}

	ret = camera_->acquire();
	if (ret) {
		LOG_ERROR("failed to acquire camera %s: %s", camera_->id().c_str(), std::strerror(-ret));
		camera_.reset();
		return false;
	}
	LOG_INFO("using camera '%s'", camera_->id().c_str());
	logControls();

	if (!configure(options))
		return false;

	allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
	for (Stream *stream : { viewfinderStream_, rawStream_ }) {
		if (!stream)
			continue;
		ret = allocator_->allocate(stream);
		if (ret < 0) {
			LOG_ERROR("failed to allocate frame buffers: %s", std::strerror(-ret));
			return false;
		}
		if (!mapBuffers(stream))
			return false;
	}

	/* One request per viewfinder buffer, reused for the whole session. */
	const auto &vfBuffers = allocator_->buffers(viewfinderStream_);
	for (size_t i = 0; i < vfBuffers.size(); i++) {
		std::unique_ptr<Request> request = camera_->createRequest(i);
		if (!request || request->addBuffer(viewfinderStream_, vfBuffers[i].get()) < 0) {
			LOG_ERROR("failed to create request %zu", i);
			return false;
		}
		if (rawStream_) {
			const auto &rawBuffers = allocator_->buffers(rawStream_);
			if (i >= rawBuffers.size())
				break;
			if (request->addBuffer(rawStream_, rawBuffers[i].get()) < 0) {
				LOG_ERROR("failed to attach the RAW buffer to request %zu", i);
				return false;
			}
		}
		free_.push_back(request.get());
		requests_.push_back(std::move(request));
	}
	LOG_INFO("%zu requests created, frame buffers backed by %s (DMABUF, mapped read-only)",
		 requests_.size(), bufferBacking_.c_str());

	camera_->requestCompleted.connect(this, &CameraSource::onRequestCompleted);

	pacing_ = options.framePacing;
	framePeriod_ = std::chrono::nanoseconds(int64_t(1e9 / options.fps));
	return true;
}

bool CameraSource::configure(const Options &options)
{
	/*
	 * Ask for a processed viewfinder stream plus a RAW Bayer stream. A
	 * pipeline handler without RAW support refuses the pair, in which case
	 * only the viewfinder is requested and the RAW frames are synthesised
	 * from it by the virtual sensor.
	 */
	config_ = camera_->generateConfiguration({ StreamRole::Viewfinder, StreamRole::Raw });
	bool wantRaw = config_ && config_->size() == 2;
	if (!wantRaw) {
		LOG_INFO("pipeline handler offers no RAW stream; RAW frames will come from the mosaic step");
		config_ = camera_->generateConfiguration({ StreamRole::Viewfinder });
	}
	if (!config_ || config_->empty()) {
		LOG_ERROR("camera cannot generate a viewfinder configuration");
		return false;
	}

	StreamConfiguration &vf = config_->at(0);
	LOG_INFO("viewfinder formats offered by the camera:");
	for (const PixelFormat &format : vf.formats().pixelformats()) {
		const SizeRange range = vf.formats().range(format);
		LOG_INFO("  %s %s .. %s", format.toString().c_str(),
			 range.min.toString().c_str(), range.max.toString().c_str());
	}
	vf.pixelFormat = formats::YUV420;
	vf.size = Size(options.width, options.height);

	const CameraConfiguration::Status status = config_->validate();
	if (status == CameraConfiguration::Invalid) {
		LOG_ERROR("camera configuration is invalid");
		return false;
	}
	if (status == CameraConfiguration::Adjusted)
		LOG_INFO("configuration adjusted by the pipeline handler");

	if (vf.pixelFormat != formats::NV12 && vf.pixelFormat != formats::YUV420) {
		LOG_ERROR("unsupported viewfinder format %s", vf.pixelFormat.toString().c_str());
		return false;
	}

	if (wantRaw) {
		const RawFormat *raw = findRawFormat(config_->at(1).pixelFormat);
		if (!raw) {
			LOG_ERROR("RAW format %s is not an unpacked 10/12-bit Bayer format",
				  config_->at(1).pixelFormat.toString().c_str());
			return false;
		}
		rawCfa_ = raw->cfa;
		rawBits_ = raw->bits;
	}

	const int ret = camera_->configure(config_.get());
	if (ret) {
		LOG_ERROR("failed to configure the camera: %s", std::strerror(-ret));
		return false;
	}

	viewfinderStream_ = vf.stream();
	viewfinderIsNv12_ = vf.pixelFormat == formats::NV12;
	width_ = vf.size.width;
	height_ = vf.size.height;
	LOG_INFO("viewfinder stream: %s, %u buffers", vf.toString().c_str(), vf.bufferCount);
	if (wantRaw) {
		rawStream_ = config_->at(1).stream();
		LOG_INFO("RAW stream: %s", config_->at(1).toString().c_str());
	}
	return true;
}

bool CameraSource::mapBuffers(Stream *stream)
{
	for (const std::unique_ptr<FrameBuffer> &buffer : allocator_->buffers(stream)) {
		/* Planes may share one DMABUF; map each fd once, to its full extent. */
		for (const FrameBuffer::Plane &plane : buffer->planes()) {
			const int fd = plane.fd.get();
			const size_t end = size_t(plane.offset) + plane.length;
			auto it = mappings_.find(fd);
			if (it != mappings_.end() && it->second.length >= end)
				continue;
			if (it != mappings_.end()) {
				munmap(it->second.address, it->second.length);
				mappings_.erase(it);
			}
			void *address = mmap(nullptr, end, PROT_READ, MAP_SHARED, fd, 0);
			if (address == MAP_FAILED) {
				LOG_ERROR("mmap of frame buffer failed: %s", std::strerror(errno));
				return false;
			}
			mappings_[fd] = { address, end };
			if (bufferBacking_.empty())
				bufferBacking_ = describeBacking(fd);
		}

		std::vector<const uint8_t *> planeData;
		for (const FrameBuffer::Plane &plane : buffer->planes())
			planeData.push_back(static_cast<const uint8_t *>(mappings_[plane.fd.get()].address) +
					    plane.offset);
		planes_[buffer.get()] = std::move(planeData);
	}
	return true;
}

bool CameraSource::start()
{
	/* Pin the frame duration when the camera lets us. */
	ControlList initial(controls::controls);
	if (supports(controls::FrameDurationLimits)) {
		const int64_t us = framePeriod_.count() / 1000;
		initial.set(controls::FrameDurationLimits, Span<const int64_t, 2>({ us, us }));
	}

	const int ret = camera_->start(&initial);
	if (ret) {
		LOG_ERROR("failed to start the camera: %s", std::strerror(-ret));
		return false;
	}
	started_ = true;
	interrupted_ = false;
	nextDue_ = std::chrono::steady_clock::now();
	return true;
}

void CameraSource::stop()
{
	if (!started_)
		return;
	started_ = false;
	/* Camera::stop() completes every outstanding request as cancelled. */
	camera_->stop();
	std::lock_guard<std::mutex> lock(mutex_);
	completed_.clear();
}

void CameraSource::interrupt()
{
	std::lock_guard<std::mutex> lock(mutex_);
	interrupted_ = true;
	wake_.notify_all();
}

/* Runs on libcamera's thread: hand the request over and return at once. */
void CameraSource::onRequestCompleted(Request *request)
{
	std::lock_guard<std::mutex> lock(mutex_);
	completed_.push_back(request);
	wake_.notify_all();
}

void CameraSource::queueDueRequests()
{
	const auto now = std::chrono::steady_clock::now();

	while (!free_.empty()) {
		/* With pacing, release one request per frame period. */
		if (pacing_ && now < nextDue_)
			break;

		Request *request = free_.front();
		free_.pop_front();

		/* Controls are sticky in libcamera: send changes once. */
		if (!pendingControls_.empty()) {
			request->controls().merge(pendingControls_);
			pendingControls_.clear();
		}

		const int ret = camera_->queueRequest(request);
		if (ret < 0) {
			LOG_ERROR("failed to queue request: %s", std::strerror(-ret));
			free_.push_front(request);
			break;
		}

		if (pacing_) {
			/* Keep a fixed cadence; if we fell behind, do not burst. */
			nextDue_ += framePeriod_;
			if (nextDue_ < now)
				nextDue_ = now + framePeriod_;
			break;
		}
	}
}

bool CameraSource::waitFrame(CapturedFrame &frame, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;

	for (;;) {
		queueDueRequests();

		Request *request = nullptr;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			if (interrupted_)
				return false;
			if (completed_.empty()) {
				/* Wake for a completion, the next frame tick or the timeout. */
				auto until = deadline;
				if (pacing_ && !free_.empty())
					until = std::min(until, nextDue_);
				wake_.wait_until(lock, until);
			}
			if (!completed_.empty()) {
				request = completed_.front();
				completed_.pop_front();
			}
		}

		if (request) {
			if (describe(request, frame))
				return true;
			/* Cancelled or failed: recycle it and keep waiting. */
			request->reuse(Request::ReuseBuffers);
			free_.push_back(request);
			continue;
		}
		if (std::chrono::steady_clock::now() >= deadline)
			return false;
	}
}

bool CameraSource::describe(Request *request, CapturedFrame &frame)
{
	if (request->status() == Request::RequestCancelled)
		return false;

	const FrameBuffer *vf = request->findBuffer(viewfinderStream_);
	const FrameMetadata &meta = vf->metadata();
	if (meta.status != FrameMetadata::FrameSuccess) {
		failed_++;
		LOG_WARN("frame %u completed with status %d", meta.sequence, int(meta.status));
		return false;
	}

	/* A gap in the sequence numbers means the source dropped frames. */
	if (haveSequence_ && meta.sequence > lastSequence_ + 1) {
		dropped_ += meta.sequence - lastSequence_ - 1;
		LOG_WARN("%u frame(s) dropped before sequence %u",
			 meta.sequence - lastSequence_ - 1, meta.sequence);
	}
	haveSequence_ = true;
	lastSequence_ = meta.sequence;

	const StreamConfiguration &cfg = viewfinderStream_->configuration();
	const std::vector<const uint8_t *> &p = planes_[vf];
	frame.request = request;
	frame.viewfinder.width = width_;
	frame.viewfinder.height = height_;
	frame.viewfinder.strideY = cfg.stride;
	frame.viewfinder.y = p[0];
	if (viewfinderIsNv12_) {
		/* Some pipelines put Y and UV in one plane, others in two. */
		frame.viewfinder.u = p.size() > 1 ? p[1] : p[0] + size_t(cfg.stride) * height_;
		frame.viewfinder.v = nullptr;
		frame.viewfinder.strideC = cfg.stride;
	} else {
		const size_t ySize = size_t(cfg.stride) * height_;
		frame.viewfinder.strideC = cfg.stride / 2;
		frame.viewfinder.u = p.size() > 2 ? p[1] : p[0] + ySize;
		frame.viewfinder.v = p.size() > 2 ? p[2] : p[0] + ySize + ySize / 4;
	}

	frame.hasRaw = false;
	if (rawStream_) {
		const FrameBuffer *raw = request->findBuffer(rawStream_);
		if (raw && raw->metadata().status == FrameMetadata::FrameSuccess) {
			const StreamConfiguration &rawCfg = rawStream_->configuration();
			frame.hasRaw = true;
			frame.raw.data = reinterpret_cast<const uint16_t *>(planes_[raw][0]);
			frame.raw.width = rawCfg.size.width;
			frame.raw.height = rawCfg.size.height;
			frame.raw.stride = rawCfg.stride / 2;
			frame.raw.cfa = rawCfa_;
			frame.raw.bits = rawBits_;
			frame.raw.blackLevel = 64u << (rawBits_ - 10);
		}
	}

	const auto sensorTime = request->metadata().get(controls::SensorTimestamp);
	frame.timestampNs = sensorTime ? uint64_t(*sensorTime) : meta.timestamp;
	frame.sequence = meta.sequence;
	return true;
}

void CameraSource::recycle(const CapturedFrame &frame)
{
	frame.request->reuse(Request::ReuseBuffers);
	free_.push_back(frame.request);
}

} /* namespace isp */
