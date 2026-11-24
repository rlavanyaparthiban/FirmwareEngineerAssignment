/* SPDX-License-Identifier: MIT */
#include "frame_stats.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <unistd.h>

namespace isp {

FrameStats::FrameStats()
	: start_(Clock::now()), startCpu_(cpuSeconds()), startRss_(residentMiB()),
	  intervalStart_(start_), intervalCpu_(startCpu_)
{
}

double FrameStats::cpuSeconds()
{
	timespec ts;
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
}

double FrameStats::residentMiB()
{
	/* Second field of /proc/self/statm is the resident set, in pages. */
	long pages = 0, resident = 0;
	FILE *file = std::fopen("/proc/self/statm", "r");
	if (!file)
		return 0;
	if (std::fscanf(file, "%ld %ld", &pages, &resident) != 2)
		resident = 0;
	std::fclose(file);
	return double(resident) * double(sysconf(_SC_PAGESIZE)) / (1024.0 * 1024.0);
}

void FrameStats::addFrame(const FrameTimes &t)
{
	frames_++;
	intervalFrames_++;
	sum_.sensor += t.sensor;
	sum_.isp += t.isp;
	sum_.encode += t.encode;
	sum_.total += t.total;
	max_.sensor = std::max(max_.sensor, t.sensor);
	max_.isp = std::max(max_.isp, t.isp);
	max_.encode = std::max(max_.encode, t.encode);
	max_.total = std::max(max_.total, t.total);
	worstTotal_ = std::max(worstTotal_, t.total);
}

std::string FrameStats::report(const std::string &extra)
{
	const Clock::time_point now = Clock::now();
	const double cpu = cpuSeconds();
	const double seconds = std::chrono::duration<double>(now - intervalStart_).count();
	const double elapsed = std::chrono::duration<double>(now - start_).count();
	const double rss = residentMiB();
	const double n = intervalFrames_ ? double(intervalFrames_) : 1.0;
	peakRss_ = std::max(peakRss_, rss);

	char line[512];
	std::snprintf(line, sizeof(line),
		      "stats frames=%llu fps=%.2f avg_fps=%.2f "
		      "sensor_ms=%.2f isp_ms=%.2f encode_ms=%.2f frame_ms=%.2f max_frame_ms=%.2f "
		      "cpu_pct=%.0f rss_mib=%.1f %s",
		      static_cast<unsigned long long>(frames_),
		      seconds > 0 ? intervalFrames_ / seconds : 0.0,
		      elapsed > 0 ? frames_ / elapsed : 0.0,
		      sum_.sensor / n, sum_.isp / n, sum_.encode / n, sum_.total / n, max_.total,
		      seconds > 0 ? (cpu - intervalCpu_) / seconds * 100.0 : 0.0, rss, extra.c_str());

	intervalFrames_ = 0;
	intervalStart_ = now;
	intervalCpu_ = cpu;
	sum_ = FrameTimes();
	max_ = FrameTimes();
	return line;
}

std::string FrameStats::summary() const
{
	const double elapsed = std::chrono::duration<double>(Clock::now() - start_).count();
	const double rss = residentMiB();
	char line[384];
	std::snprintf(line, sizeof(line),
		      "summary frames=%llu seconds=%.1f avg_fps=%.2f worst_frame_ms=%.2f "
		      "cpu_pct=%.0f rss_start_mib=%.1f rss_end_mib=%.1f rss_peak_mib=%.1f",
		      static_cast<unsigned long long>(frames_), elapsed,
		      elapsed > 0 ? frames_ / elapsed : 0.0, worstTotal_,
		      elapsed > 0 ? (cpuSeconds() - startCpu_) / elapsed * 100.0 : 0.0,
		      startRss_, rss, std::max(peakRss_, rss));
	return line;
}

} /* namespace isp */
