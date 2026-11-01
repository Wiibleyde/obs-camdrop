#pragma once

#include <string>

namespace camdrop {

// Adds a "Remote camera" source to the current scene (the preview scene in studio mode) when a camera
// joins for the first time and no source shows it yet. While streaming or recording outside studio
// mode the new item is added hidden, so a phone never pops up on air unexpectedly.
void auto_source_on_join(const std::string &camera_name);

// The frontend is ready (scene collection loaded) or not (startup, collection switch, exit).
void auto_source_set_ready(bool ready);

} // namespace camdrop
