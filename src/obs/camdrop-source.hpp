#pragma once

#include <functional>

namespace camdrop {

void register_camdrop_source();

// Set by the UI layer (Qt) so that the source properties can open the plugin settings.
void set_open_settings_callback(std::function<void()> fn);

} // namespace camdrop
