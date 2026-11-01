/*
Camdrop – phones and browsers as low-latency cameras for OBS (WebRTC)
Copyright (C) 2026 Nathan Bonnell

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>

#include "core/app.hpp"
#include "core/client-manager.hpp"
#include "obs/camdrop-source.hpp"
#include "obs/websocket-vendor.hpp"
#include "plugin-support.h"

#ifdef CAMDROP_HAVE_FRONTEND
#include <obs-frontend-api.h>

#include "obs/auto-source.hpp"
#endif

#ifdef CAMDROP_HAVE_QT
#include "ui/dock.hpp"
#include "ui/settings-dialog.hpp"
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Camdrop";
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Camdrop.Description");
}

#ifdef CAMDROP_HAVE_FRONTEND
static void frontend_ready()
{
	camdrop::auto_source_set_ready(true);
	// Cameras that joined while OBS was starting.
	for (const auto &c : camdrop::ClientManager::get().clients())
		if (c.live)
			camdrop::auto_source_on_join(c.name);
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
#ifdef CAMDROP_HAVE_QT
		camdrop::ui::create_dock();
		camdrop::ui::maybe_snapshot();
#endif
		frontend_ready();
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		camdrop::auto_source_set_ready(false);
		break;
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		frontend_ready();
		break;
	case OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN:
	case OBS_FRONTEND_EVENT_EXIT:
		camdrop::auto_source_set_ready(false);
#ifdef CAMDROP_HAVE_QT
		camdrop::ui::shutdown();
#endif
		break;
	default:
		break;
	}
}
#endif

bool obs_module_load(void)
{
	camdrop::register_camdrop_source();

	camdrop::ClientManager::get().set_join_hook([](const std::string &name) {
#ifdef CAMDROP_HAVE_FRONTEND
		camdrop::auto_source_on_join(name);
#endif
		camdrop::websocket_vendor_emit("CameraConnected", {{"name", name}});
	});
	camdrop::ClientManager::get().set_leave_hook([](const std::string &name, const std::string &reason) {
		camdrop::websocket_vendor_emit("CameraDisconnected", {{"name", name}, {"reason", reason}});
	});

	camdrop::App::get().load();

#ifdef CAMDROP_HAVE_QT
	camdrop::ui::register_menu();
	camdrop::set_open_settings_callback([] { camdrop::ui::open_settings(); });
#endif
#ifdef CAMDROP_HAVE_FRONTEND
	obs_frontend_add_event_callback(on_frontend_event, nullptr);
#endif

	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	camdrop::websocket_vendor_register();
}

void obs_module_unload(void)
{
	camdrop::ClientManager::get().set_join_hook(nullptr);
	camdrop::ClientManager::get().set_leave_hook(nullptr);
#ifdef CAMDROP_HAVE_FRONTEND
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
#endif
	camdrop::set_open_settings_callback(nullptr);
	camdrop::App::get().unload();
	obs_log(LOG_INFO, "plugin unloaded");
}
