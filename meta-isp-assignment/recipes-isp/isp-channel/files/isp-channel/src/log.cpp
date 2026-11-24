/* SPDX-License-Identifier: MIT */
#include "log.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace isp {

namespace {
std::atomic<int> threshold{ int(LogLevel::Info) };
std::mutex outputMutex;
} /* namespace */

void setLogLevel(LogLevel level)
{
	threshold = int(level);
}

void logMessage(LogLevel level, const char *fmt, ...)
{
	if (int(level) > threshold)
		return;

	char text[1024];
	va_list args;
	va_start(args, fmt);
	std::vsnprintf(text, sizeof(text), fmt, args);
	va_end(args);

	std::lock_guard<std::mutex> lock(outputMutex);
	std::fprintf(stderr, "<%d>isp-channel: %s\n", int(level), text);
	std::fflush(stderr);
}

} /* namespace isp */
