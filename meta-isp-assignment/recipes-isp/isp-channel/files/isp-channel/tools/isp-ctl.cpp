/* SPDX-License-Identifier: MIT */
/*
 * isp-ctl: command-line client for the isp-channel control socket.
 *
 *   isp-ctl snapshot
 *   isp-ctl set Brightness 0.2
 *   isp-ctl get
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	const char *path = "/run/isp-channel/control.sock";
	int first = 1;
	if (argc > 2 && !std::strcmp(argv[1], "-s")) {
		path = argv[2];
		first = 3;
	}
	if (first >= argc) {
		std::fprintf(stderr,
			     "Usage: %s [-s SOCKET] COMMAND...\n"
			     "  snapshot              save a PNG of the next frame\n"
			     "  get                   show controls and statistics\n"
			     "  set CONTROL VALUE     AeEnable, AwbEnable, AwbMode, ExposureTime,\n"
			     "                        AnalogueGain, Brightness, Contrast, ColourGains\n",
			     argv[0]);
		return 2;
	}

	std::string command;
	for (int i = first; i < argc; i++)
		command += (i > first ? " " : "") + std::string(argv[i]);
	command += "\n";

	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	std::strncpy(address.sun_path, path, sizeof(address.sun_path) - 1);

	const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
		std::perror(path);
		return 1;
	}
	if (write(fd, command.data(), command.size()) != ssize_t(command.size())) {
		std::perror("write");
		return 1;
	}

	char buffer[1024];
	std::string reply;
	ssize_t n;
	while ((n = read(fd, buffer, sizeof(buffer))) > 0)
		reply.append(buffer, size_t(n));
	close(fd);

	std::fputs(reply.c_str(), stdout);
	return reply.rfind("ERROR", 0) == 0 ? 1 : 0;
}
