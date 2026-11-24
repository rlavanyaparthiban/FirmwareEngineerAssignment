/* SPDX-License-Identifier: MIT */
/*
 * Run-time configuration. Every option can be given in a "key = value" file
 * (--config FILE) and overridden on the command line (--key value).
 */
#pragma once

#include <string>

#include "isp_control.h"

namespace isp {

struct Config {
	/* Camera */
	std::string camera;          /* camera id or index; empty = first camera */
	unsigned width = 1920;
	unsigned height = 1080;
	double fps = 30.0;
	bool framePacing = true;     /* app-side frame clock, for sources without one */

	/* Virtual sensor (used when the camera has no RAW stream) */
	bool fullRangeSource = true;
	float sensorWbRed = 1.90f;
	float sensorWbBlue = 1.60f;

	/* Software ISP */
	unsigned threads = 0;        /* 0 = one per online CPU */
	IspControls controls;
	float aeTarget = 0.18f;

	/* Encoder: a GStreamer pipeline description fed by "appsrc name=src". */
	std::string pipeline =
		"appsrc name=src ! "
		"x264enc speed-preset=ultrafast tune=zerolatency bitrate=4000 key-int-max=60 ! "
		"splitmuxsink location=%OUTPUT%/isp-%05d.mp4 max-size-time=60000000000 max-files=10";
	std::string outputDir = "/var/lib/isp-channel";

	/* Control and reporting */
	std::string socketPath = "/run/isp-channel/control.sock";
	unsigned statsIntervalSec = 5;
	unsigned durationSec = 0;    /* 0 = run until signalled */
	unsigned maxFrames = 0;      /* 0 = unlimited */
	bool verbose = false;
};

/* Returns false (after logging why) if the arguments are invalid. */
bool parseArguments(int argc, char **argv, Config &config);

/* Applies one "key value" pair; shared by the file and argument parsers. */
bool setOption(Config &config, const std::string &key, const std::string &value);

/* Parses a control value; shared with the control socket. */
bool parseBool(const std::string &text, bool &value);
bool parseAwbMode(const std::string &text, AwbMode &mode);
const char *awbModeName(AwbMode mode);

void printUsage(const char *argv0);

} /* namespace isp */
