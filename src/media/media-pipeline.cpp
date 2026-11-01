#include "media/media-pipeline.hpp"

#include "core/channel.hpp"
#include "plugin-support.h"

#include <obs-module.h>
#include <util/platform.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>

namespace camdrop {

namespace {

constexpr int64_t kMs = 1000000;
constexpr int64_t kKeyframeRequestIntervalNs = 250 * kMs;
constexpr size_t kMaxVideoQueue = 60;
constexpr size_t kMaxAudioQueue = 300;
constexpr int64_t kMaxAudioLateNs = 250 * kMs;
constexpr int64_t kAudioSnapNs = 30 * kMs;

bool to_obs_format(int av_format, video_format &format, bool &full_range)
{
	full_range = false;
	switch (av_format) {
	case AV_PIX_FMT_YUVJ420P:
		full_range = true;
		[[fallthrough]];
	case AV_PIX_FMT_YUV420P:
		format = VIDEO_FORMAT_I420;
		return true;
	case AV_PIX_FMT_NV12:
		format = VIDEO_FORMAT_NV12;
		return true;
	case AV_PIX_FMT_YUVJ422P:
		full_range = true;
		[[fallthrough]];
	case AV_PIX_FMT_YUV422P:
		format = VIDEO_FORMAT_I422;
		return true;
	case AV_PIX_FMT_YUVJ444P:
		full_range = true;
		[[fallthrough]];
	case AV_PIX_FMT_YUV444P:
		format = VIDEO_FORMAT_I444;
		return true;
	case AV_PIX_FMT_P010LE:
		format = VIDEO_FORMAT_P010; // H.265 Main 10, hardware decoded
		return true;
	case AV_PIX_FMT_YUV420P10LE:
		format = VIDEO_FORMAT_I010; // H.265 Main 10, software decoded
		return true;
	default:
		return false;
	}
}

} // namespace

MediaPipeline::MediaPipeline(std::shared_ptr<Channel> channel, bool hardware_decoding)
	: channel_(std::move(channel)),
	  hardware_decoding_(hardware_decoding)
{
	clock_.set_clock_rate(StreamKind::Video, 90000);
	clock_.set_clock_rate(StreamKind::Audio, 48000);
}

MediaPipeline::~MediaPipeline()
{
	stop();
}

void MediaPipeline::start()
{
	if (running_)
		return;
	running_ = true;
	thread_ = std::thread([this] { run(); });
}

void MediaPipeline::stop()
{
	running_ = false;
	cv_.notify_all();
	if (thread_.joinable())
		thread_.join();
	std::lock_guard<std::mutex> lock(mutex_);
	video_in_.clear();
	audio_in_.clear();
}

void MediaPipeline::set_keyframe_requester(std::function<void()> fn)
{
	std::lock_guard<std::mutex> lock(mutex_);
	keyframe_requester_ = std::move(fn);
}

void MediaPipeline::push_video_frame(EncodedVideoFrame &&frame)
{
	clock_.observe(StreamKind::Video, frame.rtp_timestamp, frame.complete_ns);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		video_bytes_ += frame.data.size();
		video_in_.push_back(std::move(frame));
		while (video_in_.size() > kMaxVideoQueue)
			video_in_.pop_front();
	}
	cv_.notify_one();
}

void MediaPipeline::push_audio_packet(RtpPacket &&packet)
{
	clock_.observe(StreamKind::Audio, packet.timestamp, packet.arrival_ns);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		audio_bytes_ += packet.size;
		audio_in_.emplace(packet.ext_seq, std::move(packet));
		while (audio_in_.size() > kMaxAudioQueue)
			audio_in_.erase(audio_in_.begin());
	}
	cv_.notify_one();
}

void MediaPipeline::request_keyframe()
{
	const int64_t now = static_cast<int64_t>(os_gettime_ns());
	if (now - last_keyframe_request_ns_ < kKeyframeRequestIntervalNs)
		return;
	last_keyframe_request_ns_ = now;
	std::function<void()> fn;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		fn = keyframe_requester_;
	}
	if (fn)
		fn();
}

void MediaPipeline::run()
{
	if (!audio_decoder_.open())
		obs_log(LOG_WARNING, "Audio decoding unavailable");
	rate_window_start_ = static_cast<int64_t>(os_gettime_ns());

	while (running_) {
		std::deque<EncodedVideoFrame> frames;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			const int64_t now = static_cast<int64_t>(os_gettime_ns());
			int64_t wait_ns = 10 * kMs;
			if (!video_out_.empty())
				wait_ns = std::min(wait_ns, video_out_.front().play_ns - now);
			if (!audio_in_.empty())
				wait_ns = std::min(wait_ns, 5 * kMs);
			if (video_in_.empty() && wait_ns > 0) {
				cv_.wait_for(lock, std::chrono::nanoseconds(wait_ns),
					     [this] { return !running_ || !video_in_.empty(); });
			}
			frames.swap(video_in_);
		}
		if (!running_)
			break;

		const int buffer_ms = channel_->buffer_ms();
		const int64_t buffer_ns = static_cast<int64_t>(buffer_ms) * kMs;
		{
			std::lock_guard<std::mutex> lock(stats_mutex_);
			stats_.buffer_ms = buffer_ms;
		}

		if (!frames.empty())
			decode_video(frames);

		const int64_t now = static_cast<int64_t>(os_gettime_ns());
		output_due_video(now);
		output_due_audio(now, static_cast<int>(buffer_ns / kMs));
		update_rates(now);
	}

	video_out_.clear();
	video_decoder_.close();
	audio_decoder_.close();
}

void MediaPipeline::decode_video(std::deque<EncodedVideoFrame> &frames)
{
	const int64_t buffer_ns = static_cast<int64_t>(channel_->buffer_ms()) * kMs;

	for (auto &f : frames) {
		if (!video_decoder_.is_open() || video_decoder_.codec() != f.codec) {
			if (!f.keyframe) {
				request_keyframe();
				continue;
			}
			if (!video_decoder_.open(f.codec, hardware_decoding_))
				continue;
			std::lock_guard<std::mutex> lock(stats_mutex_);
			stats_.codec = video_codec_name(f.codec);
			stats_.hardware = video_decoder_.hardware();
		}

		std::vector<AVFramePtr> decoded;
		if (video_decoder_.decode(f, decoded) != VideoDecoder::Status::Ok) {
			{
				std::lock_guard<std::mutex> lock(stats_mutex_);
				stats_.decode_errors++;
				stats_.hardware = video_decoder_.hardware();
			}
			request_keyframe();
		}

		for (auto &d : decoded) {
			const int64_t now = static_cast<int64_t>(os_gettime_ns());
			int64_t base = 0;
			int64_t play;
			if (clock_.base_playout_ns(StreamKind::Video, static_cast<uint32_t>(d->pts), base))
				play = base + buffer_ns;
			else
				play = f.complete_ns + buffer_ns;
			if (play > now + buffer_ns + 500 * kMs)
				play = now + buffer_ns; // clock anomaly guard
			video_out_.push_back(DecodedVideo{std::move(d), play, f.complete_ns});
		}
	}

	while (video_out_.size() > kMaxVideoQueue)
		video_out_.pop_front();
}

void MediaPipeline::output_due_video(int64_t now)
{
	while (!video_out_.empty() && video_out_.front().play_ns <= now) {
		if (video_out_.size() > 1 && video_out_[1].play_ns <= now) {
			// Two frames due at once: show the newest, latency first.
			video_out_.pop_front();
			std::lock_guard<std::mutex> lock(stats_mutex_);
			stats_.frames_skipped++;
			continue;
		}
		DecodedVideo &v = video_out_.front();
		output_frame(v.frame.get(), std::max(v.play_ns, now - 5 * kMs));
		delay_acc_ms_ += static_cast<double>(now - v.complete_ns) / 1e6;
		delay_count_++;
		frames_window_++;
		video_out_.pop_front();
	}
}

void MediaPipeline::output_frame(const AVFrame *frame, int64_t timestamp_ns)
{
	video_format format;
	bool full_range = false;
	if (!to_obs_format(frame->format, format, full_range)) {
		static bool warned = false;
		if (!warned) {
			obs_log(LOG_WARNING, "Unsupported decoded pixel format %d", frame->format);
			warned = true;
		}
		return;
	}
	if (frame->color_range == AVCOL_RANGE_JPEG)
		full_range = true;

	// Browsers convert camera frames with BT.601 unless the stream says otherwise.
	video_colorspace cs = VIDEO_CS_601;
	if (frame->colorspace == AVCOL_SPC_BT709)
		cs = VIDEO_CS_709;
	else if (frame->colorspace == AVCOL_SPC_BT2020_NCL)
		cs = frame->color_trc == AVCOL_TRC_SMPTE2084      ? VIDEO_CS_2100_PQ
		     : frame->color_trc == AVCOL_TRC_ARIB_STD_B67 ? VIDEO_CS_2100_HLG
								  : VIDEO_CS_709;

	obs_source_frame out = {};
	for (int i = 0; i < 3; i++) {
		out.data[i] = frame->data[i];
		out.linesize[i] = static_cast<uint32_t>(frame->linesize[i]);
	}
	out.width = static_cast<uint32_t>(frame->width);
	out.height = static_cast<uint32_t>(frame->height);
	out.format = format;
	out.timestamp = static_cast<uint64_t>(timestamp_ns);
	out.full_range = full_range;
	video_format_get_parameters_for_format(cs, full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL, format,
					       out.color_matrix, out.color_range_min, out.color_range_max);
	channel_->output_video(&out);

	std::lock_guard<std::mutex> lock(stats_mutex_);
	stats_.width = frame->width;
	stats_.height = frame->height;
	stats_.frames_decoded++;
}

void MediaPipeline::output_due_audio(int64_t now, int buffer_ms)
{
	const int64_t buffer_ns = static_cast<int64_t>(buffer_ms) * kMs;
	std::vector<RtpPacket> due;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		while (!audio_in_.empty()) {
			RtpPacket &p = audio_in_.begin()->second;
			int64_t base = 0;
			int64_t play = clock_.base_playout_ns(StreamKind::Audio, p.timestamp, base)
					       ? base + buffer_ns
					       : p.arrival_ns + buffer_ns;
			if (play > now + buffer_ns + 1000 * kMs)
				play = now; // clock anomaly guard
			if (play > now + 2 * kMs)
				break;
			p.arrival_ns = play; // reused as the playout time from here on
			due.push_back(std::move(p));
			audio_in_.erase(audio_in_.begin());
		}
	}

	for (auto &p : due) {
		if (p.ext_seq <= last_output_ext_ || p.arrival_ns < now - kMaxAudioLateNs) {
			std::lock_guard<std::mutex> lock(stats_mutex_);
			stats_.audio_dropped++;
			continue;
		}
		last_output_ext_ = p.ext_seq;

		std::vector<AVFramePtr> frames;
		if (!audio_decoder_.decode(p.payload.data(), p.payload.size(), frames))
			continue;

		int64_t ts = p.arrival_ns;
		for (auto &f : frames) {
			// Keep the audio timeline continuous when the computed time is close to it.
			if (last_audio_ts_ != 0 && std::llabs(ts - last_audio_ts_) < kAudioSnapNs)
				ts = last_audio_ts_;

			obs_source_audio audio = {};
			const int channels = f->ch_layout.nb_channels;
			for (int i = 0; i < channels && i < MAX_AV_PLANES; i++)
				audio.data[i] = f->extended_data[i];
			audio.frames = static_cast<uint32_t>(f->nb_samples);
			audio.speakers = channels == 1 ? SPEAKERS_MONO : SPEAKERS_STEREO;
			audio.format = f->format == AV_SAMPLE_FMT_FLTP  ? AUDIO_FORMAT_FLOAT_PLANAR
				       : f->format == AV_SAMPLE_FMT_FLT ? AUDIO_FORMAT_FLOAT
				       : f->format == AV_SAMPLE_FMT_S16 ? AUDIO_FORMAT_16BIT
									: AUDIO_FORMAT_FLOAT_PLANAR;
			audio.samples_per_sec = static_cast<uint32_t>(f->sample_rate > 0 ? f->sample_rate : 48000);
			audio.timestamp = static_cast<uint64_t>(ts);
			channel_->output_audio(&audio);

			const int64_t duration =
				static_cast<int64_t>(f->nb_samples) * 1000000000LL / audio.samples_per_sec;
			last_audio_ts_ = ts + duration;
			ts += duration;
		}
	}
}

void MediaPipeline::update_rates(int64_t now)
{
	const int64_t elapsed = now - rate_window_start_;
	if (elapsed < 1000 * kMs)
		return;
	uint64_t vbytes, abytes;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		vbytes = video_bytes_;
		abytes = audio_bytes_;
		video_bytes_ = 0;
		audio_bytes_ = 0;
	}
	const double seconds = static_cast<double>(elapsed) / 1e9;
	std::lock_guard<std::mutex> lock(stats_mutex_);
	stats_.fps = static_cast<double>(frames_window_) / seconds;
	stats_.video_kbps = static_cast<double>(vbytes) * 8.0 / 1000.0 / seconds;
	stats_.audio_kbps = static_cast<double>(abytes) * 8.0 / 1000.0 / seconds;
	stats_.delay_ms = delay_count_ ? delay_acc_ms_ / static_cast<double>(delay_count_) : 0;
	stats_.av_synced = clock_.synced();
	frames_window_ = 0;
	delay_acc_ms_ = 0;
	delay_count_ = 0;
	rate_window_start_ = now;
}

PipelineStats MediaPipeline::stats() const
{
	std::lock_guard<std::mutex> lock(stats_mutex_);
	return stats_;
}

} // namespace camdrop
