#include "media/video-decoder.hpp"

#include "plugin-support.h"

#include <obs-module.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <cstring>

namespace camdrop {

void AVFrameDeleter::operator()(AVFrame *frame) const
{
	av_frame_free(&frame);
}

namespace {

AVHWDeviceType platform_hw_type()
{
#if defined(_WIN32)
	return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
	return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
	return AV_HWDEVICE_TYPE_VAAPI;
#endif
}

enum AVPixelFormat pick_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *formats)
{
	const auto wanted = static_cast<AVPixelFormat>(reinterpret_cast<intptr_t>(ctx->opaque));
	for (const AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
		if (*p == wanted)
			return *p;
	}
	// Hardware path refused: first software format.
	for (const AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(*p);
		if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL))
			return *p;
	}
	return formats[0];
}

} // namespace

VideoDecoder::~VideoDecoder()
{
	close();
}

void VideoDecoder::close()
{
	if (ctx_)
		avcodec_free_context(&ctx_);
	if (packet_)
		av_packet_free(&packet_);
	if (hw_device_)
		av_buffer_unref(&hw_device_);
	hw_pix_fmt_ = -1;
}

bool VideoDecoder::open(VideoCodec codec, bool try_hardware)
{
	close();
	codec_ = codec;
	want_hardware_ = try_hardware;
	hw_errors_ = 0;
	// Hardware decoding only matters for H.264 (VP8 has no D3D11VA support).
	if (try_hardware && codec == VideoCodec::H264 && open_internal(true))
		return true;
	return open_internal(false);
}

bool VideoDecoder::open_internal(bool hardware)
{
	close();
	const AVCodecID id = codec_ == VideoCodec::H264 ? AV_CODEC_ID_H264
			     : codec_ == VideoCodec::VP8 ? AV_CODEC_ID_VP8
							  : AV_CODEC_ID_NONE;
	const AVCodec *decoder = avcodec_find_decoder(id);
	if (!decoder) {
		obs_log(LOG_ERROR, "No FFmpeg decoder for %s", video_codec_name(codec_));
		return false;
	}

	ctx_ = avcodec_alloc_context3(decoder);
	packet_ = av_packet_alloc();
	if (!ctx_ || !packet_) {
		close();
		return false;
	}

	ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
	if (codec_ == VideoCodec::H264)
		ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
	// Frame threading adds one frame of latency per thread: slice threading only.
	ctx_->thread_type = FF_THREAD_SLICE;
	ctx_->thread_count = hardware ? 1 : 2;

	if (hardware) {
		const AVHWDeviceType type = platform_hw_type();
		int pix_fmt = -1;
		for (int i = 0;; i++) {
			const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i);
			if (!config)
				break;
			if (config->device_type == type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX)) {
				pix_fmt = config->pix_fmt;
				break;
			}
		}
		if (pix_fmt < 0 || av_hwdevice_ctx_create(&hw_device_, type, nullptr, nullptr, 0) < 0) {
			close();
			return false;
		}
		ctx_->hw_device_ctx = av_buffer_ref(hw_device_);
		ctx_->opaque = reinterpret_cast<void *>(static_cast<intptr_t>(pix_fmt));
		ctx_->get_format = pick_hw_format;
		hw_pix_fmt_ = pix_fmt;
	}

	if (avcodec_open2(ctx_, decoder, nullptr) < 0) {
		obs_log(LOG_WARNING, "Cannot open %s decoder (%s)", video_codec_name(codec_),
			hardware ? "hardware" : "software");
		close();
		return false;
	}
	obs_log(LOG_INFO, "%s decoder ready (%s)", video_codec_name(codec_), hardware ? "hardware" : "software");
	return true;
}

VideoDecoder::Status VideoDecoder::decode(const EncodedVideoFrame &frame, std::vector<AVFramePtr> &out)
{
	if (!ctx_)
		return Status::Error;

	av_packet_unref(packet_);
	if (av_new_packet(packet_, static_cast<int>(frame.data.size())) < 0)
		return Status::Error;
	memcpy(packet_->data, frame.data.data(), frame.data.size());
	packet_->pts = frame.rtp_timestamp;
	packet_->dts = frame.rtp_timestamp;
	if (frame.keyframe)
		packet_->flags |= AV_PKT_FLAG_KEY;

	int ret = avcodec_send_packet(ctx_, packet_);
	bool failed = ret < 0 && ret != AVERROR(EAGAIN);

	while (!failed) {
		AVFrame *decoded = av_frame_alloc();
		ret = avcodec_receive_frame(ctx_, decoded);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
			av_frame_free(&decoded);
			break;
		}
		if (ret < 0) {
			av_frame_free(&decoded);
			failed = true;
			break;
		}
		if (decoded->decode_error_flags || (decoded->flags & AV_FRAME_FLAG_CORRUPT)) {
			av_frame_free(&decoded);
			failed = true;
			break;
		}

		if (hw_pix_fmt_ >= 0 && decoded->format == hw_pix_fmt_) {
			AVFrame *sw = av_frame_alloc();
			if (av_hwframe_transfer_data(sw, decoded, 0) < 0) {
				av_frame_free(&sw);
				av_frame_free(&decoded);
				failed = true;
				break;
			}
			av_frame_copy_props(sw, decoded);
			av_frame_free(&decoded);
			decoded = sw;
		}
		out.emplace_back(decoded);
	}

	if (!failed) {
		hw_errors_ = 0;
		return Status::Ok;
	}

	if (hardware() && ++hw_errors_ >= 3) {
		obs_log(LOG_WARNING, "Hardware decoding failed repeatedly, switching to software decoding");
		open_internal(false);
	}
	return Status::NeedKeyframe;
}

} // namespace camdrop
