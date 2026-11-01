#include "media/rtp.hpp"

namespace camdrop {

static inline uint16_t rd16(const uint8_t *p)
{
	return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static inline uint32_t rd32(const uint8_t *p)
{
	return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

static inline void wr16(std::vector<uint8_t> &v, uint16_t x)
{
	v.push_back(static_cast<uint8_t>(x >> 8));
	v.push_back(static_cast<uint8_t>(x & 0xff));
}

static inline void wr32(std::vector<uint8_t> &v, uint32_t x)
{
	v.push_back(static_cast<uint8_t>(x >> 24));
	v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
	v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
	v.push_back(static_cast<uint8_t>(x & 0xff));
}

bool is_rtcp(const uint8_t *data, size_t len)
{
	if (len < 8 || (data[0] >> 6) != 2)
		return false;
	return data[1] >= 192 && data[1] <= 223;
}

bool parse_rtp(const uint8_t *data, size_t len, RtpPacket &out)
{
	if (len < 12 || (data[0] >> 6) != 2)
		return false;
	const bool padding = (data[0] & 0x20) != 0;
	const bool extension = (data[0] & 0x10) != 0;
	const size_t csrc = data[0] & 0x0F;
	size_t pos = 12 + csrc * 4;
	if (pos > len)
		return false;
	if (extension) {
		if (pos + 4 > len)
			return false;
		const size_t ext_words = rd16(data + pos + 2);
		pos += 4 + ext_words * 4;
		if (pos > len)
			return false;
	}
	size_t end = len;
	if (padding) {
		const size_t pad = data[len - 1];
		if (pad == 0 || pad > end - pos)
			return false;
		end -= pad;
	}
	out.marker = (data[1] & 0x80) != 0;
	out.payload_type = data[1] & 0x7F;
	out.seq = rd16(data + 2);
	out.timestamp = rd32(data + 4);
	out.ssrc = rd32(data + 8);
	out.payload.assign(data + pos, data + end);
	out.size = len;
	return true;
}

std::vector<SenderReport> parse_sender_reports(const uint8_t *data, size_t len)
{
	std::vector<SenderReport> out;
	size_t pos = 0;
	while (pos + 4 <= len) {
		const uint8_t *p = data + pos;
		if ((p[0] >> 6) != 2)
			break;
		const size_t length = (static_cast<size_t>(rd16(p + 2)) + 1) * 4;
		if (pos + length > len)
			break;
		if (p[1] == 200 && length >= 28) {
			SenderReport sr;
			sr.ssrc = rd32(p + 4);
			sr.ntp = (uint64_t(rd32(p + 8)) << 32) | rd32(p + 12);
			sr.rtp_timestamp = rd32(p + 16);
			out.push_back(sr);
		}
		pos += length;
	}
	return out;
}

std::vector<uint8_t> build_nack(uint32_t sender_ssrc, uint32_t media_ssrc, const std::vector<uint16_t> &seqs)
{
	// Group sequence numbers into (PID, BLP) pairs.
	std::vector<std::pair<uint16_t, uint16_t>> fci;
	for (uint16_t seq : seqs) {
		if (!fci.empty()) {
			uint16_t diff = static_cast<uint16_t>(seq - fci.back().first);
			if (diff >= 1 && diff <= 16) {
				fci.back().second |= static_cast<uint16_t>(1u << (diff - 1));
				continue;
			}
		}
		fci.emplace_back(seq, 0);
	}

	std::vector<uint8_t> out;
	const uint16_t length_words = static_cast<uint16_t>(2 + fci.size()); // (size / 4) - 1
	out.push_back(0x80 | 1);                                             // V=2, FMT=1
	out.push_back(205);                                                  // RTPFB
	wr16(out, length_words);
	wr32(out, sender_ssrc);
	wr32(out, media_ssrc);
	for (const auto &f : fci) {
		wr16(out, f.first);
		wr16(out, f.second);
	}
	return out;
}

std::vector<uint8_t> build_pli(uint32_t sender_ssrc, uint32_t media_ssrc)
{
	std::vector<uint8_t> out;
	out.push_back(0x80 | 1); // V=2, FMT=1
	out.push_back(206);      // PSFB
	wr16(out, 2);
	wr32(out, sender_ssrc);
	wr32(out, media_ssrc);
	return out;
}

int64_t ntp_to_ns(uint64_t ntp)
{
	const uint64_t sec = ntp >> 32;
	const uint64_t frac = ntp & 0xFFFFFFFFull;
	return static_cast<int64_t>(sec * 1000000000ull + ((frac * 1000000000ull) >> 32));
}

int64_t SeqUnwrapper::unwrap(uint16_t seq)
{
	if (!has_last_) {
		has_last_ = true;
		last_ = seq + 65536 * 4; // headroom so early reordered packets stay positive
		return last_;
	}
	const uint16_t last16 = static_cast<uint16_t>(last_ & 0xFFFF);
	const int16_t diff = static_cast<int16_t>(seq - last16);
	const int64_t value = last_ + diff;
	if (value > last_)
		last_ = value;
	return value;
}

} // namespace camdrop
