#include "obs/websocket-vendor.hpp"

#include "core/app.hpp"
#include "core/client-manager.hpp"
#include "plugin-support.h"

#include <obs-module.h>

#include <assert.h>

#include "third_party/obs-websocket-api.h"

#include <atomic>

namespace camdrop {

namespace {

std::atomic<obs_websocket_vendor> g_vendor{nullptr};

json request_json(obs_data_t *request)
{
	const char *text = request ? obs_data_get_json(request) : nullptr;
	json j = json::parse(text ? text : "{}", nullptr, false);
	return j.is_object() ? j : json::object();
}

void respond(obs_data_t *response, const json &j)
{
	obs_data_t *tmp = obs_data_create_from_json(j.dump().c_str());
	if (tmp) {
		obs_data_apply(response, tmp);
		obs_data_release(tmp);
	}
}

void get_cameras(obs_data_t *, obs_data_t *response, void *)
{
	static const char *copied[] = {"id",      "name",    "state",    "tally",  "video",    "ip",
				       "width",   "height",  "fps",      "codec",  "hardware", "rttMs",
				       "lossPct", "delayMs", "bufferMs", "uptime", "watchers"};
	static const char *telemetry[] = {"battery", "charging", "visible", "muted", "facing", "orientation"};

	const json status = ClientManager::get().status_json();
	json cameras = json::array();
	for (const auto &c : status.value("cameras", json::array())) {
		json cam = json::object();
		for (const char *k : copied)
			if (c.contains(k))
				cam[k] = c[k];
		const json tel = c.value("telemetry", json::object());
		for (const char *k : telemetry)
			if (tel.contains(k))
				cam[k] = tel[k];
		cam["controls"] = c.value("caps", json::object());
		cameras.push_back(std::move(cam));
	}
	const App::Status st = App::get().status();
	json urls = json::array();
	for (std::string url : st.camera_urls) {
		const size_t q = url.find("/?code=");
		urls.push_back(q == std::string::npos ? url : url.substr(0, q));
	}
	respond(response, {{"ok", true}, {"serverRunning", st.running}, {"urls", urls}, {"cameras", cameras}});
}

void run(const char *type, obs_data_t *request, obs_data_t *response)
{
	const json req = request_json(request);
	const std::string name = json_get<std::string>(req, "name", "");
	std::string error;
	json payload = req;
	if (std::string(type) == "control") {
		// SetCameraControl accepts the fields at the top level or inside "set".
		json set = req.contains("set") && req["set"].is_object() ? req["set"] : req;
		set.erase("name");
		payload = {{"set", set}};
	}
	const bool ok = !name.empty() && ClientManager::get().command(name, type, payload, error);
	if (name.empty())
		error = "missing \"name\"";
	respond(response, ok ? json{{"ok", true}} : json{{"ok", false}, {"error", error}});
}

void request_keyframe(obs_data_t *request, obs_data_t *response, void *)
{
	run("keyframe", request, response);
}

void identify(obs_data_t *request, obs_data_t *response, void *)
{
	run("identify", request, response);
}

void disconnect(obs_data_t *request, obs_data_t *response, void *)
{
	run("kick", request, response);
}

void set_control(obs_data_t *request, obs_data_t *response, void *)
{
	run("control", request, response);
}

} // namespace

void websocket_vendor_register()
{
	if (g_vendor)
		return;
	obs_websocket_vendor vendor = obs_websocket_register_vendor("camdrop");
	if (!vendor) {
		obs_log(LOG_INFO, "obs-websocket not available: vendor requests disabled");
		return;
	}
	obs_websocket_vendor_register_request(vendor, "GetCameras", get_cameras, nullptr);
	obs_websocket_vendor_register_request(vendor, "RequestKeyframe", request_keyframe, nullptr);
	obs_websocket_vendor_register_request(vendor, "Identify", identify, nullptr);
	obs_websocket_vendor_register_request(vendor, "Disconnect", disconnect, nullptr);
	obs_websocket_vendor_register_request(vendor, "SetCameraControl", set_control, nullptr);
	g_vendor = vendor;
	obs_log(LOG_INFO, "obs-websocket vendor \"camdrop\" registered");
}

void websocket_vendor_emit(const char *event_name, const json &data)
{
	obs_websocket_vendor vendor = g_vendor;
	if (!vendor)
		return;
	obs_data_t *event = obs_data_create_from_json(data.dump().c_str());
	if (!event)
		return;
	obs_websocket_vendor_emit_event(vendor, event_name, event);
	obs_data_release(event);
}

} // namespace camdrop
