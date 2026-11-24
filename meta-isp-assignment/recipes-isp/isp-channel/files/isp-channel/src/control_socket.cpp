/* SPDX-License-Identifier: MIT */
#include "control_socket.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "log.h"

namespace isp {

ControlSocket::~ControlSocket()
{
	stop();
}

bool ControlSocket::start(const std::string &path, Handler handler)
{
	sockaddr_un address = {};
	address.sun_family = AF_UNIX;
	if (path.size() >= sizeof(address.sun_path)) {
		LOG_ERROR("control socket path too long: %s", path.c_str());
		return false;
	}
	std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

	listenFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	wakeFd_ = eventfd(0, EFD_CLOEXEC);
	if (listenFd_ < 0 || wakeFd_ < 0) {
		LOG_ERROR("cannot create control socket: %s", std::strerror(errno));
		stop();
		return false;
	}

	unlink(path.c_str()); /* remove a stale socket from a previous run */
	if (bind(listenFd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0 ||
	    listen(listenFd_, 4) < 0) {
		LOG_ERROR("cannot listen on %s: %s", path.c_str(), std::strerror(errno));
		stop();
		return false;
	}
	chmod(path.c_str(), 0660);

	path_ = path;
	handler_ = std::move(handler);
	thread_ = std::thread([this] { run(); });
	LOG_INFO("control socket: %s", path.c_str());
	return true;
}

void ControlSocket::stop()
{
	if (thread_.joinable()) {
		const uint64_t one = 1;
		if (write(wakeFd_, &one, sizeof(one)) < 0)
			LOG_WARN("control socket wake-up failed: %s", std::strerror(errno));
		thread_.join();
	}
	if (listenFd_ >= 0)
		close(listenFd_);
	if (wakeFd_ >= 0)
		close(wakeFd_);
	listenFd_ = wakeFd_ = -1;
	if (!path_.empty())
		unlink(path_.c_str());
	path_.clear();
}

void ControlSocket::run()
{
	for (;;) {
		pollfd fds[2] = { { listenFd_, POLLIN, 0 }, { wakeFd_, POLLIN, 0 } };
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (fds[1].revents)
			return;

		const int client = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
		if (client < 0)
			continue;

		/* A silent client must not block the next one for long. */
		const timeval timeout = { 2, 0 };
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

		char buffer[512];
		std::string command;
		ssize_t n;
		while (command.find('\n') == std::string::npos && command.size() < sizeof(buffer) &&
		       (n = read(client, buffer, sizeof(buffer))) > 0)
			command.append(buffer, size_t(n));
		command = command.substr(0, command.find('\n'));

		const std::string reply = handler_(command) + "\n";
		if (write(client, reply.data(), reply.size()) < 0)
			LOG_DEBUG("control client went away: %s", std::strerror(errno));
		close(client);
	}
}

} /* namespace isp */
