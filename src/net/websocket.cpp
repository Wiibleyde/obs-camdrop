#include "net/websocket.hpp"

#include "util/strings.hpp"

#include <mbedtls/sha1.h>

namespace camdrop {

std::string websocket_accept_key(const std::string &client_key)
{
	static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	std::string input = trim(client_key) + guid;
	unsigned char hash[20];
	mbedtls_sha1(reinterpret_cast<const unsigned char *>(input.data()), input.size(), hash);
	return base64_encode(hash, sizeof(hash));
}

WsParse ws_parse_frame(const std::string &buffer, size_t &consumed, WsFrame &frame, size_t max_payload)
{
	if (buffer.size() < 2)
		return WsParse::NeedMore;
	const auto *b = reinterpret_cast<const uint8_t *>(buffer.data());
	frame.fin = (b[0] & 0x80) != 0;
	if (b[0] & 0x70)
		return WsParse::Error; // no extension negotiated
	frame.opcode = b[0] & 0x0F;
	const bool masked = (b[1] & 0x80) != 0;
	if (!masked)
		return WsParse::Error; // RFC 6455: client frames must be masked
	uint64_t len = b[1] & 0x7F;
	size_t pos = 2;
	if (len == 126) {
		if (buffer.size() < 4)
			return WsParse::NeedMore;
		len = (uint64_t(b[2]) << 8) | b[3];
		pos = 4;
	} else if (len == 127) {
		if (buffer.size() < 10)
			return WsParse::NeedMore;
		len = 0;
		for (int i = 0; i < 8; i++)
			len = (len << 8) | b[2 + i];
		pos = 10;
	}
	if (len > max_payload)
		return WsParse::Error;
	if ((frame.opcode & 0x8) && (len > 125 || !frame.fin))
		return WsParse::Error; // control frames: short, unfragmented
	if (buffer.size() < pos + 4)
		return WsParse::NeedMore;
	uint8_t mask[4] = {b[pos], b[pos + 1], b[pos + 2], b[pos + 3]};
	pos += 4;
	if (buffer.size() < pos + len)
		return WsParse::NeedMore;
	frame.payload.assign(buffer.data() + pos, static_cast<size_t>(len));
	for (size_t i = 0; i < frame.payload.size(); i++)
		frame.payload[i] = static_cast<char>(frame.payload[i] ^ mask[i & 3]);
	consumed = pos + static_cast<size_t>(len);
	return WsParse::Complete;
}

std::string ws_build_frame(uint8_t opcode, const std::string &payload, bool fin)
{
	std::string out;
	out.reserve(payload.size() + 10);
	out.push_back(static_cast<char>((fin ? 0x80 : 0) | (opcode & 0x0F)));
	const size_t len = payload.size();
	if (len < 126) {
		out.push_back(static_cast<char>(len));
	} else if (len <= 0xFFFF) {
		out.push_back(126);
		out.push_back(static_cast<char>((len >> 8) & 0xFF));
		out.push_back(static_cast<char>(len & 0xFF));
	} else {
		out.push_back(127);
		for (int i = 7; i >= 0; i--)
			out.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (8 * i)) & 0xFF));
	}
	out += payload;
	return out;
}

} // namespace camdrop
