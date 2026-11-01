#pragma once

#include "media/video-receiver.hpp"

#include <memory>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;

namespace camdrop {

struct AVFrameDeleter {
	void operator()(AVFrame *frame) const;
};
using AVFramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;

// FFmpeg video decoder (the libavcodec shipped with OBS). Uses D3D11VA (or the platform's API)
// when available and falls back to software decoding on any hardware error.
class VideoDecoder {
public:
	enum class Status { Ok, NeedKeyframe, Error };

	VideoDecoder() = default;
	~VideoDecoder();
	VideoDecoder(const VideoDecoder &) = delete;
	VideoDecoder &operator=(const VideoDecoder &) = delete;

	bool open(VideoCodec codec, bool try_hardware);
	void close();
	bool is_open() const { return ctx_ != nullptr; }
	VideoCodec codec() const { return codec_; }
	bool hardware() const { return hw_pix_fmt_ >= 0; }

	// Decodes one access unit. Decoded frames (system memory) are appended to `out`, their pts
	// is the RTP timestamp of the input.
	Status decode(const EncodedVideoFrame &frame, std::vector<AVFramePtr> &out);

private:
	bool open_internal(bool hardware);

	VideoCodec codec_ = VideoCodec::Unknown;
	AVCodecContext *ctx_ = nullptr;
	AVPacket *packet_ = nullptr;
	AVBufferRef *hw_device_ = nullptr;
	int hw_pix_fmt_ = -1;
	int hw_errors_ = 0;
	bool want_hardware_ = false;
};

} // namespace camdrop
