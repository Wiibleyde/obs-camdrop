#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace camdrop {

enum WsOpcode : uint8_t {
	WS_CONTINUATION = 0x0,
	WS_TEXT = 0x1,
	WS_BINARY = 0x2,
	WS_CLOSE = 0x8,
	WS_PING = 0x9,
	WS_PONG = 0xA,
};

struct WsFrame {
	bool fin = true;
	uint8_t opcode = WS_TEXT;
	std::string payload;
};

std::string websocket_accept_key(const std::string &client_key);

enum class WsParse { Complete, NeedMore, Error };

// Parses one client frame (clients must mask their frames).
WsParse ws_parse_frame(const std::string &buffer, size_t &consumed, WsFrame &frame, size_t max_payload);

// Builds one unmasked server frame.
std::string ws_build_frame(uint8_t opcode, const std::string &payload, bool fin = true);

// Interface given to application code for an open WebSocket.
class WsConnection {
public:
	virtual ~WsConnection() = default;
	// Thread-safe: queues a text message, sent by the connection thread.
	virtual void send_text(const std::string &message) = 0;
	// Thread-safe: closes the connection after the queued messages are sent.
	virtual void close(uint16_t code = 1000, const std::string &reason = {}) = 0;
	virtual const std::string &remote_ip() const = 0;
};

class WsHandler {
public:
	virtual ~WsHandler() = default;
	virtual void on_open(const std::shared_ptr<WsConnection> &connection) { (void)connection; }
	virtual void on_message(const std::string &message) = 0;
	virtual void on_close() {}
};

} // namespace camdrop
