#pragma once

#include "media/video-decoder.hpp" // AVFramePtr

#include <cstddef>
#include <cstdint>
#include <vector>

namespace camdrop {

// Opus decoder (libavcodec), 48 kHz stereo float planar output.
class AudioDecoder {
public:
	AudioDecoder() = default;
	~AudioDecoder();
	AudioDecoder(const AudioDecoder &) = delete;
	AudioDecoder &operator=(const AudioDecoder &) = delete;

	bool open();
	void close();
	bool is_open() const { return ctx_ != nullptr; }
	bool decode(const uint8_t *data, size_t size, std::vector<AVFramePtr> &out);

private:
	AVCodecContext *ctx_ = nullptr;
	AVPacket *packet_ = nullptr;
};

} // namespace camdrop
