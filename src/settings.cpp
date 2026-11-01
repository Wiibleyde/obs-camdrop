#include "settings.hpp"

#include "plugin-support.h"
#include "util/random.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>

namespace camdrop {

std::string config_file_path(const char *name)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path(name);
	std::string out = path ? path : name;
	bfree(path);
	return out;
}

std::string generate_access_code()
{
	return random_digits(6);
}

static int clamp_int(long long v, int lo, int hi)
{
	return static_cast<int>(std::clamp<long long>(v, lo, hi));
}

Settings load_settings()
{
	Settings s;
	const std::string path = config_file_path("settings.json");
	obs_data_t *data = obs_data_create_from_json_file_safe(path.c_str(), "bak");
	bool needs_save = false;

	if (data) {
		s.server_enabled = obs_data_get_bool(data, "server_enabled");
		if (obs_data_has_user_value(data, "port"))
			s.port = clamp_int(obs_data_get_int(data, "port"), 1, 65535);
		if (obs_data_has_user_value(data, "udp_port_min"))
			s.udp_port_min = clamp_int(obs_data_get_int(data, "udp_port_min"), 1024, 65535);
		if (obs_data_has_user_value(data, "udp_port_max"))
			s.udp_port_max = clamp_int(obs_data_get_int(data, "udp_port_max"), 1024, 65535);
		s.custom_cert = obs_data_get_bool(data, "custom_cert");
		s.cert_path = obs_data_get_string(data, "cert_path");
		s.key_path = obs_data_get_string(data, "key_path");
		s.access_code = obs_data_get_string(data, "access_code");
		if (obs_data_has_user_value(data, "max_clients"))
			s.max_clients = clamp_int(obs_data_get_int(data, "max_clients"), 1, 32);
		if (obs_data_has_user_value(data, "video_height"))
			s.video_height = clamp_int(obs_data_get_int(data, "video_height"), 240, 2160);
		if (obs_data_has_user_value(data, "video_fps"))
			s.video_fps = clamp_int(obs_data_get_int(data, "video_fps"), 10, 60);
		if (obs_data_has_user_value(data, "max_bitrate_kbps"))
			s.max_bitrate_kbps = clamp_int(obs_data_get_int(data, "max_bitrate_kbps"), 300, 50000);
		std::string codec = obs_data_get_string(data, "preferred_codec");
		if (codec == "h264" || codec == "h265" || codec == "vp8")
			s.preferred_codec = codec;
		if (obs_data_has_user_value(data, "hw_decode"))
			s.hw_decode = obs_data_get_bool(data, "hw_decode");
		s.opus_10ms = obs_data_get_bool(data, "opus_10ms");
		std::string orientation = obs_data_get_string(data, "orientation");
		if (orientation == "any" || orientation == "landscape" || orientation == "portrait")
			s.orientation = orientation;
		if (obs_data_has_user_value(data, "tally"))
			s.tally = obs_data_get_bool(data, "tally");
		if (obs_data_has_user_value(data, "auto_add_sources"))
			s.auto_add_sources = obs_data_get_bool(data, "auto_add_sources");
		obs_data_release(data);
	} else {
		needs_save = true;
	}

	if (s.udp_port_max < s.udp_port_min)
		std::swap(s.udp_port_min, s.udp_port_max);

	// Random access code generated on first launch.
	if (s.access_code.empty()) {
		s.access_code = generate_access_code();
		needs_save = true;
	}

	if (needs_save)
		save_settings(s);
	return s;
}

bool save_settings(const Settings &s)
{
	obs_data_t *data = obs_data_create();
	obs_data_set_bool(data, "server_enabled", s.server_enabled);
	obs_data_set_int(data, "port", s.port);
	obs_data_set_int(data, "udp_port_min", s.udp_port_min);
	obs_data_set_int(data, "udp_port_max", s.udp_port_max);
	obs_data_set_bool(data, "custom_cert", s.custom_cert);
	obs_data_set_string(data, "cert_path", s.cert_path.c_str());
	obs_data_set_string(data, "key_path", s.key_path.c_str());
	obs_data_set_string(data, "access_code", s.access_code.c_str());
	obs_data_set_int(data, "max_clients", s.max_clients);
	obs_data_set_int(data, "video_height", s.video_height);
	obs_data_set_int(data, "video_fps", s.video_fps);
	obs_data_set_int(data, "max_bitrate_kbps", s.max_bitrate_kbps);
	obs_data_set_string(data, "preferred_codec", s.preferred_codec.c_str());
	obs_data_set_bool(data, "hw_decode", s.hw_decode);
	obs_data_set_bool(data, "opus_10ms", s.opus_10ms);
	obs_data_set_string(data, "orientation", s.orientation.c_str());
	obs_data_set_bool(data, "tally", s.tally);
	obs_data_set_bool(data, "auto_add_sources", s.auto_add_sources);

	const std::string path = config_file_path("settings.json");
	bool ok = obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
	obs_data_release(data);
	if (!ok)
		obs_log(LOG_WARNING, "Could not save settings to %s", path.c_str());
	return ok;
}

} // namespace camdrop
