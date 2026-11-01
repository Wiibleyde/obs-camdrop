#include "core/client-manager.hpp"

#include "core/camera-session.hpp"
#include "core/channel.hpp"
#include "plugin-support.h"
#include "util/random.hpp"
#include "util/strings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>

namespace camdrop {

namespace {

constexpr int64_t kSec = 1000000000LL;
constexpr int64_t kClearDelayNs = 3 * kSec;     // blank sources 3 s after a disconnection
constexpr int64_t kDepartedKeepNs = 120 * kSec; // dock keeps disconnected cameras for 2 min

int64_t now_ns()
{
	return static_cast<int64_t>(os_gettime_ns());
}

std::string sanitize_client_id(const std::string &id)
{
	std::string out;
	for (char c : id) {
		if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')
			out.push_back(c);
		if (out.size() >= 64)
			break;
	}
	return out;
}

// --------------------------------------------------------------------------------------------

class CameraWsHandler : public WsHandler {
public:
	CameraWsHandler(PeerInfo peer, std::string user_agent) : peer_(std::move(peer)), ua_(std::move(user_agent)) {}

	void on_open(const std::shared_ptr<WsConnection> &connection) override { ws_ = connection; }

	void on_message(const std::string &text) override
	{
		auto ws = ws_.lock();
		if (!ws)
			return;
		json j = json::parse(text, nullptr, false);
		if (j.is_discarded() || !j.is_object())
			return;

		if (!session_) {
			if (json_get<std::string>(j, "type", "") != "hello") {
				ws->send_text(json{{"type", "error"}, {"code", "hello_required"}}.dump());
				return;
			}
			std::string error;
			int retry_after = 0;
			session_ = ClientManager::get().join(j, peer_, ua_, ws, error, retry_after);
			if (!session_) {
				ws->send_text(
					json{{"type", "error"}, {"code", error}, {"retryAfter", retry_after}}.dump());
				ws->close(4000, error);
				return;
			}
			const Settings s = ClientManager::get().settings();
			ws->send_text(json{
				{"type", "welcome"},
				{"id", session_->id()},
				{"name", session_->name()},
				{"version", PLUGIN_VERSION},
				{"video",
				 {{"height", s.video_height},
				  {"fps", s.video_fps},
				  {"maxBitrateKbps", s.max_bitrate_kbps},
				  {"codec", s.preferred_codec},
				  {"orientation", s.orientation}}},
				{"audio", {{"opus10ms", s.opus_10ms}}},
				{"tally",
				 s.tally}}.dump());
			ClientManager::get().request_tally_update();
			return;
		}
		session_->handle_message(j);
	}

	void on_close() override
	{
		if (session_)
			ClientManager::get().leave(session_, session_->closed() ? "closed" : "disconnected");
		session_.reset();
	}

private:
	PeerInfo peer_;
	std::string ua_;
	std::weak_ptr<WsConnection> ws_;
	std::shared_ptr<CameraSession> session_;
};

class DockWsHandler : public WsHandler {
public:
	void on_open(const std::shared_ptr<WsConnection> &connection) override
	{
		ws_ = connection;
		ClientManager::get().add_dock(connection);
		connection->send_text(ClientManager::get().status_json().dump());
	}

	void on_message(const std::string &text) override
	{
		json j = json::parse(text, nullptr, false);
		if (!j.is_discarded() && j.is_object())
			ClientManager::get().handle_dock_message(j);
	}

	void on_close() override
	{
		if (auto ws = ws_.lock())
			ClientManager::get().remove_dock(ws.get());
	}

private:
	std::weak_ptr<WsConnection> ws_;
};

} // namespace

// --------------------------------------------------------------------------------------------

ClientManager &ClientManager::get()
{
	static ClientManager instance;
	return instance;
}

void ClientManager::start()
{
	std::lock_guard<std::mutex> lock(thread_mutex_);
	if (running_)
		return;
	running_ = true;
	thread_ = std::thread([this] { maintenance_loop(); });
}

void ClientManager::stop()
{
	disconnect_all("server_stopped");
	{
		std::lock_guard<std::mutex> lock(thread_mutex_);
		if (!running_)
			return;
		running_ = false;
	}
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();

	// Blank every source.
	std::vector<std::shared_ptr<Channel>> channels;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &[name, weak] : channels_)
			if (auto c = weak.lock())
				channels.push_back(c);
		docks_.clear();
	}
	for (auto &c : channels)
		c->clear();
}

void ClientManager::disconnect_all(const std::string &reason)
{
	std::map<std::string, std::shared_ptr<CameraSession>> sessions;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		sessions.swap(sessions_);
	}
	for (auto &[id, s] : sessions)
		s->close(reason, true);
}

void ClientManager::update_settings(const Settings &settings)
{
	std::vector<std::shared_ptr<CameraSession>> sessions;
	bool changed;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		changed = settings_.orientation != settings.orientation || settings_.tally != settings.tally;
		settings_ = settings;
		if (changed)
			for (auto &[id, s] : sessions_)
				sessions.push_back(s);
	}
	for (auto &s : sessions)
		s->send({{"type", "config"}, {"orientation", settings.orientation}, {"tally", settings.tally}});
	if (changed)
		request_tally_update();
}

Settings ClientManager::settings() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return settings_;
}

bool ClientManager::check_code(const std::string &code, const std::string &ip, int &retry_after)
{
	retry_after = limiter_.retry_after(ip);
	if (retry_after > 0)
		return false;
	const std::string expected = settings().access_code;
	if (expected.empty() || !secure_equals(code, expected)) {
		limiter_.record_failure(ip);
		retry_after = limiter_.retry_after(ip);
		return false;
	}
	limiter_.record_success(ip);
	return true;
}

std::shared_ptr<WsHandler> ClientManager::create_camera_handler(const PeerInfo &peer, const std::string &user_agent)
{
	return std::make_shared<CameraWsHandler>(peer, user_agent.substr(0, 256));
}

std::shared_ptr<WsHandler> ClientManager::create_dock_handler(const PeerInfo &)
{
	return std::make_shared<DockWsHandler>();
}

std::shared_ptr<Channel> ClientManager::channel(const std::string &name)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = channels_.find(name);
	if (it != channels_.end()) {
		if (auto c = it->second.lock())
			return c;
	}
	auto c = std::make_shared<Channel>(name);
	channels_[name] = c;
	return c;
}

std::shared_ptr<CameraSession> ClientManager::join(const json &hello, const PeerInfo &peer,
						   const std::string &user_agent,
						   const std::shared_ptr<WsConnection> &ws, std::string &error,
						   int &retry_after)
{
	const std::string code = json_get<std::string>(hello, "code", "");
	retry_after = limiter_.retry_after(peer.ip);
	if (retry_after > 0) {
		error = "rate_limited";
		return nullptr;
	}
	if (!check_code(code, peer.ip, retry_after)) {
		error = "bad_code";
		return nullptr;
	}

	const std::string name = sanitize_name(json_get<std::string>(hello, "name", ""));
	const std::string client_id = sanitize_client_id(json_get<std::string>(hello, "clientId", ""));
	if (name.empty()) {
		error = "bad_name";
		return nullptr;
	}

	std::shared_ptr<CameraSession> replaced;
	std::shared_ptr<CameraSession> session;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		size_t active = 0;
		for (auto &[id, s] : sessions_) {
			if (s->name() == name) {
				if (!client_id.empty() && s->client_id() == client_id) {
					replaced = s; // same device reconnecting: the new session wins
					continue;
				}
				error = "name_taken";
				return nullptr;
			}
			active++;
		}
		if (active >= static_cast<size_t>(std::max(1, settings_.max_clients))) {
			error = "full";
			return nullptr;
		}
		if (replaced)
			sessions_.erase(replaced->id());

		std::shared_ptr<Channel> ch;
		auto it = channels_.find(name);
		if (it != channels_.end())
			ch = it->second.lock();
		if (!ch) {
			ch = std::make_shared<Channel>(name);
			channels_[name] = ch;
		}

		SessionConfig cfg;
		cfg.udp_port_min = settings_.udp_port_min;
		cfg.udp_port_max = settings_.udp_port_max;
		cfg.max_bitrate_kbps = settings_.max_bitrate_kbps;
		cfg.opus_10ms = settings_.opus_10ms;
		cfg.hw_decode = settings_.hw_decode;
		cfg.preferred_codec = settings_.preferred_codec;

		session = std::make_shared<CameraSession>(random_id(8), name, client_id, peer.ip, user_agent, ws, ch,
							  cfg);
		sessions_[session->id()] = session;
		departed_.erase(std::remove_if(departed_.begin(), departed_.end(),
					       [&](const Departed &d) { return d.name == name; }),
				departed_.end());
	}

	if (replaced)
		replaced->close("replaced", true);
	obs_log(LOG_INFO, "Camera \"%s\" joined from %s", name.c_str(), peer.ip.c_str());

	std::function<void(const std::string &)> hook;
	{
		std::lock_guard<std::mutex> lock(hooks_mutex_);
		hook = join_hook_;
	}
	if (hook && !replaced)
		hook(name);
	return session;
}

void ClientManager::leave(const std::shared_ptr<CameraSession> &session, const std::string &reason)
{
	if (!session)
		return;
	json last = session->status();
	session->close(reason, false);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = sessions_.find(session->id());
		if (it == sessions_.end() || it->second != session)
			return; // replaced or already removed
		sessions_.erase(it);
		Departed d;
		d.name = session->name();
		d.reason = reason;
		d.when_ns = now_ns();
		d.last_status = std::move(last);
		departed_.push_back(std::move(d));
		if (departed_.size() > 32)
			departed_.erase(departed_.begin());
	}
	std::function<void(const std::string &, const std::string &)> hook;
	{
		std::lock_guard<std::mutex> lock(hooks_mutex_);
		hook = leave_hook_;
	}
	if (hook)
		hook(session->name(), reason);
}

std::vector<ClientManager::ClientInfo> ClientManager::clients() const
{
	std::vector<ClientInfo> out;
	std::lock_guard<std::mutex> lock(mutex_);
	auto listed = [&out](const std::string &name) {
		return std::any_of(out.begin(), out.end(), [&name](const ClientInfo &c) { return c.name == name; });
	};
	for (const auto &entry : sessions_) {
		if (!listed(entry.second->name()))
			out.push_back({entry.second->name(), true});
	}
	for (const auto &entry : channels_) {
		if (!entry.second.expired() && !listed(entry.first))
			out.push_back({entry.first, false});
	}
	std::sort(out.begin(), out.end(), [](const ClientInfo &a, const ClientInfo &b) { return a.name < b.name; });
	return out;
}

size_t ClientManager::live_count() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return sessions_.size();
}

json ClientManager::status_json() const
{
	std::vector<std::shared_ptr<CameraSession>> sessions;
	json departed = json::array();
	Settings s;
	const int64_t now = now_ns();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto &[id, session] : sessions_)
			sessions.push_back(session);
		for (const auto &d : departed_) {
			json j = d.last_status;
			j["name"] = d.name;
			j["state"] = "disconnected";
			j["reason"] = d.reason;
			j["ago"] = static_cast<double>(now - d.when_ns) / 1e9;
			departed.push_back(std::move(j));
		}
		s = settings_;
	}
	json cameras = json::array();
	for (auto &session : sessions)
		cameras.push_back(session->status());
	std::sort(cameras.begin(), cameras.end(),
		  [](const json &a, const json &b) { return a.value("name", "") < b.value("name", ""); });
	return json{{"type", "status"},
		    {"version", PLUGIN_VERSION},
		    {"maxClients", s.max_clients},
		    {"orientation", s.orientation},
		    {"cameras", cameras},
		    {"departed", departed}};
}

void ClientManager::add_dock(const std::shared_ptr<WsConnection> &ws)
{
	std::lock_guard<std::mutex> lock(mutex_);
	docks_.push_back(ws);
}

void ClientManager::remove_dock(const WsConnection *ws)
{
	std::lock_guard<std::mutex> lock(mutex_);
	docks_.erase(std::remove_if(docks_.begin(), docks_.end(),
				    [&](const std::weak_ptr<WsConnection> &w) {
					    auto p = w.lock();
					    return !p || p.get() == ws;
				    }),
		     docks_.end());
}

void ClientManager::handle_dock_message(const json &message)
{
	const std::string type = json_get<std::string>(message, "type", "");
	const std::string id = json_get<std::string>(message, "id", "");
	std::shared_ptr<CameraSession> session;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		auto it = sessions_.find(id);
		if (it != sessions_.end())
			session = it->second;
	}
	if (type == "forget") {
		const std::string name = json_get<std::string>(message, "name", "");
		std::lock_guard<std::mutex> lock(mutex_);
		departed_.erase(std::remove_if(departed_.begin(), departed_.end(),
					       [&](const Departed &d) { return d.name == name; }),
				departed_.end());
		return;
	}
	if (session)
		run_command(session, type, message);
}

void ClientManager::run_command(const std::shared_ptr<CameraSession> &session, const std::string &type,
				const json &payload)
{
	if (type == "keyframe") {
		session->request_keyframe();
	} else if (type == "kick") {
		leave(session, "kicked");
	} else if (type == "identify") {
		session->send({{"type", "identify"}});
	} else if (type == "control") {
		auto it = payload.find("set");
		if (it == payload.end() || !it->is_object())
			return;
		json set = validate_control(*it);
		if (!set.empty())
			session->send_control(set);
	}
}

bool ClientManager::command(const std::string &camera_name, const std::string &type, const json &payload,
			    std::string &error)
{
	if (type != "keyframe" && type != "kick" && type != "identify" && type != "control") {
		error = "unknown command";
		return false;
	}
	std::shared_ptr<CameraSession> session;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (auto &[id, s] : sessions_) {
			if (s->name() == camera_name && !s->closed()) {
				session = s;
				break;
			}
		}
	}
	if (!session) {
		error = "camera not connected";
		return false;
	}
	if (type == "control") {
		auto it = payload.find("set");
		if (it == payload.end() || !it->is_object() || validate_control(*it).empty()) {
			error = "nothing to change";
			return false;
		}
	}
	run_command(session, type, payload);
	return true;
}

json ClientManager::validate_control(const json &set)
{
	json out = json::object();
	auto str = [&](const char *key, size_t max) -> const json * {
		auto it = set.find(key);
		return it != set.end() && it->is_string() && it->get<std::string>().size() <= max ? &*it : nullptr;
	};
	if (const json *v = str("deviceId", 256))
		out["deviceId"] = *v;
	if (const json *v = str("facing", 16)) {
		const std::string f = v->get<std::string>();
		if (f == "user" || f == "environment" || f == "toggle")
			out["facing"] = f;
	}
	if (const json *v = str("video", 16)) {
		const std::string m = v->get<std::string>();
		if (m == "camera" || m == "none")
			out["video"] = m;
	}
	auto it = set.find("zoom");
	if (it != set.end() && it->is_number())
		out["zoom"] = std::clamp(it->get<double>(), 0.0, 100.0);
	it = set.find("torch");
	if (it != set.end() && it->is_boolean())
		out["torch"] = it->get<bool>();
	it = set.find("muted");
	if (it != set.end() && it->is_boolean())
		out["muted"] = it->get<bool>();
	it = set.find("height");
	if (it != set.end() && it->is_number_integer()) {
		const int h = it->get<int>();
		if (h == 360 || h == 480 || h == 720 || h == 1080 || h == 1440 || h == 2160)
			out["height"] = h;
	}
	it = set.find("fps");
	if (it != set.end() && it->is_number_integer()) {
		const int f = it->get<int>();
		if (f == 15 || f == 24 || f == 25 || f == 30 || f == 50 || f == 60)
			out["fps"] = f;
	}
	return out;
}

void ClientManager::request_tally_update()
{
	{
		std::lock_guard<std::mutex> lock(thread_mutex_);
		tally_dirty_ = true;
	}
	cv_.notify_all();
}

void ClientManager::update_tallies()
{
	std::vector<std::shared_ptr<CameraSession>> sessions;
	bool enabled;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		enabled = settings_.tally;
		for (auto &[id, s] : sessions_)
			sessions.push_back(s);
	}
	for (auto &s : sessions)
		if (!s->closed())
			s->set_tally(enabled ? s->channel()->tally() : Tally::Off);
}

void ClientManager::set_join_hook(std::function<void(const std::string &)> fn)
{
	std::lock_guard<std::mutex> lock(hooks_mutex_);
	join_hook_ = std::move(fn);
}

void ClientManager::set_leave_hook(std::function<void(const std::string &, const std::string &)> fn)
{
	std::lock_guard<std::mutex> lock(hooks_mutex_);
	leave_hook_ = std::move(fn);
}

void ClientManager::maintenance_loop()
{
	auto next_tick = std::chrono::steady_clock::now() + std::chrono::seconds(1);
	for (;;) {
		bool full_tick;
		{
			std::unique_lock<std::mutex> lock(thread_mutex_);
			cv_.wait_until(lock, next_tick, [this] { return !running_ || tally_dirty_; });
			if (!running_)
				break;
			tally_dirty_ = false;
			full_tick = std::chrono::steady_clock::now() >= next_tick;
			if (full_tick)
				next_tick = std::chrono::steady_clock::now() + std::chrono::seconds(1);
		}

		// Tally changes are pushed right away; the rest runs once per second.
		update_tallies();
		if (!full_tick)
			continue;

		const int64_t now = now_ns();
		std::vector<std::shared_ptr<CameraSession>> sessions;
		std::vector<std::shared_ptr<Channel>> channels;
		std::vector<std::shared_ptr<WsConnection>> docks;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			for (auto &[id, s] : sessions_)
				sessions.push_back(s);
			for (auto it = channels_.begin(); it != channels_.end();) {
				if (auto c = it->second.lock()) {
					channels.push_back(c);
					++it;
				} else {
					it = channels_.erase(it);
				}
			}
			departed_.erase(
				std::remove_if(departed_.begin(), departed_.end(),
					       [&](const Departed &d) { return now - d.when_ns > kDepartedKeepNs; }),
				departed_.end());
			for (auto it = docks_.begin(); it != docks_.end();) {
				if (auto d = it->lock()) {
					docks.push_back(d);
					++it;
				} else {
					it = docks_.erase(it);
				}
			}
		}

		for (auto &s : sessions)
			s->tick(now);

		for (auto &c : channels) {
			const bool has_session = std::any_of(sessions.begin(), sessions.end(), [&](const auto &s) {
				return s->name() == c->name() && !s->closed();
			});
			if (!c->live() && !c->cleared() && !has_session && now - c->offline_since_ns() > kClearDelayNs)
				c->clear();
		}

		if (!docks.empty()) {
			const std::string status = status_json().dump();
			for (auto &d : docks)
				d->send_text(status);
		}
	}
}

} // namespace camdrop
