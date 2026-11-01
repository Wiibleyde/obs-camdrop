#pragma once

// Platform abstraction. Everything OS-specific lives in platform-<os>.cpp so that the rest of the
// plugin stays portable (macOS / Linux ports only need a new implementation of this header).

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace camdrop::platform {

struct LocalAddress {
	std::string ip;             // dotted IPv4
	std::string interface_name; // friendly adapter name
	bool has_gateway = false;   // adapter with a default gateway (most likely the LAN)
	bool is_virtual = false;    // VPN / virtual switch / WSL adapters
};

// Up, non-loopback IPv4 addresses, the most relevant ones first.
std::vector<LocalAddress> local_ipv4_addresses();

// Machine host name (without domain).
std::string host_name();

// Creates a listening TCP socket (exclusive bind, so a port already used by another program is
// reported instead of being shared). Returns the socket as an int usable by mbedtls_net, or -1.
int create_tcp_listener(const char *bind_ip, int port, std::string &error);

// Waits up to `timeout_ms` for data and returns the first byte without consuming it,
// -1 on error/close, -2 on timeout.
int peek_first_byte(int fd, int timeout_ms);

// mbedTLS send callback (context: mbedtls_net_context). Unlike mbedtls_net_send, a peer that went
// away never raises SIGPIPE on Linux and macOS.
int net_send(void *ctx, const unsigned char *buf, size_t len);

// Address ranges accepted by the server: loopback, RFC 1918, link-local, CGNAT (Tailscale).
inline bool is_local_ipv4(const uint8_t ip[4])
{
	if (ip[0] == 127 || ip[0] == 10)
		return true;
	if (ip[0] == 172 && (ip[1] & 0xF0) == 16)
		return true;
	if (ip[0] == 192 && ip[1] == 168)
		return true;
	if (ip[0] == 169 && ip[1] == 254)
		return true;
	if (ip[0] == 100 && (ip[1] & 0xC0) == 64)
		return true;
	return false;
}

inline bool parse_ipv4(const std::string &s, uint8_t out[4])
{
	unsigned a, b, c, d;
	char tail;
	if (sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
		return false;
	if (a > 255 || b > 255 || c > 255 || d > 255)
		return false;
	out[0] = (uint8_t)a;
	out[1] = (uint8_t)b;
	out[2] = (uint8_t)c;
	out[3] = (uint8_t)d;
	return true;
}

} // namespace camdrop::platform
