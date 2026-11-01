#include "core/app.hpp"

#include "core/client-manager.hpp"
#include "net/certificate.hpp"
#include "platform/platform.hpp"
#include "plugin-support.h"
#include "util/json.hpp"
#include "util/strings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <sys/stat.h>

#include <algorithm>

namespace camdrop {

namespace {

int64_t file_mtime(const std::string &path)
{
	struct stat st{};
	if (path.empty() || os_stat(path.c_str(), &st) != 0)
		return 0;
	return static_cast<int64_t>(st.st_mtime) ^ (static_cast<int64_t>(st.st_size) << 40);
}

const char *kCsp = "default-src 'self'; img-src 'self' data: blob:; media-src 'self' blob: mediastream:; "
		   "style-src 'self'; script-src 'self'; connect-src 'self' wss: ws:; frame-ancestors 'none'";

void add_common_headers(HttpResponse &res)
{
	res.set("Cache-Control", "no-cache");
	res.set("X-Content-Type-Options", "nosniff");
	res.set("Referrer-Policy", "no-referrer");
	res.set("Content-Security-Policy", kCsp);
	res.set("Permissions-Policy", "camera=(self), microphone=(self), screen-wake-lock=(self)");
}

bool safe_relative_path(const std::string &p)
{
	if (p.empty() || p.size() > 128 || p.find("..") != std::string::npos)
		return false;
	for (char c : p) {
		if (!(isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '/'))
			return false;
	}
	return p[0] != '/';
}

} // namespace

App &App::get()
{
	static App instance;
	return instance;
}

void App::load()
{
	char *root = obs_module_file("web");
	if (root) {
		web_root_ = root;
		bfree(root);
	}

	std::lock_guard<std::recursive_mutex> lock(mutex_);
	settings_ = load_settings();
	ClientManager::get().update_settings(settings_);
	ClientManager::get().start();

	if (settings_.server_enabled) {
		std::string error;
		if (!start_server_locked(error))
			obs_log(LOG_WARNING, "Server not started: %s", error.c_str());
	}

	{
		std::lock_guard<std::mutex> wl(watch_mutex_);
		watching_ = true;
	}
	watch_thread_ = std::thread([this] { watch_loop(); });
}

void App::unload()
{
	{
		std::lock_guard<std::mutex> wl(watch_mutex_);
		watching_ = false;
	}
	watch_cv_.notify_all();
	if (watch_thread_.joinable())
		watch_thread_.join();

	ClientManager::get().stop();
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	stop_server_locked();
	std::lock_guard<std::mutex> ll(listeners_mutex_);
	listeners_.clear();
}

Settings App::settings() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	return settings_;
}

bool App::load_credentials(TlsCredentials &creds, std::string &error, bool regenerate)
{
	if (settings_.custom_cert) {
		if (!read_text_file(settings_.cert_path, creds.cert_pem)) {
			error = obs_module_text("Error.CertRead");
			return false;
		}
		if (!read_text_file(settings_.key_path, creds.key_pem)) {
			error = obs_module_text("Error.KeyRead");
			return false;
		}
		if (!validate_certificate(creds.cert_pem, creds.key_pem, error))
			return false;
		cert_mtime_ = file_mtime(settings_.cert_path);
		key_mtime_ = file_mtime(settings_.key_path);
		return true;
	}

	std::vector<std::string> ips;
	for (const auto &a : platform::local_ipv4_addresses())
		ips.push_back(a.ip);
	return load_or_create_self_signed(ips, regenerate, creds.cert_pem, creds.key_pem, error);
}

bool App::start_server_locked(std::string &error)
{
	stop_server_locked();

	TlsCredentials creds;
	if (!load_credentials(creds, error)) {
		last_error_ = error;
		notify();
		return false;
	}
	set_cert_pem(creds.cert_pem);

	server_ = std::make_unique<Server>([this](const HttpRequest &req,
						  const PeerInfo &peer) { return handle_http(req, peer); },
					   [this](const HttpRequest &req, const PeerInfo &peer, HttpResponse &reject) {
						   return handle_ws(req, peer, reject);
					   });
	if (!server_->start(settings_.port, creds, error)) {
		server_.reset();
		last_error_ = error;
		notify();
		return false;
	}
	last_error_.clear();
	notify();
	return true;
}

void App::stop_server_locked()
{
	if (!server_)
		return;
	ClientManager::get().disconnect_all("server_stopped");
	server_->stop();
	server_.reset();
	notify();
}

bool App::apply(const Settings &next, std::string &error)
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	const Settings prev = settings_;
	settings_ = next;
	save_settings(settings_);
	ClientManager::get().update_settings(settings_);

	const bool restart = !server_ || prev.port != next.port || prev.custom_cert != next.custom_cert ||
			     prev.cert_path != next.cert_path || prev.key_path != next.key_path;

	if (!next.server_enabled) {
		stop_server_locked();
		last_error_.clear();
		notify();
		return true;
	}

	if (prev.access_code != next.access_code)
		ClientManager::get().disconnect_all("code_changed");

	if (restart)
		return start_server_locked(error);
	notify();
	return true;
}

bool App::regenerate_certificate(std::string &error)
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	TlsCredentials creds;
	std::vector<std::string> ips;
	for (const auto &a : platform::local_ipv4_addresses())
		ips.push_back(a.ip);
	if (!load_or_create_self_signed(ips, true, creds.cert_pem, creds.key_pem, error))
		return false;
	if (server_ && !settings_.custom_cert) {
		if (!server_->set_credentials(creds, error))
			return false;
		set_cert_pem(creds.cert_pem);
	}
	notify();
	return true;
}

App::Status App::status() const
{
	std::lock_guard<std::recursive_mutex> lock(mutex_);
	Status st;
	st.running = server_ && server_->running();
	st.error = last_error_;
	st.port = settings_.port;
	st.self_signed = !settings_.custom_cert;
	st.clients = ClientManager::get().live_count();
	const std::string port = std::to_string(settings_.port);
	for (const auto &a : platform::local_ipv4_addresses())
		st.camera_urls.push_back("https://" + a.ip + ":" + port + "/?code=" + settings_.access_code);
	st.dock_url = "http://127.0.0.1:" + port + "/dock?code=" + settings_.access_code;

	if (!cert_pem_.empty()) {
		CertificateInfo info;
		if (describe_certificate(cert_pem_, info))
			st.cert_summary = info.subject + " — " + info.valid_to;
	}
	return st;
}

void App::set_cert_pem(const std::string &pem)
{
	cert_pem_ = pem;
	std::lock_guard<std::mutex> lock(served_cert_mutex_);
	served_cert_pem_ = settings_.custom_cert ? std::string() : pem;
}

int App::add_listener(std::function<void()> fn)
{
	std::lock_guard<std::mutex> lock(listeners_mutex_);
	const int id = next_listener_++;
	listeners_.emplace_back(id, std::move(fn));
	return id;
}

void App::remove_listener(int id)
{
	std::lock_guard<std::mutex> lock(listeners_mutex_);
	listeners_.erase(std::remove_if(listeners_.begin(), listeners_.end(),
					[id](const auto &l) { return l.first == id; }),
			 listeners_.end());
}

void App::notify()
{
	std::vector<std::function<void()>> fns;
	{
		std::lock_guard<std::mutex> lock(listeners_mutex_);
		for (auto &l : listeners_)
			fns.push_back(l.second);
	}
	for (auto &fn : fns)
		fn();
}

void App::watch_loop()
{
	// Reloads an imported certificate when it is renewed (Let's Encrypt: every 60-90 days).
	for (;;) {
		{
			std::unique_lock<std::mutex> lock(watch_mutex_);
			watch_cv_.wait_for(lock, std::chrono::seconds(30), [this] { return !watching_; });
			if (!watching_)
				return;
		}
		std::lock_guard<std::recursive_mutex> lock(mutex_);
		if (!server_ || !settings_.custom_cert)
			continue;
		const int64_t c = file_mtime(settings_.cert_path);
		const int64_t k = file_mtime(settings_.key_path);
		if (c == cert_mtime_ && k == key_mtime_)
			continue;
		TlsCredentials creds;
		std::string error;
		if (load_credentials(creds, error) && server_->set_credentials(creds, error)) {
			set_cert_pem(creds.cert_pem);
			obs_log(LOG_INFO, "Certificate reloaded from %s", settings_.cert_path.c_str());
			notify();
		} else {
			cert_mtime_ = c;
			key_mtime_ = k;
			obs_log(LOG_WARNING, "Certificate reload failed: %s", error.c_str());
		}
	}
}

// ---------------------------------------------------------------------------------------------
// HTTP routes

HttpResponse App::serve_file(const std::string &relative)
{
	if (!safe_relative_path(relative))
		return HttpResponse::text(404, "Not Found");
	std::string body;
	if (web_root_.empty() || !read_text_file(web_root_ + "/" + relative, body))
		return HttpResponse::text(404, "Not Found");
	HttpResponse res;
	res.body = std::move(body);
	res.set("Content-Type", mime_type_for(relative));
	return res;
}

HttpResponse App::handle_http(const HttpRequest &req, const PeerInfo &peer)
{
	(void)peer;
	HttpResponse res;
	if (req.method != "GET" && req.method != "HEAD") {
		res = HttpResponse::text(405, "Method Not Allowed");
		res.set("Allow", "GET, HEAD");
	} else if (req.path == "/" || req.path == "/index.html") {
		res = serve_file("index.html");
	} else if (req.path == "/dock" || req.path == "/dock.html") {
		res = serve_file("dock.html");
	} else if (req.path == "/camdrop.crt") {
		std::string pem;
		{
			std::lock_guard<std::mutex> lock(served_cert_mutex_);
			pem = served_cert_pem_;
		}
		auto der = certificate_der(pem);
		if (der.empty()) {
			res = HttpResponse::text(404, "Not Found");
		} else {
			res.body.assign(der.begin(), der.end());
			res.set("Content-Type", "application/x-x509-ca-cert");
			res.set("Content-Disposition", "attachment; filename=\"camdrop.crt\"");
		}
	} else if (req.path == "/api/info") {
		res = HttpResponse::json(200, json{{"name", "Camdrop"}, {"version", PLUGIN_VERSION}}.dump());
	} else if (req.path == "/favicon.ico") {
		res = serve_file("icon.svg");
	} else if (starts_with(req.path, "/")) {
		res = serve_file(req.path.substr(1));
	} else {
		res = HttpResponse::text(404, "Not Found");
	}
	add_common_headers(res);
	return res;
}

std::shared_ptr<WsHandler> App::handle_ws(const HttpRequest &req, const PeerInfo &peer, HttpResponse &reject)
{
	if (req.path == "/ws")
		return ClientManager::get().create_camera_handler(peer, req.header("user-agent"));

	if (req.path == "/ws/dock") {
		const auto params = req.query_params();
		auto it = params.find("code");
		int retry_after = 0;
		if (it == params.end() || !ClientManager::get().check_code(it->second, peer.ip, retry_after)) {
			reject = HttpResponse::text(retry_after > 0 ? 429 : 401, "Invalid access code");
			if (retry_after > 0)
				reject.set("Retry-After", std::to_string(retry_after));
			return nullptr;
		}
		return ClientManager::get().create_dock_handler(peer);
	}
	reject = HttpResponse::text(404, "Not Found");
	return nullptr;
}

} // namespace camdrop
