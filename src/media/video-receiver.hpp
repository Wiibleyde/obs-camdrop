#pragma once

#include "media/rtp.hpp"

#include <cstdint>
#include <map>
#include <vector>

namespace camdrop {

enum class VideoCodec { Unknown, H264, VP8 };

const char *video_codec_name(VideoCodec codec);

struct EncodedVideoFrame {
	VideoCodec codec = VideoCodec::Unknown;
	std::vector<uint8_t> data; // H.264 Annex B access unit, or a VP8 frame
	uint32_t rtp_timestamp = 0;
	bool keyframe = false;
	int64_t first_arrival_ns = 0;
	int64_t complete_ns = 0;
	size_t packets = 0;
};

// Jitter buffer, first stage: reorders RTP packets, asks for retransmissions (NACK), assembles
// complete frames and gives them out in order. After an unrecoverable loss it drops everything
// until the next key frame (and asks for one), so the decoder never sees a broken reference chain.
class VideoReceiver {
public:
	struct Result {
		std::vector<EncodedVideoFrame> frames;
		std::vector<uint16_t> nacks;
		bool request_keyframe = false;
	};

	struct Stats {
		uint64_t frames = 0;
		uint64_t keyframes = 0;
		uint64_t lost_packets = 0;      // never recovered
		uint64_t recovered_packets = 0; // arrived after a NACK
		uint64_t dropped_frames = 0;
		uint64_t resyncs = 0;
	};

	void set_payload_codec(int payload_type, VideoCodec codec);
	// Maximum time to wait for a missing packet before skipping to the next key frame.
	void set_max_wait_ms(int ms) { max_wait_ns_ = static_cast<int64_t>(ms) * 1000000; }
	void set_rtt_ms(int ms) { rtt_ns_ = static_cast<int64_t>(ms) * 1000000; }

	Result insert(RtpPacket &&packet);
	Stats stats() const { return stats_; }

private:
	struct Missing {
		int64_t first_seen_ns = 0;
		int64_t last_nack_ns = 0;
		int retries = 0;
	};

	VideoCodec codec_for(uint8_t pt) const;
	bool is_frame_start(int64_t ext) const;
	bool frame_is_keyframe(int64_t first, int64_t last) const;
	// Returns the ext seq of the marker packet if [first..] forms a complete frame, -1 if a packet is
	// missing (then `missing_at` is set), -2 if the frame is not finished yet.
	int64_t find_frame_end(int64_t first, int64_t &missing_at) const;
	EncodedVideoFrame assemble(int64_t first, int64_t last) const;
	void reset(Result &result);
	void drop_before(int64_t ext);
	void collect_nacks(int64_t now, Result &result);

	std::map<int, VideoCodec> codecs_;
	SeqUnwrapper unwrapper_;
	std::map<int64_t, RtpPacket> packets_;
	std::map<int64_t, Missing> missing_;
	int64_t highest_ = -1;
	int64_t next_ = -1; // first packet of the next frame to give out, -1 = waiting for a key frame
	int64_t max_wait_ns_ = 250 * 1000000LL;
	int64_t rtt_ns_ = 10 * 1000000LL;
	int64_t last_keyframe_request_ns_ = 0;
	Stats stats_;
};

} // namespace camdrop
