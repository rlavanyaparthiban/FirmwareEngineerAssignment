/* SPDX-License-Identifier: MIT */
/*
 * isp-channel: camera ISP channel.
 *
 *   libcamera camera -> RAW Bayer -> software ISP -> H.264 -> MP4 segments
 *
 * Signals:
 *   SIGUSR1           save a PNG snapshot of the next frame
 *   SIGINT, SIGTERM   finish the current recording segment and exit
 */
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <thread>

#include "channel.h"
#include "config.h"
#include "log.h"

int main(int argc, char **argv)
{
	isp::Config config;
	if (!isp::parseArguments(argc, argv, config))
		return EXIT_FAILURE;
	if (config.verbose)
		isp::setLogLevel(isp::LogLevel::Debug);

	/*
	 * Block the signals we handle before any thread exists, so that every
	 * thread inherits the mask and only the dedicated thread below ever
	 * receives them (synchronously, through sigwait). No handler runs in
	 * signal context, so no async-signal-safety rules apply.
	 */
	sigset_t signals;
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGUSR1);
	pthread_sigmask(SIG_BLOCK, &signals, nullptr);

	isp::Channel channel(config);

	std::thread signalThread([&] {
		for (;;) {
			int signal = 0;
			if (sigwait(&signals, &signal) != 0)
				continue;
			if (signal == SIGUSR1) {
				channel.requestSnapshot();
				continue;
			}
			channel.requestStop();
			return;
		}
	});

	const int status = channel.init() ? channel.run() : EXIT_FAILURE;

	/* Wake the signal thread if we are exiting for another reason. */
	pthread_kill(signalThread.native_handle(), SIGTERM);
	signalThread.join();
	return status;
}
