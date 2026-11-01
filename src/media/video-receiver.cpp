#include "media/video-receiver.hpp"

#include <algorithm>

namespace camdrop {

namespace {

constexpr int64_t kMs = 1000000;
constexpr int64_t kKeyframeRequestIntervalNs = 300 * kMs;
constexpr size_t kMaxBufferedPackets = 4000;
constexpr int64_t kMaxGap = 1000;
constexpr int kMaxNackRetries = 5;
constexpr size_t kMaxNacksPerPacket = 64;

const uint8_t kStartCode[4] = {0, 0, 0, 1};

inline uint16_t rd16(const uint8_t *p)
{
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

// Size of the VP8 payload descriptor (RFC 7741), 0 if invalid.
size_t vp8_descriptor_size(const std::vector<uint8_t> &p)
{
	if (p.empty())
		return 0;
	size_t pos = 1;
	if (p[0] & 0x80) {
		if (p.size() < 2)
			return 0;
		const uint8_t x = p[1];
		pos = 2;
		if (x & 0x80) { // I: picture ID
			if (pos >= p.size())
				return 0;
			pos += (p[pos] & 0x80) ? 2 : 1;
		}
		if (x & 0x40) // L: TL0PICIDX
			pos += 1;
		if (x & 0x30) // T or K: TID/Y/KEYIDX
			pos += 1;
	}
	return pos < p.size() ? pos : 0;
}

bool vp8_is_partition_start(const std::vector<uint8_t> &p)
{
	return !p.empty() && (p[0] & 0x10) && (p[0] & 0x07) == 0;
}

bool h264_payload_has_keyframe_nal(const std::vector<uint8_t> &p)
{
	if (p.empty())
		return false;
	const uint8_t type = p[0] & 0x1F;
	if (type == 5 || type == 7)
		return true;
	if (type == 24) {
		size_t pos = 1;
		while (pos + 2 < p.size()) {
			const size_t n = rd16(&p[pos]);
			pos += 2;
			if (n == 0 || pos + n > p.size())
				break;
			const uint8_t t = p[pos] & 0x1F;
			if (t == 5 || t == 7)
				return true;
			pos += n;
		}
		return false;
	}
	if (type == 28 && p.size() >= 2)
		return (p[1] & 0x1F) == 5;
	return false;
}

} // namespace

const char *video_codec_name(VideoCodec codec)
{
	switch (codec) {
	case VideoCodec::H264:
		return "H.264";
	case VideoCodec::VP8:
		return "VP8";
	default:
		return "?";
	}
}

void VideoReceiver::set_payload_codec(int payload_type, VideoCodec codec)
{
	codecs_[payload_type] = codec;
}

VideoCodec VideoReceiver::codec_for(uint8_t pt) const
{
	auto it = codecs_.find(pt);
	return it == codecs_.end() ? VideoCodec::Unknown : it->second;
}

bool VideoReceiver::is_frame_start(int64_t ext) const
{
	auto it = packets_.find(ext);
	if (it == packets_.end())
		return false;
	const RtpPacket &p = it->second;

	auto prev = packets_.find(ext - 1);
	if (prev != packets_.end() && (prev->second.timestamp != p.timestamp || prev->second.marker))
		return true;

	switch (codec_for(p.payload_type)) {
	case VideoCodec::H264: {
		if (p.payload.empty())
			return false;
		const uint8_t type = p.payload[0] & 0x1F;
		if (type == 7 || type == 9)
			return true; // SPS or access unit delimiter
		if (type == 24 && p.payload.size() > 3) {
			const uint8_t first = p.payload[3] & 0x1F;
			return first == 7 || first == 9;
		}
		return false;
	}
	case VideoCodec::VP8:
		return vp8_is_partition_start(p.payload);
	default:
		return false;
	}
}

bool VideoReceiver::frame_is_keyframe(int64_t first, int64_t last) const
{
	auto it = packets_.find(first);
	if (it == packets_.end())
		return false;
	const VideoCodec codec = codec_for(it->second.payload_type);
	if (codec == VideoCodec::VP8) {
		const auto &p = it->second.payload;
		if (!vp8_is_partition_start(p))
			return false;
		const size_t d = vp8_descriptor_size(p);
		return d > 0 && (p[d] & 0x01) == 0;
	}
	if (codec == VideoCodec::H264) {
		for (int64_t e = first; e <= last; e++) {
			auto p = packets_.find(e);
			if (p != packets_.end() && h264_payload_has_keyframe_nal(p->second.payload))
				return true;
		}
	}
	return false;
}

int64_t VideoReceiver::find_frame_end(int64_t first, int64_t &missing_at) const
{
	auto it = packets_.find(first);
	if (it == packets_.end()) {
		missing_at = first;
		return first <= highest_ ? -1 : -2;
	}
	const uint32_t ts = it->second.timestamp;
	for (int64_t e = first;; e++) {
		auto p = packets_.find(e);
		if (p == packets_.end()) {
			if (e > highest_)
				return -2;
			missing_at = e;
			return -1;
		}
		if (p->second.timestamp != ts)
			return e - 1; // marker bit lost: the timestamp change closes the frame
		if (p->second.marker)
			return e;
	}
}

EncodedVideoFrame VideoReceiver::assemble(int64_t first, int64_t last) const
{
	EncodedVideoFrame frame;
	auto first_it = packets_.find(first);
	frame.codec = codec_for(first_it->second.payload_type);
	frame.rtp_timestamp = first_it->second.timestamp;
	frame.first_arrival_ns = first_it->second.arrival_ns;
	frame.keyframe = frame_is_keyframe(first, last);

	for (int64_t e = first; e <= last; e++) {
		auto it = packets_.find(e);
		if (it == packets_.end())
			continue;
		const RtpPacket &pkt = it->second;
		const auto &p = pkt.payload;
		frame.complete_ns = std::max(frame.complete_ns, pkt.arrival_ns);
		frame.first_arrival_ns = std::min(frame.first_arrival_ns, pkt.arrival_ns);
		frame.packets++;
		if (p.empty())
			continue;

		if (frame.codec == VideoCodec::H264) {
			const uint8_t type = p[0] & 0x1F;
			if (type >= 1 && type <= 23) {
				frame.data.insert(frame.data.end(), kStartCode, kStartCode + 4);
				frame.data.insert(frame.data.end(), p.begin(), p.end());
			} else if (type == 24) {
				size_t pos = 1;
				while (pos + 2 <= p.size()) {
					const size_t n = rd16(&p[pos]);
					pos += 2;
					if (n == 0 || pos + n > p.size())
						break;
					frame.data.insert(frame.data.end(), kStartCode, kStartCode + 4);
					frame.data.insert(frame.data.end(), p.begin() + static_cast<ptrdiff_t>(pos),
							  p.begin() + static_cast<ptrdiff_t>(pos + n));
					pos += n;
				}
			} else if (type == 28 && p.size() > 2) {
				if (p[1] & 0x80) {
					frame.data.insert(frame.data.end(), kStartCode, kStartCode + 4);
					frame.data.push_back(static_cast<uint8_t>((p[0] & 0xE0) | (p[1] & 0x1F)));
				}
				frame.data.insert(frame.data.end(), p.begin() + 2, p.end());
			}
		} else if (frame.codec == VideoCodec::VP8) {
			const size_t d = vp8_descriptor_size(p);
			if (d > 0)
				frame.data.insert(frame.data.end(), p.begin() + static_cast<ptrdiff_t>(d), p.end());
		}
	}
	return frame;
}

void VideoReceiver::drop_before(int64_t ext)
{
	packets_.erase(packets_.begin(), packets_.lower_bound(ext));
	for (auto it = missing_.begin(); it != missing_.end() && it->first < ext;) {
		stats_.lost_packets++;
		it = missing_.erase(it);
	}
}

void VideoReceiver::reset(Result &result)
{
	stats_.lost_packets += missing_.size();
	packets_.clear();
	missing_.clear();
	highest_ = -1;
	next_ = -1;
	result.request_keyframe = true;
}

void VideoReceiver::collect_nacks(int64_t now, Result &result)
{
	const int64_t interval = std::max<int64_t>(rtt_ns_ + 5 * kMs, 20 * kMs);
	for (auto &[seq, m] : missing_) {
		if (now - m.first_seen_ns > max_wait_ns_ || m.retries >= kMaxNackRetries)
			continue;
		if (m.retries == 0 || now - m.last_nack_ns >= interval) {
			result.nacks.push_back(static_cast<uint16_t>(seq & 0xFFFF));
			m.retries++;
			m.last_nack_ns = now;
			if (result.nacks.size() >= kMaxNacksPerPacket)
				break;
		}
	}
}

VideoReceiver::Result VideoReceiver::insert(RtpPacket &&packet)
{
	Result result;
	if (codec_for(packet.payload_type) == VideoCodec::Unknown)
		return result;

	const int64_t now = packet.arrival_ns;
	const int64_t ext = unwrapper_.unwrap(packet.seq);
	packet.ext_seq = ext;

	if (next_ >= 0 && ext < next_) {
		missing_.erase(ext); // too late, already skipped
		return result;
	}
	if (packets_.count(ext))
		return result; // duplicate (retransmission of a packet we already had)

	if (highest_ >= 0 && ext > highest_ + 1) {
		if (ext - highest_ - 1 > kMaxGap) {
			reset(result);
		} else {
			for (int64_t s = highest_ + 1; s < ext; s++)
				missing_.emplace(s, Missing{now, 0, 0});
		}
	}

	auto mit = missing_.find(ext);
	if (mit != missing_.end()) {
		if (mit->second.retries > 0)
			stats_.recovered_packets++;
		missing_.erase(mit);
	}
	highest_ = std::max(highest_, ext);
	packets_.emplace(ext, std::move(packet));

	if (packets_.size() > kMaxBufferedPackets) {
		reset(result);
		return result;
	}

	collect_nacks(now, result);

	for (;;) {
		if (next_ < 0) {
			// Waiting for a key frame: the first complete one in the buffer restarts decoding.
			bool found = false;
			for (const auto &entry : packets_) {
				const int64_t e = entry.first;
				if (!is_frame_start(e))
					continue;
				int64_t miss = -1;
				const int64_t end = find_frame_end(e, miss);
				if (end >= 0 && frame_is_keyframe(e, end)) {
					drop_before(e);
					next_ = e;
					found = true;
					break;
				}
			}
			if (!found) {
				if (now - last_keyframe_request_ns_ > kKeyframeRequestIntervalNs) {
					result.request_keyframe = true;
					last_keyframe_request_ns_ = now;
				}
				break;
			}
			stats_.resyncs++;
		}

		int64_t miss = -1;
		const int64_t end = find_frame_end(next_, miss);
		if (end >= 0) {
			EncodedVideoFrame frame = assemble(next_, end);
			packets_.erase(packets_.lower_bound(next_), packets_.upper_bound(end));
			next_ = end + 1;
			stats_.frames++;
			if (frame.keyframe)
				stats_.keyframes++;
			if (!frame.data.empty())
				result.frames.push_back(std::move(frame));
			continue;
		}
		if (end == -2)
			break; // frame still arriving

		// A packet is missing: wait for the retransmission, up to max_wait.
		auto it = missing_.find(miss);
		const bool expired = it == missing_.end() || now - it->second.first_seen_ns > max_wait_ns_;
		if (!expired)
			break;

		stats_.dropped_frames++;
		next_ = -1;
		if (now - last_keyframe_request_ns_ > kKeyframeRequestIntervalNs) {
			result.request_keyframe = true;
			last_keyframe_request_ns_ = now;
		}
	}
	return result;
}

} // namespace camdrop
