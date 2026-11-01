#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace camdrop {

struct RtpPacket {
	uint8_t payload_type = 0;
	bool marker = false;
	uint16_t seq = 0;
	uint32_t timestamp = 0;
	uint32_t ssrc = 0;
	std::vector<uint8_t> payload; // without header, extensions and padding
	int64_t arrival_ns = 0;
	int64_t ext_seq = 0; // unwrapped sequence number (set by receivers)
	size_t size = 0;     // total packet size (stats)
};

// RFC 5761 demultiplexing: RTCP packet types 192..223 share the second byte with RTP M+PT.
bool is_rtcp(const uint8_t *data, size_t len);
bool parse_rtp(const uint8_t *data, size_t len, RtpPacket &out);

struct SenderReport {
	uint32_t ssrc = 0;
	uint64_t ntp = 0; // 32.32 fixed point, seconds since 1900
	uint32_t rtp_timestamp = 0;
};

// Extracts every sender report of a (compound) RTCP packet.
std::vector<SenderReport> parse_sender_reports(const uint8_t *data, size_t len);

// RFC 4585 generic NACK (RTPFB, FMT 1).
std::vector<uint8_t> build_nack(uint32_t sender_ssrc, uint32_t media_ssrc, const std::vector<uint16_t> &seqs);

// RFC 4585 picture loss indication (PSFB, FMT 1).
std::vector<uint8_t> build_pli(uint32_t sender_ssrc, uint32_t media_ssrc);

int64_t ntp_to_ns(uint64_t ntp);

// Turns 16-bit sequence numbers into a monotonic 64-bit counter.
class SeqUnwrapper {
public:
	int64_t unwrap(uint16_t seq);
	void reset() { has_last_ = false; }

private:
	bool has_last_ = false;
	int64_t last_ = 0;
};

} // namespace camdrop
