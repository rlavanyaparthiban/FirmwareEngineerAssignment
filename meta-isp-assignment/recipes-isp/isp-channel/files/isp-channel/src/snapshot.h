/* SPDX-License-Identifier: MIT */
/*
 * PNG snapshot writer. Compression runs on its own thread so that saving a
 * snapshot never stalls the frame loop.
 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace isp {

class SnapshotWriter
{
public:
	SnapshotWriter();
	~SnapshotWriter();

	/* Queues an RGB888 image for writing; takes ownership of the pixels. */
	void submit(std::vector<uint8_t> &&rgb, unsigned width, unsigned height, const std::string &path);

	/* Synchronous write; returns false on I/O or encoder errors. */
	static bool writePng(const uint8_t *rgb, unsigned width, unsigned height, const std::string &path);

private:
	struct Job {
		std::vector<uint8_t> rgb;
		unsigned width, height;
		std::string path;
	};

	void run();

	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<Job> jobs_;
	bool exit_ = false;
	std::thread thread_;
};

} /* namespace isp */
