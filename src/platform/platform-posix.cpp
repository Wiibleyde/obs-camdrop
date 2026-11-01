// POSIX (macOS / Linux) implementation of the platform module.

#include <cstdio>

#include "platform/platform.hpp"

#include <mbedtls/net_sockets.h>

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <poll.h>
#include <sys/socket.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>

#include <algorithm>

namespace camdrop::platform {

std::vector<LocalAddress> local_ipv4_addresses()
{
	std::vector<LocalAddress> out;
	ifaddrs *list = nullptr;
	if (getifaddrs(&list) != 0)
		return out;
	for (ifaddrs *i = list; i; i = i->ifa_next) {
		if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET)
			continue;
		if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK))
			continue;
		auto *sin = reinterpret_cast<sockaddr_in *>(i->ifa_addr);
		const uint8_t *b = reinterpret_cast<const uint8_t *>(&sin->sin_addr);
		if (b[0] == 169 && b[1] == 254)
			continue;
		char ip[32];
		snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
		std::string name = i->ifa_name ? i->ifa_name : "";
		bool is_virtual = name.rfind("docker", 0) == 0 || name.rfind("veth", 0) == 0 ||
				  name.rfind("br-", 0) == 0 || name.rfind("utun", 0) == 0 ||
				  name.rfind("tailscale", 0) == 0 || name.rfind("virbr", 0) == 0;
		out.push_back({ip, name, false, is_virtual});
	}
	freeifaddrs(list);
	std::stable_sort(out.begin(), out.end(),
			 [](const LocalAddress &x, const LocalAddress &y) { return !x.is_virtual && y.is_virtual; });
	return out;
}

int create_tcp_listener(const char *bind_ip, int port, std::string &error)
{
	int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) {
		error = "socket() failed";
		return -1;
	}
	fcntl(s, F_SETFD, FD_CLOEXEC); // macOS has no SOCK_CLOEXEC
	int one = 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<uint16_t>(port));
	if (inet_pton(AF_INET, bind_ip ? bind_ip : "0.0.0.0", &addr.sin_addr) != 1) {
		close(s);
		error = "invalid bind address";
		return -1;
	}
	if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		error = errno == EADDRINUSE ? "port " + std::to_string(port) + " is already in use" : "bind() failed";
		close(s);
		return -1;
	}
	if (listen(s, 64) != 0) {
		error = "listen() failed";
		close(s);
		return -1;
	}
	return s;
}

int peek_first_byte(int fd, int timeout_ms)
{
	pollfd pfd{};
	pfd.fd = fd;
	pfd.events = POLLIN;
	int r = poll(&pfd, 1, timeout_ms);
	if (r == 0)
		return -2;
	if (r < 0)
		return -1;
	unsigned char c = 0;
	ssize_t n = recv(fd, &c, 1, MSG_PEEK);
	if (n != 1)
		return -1;
	return c;
}

int net_send(void *ctx, const unsigned char *buf, size_t len)
{
	const int fd = static_cast<mbedtls_net_context *>(ctx)->fd;
	if (fd < 0)
		return MBEDTLS_ERR_NET_INVALID_CONTEXT;
#ifdef MSG_NOSIGNAL
	const ssize_t ret = ::send(fd, buf, len, MSG_NOSIGNAL);
#else
	int one = 1; // macOS: per-socket option instead of a send flag
	setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
	const ssize_t ret = ::send(fd, buf, len, 0);
#endif
	if (ret >= 0)
		return static_cast<int>(ret);
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
		return MBEDTLS_ERR_SSL_WANT_WRITE;
	if (errno == EPIPE || errno == ECONNRESET)
		return MBEDTLS_ERR_NET_CONN_RESET;
	return MBEDTLS_ERR_NET_SEND_FAILED;
}

std::string host_name()
{
	char buf[256] = {};
	if (gethostname(buf, sizeof(buf) - 1) != 0)
		return "localhost";
	std::string name = buf;
	size_t dot = name.find('.');
	if (dot != std::string::npos)
		name.resize(dot);
	return name;
}

} // namespace camdrop::platform
