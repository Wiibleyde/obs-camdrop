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
#include "obs/camdrop-source.hpp"
#include "plugin-support.h"

#ifdef CAMDROP_HAVE_FRONTEND
#include <obs-frontend-api.h>
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
static void on_frontend_event(enum obs_frontend_event event, void *)
{
#ifdef CAMDROP_HAVE_QT
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING)
		camdrop::ui::create_dock();
	else if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN)
		camdrop::ui::shutdown();
#else
	(void)event;
#endif
}
#endif

bool obs_module_load(void)
{
	camdrop::register_camdrop_source();
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

void obs_module_unload(void)
{
#ifdef CAMDROP_HAVE_FRONTEND
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
#endif
	camdrop::set_open_settings_callback(nullptr);
	camdrop::App::get().unload();
	obs_log(LOG_INFO, "plugin unloaded");
}
