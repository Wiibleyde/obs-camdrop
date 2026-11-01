#pragma once

#include "media/audio-decoder.hpp"
#include "media/clock-sync.hpp"
#include "media/rtp.hpp"
#include "media/video-decoder.hpp"
#include "media/video-receiver.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace camdrop {

class Channel;

struct PipelineStats {
	double fps = 0;
	double video_kbps = 0;
	double audio_kbps = 0;
	int width = 0;
	int height = 0;
	std::string codec;
	bool hardware = false;
	uint64_t frames_decoded = 0;
	uint64_t frames_skipped = 0; // late frames dropped to keep the latency
	uint64_t decode_errors = 0;
	uint64_t audio_dropped = 0;
	double delay_ms = 0;  // average time between frame completion and display
	int buffer_ms = 0;    // current target
	bool av_synced = false;
};

// Second stage of the jitter buffer: decodes complete frames and hands video and audio to OBS at
// their playout time (sender time + network delay + buffer). Runs one thread per camera.
class MediaPipeline {
public:
	MediaPipeline(std::shared_ptr<Channel> channel, bool hardware_decoding);
	~MediaPipeline();

	void start();
	void stop();

	// Called from the network threads.
	void push_video_frame(EncodedVideoFrame &&frame);
	void push_audio_packet(RtpPacket &&packet);

	ClockSync &clock() { return clock_; }
	void set_keyframe_requester(std::function<void()> fn);

	PipelineStats stats() const;

private:
	struct DecodedVideo {
		AVFramePtr frame;
		int64_t play_ns = 0;
		int64_t complete_ns = 0;
	};

	void run();
	void decode_video(std::deque<EncodedVideoFrame> &frames);
	void output_due_video(int64_t now);
	void output_due_audio(int64_t now, int buffer_ns);
	void output_frame(const AVFrame *frame, int64_t timestamp_ns);
	void request_keyframe();
	void update_rates(int64_t now);

	std::shared_ptr<Channel> channel_;
	bool hardware_decoding_;
	ClockSync clock_;

	std::thread thread_;
	std::atomic<bool> running_{false};
	mutable std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<EncodedVideoFrame> video_in_;
	std::map<int64_t, RtpPacket> audio_in_; // by extended sequence number

	// Pipeline thread only
	VideoDecoder video_decoder_;
	AudioDecoder audio_decoder_;
	std::deque<DecodedVideo> video_out_;
	int64_t last_keyframe_request_ns_ = 0;
	int64_t last_audio_ts_ = 0;
	int64_t last_output_ext_ = -1;
	std::function<void()> keyframe_requester_;

	// Statistics
	mutable std::mutex stats_mutex_;
	PipelineStats stats_;
	uint64_t video_bytes_ = 0;
	uint64_t audio_bytes_ = 0;
	uint64_t frames_window_ = 0;
	int64_t rate_window_start_ = 0;
	double delay_acc_ms_ = 0;
	uint64_t delay_count_ = 0;
};

} // namespace camdrop
