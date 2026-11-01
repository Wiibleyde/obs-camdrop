#pragma once

#include "util/json.hpp"

#include <string>

namespace camdrop {

// obs-websocket vendor "camdrop": lets Stream Deck, Touch Portal, scripts... drive the cameras with
// CallVendorRequest (GetCameras, RequestKeyframe, Identify, Disconnect, SetCameraControl) and
// receive VendorEvent notifications (CameraConnected, CameraDisconnected).
void websocket_vendor_register(); // from obs_module_post_load, once obs-websocket is loaded
void websocket_vendor_emit(const char *event_name, const json &data);

} // namespace camdrop
