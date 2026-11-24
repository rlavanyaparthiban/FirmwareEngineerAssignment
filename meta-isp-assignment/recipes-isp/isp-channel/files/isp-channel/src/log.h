/* SPDX-License-Identifier: MIT */
/*
 * Minimal thread-safe logger. Lines go to stderr with a syslog priority
 * prefix ("<6>" and so on), which systemd-journald turns into the entry's
 * priority, so `journalctl -p warning -u isp-channel` works as expected.
 */
#pragma once

namespace isp {

enum class LogLevel { Error = 3, Warning = 4, Info = 6, Debug = 7 };

void setLogLevel(LogLevel level);
void logMessage(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

} /* namespace isp */

#define LOG_ERROR(...) ::isp::logMessage(::isp::LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(...) ::isp::logMessage(::isp::LogLevel::Warning, __VA_ARGS__)
#define LOG_INFO(...) ::isp::logMessage(::isp::LogLevel::Info, __VA_ARGS__)
#define LOG_DEBUG(...) ::isp::logMessage(::isp::LogLevel::Debug, __VA_ARGS__)
