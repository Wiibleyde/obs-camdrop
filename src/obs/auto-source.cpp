#include "obs/auto-source.hpp"

#include "core/client-manager.hpp"
#include "plugin-support.h"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>

namespace camdrop {

namespace {

std::mutex g_mutex;
std::set<std::string> g_handled; // cameras already considered during this scene collection
std::atomic<bool> g_ready{false};

struct FindData {
	const std::string *name;
	bool found;
};

bool find_camera_source(void *param, obs_source_t *source)
{
	auto *data = static_cast<FindData *>(param);
	const char *id = obs_source_get_unversioned_id(source);
	if (!id || strcmp(id, "camdrop_source") != 0)
		return true;
	obs_data_t *settings = obs_source_get_settings(source);
	const char *client = obs_data_get_string(settings, "client");
	if (client && *data->name == client)
		data->found = true;
	obs_data_release(settings);
	return !data->found;
}

std::string unique_source_name(const std::string &base)
{
	std::string name = base;
	for (int i = 2; i < 100; i++) {
		obs_source_t *existing = obs_get_source_by_name(name.c_str());
		if (!existing)
			break;
		obs_source_release(existing);
		name = base + " " + std::to_string(i);
	}
	return name;
}

void add_source_task(void *param)
{
	std::unique_ptr<std::string> camera(static_cast<std::string *>(param));
	if (!g_ready || !ClientManager::get().settings().auto_add_sources)
		return;

	FindData data{camera.get(), false};
	obs_enum_sources(find_camera_source, &data);
	if (data.found)
		return;

	const bool studio = obs_frontend_preview_program_mode_active();
	obs_source_t *scene_source = studio ? obs_frontend_get_current_preview_scene()
					    : obs_frontend_get_current_scene();
	obs_scene_t *scene = scene_source ? obs_scene_from_source(scene_source) : nullptr;
	if (!scene) {
		obs_source_release(scene_source);
		return;
	}

	std::string label = obs_module_text("Source.AutoName");
	const size_t pos = label.find("%1");
	if (pos != std::string::npos)
		label.replace(pos, 2, *camera);
	else
		label += " " + *camera;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "client", camera->c_str());
	obs_source_t *source =
		obs_source_create("camdrop_source", unique_source_name(label).c_str(), settings, nullptr);
	obs_data_release(settings);

	if (source) {
		if (obs_sceneitem_t *item = obs_scene_add(scene, source)) {
			// Fit the canvas whatever the phone sends: rotating the phone (portrait / landscape)
			// then keeps the layout instead of resizing the item.
			obs_video_info ovi = {};
			if (obs_get_video_info(&ovi)) {
				vec2 bounds;
				vec2_set(&bounds, static_cast<float>(ovi.base_width),
					 static_cast<float>(ovi.base_height));
				obs_sceneitem_set_bounds_type(item, OBS_BOUNDS_SCALE_INNER);
				obs_sceneitem_set_bounds(item, &bounds);
				obs_sceneitem_set_bounds_alignment(item, OBS_ALIGN_CENTER);
			}
			const bool on_air = obs_frontend_streaming_active() || obs_frontend_recording_active() ||
					    obs_frontend_virtualcam_active();
			if (!studio && on_air)
				obs_sceneitem_set_visible(item, false);
		}
		obs_log(LOG_INFO, "Added the source \"%s\" for the new camera \"%s\"", obs_source_get_name(source),
			camera->c_str());
		obs_source_release(source);
	}
	obs_source_release(scene_source);
}

} // namespace

void auto_source_on_join(const std::string &camera_name)
{
	if (!g_ready || !ClientManager::get().settings().auto_add_sources)
		return;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		// Once per camera and scene collection: a source the user deleted is not added back when
		// the phone reconnects.
		if (!g_handled.insert(camera_name).second)
			return;
	}
	obs_queue_task(OBS_TASK_UI, add_source_task, new std::string(camera_name), false);
}

void auto_source_set_ready(bool ready)
{
	g_ready = ready;
	if (!ready) {
		std::lock_guard<std::mutex> lock(g_mutex);
		g_handled.clear();
	}
}

} // namespace camdrop
