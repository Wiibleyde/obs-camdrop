#include "media/audio-decoder.hpp"

#include "plugin-support.h"

#include <obs-module.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <cstring>

namespace camdrop {

AudioDecoder::~AudioDecoder()
{
	close();
}

void AudioDecoder::close()
{
	if (ctx_)
		avcodec_free_context(&ctx_);
	if (packet_)
		av_packet_free(&packet_);
}

bool AudioDecoder::open()
{
	close();
	const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_OPUS);
	if (!decoder) {
		obs_log(LOG_ERROR, "No FFmpeg Opus decoder");
		return false;
	}
	ctx_ = avcodec_alloc_context3(decoder);
	packet_ = av_packet_alloc();
	if (!ctx_ || !packet_) {
		close();
		return false;
	}
	ctx_->sample_rate = 48000;
	av_channel_layout_default(&ctx_->ch_layout, 2);
	if (avcodec_open2(ctx_, decoder, nullptr) < 0) {
		obs_log(LOG_ERROR, "Cannot open the Opus decoder");
		close();
		return false;
	}
	return true;
}

bool AudioDecoder::decode(const uint8_t *data, size_t size, std::vector<AVFramePtr> &out)
{
	if (!ctx_ || size == 0)
		return false;
	av_packet_unref(packet_);
	if (av_new_packet(packet_, static_cast<int>(size)) < 0)
		return false;
	memcpy(packet_->data, data, size);

	if (avcodec_send_packet(ctx_, packet_) < 0)
		return false;
	for (;;) {
		AVFrame *frame = av_frame_alloc();
		int ret = avcodec_receive_frame(ctx_, frame);
		if (ret < 0) {
			av_frame_free(&frame);
			break;
		}
		out.emplace_back(frame);
	}
	return true;
}

} // namespace camdrop
