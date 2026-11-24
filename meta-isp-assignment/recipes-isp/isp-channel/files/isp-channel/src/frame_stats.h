/* SPDX-License-Identifier: MIT */
/*
 * Per-frame timing, frame rate, CPU and memory accounting, reported as one
 * "key=value" log line per interval so that it is easy to read and to parse.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace isp {

/* Milliseconds spent in each step of one frame. */
struct FrameTimes {
	double sensor = 0; /* virtual sensor (mosaic); 0 with a real RAW stream */
	double isp = 0;    /* statistics + all ISP stages */
	double encode = 0; /* handing the frame to the encoder, incl. back-pressure */
	double total = 0;
};

class FrameStats
{
public:
	FrameStats();

	void addFrame(const FrameTimes &times);

	uint64_t frames() const { return frames_; }

	/* Formats the interval report and starts a new interval. */
	std::string report(const std::string &extra);

	/* Whole-run summary for the exit log. */
	std::string summary() const;

	static double residentMiB();

private:
	using Clock = std::chrono::steady_clock;

	static double cpuSeconds();

	uint64_t frames_ = 0;
	Clock::time_point start_;
	double startCpu_;
	double startRss_;
	double peakRss_ = 0;
	double worstTotal_ = 0;

	/* Current interval. */
	uint64_t intervalFrames_ = 0;
	Clock::time_point intervalStart_;
	double intervalCpu_;
	FrameTimes sum_, max_;
};

} /* namespace isp */
