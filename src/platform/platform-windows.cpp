// Windows implementation of the platform module (the only file using Win32 networking APIs).

#include <cstdio>

#include "platform/platform.hpp"

#include <mbedtls/net_sockets.h>

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>

namespace camdrop::platform {

static std::string narrow(const wchar_t *w)
{
	if (!w)
		return {};
	int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (len <= 1)
		return {};
	std::string out(static_cast<size_t>(len - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), len, nullptr, nullptr);
	return out;
}

static bool looks_virtual(const std::string &name, const std::string &desc)
{
	static const char *markers[] = {"vEthernet", "VirtualBox", "VMware",   "Hyper-V",   "WSL",       "Loopback",
					"TAP-",      "Tailscale",  "ZeroTier", "WireGuard", "Bluetooth", "vpn"};
	for (const char *m : markers) {
		if (name.find(m) != std::string::npos || desc.find(m) != std::string::npos)
			return true;
	}
	return false;
}

std::vector<LocalAddress> local_ipv4_addresses()
{
	std::vector<LocalAddress> out;

	ULONG size = 16 * 1024;
	std::vector<unsigned char> buffer;
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER |
		      GAA_FLAG_INCLUDE_GATEWAYS;
	ULONG ret = ERROR_BUFFER_OVERFLOW;
	for (int attempt = 0; attempt < 3 && ret == ERROR_BUFFER_OVERFLOW; attempt++) {
		buffer.resize(size);
		ret = GetAdaptersAddresses(AF_INET, flags, nullptr,
					   reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()), &size);
	}
	if (ret != NO_ERROR)
		return out;

	for (auto *a = reinterpret_cast<IP_ADAPTER_ADDRESSES *>(buffer.data()); a; a = a->Next) {
		if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
			continue;
		std::string name = narrow(a->FriendlyName);
		std::string desc = narrow(a->Description);
		bool gateway = a->FirstGatewayAddress != nullptr;
		bool is_virtual = looks_virtual(name, desc);

		for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
			if (!u->Address.lpSockaddr || u->Address.lpSockaddr->sa_family != AF_INET)
				continue;
			auto *sin = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
			const uint8_t *b = reinterpret_cast<const uint8_t *>(&sin->sin_addr);
			if (b[0] == 127 || (b[0] == 169 && b[1] == 254))
				continue;
			char ip[32];
			snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
			out.push_back({ip, name, gateway, is_virtual});
		}
	}

	std::stable_sort(out.begin(), out.end(), [](const LocalAddress &x, const LocalAddress &y) {
		if (x.is_virtual != y.is_virtual)
			return !x.is_virtual;
		return x.has_gateway && !y.has_gateway;
	});
	return out;
}

int create_tcp_listener(const char *bind_ip, int port, std::string &error)
{
	static bool wsa_ready = [] {
		WSADATA data;
		return WSAStartup(MAKEWORD(2, 2), &data) == 0;
	}();
	if (!wsa_ready) {
		error = "WSAStartup failed";
		return -1;
	}

	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		error = "socket() failed (" + std::to_string(WSAGetLastError()) + ")";
		return -1;
	}
	BOOL exclusive = TRUE;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive), sizeof(exclusive));
	// Do not leak the socket into child processes.
	SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(port));
	if (inet_pton(AF_INET, bind_ip ? bind_ip : "0.0.0.0", &addr.sin_addr) != 1) {
		closesocket(s);
		error = "invalid bind address";
		return -1;
	}
	if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
		int err = WSAGetLastError();
		closesocket(s);
		if (err == WSAEADDRINUSE || err == WSAEACCES)
			error = "port " + std::to_string(port) + " is already in use";
		else
			error = "bind() failed (" + std::to_string(err) + ")";
		return -1;
	}
	if (listen(s, 64) != 0) {
		error = "listen() failed (" + std::to_string(WSAGetLastError()) + ")";
		closesocket(s);
		return -1;
	}
	return static_cast<int>(s);
}

int peek_first_byte(int fd, int timeout_ms)
{
	SOCKET s = static_cast<SOCKET>(fd);
	WSAPOLLFD pfd{};
	pfd.fd = s;
	pfd.events = POLLRDNORM;
	int r = WSAPoll(&pfd, 1, timeout_ms);
	if (r == 0)
		return -2;
	if (r < 0)
		return -1;
	char c = 0;
	int n = recv(s, &c, 1, MSG_PEEK);
	if (n != 1)
		return -1;
	return static_cast<unsigned char>(c);
}

std::string host_name()
{
	wchar_t buf[256];
	DWORD len = 256;
	if (GetComputerNameExW(ComputerNameDnsHostname, buf, &len))
		return narrow(buf);
	return "localhost";
}

int net_send(void *ctx, const unsigned char *buf, size_t len)
{
	return mbedtls_net_send(ctx, buf, len); // no SIGPIPE on Windows
}

} // namespace camdrop::platform
