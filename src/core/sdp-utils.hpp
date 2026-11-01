#pragma once

#include <string>

namespace camdrop {

// Restricts a browser offer to what the plugin decodes: H.264 (packetization-mode 1, constrained
// baseline / baseline first) and VP8 for video, ordered by `preferred_codec`, Opus for audio.
// RED, FEC and transport-wide congestion control are removed (the plugin answers with NACK, PLI
// and REMB), as well as the video orientation extension (browsers then rotate the frames).
std::string filter_offer(const std::string &sdp, const std::string &preferred_codec);

// Adds the bitrate limit (b=AS and Chrome's x-google-*-bitrate hints) and the optional 10 ms Opus
// framing to the answer sent back to the browser.
std::string tune_answer(const std::string &sdp, int max_bitrate_kbps, bool opus_10ms);

} // namespace camdrop
