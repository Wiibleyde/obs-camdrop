#pragma once

#include "net/server.hpp"
#include "settings.hpp"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace camdrop {

// Plugin-wide state: settings, certificate and the HTTPS/WebSocket server.
class App {
public:
	static App &get();

	void load();   // obs_module_load
	void unload(); // obs_module_unload

	Settings settings() const;
	// Saves the settings and restarts the server when needed. Returns false with `error` if the
	// server could not start (the settings are saved anyway).
	bool apply(const Settings &settings, std::string &error);
	bool regenerate_certificate(std::string &error);

	struct Status {
		bool running = false;
		std::string error;
		int port = 0;
		std::vector<std::string> camera_urls; // https://ip:port/?code=...
		std::string dock_url;                 // http://127.0.0.1:port/dock?code=...
		std::string cert_summary;
		bool self_signed = true;
		size_t clients = 0;
	};
	Status status() const;

	std::string web_root() const { return web_root_; }

	// UI listeners (settings dialog, dock) are told when the status changes. Called on any thread.
	int add_listener(std::function<void()> fn);
	void remove_listener(int id);

private:
	App() = default;
	bool start_server_locked(std::string &error);
	void stop_server_locked();
	bool load_credentials(TlsCredentials &creds, std::string &error, bool regenerate = false);
	void notify();
	void watch_loop();

	HttpResponse handle_http(const HttpRequest &req, const PeerInfo &peer);
	std::shared_ptr<WsHandler> handle_ws(const HttpRequest &req, const PeerInfo &peer, HttpResponse &reject);
	HttpResponse serve_file(const std::string &relative);

	mutable std::recursive_mutex mutex_;
	Settings settings_;
	std::unique_ptr<Server> server_;
	std::string last_error_;
	std::string cert_pem_;
	// Copy of the self-signed certificate served at /camdrop.crt. Server threads read it without
	// taking `mutex_`, which is held while the server stops (and joins those threads).
	mutable std::mutex served_cert_mutex_;
	std::string served_cert_pem_;
	void set_cert_pem(const std::string &pem);
	std::string web_root_;
	int64_t cert_mtime_ = 0;
	int64_t key_mtime_ = 0;

	std::mutex listeners_mutex_;
	std::vector<std::pair<int, std::function<void()>>> listeners_;
	int next_listener_ = 1;

	std::thread watch_thread_;
	std::mutex watch_mutex_;
	std::condition_variable watch_cv_;
	bool watching_ = false;
};

} // namespace camdrop
