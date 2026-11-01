#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace camdrop {

enum class StreamKind { Video = 0, Audio = 1 };

// Maps RTP timestamps of both streams to local playout times.
//
// Each stream converts its RTP timestamps to the sender's NTP clock with the latest RTCP sender
// report (before the first report: to a local clock anchored on the first packet). The offset
// between that clock and our clock is the smallest (arrival - send time) seen over a sliding
// window, i.e. the delay of the fastest packets. Audio and video share this offset once both have
// sender reports, which keeps them in sync. Playout time = send time + offset + buffer.
class ClockSync {
public:
	void set_clock_rate(StreamKind kind, int rate);
	void on_sender_report(StreamKind kind, uint64_t ntp, uint32_t rtp_timestamp);

	// Records an arrival (frame completion for video, packet arrival for audio).
	void observe(StreamKind kind, uint32_t rtp_timestamp, int64_t arrival_ns);

	// Local playout time (os_gettime_ns clock) of an RTP timestamp, without the buffer delay.
	// Returns false while the stream has no mapping yet.
	bool base_playout_ns(StreamKind kind, uint32_t rtp_timestamp, int64_t &out);

	// Smallest transit observed (ms), for statistics.
	bool synced() const;

private:
	class MinWindow {
	public:
		void add(int64_t now_ns, int64_t value);
		bool get(int64_t &out) const;
		void reset();

	private:
		static constexpr int kBuckets = 6;
		static constexpr int64_t kBucketNs = 500 * 1000000LL; // 6 x 500 ms = 3 s window
		std::array<int64_t, kBuckets> mins_{};
		std::array<int64_t, kBuckets> starts_{};
		std::array<bool, kBuckets> used_{};
		int current_ = -1;
	};

	struct Stream {
		int rate = 90000;
		bool has_sr = false;
		int64_t sr_ntp_ns = 0;
		uint32_t sr_rtp = 0;
		bool has_anchor = false;
		int64_t anchor_ns = 0; // local anchor before the first sender report
		uint32_t anchor_rtp = 0;
		MinWindow local_offset; // offset in the anchored (local) domain
	};

	bool send_time_ns(const Stream &s, uint32_t rtp, int64_t &out, bool &ntp_domain) const;

	mutable std::mutex mutex_;
	std::array<Stream, 2> streams_;
	MinWindow ntp_offset_; // shared offset in the sender NTP domain
};

} // namespace camdrop
