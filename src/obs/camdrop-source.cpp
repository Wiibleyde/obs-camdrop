#include "obs/camdrop-source.hpp"

#include "core/app.hpp"
#include "core/channel.hpp"
#include "core/client-manager.hpp"
#include "plugin-support.h"

#include <obs-module.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace camdrop {

namespace {

std::function<void()> g_open_settings;

class CamdropSource final : public ChannelSink {
public:
	CamdropSource(obs_data_t *settings, obs_source_t *source) : source_(source)
	{
		// Frames are already paced by the plugin's jitter buffer: show them as they come.
		obs_source_set_async_unbuffered(source_, true);
		update(settings);
	}

	~CamdropSource() override { detach(); }

	void update(obs_data_t *settings)
	{
		buffer_ms_ = static_cast<int>(obs_data_get_int(settings, "buffer_ms"));
		audio_ = obs_data_get_bool(settings, "audio");
		const std::string name = obs_data_get_string(settings, "client");

		std::lock_guard<std::mutex> lock(mutex_);
		if (name == name_ && channel_)
			return;
		if (channel_) {
			channel_->remove_sink(this);
			channel_.reset();
		}
		obs_source_output_video(source_, nullptr);
		name_ = name;
		if (!name_.empty()) {
			channel_ = ClientManager::get().channel(name_);
			channel_->add_sink(this);
		}
	}

	void detach()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (channel_) {
			channel_->remove_sink(this);
			channel_.reset();
		}
	}

	// ChannelSink (called from the camera's pipeline thread)
	void channel_video(const obs_source_frame *frame) override { obs_source_output_video(source_, frame); }

	void channel_audio(const obs_source_audio *audio) override
	{
		if (audio_)
			obs_source_output_audio(source_, audio);
	}

	void channel_clear() override { obs_source_output_video(source_, nullptr); }

	int sink_buffer_ms() const override { return buffer_ms_; }

	std::string name() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return name_;
	}

private:
	obs_source_t *source_;
	mutable std::mutex mutex_;
	std::string name_;
	std::shared_ptr<Channel> channel_;
	std::atomic<int> buffer_ms_{Channel::kDefaultBufferMs};
	std::atomic<bool> audio_{true};
};

const char *source_get_name(void *)
{
	return obs_module_text("Source.Name");
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	return new CamdropSource(settings, source);
}

void source_destroy(void *data)
{
	delete static_cast<CamdropSource *>(data);
}

void source_update(void *data, obs_data_t *settings)
{
	static_cast<CamdropSource *>(data)->update(settings);
}

void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "buffer_ms", Channel::kDefaultBufferMs);
	obs_data_set_default_bool(settings, "audio", true);
	obs_data_set_default_string(settings, "client", "");
}

bool refresh_clicked(obs_properties_t *, obs_property_t *, void *)
{
	return true; // reload the properties (and the camera list)
}

bool settings_clicked(obs_properties_t *, obs_property_t *, void *)
{
	if (g_open_settings)
		g_open_settings();
	return false;
}

obs_properties_t *source_properties(void *data)
{
	auto *ctx = static_cast<CamdropSource *>(data);
	const std::string current = ctx ? ctx->name() : std::string();

	obs_properties_t *props = obs_properties_create();

	const App::Status status = App::get().status();
	std::string info;
	if (status.running && !status.camera_urls.empty()) {
		std::string url = status.camera_urls.front();
		const size_t q = url.find("/?code=");
		if (q != std::string::npos)
			url = url.substr(0, q);
		info = std::string(obs_module_text("Source.ServerOn")) + " " + url + "  —  " +
		       obs_module_text("Source.Code") + " " + App::get().settings().access_code;
	} else if (status.running) {
		info = obs_module_text("Source.ServerOnNoNetwork");
	} else {
		info = obs_module_text("Source.ServerOff");
	}
	obs_properties_add_text(props, "server_info", info.c_str(), OBS_TEXT_INFO);

	obs_property_t *list = obs_properties_add_list(props, "client", obs_module_text("Source.Client"),
						       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	bool has_current = current.empty();
	for (const auto &c : ClientManager::get().clients()) {
		std::string label = c.name;
		if (!c.live)
			label += std::string(" (") + obs_module_text("Source.Offline") + ")";
		obs_property_list_add_string(list, label.c_str(), c.name.c_str());
		if (c.name == current)
			has_current = true;
	}
	if (!has_current) {
		std::string label = current + " (" + obs_module_text("Source.Offline") + ")";
		obs_property_list_add_string(list, label.c_str(), current.c_str());
	}
	obs_property_set_long_description(list, obs_module_text("Source.Client.Hint"));

	obs_properties_add_button2(props, "refresh", obs_module_text("Source.Refresh"), refresh_clicked, nullptr);

	obs_property_t *buffer =
		obs_properties_add_int_slider(props, "buffer_ms", obs_module_text("Source.Buffer"), 0, 1000, 10);
	obs_property_int_set_suffix(buffer, " ms");
	obs_property_set_long_description(buffer, obs_module_text("Source.Buffer.Hint"));

	obs_properties_add_bool(props, "audio", obs_module_text("Source.Audio"));

	if (g_open_settings)
		obs_properties_add_button2(props, "open_settings", obs_module_text("Source.OpenSettings"),
					  settings_clicked, nullptr);
	return props;
}

} // namespace

void set_open_settings_callback(std::function<void()> fn)
{
	g_open_settings = std::move(fn);
}

void register_camdrop_source()
{
	obs_source_info info = {};
	info.id = "camdrop_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_CAMERA;
	info.get_name = source_get_name;
	info.create = source_create;
	info.destroy = source_destroy;
	info.update = source_update;
	info.get_defaults = source_defaults;
	info.get_properties = source_properties;
	obs_register_source(&info);
}

} // namespace camdrop
