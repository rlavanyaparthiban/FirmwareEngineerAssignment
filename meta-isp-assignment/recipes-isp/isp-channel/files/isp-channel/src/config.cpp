/* SPDX-License-Identifier: MIT */
#include "config.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <strings.h>

#include "log.h"

namespace isp {

namespace {

struct ModeName {
	AwbMode mode;
	const char *name;
};

const ModeName kAwbModes[] = {
	{ AwbMode::Auto, "auto" },
	{ AwbMode::Incandescent, "incandescent" },
	{ AwbMode::Tungsten, "tungsten" },
	{ AwbMode::Fluorescent, "fluorescent" },
	{ AwbMode::Indoor, "indoor" },
	{ AwbMode::Daylight, "daylight" },
	{ AwbMode::Cloudy, "cloudy" },
	{ AwbMode::Custom, "custom" },
};

std::string trim(const std::string &s)
{
	const char *ws = " \t\r\n";
	const size_t begin = s.find_first_not_of(ws);
	if (begin == std::string::npos)
		return "";
	return s.substr(begin, s.find_last_not_of(ws) - begin + 1);
}

bool parseFloat(const std::string &text, float &value)
{
	char *end = nullptr;
	const float v = std::strtof(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0')
		return false;
	value = v;
	return true;
}

bool parseUnsigned(const std::string &text, unsigned &value)
{
	char *end = nullptr;
	const unsigned long v = std::strtoul(text.c_str(), &end, 10);
	if (end == text.c_str() || *end != '\0')
		return false;
	value = unsigned(v);
	return true;
}

bool loadFile(const std::string &path, Config &config)
{
	std::ifstream file(path);
	if (!file) {
		LOG_ERROR("cannot open configuration file %s", path.c_str());
		return false;
	}

	std::string line;
	unsigned number = 0;
	while (std::getline(file, line)) {
		number++;
		line = trim(line.substr(0, line.find('#')));
		if (line.empty())
			continue;
		const size_t eq = line.find('=');
		if (eq == std::string::npos ||
		    !setOption(config, trim(line.substr(0, eq)), trim(line.substr(eq + 1)))) {
			LOG_ERROR("%s:%u: invalid setting", path.c_str(), number);
			return false;
		}
	}
	return true;
}

} /* namespace */

bool parseBool(const std::string &text, bool &value)
{
	for (const char *t : { "1", "on", "true", "yes" })
		if (!strcasecmp(text.c_str(), t)) {
			value = true;
			return true;
		}
	for (const char *f : { "0", "off", "false", "no" })
		if (!strcasecmp(text.c_str(), f)) {
			value = false;
			return true;
		}
	return false;
}

bool parseAwbMode(const std::string &text, AwbMode &mode)
{
	for (const ModeName &m : kAwbModes)
		if (!strcasecmp(text.c_str(), m.name)) {
			mode = m.mode;
			return true;
		}
	return false;
}

const char *awbModeName(AwbMode mode)
{
	for (const ModeName &m : kAwbModes)
		if (m.mode == mode)
			return m.name;
	return "?";
}

bool setOption(Config &c, const std::string &key, const std::string &value)
{
	float f;
	if (key == "camera") { c.camera = value; return true; }
	if (key == "width") return parseUnsigned(value, c.width);
	if (key == "height") return parseUnsigned(value, c.height);
	if (key == "fps") { if (!parseFloat(value, f) || f <= 0) return false; c.fps = f; return true; }
	if (key == "frame-pacing") return parseBool(value, c.framePacing);
	if (key == "source-range") {
		if (value != "full" && value != "limited") return false;
		c.fullRangeSource = value == "full";
		return true;
	}
	if (key == "sensor-wb-red") return parseFloat(value, c.sensorWbRed) && c.sensorWbRed > 0;
	if (key == "sensor-wb-blue") return parseFloat(value, c.sensorWbBlue) && c.sensorWbBlue > 0;
	if (key == "threads") return parseUnsigned(value, c.threads);
	if (key == "ae") return parseBool(value, c.controls.aeEnable);
	if (key == "awb") return parseBool(value, c.controls.awbEnable);
	if (key == "awb-mode") return parseAwbMode(value, c.controls.awbMode);
	if (key == "exposure") return parseFloat(value, c.controls.exposureTimeUs);
	if (key == "gain") return parseFloat(value, c.controls.analogueGain);
	if (key == "brightness") return parseFloat(value, c.controls.brightness);
	if (key == "contrast") return parseFloat(value, c.controls.contrast);
	if (key == "ae-target") return parseFloat(value, c.aeTarget) && c.aeTarget > 0;
	if (key == "pipeline") { c.pipeline = value; return true; }
	if (key == "output") { c.outputDir = value; return true; }
	if (key == "socket") { c.socketPath = value; return true; }
	if (key == "stats-interval") return parseUnsigned(value, c.statsIntervalSec) && c.statsIntervalSec > 0;
	if (key == "duration") return parseUnsigned(value, c.durationSec);
	if (key == "frames") return parseUnsigned(value, c.maxFrames);
	if (key == "verbose") return parseBool(value, c.verbose);
	return false;
}

void printUsage(const char *argv0)
{
	std::printf(
		"Usage: %s [--config FILE] [--KEY VALUE]...\n"
		"\n"
		"Camera ISP channel: libcamera capture, software ISP, H.264 encode.\n"
		"\n"
		"  camera ID|INDEX      camera to use (default: first)\n"
		"  width N, height N    viewfinder size (default 1920x1080)\n"
		"  fps F                target frame rate (default 30)\n"
		"  frame-pacing on|off  pace requests in the app (default on)\n"
		"  source-range full|limited   YUV range of the camera frames\n"
		"  sensor-wb-red F, sensor-wb-blue F   virtual sensor illuminant\n"
		"  threads N            ISP worker threads (default: all CPUs)\n"
		"  ae on|off, exposure US, gain F      exposure control\n"
		"  awb on|off, awb-mode MODE           white balance\n"
		"  brightness F, contrast F            tone controls\n"
		"  pipeline DESC        GStreamer pipeline fed by 'appsrc name=src'\n"
		"  output DIR           recordings and snapshots directory\n"
		"  socket PATH          control socket path\n"
		"  stats-interval SEC   statistics period (default 5)\n"
		"  duration SEC, frames N              stop after a time / frame count\n"
		"  verbose on|off       debug logging\n"
		"\n"
		"Signals: SIGUSR1 saves a PNG snapshot, SIGINT/SIGTERM stop cleanly.\n",
		argv0);
}

bool parseArguments(int argc, char **argv, Config &config)
{
	for (int i = 1; i < argc; i++) {
		const std::string arg = argv[i];
		if (arg == "--help" || arg == "-h") {
			printUsage(argv[0]);
			std::exit(0);
		}
		if (arg.rfind("--", 0) != 0 || i + 1 >= argc) {
			LOG_ERROR("unexpected argument '%s' (try --help)", arg.c_str());
			return false;
		}
		const std::string key = arg.substr(2), value = argv[++i];
		if (key == "config") {
			if (!loadFile(value, config))
				return false;
		} else if (!setOption(config, key, value)) {
			LOG_ERROR("invalid option --%s %s", key.c_str(), value.c_str());
			return false;
		}
	}

	if (config.width % 4 || config.height % 2 || config.width < 16 || config.height < 16) {
		LOG_ERROR("width must be a multiple of 4 and height even");
		return false;
	}
	return true;
}

} /* namespace isp */
