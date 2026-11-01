#pragma once

#include "media/audio-receiver.hpp"
#include "media/receiver-handler.hpp"
#include "media/video-receiver.hpp"
#include "net/websocket.hpp"
#include "util/json.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace camdrop {

class Channel;
class MediaPipeline;

struct SessionConfig {
	int udp_port_min = 51000;
	int udp_port_max = 51100;
	int max_bitrate_kbps = 5000;
	bool opus_10ms = false;
	bool hw_decode = true;
	std::string preferred_codec = "h264";
};

// One connected camera (phone or browser): WebSocket signaling, WebRTC peer connection, receivers
// and media pipeline. Renegotiations (new offer after an ICE failure) rebuild the peer connection.
class CameraSession : public std::enable_shared_from_this<CameraSession> {
public:
	CameraSession(std::string id, std::string name, std::string client_id, std::string ip, std::string user_agent,
		      std::shared_ptr<WsConnection> ws, std::shared_ptr<Channel> channel, SessionConfig config);
	~CameraSession();

	const std::string &id() const { return id_; }
	const std::string &name() const { return name_; }
	const std::string &client_id() const { return client_id_; }
	const std::string &ip() const { return ip_; }
	std::shared_ptr<Channel> channel() const { return channel_; }

	void handle_message(const json &message);
	void send(const json &message);
	void close(const std::string &reason, bool notify_client);
	bool closed() const { return closed_; }

	// Called once per second by the client manager.
	void tick(int64_t now_ns);
	json status() const;
	void request_keyframe();

private:
	void handle_offer(const std::string &sdp);
	void handle_candidate(const json &message);
	void handle_telemetry(const json &message);
	void setup_track(const std::shared_ptr<rtc::Track> &track);
	void teardown_peer();
	void set_state(const std::string &state);
	ReceiverHandler::Feedback on_video_packet(RtpPacket &&packet);
	ReceiverHandler::Feedback on_audio_packet(RtpPacket &&packet);

	const std::string id_;
	const std::string name_;
	const std::string client_id_;
	const std::string ip_;
	const std::string user_agent_;
	std::shared_ptr<WsConnection> ws_;
	std::shared_ptr<Channel> channel_;
	const SessionConfig config_;
	const int64_t created_ns_;

	mutable std::mutex mutex_;
	std::shared_ptr<rtc::PeerConnection> pc_;
	std::shared_ptr<rtc::Track> video_track_;
	std::shared_ptr<rtc::Track> audio_track_;
	std::shared_ptr<ReceiverHandler> video_handler_;
	std::shared_ptr<ReceiverHandler> audio_handler_;
	std::shared_ptr<MediaPipeline> pipeline_;
	std::string state_ = "signaling";
	int64_t state_since_ns_ = 0;
	int64_t live_since_ns_ = 0;
	int negotiations_ = 0;
	json telemetry_ = json::object();
	int64_t telemetry_ns_ = 0;
	bool hidden_ = false;

	mutable std::mutex video_rx_mutex_;
	VideoReceiver video_rx_;
	std::map<int, int> rtx_apt_; // RTX payload type -> original payload type
	mutable std::mutex audio_rx_mutex_;
	AudioReceiver audio_rx_;

	std::atomic<int> rtt_ms_{10};
	std::atomic<bool> closed_{false};

	// Loss over the last ticks
	uint64_t last_packets_ = 0;
	uint64_t last_lost_ = 0;
	double loss_pct_ = 0;
	int64_t last_stats_sent_ns_ = 0;
};

} // namespace camdrop
