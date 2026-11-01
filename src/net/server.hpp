#pragma once

#include "net/http.hpp"
#include "net/websocket.hpp"

#include <atomic>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace camdrop {

struct PeerInfo {
	std::string ip;
	bool loopback = false;
	bool secure = false; // TLS
};

struct TlsCredentials {
	std::string cert_pem;
	std::string key_pem;
};

// HTTPS + WebSocket server on a single TCP port (mbedTLS, one thread per connection).
// Plain HTTP is accepted from loopback only (used by the OBS browser dock); other plain HTTP
// requests are redirected to HTTPS. Connections from non-local addresses are refused.
class Server {
public:
	using HttpHandler = std::function<HttpResponse(const HttpRequest &, const PeerInfo &)>;
	using WsFactory =
		std::function<std::shared_ptr<WsHandler>(const HttpRequest &, const PeerInfo &, HttpResponse &reject)>;

	Server(HttpHandler http, WsFactory ws);
	~Server();

	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	bool start(int port, const TlsCredentials &credentials, std::string &error);
	void stop();
	bool running() const { return running_; }
	int port() const { return port_; }

	// Replaces the certificate for new connections (renewed Let's Encrypt certificate, ...).
	bool set_credentials(const TlsCredentials &credentials, std::string &error);

	struct TlsConfig;
	struct Connection;

private:
	void accept_loop();
	void serve(const std::shared_ptr<Connection> &conn);
	bool serve_websocket(const std::shared_ptr<Connection> &conn, const HttpRequest &req, const PeerInfo &peer);
	void reap_finished();

	HttpHandler http_;
	WsFactory ws_;

	std::atomic<bool> running_{false};
	std::atomic<bool> stopping_{false};
	int listen_fd_ = -1;
	int port_ = 0;
	std::thread accept_thread_;

	std::mutex tls_mutex_;
	std::shared_ptr<TlsConfig> tls_;

	struct Worker {
		std::thread thread;
		std::shared_ptr<std::atomic<bool>> done;
	};
	std::mutex workers_mutex_;
	std::list<Worker> workers_;
};

} // namespace camdrop
