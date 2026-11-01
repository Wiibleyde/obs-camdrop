#pragma once

#include "net/rate-limiter.hpp"
#include "net/server.hpp"
#include "settings.hpp"
#include "util/json.hpp"

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace camdrop {

class CameraSession;
class Channel;

// Registry of connected cameras, named channels and supervision docks.
class ClientManager {
public:
	static ClientManager &get();

	void start();
	void stop(); // closes every session
	void update_settings(const Settings &settings);
	void disconnect_all(const std::string &reason);

	// WebSocket handlers (created by the HTTP server on upgrade).
	std::shared_ptr<WsHandler> create_camera_handler(const PeerInfo &peer, const std::string &user_agent);
	std::shared_ptr<WsHandler> create_dock_handler(const PeerInfo &peer);
	bool check_code(const std::string &code, const std::string &ip, int &retry_after);

	// Channels are shared by sources and sessions using the same camera name.
	std::shared_ptr<Channel> channel(const std::string &name);

	struct ClientInfo {
		std::string name;
		bool live = false;
	};
	std::vector<ClientInfo> clients() const;
	size_t live_count() const;
	json status_json() const;

	// Handshake of a camera page. Fills `error` ("bad_code", "rate_limited", "full", "name_taken",
	// "bad_name") on failure.
	std::shared_ptr<CameraSession> join(const json &hello, const PeerInfo &peer, const std::string &user_agent,
					    const std::shared_ptr<WsConnection> &ws, std::string &error, int &retry_after);
	void leave(const std::shared_ptr<CameraSession> &session, const std::string &reason);

	void add_dock(const std::shared_ptr<WsConnection> &ws);
	void remove_dock(const WsConnection *ws);
	void handle_dock_message(const json &message);

	// Called by sources when their camera list must refresh, etc.
	Settings settings() const;

private:
	ClientManager() = default;
	void maintenance_loop();

	mutable std::mutex mutex_;
	Settings settings_;
	RateLimiter limiter_;
	std::map<std::string, std::shared_ptr<CameraSession>> sessions_; // by session id
	std::map<std::string, std::weak_ptr<Channel>> channels_;         // by name

	struct Departed {
		std::string name;
		std::string reason;
		int64_t when_ns = 0;
		json last_status;
	};
	std::vector<Departed> departed_; // recently disconnected cameras, for the dock

	std::vector<std::weak_ptr<WsConnection>> docks_;

	std::thread thread_;
	std::mutex thread_mutex_;
	std::condition_variable cv_;
	bool running_ = false;
};

} // namespace camdrop
