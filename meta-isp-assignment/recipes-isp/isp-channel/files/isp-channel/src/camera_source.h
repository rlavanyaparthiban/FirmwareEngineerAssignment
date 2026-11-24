/* SPDX-License-Identifier: MIT */
/*
 * Camera access through the libcamera C++ API, and nothing else: enumerate,
 * configure, allocate DMABUF frame buffers, and run the request loop.
 *
 * Requests are recycled. A fixed set is created once (one per buffer); each
 * completed request is handed to the caller, then reset with
 * Request::reuse() and queued again. Nothing is allocated per frame.
 */
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <libcamera/libcamera.h>

#include "image.h"

namespace isp {

struct CapturedFrame {
	libcamera::Request *request = nullptr;
	Yuv420View viewfinder;   /* points into the mapped DMABUF: zero copy */
	bool hasRaw = false;
	BayerView raw;           /* valid when the camera has a RAW stream */
	uint64_t timestampNs = 0;
	uint32_t sequence = 0;
};

class CameraSource
{
public:
	struct Options {
		std::string camera;
		unsigned width = 1920;
		unsigned height = 1080;
		double fps = 30.0;
		bool framePacing = true;
	};

	CameraSource();
	~CameraSource();

	/* Enumerates cameras, acquires and configures one, maps its buffers. */
	bool open(const Options &options);
	bool start();
	void stop();

	/* Wakes waitFrame() up early; safe from any thread. */
	void interrupt();

	/*
	 * Queues free requests when they are due and waits for a completed
	 * one. Returns false on timeout or interrupt.
	 */
	bool waitFrame(CapturedFrame &frame, std::chrono::milliseconds timeout);

	/* Returns a frame's request to the free list for reuse. */
	void recycle(const CapturedFrame &frame);

	/* True if the camera lists the control in its ControlInfoMap. */
	bool supports(const libcamera::ControlId &id) const;

	/* Controls to send with the next queued request. */
	libcamera::ControlList &pendingControls() { return pendingControls_; }

	unsigned width() const { return width_; }
	unsigned height() const { return height_; }
	bool hasRawStream() const { return rawStream_ != nullptr; }
	uint64_t droppedFrames() const { return dropped_; }
	uint64_t failedFrames() const { return failed_; }
	const std::string &bufferBacking() const { return bufferBacking_; }

private:
	struct Mapping {
		void *address;
		size_t length;
	};

	void logCameras() const;
	void logControls() const;
	bool configure(const Options &options);
	bool mapBuffers(libcamera::Stream *stream);
	void queueDueRequests();
	void onRequestCompleted(libcamera::Request *request);
	bool describe(libcamera::Request *request, CapturedFrame &frame);

	std::unique_ptr<libcamera::CameraManager> manager_;
	std::shared_ptr<libcamera::Camera> camera_;
	std::unique_ptr<libcamera::CameraConfiguration> config_;
	std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
	std::vector<std::unique_ptr<libcamera::Request>> requests_;
	libcamera::Stream *viewfinderStream_ = nullptr;
	libcamera::Stream *rawStream_ = nullptr;
	libcamera::ControlList pendingControls_;

	/* One mmap per DMABUF fd, and per buffer the address of each plane. */
	std::map<int, Mapping> mappings_;
	std::map<const libcamera::FrameBuffer *, std::vector<const uint8_t *>> planes_;
	std::string bufferBacking_;

	unsigned width_ = 0, height_ = 0;
	bool viewfinderIsNv12_ = true;
	Cfa rawCfa_ = Cfa::RGGB;
	unsigned rawBits_ = 10;

	/* Frame clock used when the source does not pace itself. */
	bool pacing_ = true;
	std::chrono::nanoseconds framePeriod_{ 0 };
	std::chrono::steady_clock::time_point nextDue_;

	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<libcamera::Request *> completed_; /* filled by libcamera's thread */
	std::deque<libcamera::Request *> free_;      /* owned by the caller's thread */
	bool interrupted_ = false;
	bool started_ = false;

	bool haveSequence_ = false;
	uint32_t lastSequence_ = 0;
	uint64_t dropped_ = 0;
	uint64_t failed_ = 0;
};

} /* namespace isp */
