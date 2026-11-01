#include "net/server.hpp"

#include "platform/platform.hpp"
#include "plugin-support.h"
#include "util/random.hpp"
#include "util/strings.hpp"

#include <obs-module.h>

#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <psa/crypto.h>

#include <chrono>
#include <cstring>
#include <deque>

namespace camdrop {

namespace {

constexpr uint32_t kPollMs = 20;            // read timeout of the connection loops
constexpr int64_t kHandshakeTimeoutMs = 10000;
constexpr int64_t kRequestTimeoutMs = 15000; // idle keep-alive connection
constexpr int64_t kWsPingIntervalMs = 15000;
constexpr int64_t kWsIdleTimeoutMs = 45000;
constexpr size_t kMaxConnections = 96;
constexpr size_t kMaxWsMessage = 1024 * 1024;

// Handshakes are serialized: mbedTLS is built without MBEDTLS_THREADING_C, and the shared
// certificate key / EC group caches must not be used concurrently. Each call is short because
// reads time out after kPollMs; the lock is released between calls.
std::mutex g_handshake_mutex;

int64_t now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string mbed_error(int code)
{
	char buf[128];
	mbedtls_strerror(code, buf, sizeof(buf));
	return buf;
}

const int kCiphersuites[] = {
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
	MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
	MBEDTLS_TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256,
	0,
};

const char *kAlpn[] = {"http/1.1", nullptr};

} // namespace

// ---------------------------------------------------------------------------------------------
// TLS configuration (shared by connections, kept alive while they use it)

struct Server::TlsConfig {
	mbedtls_ssl_config conf;
	mbedtls_x509_crt cert;
	mbedtls_pk_context key;

	TlsConfig()
	{
		mbedtls_ssl_config_init(&conf);
		mbedtls_x509_crt_init(&cert);
		mbedtls_pk_init(&key);
	}

	~TlsConfig()
	{
		mbedtls_ssl_config_free(&conf);
		mbedtls_x509_crt_free(&cert);
		mbedtls_pk_free(&key);
	}

	bool setup(const TlsCredentials &creds, std::string &error)
	{
		static std::once_flag psa_once;
		std::call_once(psa_once, [] { psa_crypto_init(); });

		int ret = mbedtls_x509_crt_parse(&cert, reinterpret_cast<const unsigned char *>(creds.cert_pem.c_str()),
						 creds.cert_pem.size() + 1);
		if (ret != 0) {
			error = "certificate: " + mbed_error(ret);
			return false;
		}
		ret = mbedtls_pk_parse_key(&key, reinterpret_cast<const unsigned char *>(creds.key_pem.c_str()),
					   creds.key_pem.size() + 1, nullptr, 0, random_mbedtls, nullptr);
		if (ret != 0) {
			error = "private key: " + mbed_error(ret);
			return false;
		}
		ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
						  MBEDTLS_SSL_PRESET_DEFAULT);
		if (ret != 0) {
			error = "TLS config: " + mbed_error(ret);
			return false;
		}
		mbedtls_ssl_conf_rng(&conf, random_mbedtls, nullptr);
		// TLS 1.2 only: TLS 1.3 in mbedTLS 3.6 relies on PSA key slots, which are not
		// thread-safe in this build.
		mbedtls_ssl_conf_min_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_max_tls_version(&conf, MBEDTLS_SSL_VERSION_TLS1_2);
		mbedtls_ssl_conf_ciphersuites(&conf, kCiphersuites);
		mbedtls_ssl_conf_read_timeout(&conf, kPollMs);
		mbedtls_ssl_conf_alpn_protocols(&conf, kAlpn);
		ret = mbedtls_ssl_conf_own_cert(&conf, &cert, &key);
		if (ret != 0) {
			error = "certificate/key: " + mbed_error(ret);
			return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------------------------------------
// Connection

struct Server::Connection : public WsConnection, public std::enable_shared_from_this<Server::Connection> {
	mbedtls_net_context net;
	mbedtls_ssl_context ssl;
	bool tls = false;
	std::shared_ptr<TlsConfig> config;
	std::string ip;
	bool loopback = false;
	std::atomic<bool> *stopping = nullptr;

	std::mutex out_mutex;
	std::deque<std::string> out_queue; // raw frames
	bool close_requested = false;
	uint16_t close_code = 1000;
	std::string close_reason;

	Connection()
	{
		mbedtls_net_init(&net);
		mbedtls_ssl_init(&ssl);
	}

	~Connection() override
	{
		mbedtls_ssl_free(&ssl);
		mbedtls_net_free(&net);
	}

	// > 0: bytes read, 0: nothing yet (timeout), < 0: closed or error.
	int read(unsigned char *buf, size_t len)
	{
		if (tls) {
			int ret = mbedtls_ssl_read(&ssl, buf, len);
			if (ret > 0)
				return ret;
			if (ret == MBEDTLS_ERR_SSL_TIMEOUT || ret == MBEDTLS_ERR_SSL_WANT_READ ||
			    ret == MBEDTLS_ERR_SSL_WANT_WRITE)
				return 0;
			return -1;
		}
		int ret = mbedtls_net_recv_timeout(&net, buf, len, kPollMs);
		if (ret > 0)
			return ret;
		if (ret == MBEDTLS_ERR_SSL_TIMEOUT)
			return 0;
		return -1;
	}

	bool write_all(const std::string &data)
	{
		const auto *p = reinterpret_cast<const unsigned char *>(data.data());
		size_t left = data.size();
		const int64_t deadline = now_ms() + 10000;
		while (left > 0) {
			int ret = tls ? mbedtls_ssl_write(&ssl, p, left) : mbedtls_net_send(&net, p, left);
			if (ret > 0) {
				p += ret;
				left -= static_cast<size_t>(ret);
				continue;
			}
			if ((ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) && now_ms() < deadline)
				continue;
			return false;
		}
		return true;
	}

	void send_text(const std::string &message) override
	{
		std::lock_guard<std::mutex> lock(out_mutex);
		if (close_requested)
			return;
		out_queue.push_back(ws_build_frame(WS_TEXT, message));
	}

	void close(uint16_t code, const std::string &reason) override
	{
		std::lock_guard<std::mutex> lock(out_mutex);
		if (close_requested)
			return;
		close_requested = true;
		close_code = code;
		close_reason = reason.substr(0, 120);
	}

	const std::string &remote_ip() const override { return ip; }
};

// ---------------------------------------------------------------------------------------------

Server::Server(HttpHandler http, WsFactory ws) : http_(std::move(http)), ws_(std::move(ws)) {}

Server::~Server()
{
	stop();
}

bool Server::set_credentials(const TlsCredentials &credentials, std::string &error)
{
	auto cfg = std::make_shared<TlsConfig>();
	if (!cfg->setup(credentials, error))
		return false;
	std::lock_guard<std::mutex> lock(tls_mutex_);
	tls_ = std::move(cfg);
	return true;
}

bool Server::start(int port, const TlsCredentials &credentials, std::string &error)
{
	stop();
	if (!set_credentials(credentials, error))
		return false;

	listen_fd_ = platform::create_tcp_listener("0.0.0.0", port, error);
	if (listen_fd_ < 0)
		return false;

	port_ = port;
	stopping_ = false;
	running_ = true;
	accept_thread_ = std::thread([this] { accept_loop(); });
	obs_log(LOG_INFO, "Server listening on port %d", port);
	return true;
}

void Server::stop()
{
	if (!running_ && !accept_thread_.joinable())
		return;
	stopping_ = true;
	if (accept_thread_.joinable())
		accept_thread_.join();
	if (listen_fd_ >= 0) {
		mbedtls_net_context ctx;
		ctx.fd = listen_fd_;
		mbedtls_net_free(&ctx);
		listen_fd_ = -1;
	}

	std::list<Worker> workers;
	{
		std::lock_guard<std::mutex> lock(workers_mutex_);
		workers.swap(workers_);
	}
	for (auto &w : workers)
		if (w.thread.joinable())
			w.thread.join();

	running_ = false;
	obs_log(LOG_INFO, "Server stopped");
}

void Server::reap_finished()
{
	std::lock_guard<std::mutex> lock(workers_mutex_);
	for (auto it = workers_.begin(); it != workers_.end();) {
		if (it->done->load()) {
			if (it->thread.joinable())
				it->thread.join();
			it = workers_.erase(it);
		} else {
			++it;
		}
	}
}

void Server::accept_loop()
{
	mbedtls_net_context listener;
	listener.fd = listen_fd_;

	while (!stopping_) {
		int ready = mbedtls_net_poll(&listener, MBEDTLS_NET_POLL_READ, 200);
		reap_finished();
		if (ready <= 0)
			continue;

		auto conn = std::make_shared<Connection>();
		unsigned char ip_raw[16] = {};
		size_t ip_len = 0;
		if (mbedtls_net_accept(&listener, &conn->net, ip_raw, sizeof(ip_raw), &ip_len) != 0)
			continue;

		if (ip_len != 4 || !platform::is_local_ipv4(ip_raw)) {
			// Local network only: refuse anything coming from a public address.
			continue; // conn destructor closes the socket
		}
		char ip[32];
		snprintf(ip, sizeof(ip), "%u.%u.%u.%u", ip_raw[0], ip_raw[1], ip_raw[2], ip_raw[3]);
		conn->ip = ip;
		conn->loopback = ip_raw[0] == 127;
		conn->stopping = &stopping_;

		{
			std::lock_guard<std::mutex> lock(workers_mutex_);
			if (workers_.size() >= kMaxConnections)
				continue;
			auto done = std::make_shared<std::atomic<bool>>(false);
			workers_.push_back(Worker{std::thread([this, conn, done] {
							  serve(conn);
							  done->store(true);
						  }),
						  done});
		}
	}
}

void Server::serve(const std::shared_ptr<Connection> &conn)
{
	// Plain HTTP (loopback dock) or TLS? A TLS connection starts with a handshake record (0x16).
	int first = platform::peek_first_byte(conn->net.fd, 5000);
	if (first < 0)
		return;

	PeerInfo peer;
	peer.ip = conn->ip;
	peer.loopback = conn->loopback;

	if (first == 0x16) {
		{
			std::lock_guard<std::mutex> lock(tls_mutex_);
			conn->config = tls_;
		}
		if (!conn->config || mbedtls_ssl_setup(&conn->ssl, &conn->config->conf) != 0)
			return;
		mbedtls_ssl_set_bio(&conn->ssl, &conn->net, mbedtls_net_send, nullptr, mbedtls_net_recv_timeout);
		conn->tls = true;

		const int64_t deadline = now_ms() + kHandshakeTimeoutMs;
		for (;;) {
			int ret;
			{
				std::lock_guard<std::mutex> lock(g_handshake_mutex);
				ret = mbedtls_ssl_handshake(&conn->ssl);
			}
			if (ret == 0)
				break;
			if ((ret == MBEDTLS_ERR_SSL_TIMEOUT || ret == MBEDTLS_ERR_SSL_WANT_READ ||
			     ret == MBEDTLS_ERR_SSL_WANT_WRITE) &&
			    now_ms() < deadline && !stopping_) {
				continue;
			}
			// Typical: the browser refuses the self-signed certificate on its first visit.
			obs_log(LOG_DEBUG, "TLS handshake with %s failed: %s", conn->ip.c_str(), mbed_error(ret).c_str());
			return;
		}
		peer.secure = true;
	}

	std::string inbuf;
	unsigned char buf[16384];
	int64_t deadline = now_ms() + kRequestTimeoutMs;

	while (!stopping_) {
		HttpRequest req;
		size_t consumed = 0;
		ParseResult pr = parse_http_request(inbuf, req, consumed);
		if (pr == ParseResult::Error) {
			conn->write_all(serialize_response(HttpResponse::text(400, "Bad Request"), false));
			return;
		}
		if (pr == ParseResult::NeedMore) {
			if (now_ms() > deadline)
				return;
			int n = conn->read(buf, sizeof(buf));
			if (n < 0)
				return;
			if (n > 0)
				inbuf.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
			continue;
		}
		inbuf.erase(0, consumed);
		deadline = now_ms() + kRequestTimeoutMs;

		if (!peer.secure && !peer.loopback) {
			// Cameras need HTTPS (getUserMedia): send them to the right URL.
			std::string host = req.header("host");
			size_t colon = host.rfind(':');
			if (colon != std::string::npos && host.find(']') == std::string::npos)
				host = host.substr(0, colon);
			if (host.empty())
				host = "localhost";
			std::string location = "https://" + host + ":" + std::to_string(port_) + req.target;
			conn->write_all(serialize_response(HttpResponse::redirect(location), false));
			return;
		}

		if (req.wants_websocket()) {
			serve_websocket(conn, req, peer);
			return;
		}

		HttpResponse res;
		try {
			res = http_(req, peer);
		} catch (const std::exception &e) {
			obs_log(LOG_WARNING, "HTTP handler error: %s", e.what());
			res = HttpResponse::text(500, "Internal Server Error");
		}
		const bool keep_alive = req.keep_alive();
		if (!conn->write_all(serialize_response(res, keep_alive, req.method == "HEAD")) || !keep_alive)
			return;
	}
}

bool Server::serve_websocket(const std::shared_ptr<Connection> &conn, const HttpRequest &req, const PeerInfo &peer)
{
	if (req.method != "GET" || req.header("sec-websocket-version") != "13" || req.header("sec-websocket-key").empty()) {
		conn->write_all(serialize_response(HttpResponse::text(400, "Bad WebSocket request"), false));
		return false;
	}

	HttpResponse reject = HttpResponse::text(404, "Not Found");
	std::shared_ptr<WsHandler> handler;
	try {
		handler = ws_(req, peer, reject);
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "WebSocket factory error: %s", e.what());
	}
	if (!handler) {
		conn->write_all(serialize_response(reject, false));
		return false;
	}

	HttpResponse res;
	res.status = 101;
	res.set("Upgrade", "websocket");
	res.set("Connection", "Upgrade");
	res.set("Sec-WebSocket-Accept", websocket_accept_key(req.header("sec-websocket-key")));
	if (!conn->write_all(serialize_response(res, true)))
		return false;

	handler->on_open(conn);

	std::string inbuf;
	std::string message;
	uint8_t message_opcode = 0;
	bool in_message = false;
	unsigned char buf[16384];
	int64_t last_activity = now_ms();
	int64_t last_ping = now_ms();
	bool closing = false;

	while (!stopping_ && !closing) {
		int n = conn->read(buf, sizeof(buf));
		if (n < 0)
			break;
		if (n > 0) {
			inbuf.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
			last_activity = now_ms();
		}

		for (;;) {
			WsFrame frame;
			size_t consumed = 0;
			WsParse r = ws_parse_frame(inbuf, consumed, frame, kMaxWsMessage);
			if (r == WsParse::NeedMore)
				break;
			if (r == WsParse::Error) {
				closing = true;
				break;
			}
			inbuf.erase(0, consumed);

			switch (frame.opcode) {
			case WS_TEXT:
			case WS_BINARY:
				if (in_message) {
					closing = true;
					break;
				}
				if (frame.fin) {
					if (frame.opcode == WS_TEXT)
						handler->on_message(frame.payload);
				} else {
					in_message = true;
					message_opcode = frame.opcode;
					message = std::move(frame.payload);
				}
				break;
			case WS_CONTINUATION:
				if (!in_message || message.size() + frame.payload.size() > kMaxWsMessage) {
					closing = true;
					break;
				}
				message += frame.payload;
				if (frame.fin) {
					in_message = false;
					if (message_opcode == WS_TEXT)
						handler->on_message(message);
					message.clear();
				}
				break;
			case WS_PING:
				conn->write_all(ws_build_frame(WS_PONG, frame.payload));
				break;
			case WS_PONG:
				break;
			case WS_CLOSE:
				conn->write_all(ws_build_frame(WS_CLOSE, frame.payload.substr(0, 2)));
				closing = true;
				break;
			default:
				closing = true;
				break;
			}
			if (closing)
				break;
		}

		// Flush queued messages.
		std::deque<std::string> out;
		bool close_now = false;
		uint16_t code = 1000;
		std::string reason;
		{
			std::lock_guard<std::mutex> lock(conn->out_mutex);
			out.swap(conn->out_queue);
			close_now = conn->close_requested;
			code = conn->close_code;
			reason = conn->close_reason;
		}
		for (const auto &frame : out) {
			if (!conn->write_all(frame)) {
				closing = true;
				break;
			}
		}
		if (close_now && !closing) {
			std::string payload;
			payload.push_back(static_cast<char>(code >> 8));
			payload.push_back(static_cast<char>(code & 0xff));
			payload += reason;
			conn->write_all(ws_build_frame(WS_CLOSE, payload));
			closing = true;
		}

		const int64_t now = now_ms();
		if (now - last_ping > kWsPingIntervalMs) {
			conn->write_all(ws_build_frame(WS_PING, "camdrop"));
			last_ping = now;
		}
		if (now - last_activity > kWsIdleTimeoutMs)
			break;
	}

	{
		std::lock_guard<std::mutex> lock(conn->out_mutex);
		conn->close_requested = true;
	}
	if (conn->tls)
		mbedtls_ssl_close_notify(&conn->ssl);
	handler->on_close();
	return true;
}

} // namespace camdrop
