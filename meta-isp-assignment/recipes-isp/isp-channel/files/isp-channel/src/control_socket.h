/* SPDX-License-Identifier: MIT */
/*
 * Line-based control interface on a UNIX stream socket. One command per
 * connection: the client sends a line, receives a reply, and the connection
 * is closed. See tools/isp-ctl.cpp for the client.
 */
#pragma once

#include <functional>
#include <string>
#include <thread>

namespace isp {

class ControlSocket
{
public:
	using Handler = std::function<std::string(const std::string &command)>;

	ControlSocket() = default;
	~ControlSocket();

	/* Starts listening; the handler runs on the socket's own thread. */
	bool start(const std::string &path, Handler handler);
	void stop();

private:
	void run();

	std::string path_;
	Handler handler_;
	int listenFd_ = -1;
	int wakeFd_ = -1; /* eventfd used to end the poll loop */
	std::thread thread_;
};

} /* namespace isp */
