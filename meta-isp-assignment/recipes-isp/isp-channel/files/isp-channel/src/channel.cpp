/* SPDX-License-Identifier: MIT */
#include "channel.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <sstream>
#include <strings.h>

#include "log.h"

using namespace libcamera;

namespace isp {

namespace {

using Clock = std::chrono::steady_clock;

double msSince(Clock::time_point start)
{
	return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/* Colour matrix of the virtual sensor: camera RGB -> sRGB. Rows sum to 1. */
const float kCcm[9] = { 1.80f, -0.65f, -0.15f,
			-0.30f, 1.60f, -0.30f,
			-0.05f, -0.60f, 1.65f };

IspLimits makeLimits(const Config &config)
{
	IspLimits limits;
	limits.maxExposureUs = float(1e6 / config.fps);
	limits.aeTarget = config.aeTarget;
	return limits;
}

double psnr(const uint8_t *a, const uint8_t *b, size_t n)
{
	double sse = 0;
	for (size_t i = 0; i < n; i++) {
		const double d = double(a[i]) - double(b[i]);
		sse += d * d;
	}
	return sse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 * double(n) / sse);
}

} /* namespace */

Channel::Channel(const Config &config)
	: config_(config), pool_(config.threads), sensor_(pool_), isp_(pool_),
	  controller_(kCcm, makeLimits(config)), desired_(config.controls)
{
	sensorParams_.wbRed = config.sensorWbRed;
	sensorParams_.wbBlue = config.sensorWbBlue;
	sensorParams_.fullRangeSource = config.fullRangeSource;
	std::copy(kCcm, kCcm + 9, sensorParams_.ccm);
}

bool Channel::init()
{
	std::error_code ec;
	std::filesystem::create_directories(config_.outputDir, ec);
	if (ec) {
		LOG_ERROR("cannot create %s: %s", config_.outputDir.c_str(), ec.message().c_str());
		return false;
	}

	CameraSource::Options options;
	options.camera = config_.camera;
	options.width = config_.width;
	options.height = config_.height;
	options.fps = config_.fps;
	options.framePacing = config_.framePacing;
	if (!camera_.open(options))
		return false;

	if (camera_.hasRawStream())
		LOG_INFO("ISP input: RAW Bayer stream from the camera");
	else
		LOG_INFO("ISP input: mosaic step, virtual sensor RGGB %u-bit, black level %u, "
			 "illuminant gains R %.2f B %.2f",
			 sensorParams_.bits, sensorParams_.blackLevel,
			 sensorParams_.wbRed, sensorParams_.wbBlue);
	LOG_INFO("software ISP: black level, gray-world AWB, bilinear demosaic, CCM, gamma; %u thread(s)",
		 pool_.size());
	LOG_INFO("software ISP controls: AeEnable, ExposureTime [100..%.0f us], AnalogueGain [1..16], "
		 "AwbEnable, AwbMode, ColourGains, Brightness [-1..1], Contrast [0..2]",
		 1e6 / config_.fps);

	std::string pipeline = config_.pipeline;
	const std::string placeholder = "%OUTPUT%";
	for (size_t pos; (pos = pipeline.find(placeholder)) != std::string::npos;)
		pipeline.replace(pos, placeholder.size(), config_.outputDir);
	if (!encoder_.start(pipeline, camera_.width(), camera_.height(), config_.fps))
		return false;

	if (!config_.socketPath.empty() &&
	    !socket_.start(config_.socketPath, [this](const std::string &c) { return handleCommand(c); }))
		LOG_WARN("running without a control socket");

	return true;
}

void Channel::requestStop()
{
	stop_ = true;
	camera_.interrupt();
}

void Channel::requestSnapshot()
{
	snapshotRequests_++;
}

/*
 * Sends one control to the camera if the camera supports it. Returns false
 * when the software ISP has to implement the control instead.
 */
template<typename T, typename V>
bool Channel::sendToCamera(const Control<T> &control, const V &value, const char *text,
			   bool logRouting)
{
	const bool supported = camera_.supports(control);
	if (supported)
		camera_.pendingControls().set(control, value);
	if (logRouting)
		LOG_INFO("control %-13s = %-10s -> %s", control.name().c_str(), text,
			 supported ? "camera" : "software ISP (not offered by the camera)");
	return supported;
}

void Channel::applyControls(bool logRouting)
{
	IspControls c;
	{
		std::lock_guard<std::mutex> lock(controlMutex_);
		if (!controlsDirty_)
			return;
		controlsDirty_ = false;
		c = desired_;
	}

	char text[64];
	sendToCamera(controls::AeEnable, c.aeEnable, c.aeEnable ? "on" : "off", logRouting);
	std::snprintf(text, sizeof(text), "%.0f us", c.exposureTimeUs);
	sendToCamera(controls::ExposureTime, int32_t(c.exposureTimeUs), text, logRouting);
	std::snprintf(text, sizeof(text), "%.2f", c.analogueGain);
	sendToCamera(controls::AnalogueGain, c.analogueGain, text, logRouting);
	sendToCamera(controls::AwbEnable, c.awbEnable, c.awbEnable ? "on" : "off", logRouting);
	sendToCamera(controls::AwbMode, int32_t(c.awbMode), awbModeName(c.awbMode), logRouting);
	std::snprintf(text, sizeof(text), "%.2f", c.brightness);
	sendToCamera(controls::Brightness, c.brightness, text, logRouting);
	std::snprintf(text, sizeof(text), "%.2f", c.contrast);
	sendToCamera(controls::Contrast, c.contrast, text, logRouting);
	if (c.colourGainR > 0.0f && c.colourGainB > 0.0f) {
		std::snprintf(text, sizeof(text), "%.2f,%.2f", c.colourGainR, c.colourGainB);
		sendToCamera(controls::ColourGains,
			     Span<const float, 2>({ c.colourGainR, c.colourGainB }), text, logRouting);
	}

	/*
	 * The software ISP works on RAW data, which no camera-side control
	 * has touched, so it always applies the full set itself.
	 */
	controller_.setControls(c);
}

int Channel::run()
{
	if (!camera_.start())
		return 1;

	applyControls(true);

	const uint64_t periodNs = uint64_t(1e9 / config_.fps);
	const auto started = Clock::now();
	auto nextReport = started + std::chrono::seconds(config_.statsIntervalSec);
	uint64_t firstTimestamp = 0;
	bool ok = true;

	LOG_INFO("streaming %ux%u, target %.2f fps%s", camera_.width(), camera_.height(), config_.fps,
		 config_.framePacing ? " (paced by the application)" : "");

	while (!stop_) {
		CapturedFrame frame;
		const bool got = camera_.waitFrame(frame, std::chrono::milliseconds(200));

		if (got) {
			const auto t0 = Clock::now();
			FrameTimes times;
			applyControls(true);

			/* RAW input: from the camera, or from the mosaic step. */
			BayerView raw;
			if (frame.hasRaw) {
				raw = frame.raw;
			} else {
				sensorParams_.exposureTimeUs = controller_.exposureTimeUs();
				sensorParams_.analogueGain = controller_.analogueGain();
				raw = sensor_.capture(frame.viewfinder, sensorParams_);
				times.sensor = msSince(t0);
			}

			/* Statistics drive AWB for this frame and AE for the next. */
			const auto t1 = Clock::now();
			controller_.update(SoftIsp::measure(raw));

			Encoder::Frame out;
			if (!encoder_.acquire(out)) {
				LOG_ERROR("cannot allocate an encoder buffer");
				ok = false;
				break;
			}

			std::vector<uint8_t> rgb;
			const bool snapshot = snapshotRequests_ > 0;
			if (snapshot) {
				snapshotRequests_--;
				rgb.resize(size_t(raw.width) * raw.height * 3);
			}
			isp_.process(raw, controller_.ispParams(), out.image, snapshot ? rgb.data() : nullptr);
			times.isp = msSince(t1);

			if (!firstTimestamp)
				firstTimestamp = frame.timestampNs;
			const auto t2 = Clock::now();
			if (!encoder_.submit(out, frame.timestampNs - firstTimestamp, periodNs)) {
				ok = false;
				break;
			}
			times.encode = msSince(t2);

			if (snapshot)
				saveSnapshot(std::move(rgb), frame);

			/* The DMABUF is no longer referenced: recycle the request. */
			camera_.recycle(frame);

			times.total = msSince(t0);
			stats_.addFrame(times);
		}

		const auto now = Clock::now();
		if (now >= nextReport) {
			nextReport = now + std::chrono::seconds(config_.statsIntervalSec);
			const std::string report = stats_.report(statusText());
			LOG_INFO("%s", report.c_str());
			std::lock_guard<std::mutex> lock(controlMutex_);
			lastReport_ = report;
			if (!encoder_.healthy()) {
				ok = false;
				break;
			}
		}

		if (config_.maxFrames && stats_.frames() >= config_.maxFrames)
			break;
		if (config_.durationSec && now - started >= std::chrono::seconds(config_.durationSec))
			break;
	}

	LOG_INFO("stopping");
	camera_.stop();
	socket_.stop();
	encoder_.stop();
	LOG_INFO("%s dropped=%llu failed=%llu", stats_.summary().c_str(),
		 static_cast<unsigned long long>(camera_.droppedFrames()),
		 static_cast<unsigned long long>(camera_.failedFrames()));
	return ok ? 0 : 1;
}

std::string Channel::statusText()
{
	char text[192];
	std::snprintf(text, sizeof(text),
		      "dropped=%llu failed=%llu exposure_us=%.0f gain=%.2f wb_r=%.3f wb_b=%.3f",
		      static_cast<unsigned long long>(camera_.droppedFrames()),
		      static_cast<unsigned long long>(camera_.failedFrames()),
		      controller_.exposureTimeUs(), controller_.analogueGain(),
		      controller_.gainR(), controller_.gainB());
	return text;
}

void Channel::saveSnapshot(std::vector<uint8_t> &&rgb, const CapturedFrame &frame)
{
	/*
	 * With the mosaic step the source picture is known, so the snapshot
	 * doubles as a self-test: how closely did the ISP reconstruct it?
	 */
	if (!frame.hasRaw) {
		std::vector<uint8_t> reference(rgb.size());
		sensor_.referenceRgb(frame.viewfinder, sensorParams_.fullRangeSource, reference.data());
		LOG_INFO("snapshot self-test: ISP output vs source picture PSNR %.1f dB",
			 psnr(reference.data(), rgb.data(), rgb.size()));
	}

	char stamp[32];
	const std::time_t now = std::time(nullptr);
	std::tm tm;
	gmtime_r(&now, &tm);
	std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tm);
	char name[96];
	std::snprintf(name, sizeof(name), "/snapshot-%s-%06llu.png", stamp,
		      static_cast<unsigned long long>(stats_.frames()));
	snapshots_.submit(std::move(rgb), camera_.width(), camera_.height(), config_.outputDir + name);
}

std::string Channel::handleCommand(const std::string &command)
{
	std::istringstream in(command);
	std::string verb;
	in >> verb;

	if (verb == "snapshot") {
		requestSnapshot();
		return "OK snapshot requested";
	}

	if (verb == "get") {
		std::lock_guard<std::mutex> lock(controlMutex_);
		char text[256];
		std::snprintf(text, sizeof(text),
			      "AeEnable=%d ExposureTime=%.0f AnalogueGain=%.2f AwbEnable=%d AwbMode=%s "
			      "Brightness=%.2f Contrast=%.2f\n",
			      desired_.aeEnable, desired_.exposureTimeUs, desired_.analogueGain,
			      desired_.awbEnable, awbModeName(desired_.awbMode),
			      desired_.brightness, desired_.contrast);
		return std::string("OK ") + text + (lastReport_.empty() ? "(no statistics yet)" : lastReport_);
	}

	if (verb == "set") {
		std::string name, value;
		std::vector<std::string> values;
		in >> name;
		while (in >> value)
			values.push_back(value);
		return setControl(name, values);
	}

	return "ERROR unknown command (snapshot | get | set CONTROL VALUE)";
}

std::string Channel::setControl(const std::string &name, const std::vector<std::string> &values)
{
	if (values.empty())
		return "ERROR missing value";

	auto is = [&name](const char *n) { return !strcasecmp(name.c_str(), n); };
	auto number = [&values](size_t i, float lo, float hi, float &out) {
		char *end = nullptr;
		const float v = std::strtof(values[i].c_str(), &end);
		if (end == values[i].c_str() || *end || !(v >= lo && v <= hi))
			return false;
		out = v;
		return true;
	};

	std::lock_guard<std::mutex> lock(controlMutex_);
	IspControls c = desired_;
	bool valid;
	if (is("AeEnable"))
		valid = parseBool(values[0], c.aeEnable);
	else if (is("AwbEnable"))
		valid = parseBool(values[0], c.awbEnable);
	else if (is("AwbMode"))
		valid = parseAwbMode(values[0], c.awbMode);
	else if (is("ExposureTime"))
		valid = number(0, 100.0f, float(1e6 / config_.fps), c.exposureTimeUs);
	else if (is("AnalogueGain"))
		valid = number(0, 1.0f, 16.0f, c.analogueGain);
	else if (is("Brightness"))
		valid = number(0, -1.0f, 1.0f, c.brightness);
	else if (is("Contrast"))
		valid = number(0, 0.0f, 2.0f, c.contrast);
	else if (is("ColourGains"))
		valid = values.size() == 2 && number(0, 0.25f, 8.0f, c.colourGainR) &&
			number(1, 0.25f, 8.0f, c.colourGainB);
	else
		return "ERROR unknown control " + name;

	if (!valid)
		return "ERROR value out of range for " + name;

	desired_ = c;
	controlsDirty_ = true;
	return "OK " + name + " = " + values[0];
}

} /* namespace isp */
