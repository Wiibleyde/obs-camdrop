#include "core/camera-session.hpp"

#include "core/channel.hpp"
#include "core/sdp-utils.hpp"
#include "media/media-pipeline.hpp"
#include "plugin-support.h"
#include "util/strings.hpp"

#include <obs-module.h>
#include <util/platform.h>

#include <algorithm>

namespace camdrop {

namespace {

constexpr int64_t kSec = 1000000000LL;
constexpr int64_t kConnectTimeoutNs = 30 * kSec;

int64_t now_ns()
{
	return static_cast<int64_t>(os_gettime_ns());
}

std::string lower(std::string s)
{
	return to_lower(std::move(s));
}

} // namespace

CameraSession::CameraSession(std::string id, std::string name, std::string client_id, std::string ip,
			     std::string user_agent, std::shared_ptr<WsConnection> ws, std::shared_ptr<Channel> channel,
			     SessionConfig config)
	: id_(std::move(id)),
	  name_(std::move(name)),
	  client_id_(std::move(client_id)),
	  ip_(std::move(ip)),
	  user_agent_(std::move(user_agent)),
	  ws_(std::move(ws)),
	  channel_(std::move(channel)),
	  config_(std::move(config)),
	  created_ns_(now_ns())
{
	state_since_ns_ = created_ns_;
}

CameraSession::~CameraSession()
{
	teardown_peer();
}

void CameraSession::send(const json &message)
{
	if (ws_ && !closed_)
		ws_->send_text(message.dump());
}

void CameraSession::set_state(const std::string &state)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (state_ == state)
		return;
	state_ = state;
	state_since_ns_ = now_ns();
	if (state == "live")
		live_since_ns_ = state_since_ns_;
}

void CameraSession::handle_message(const json &message)
{
	if (closed_)
		return;
	const std::string type = json_get<std::string>(message, "type", "");
	if (type == "offer") {
		handle_offer(json_get<std::string>(message, "sdp", ""));
	} else if (type == "candidate") {
		handle_candidate(message);
	} else if (type == "telemetry") {
		handle_telemetry(message);
	} else if (type == "visibility") {
		std::lock_guard<std::mutex> lock(mutex_);
		hidden_ = json_get<bool>(message, "hidden", false);
		telemetry_["visible"] = !hidden_;
	} else if (type == "caps") {
		handle_capabilities(message);
	} else if (type == "keyframe") {
		request_keyframe();
	} else if (type == "ping") {
		send({{"type", "pong"}, {"t", json_get<double>(message, "t", 0)}});
	} else if (type == "bye") {
		close("client_left", false);
	}
}

void CameraSession::handle_telemetry(const json &message)
{
	static const char *numeric[] = {"fps",     "width",   "height",     "kbps",          "rtt",       "jitter",
					"lossPct", "battery", "targetKbps", "framesEncoded", "cpuLimited"};
	static const char *strings[] = {"qualityLimitation", "pressure",    "codec", "facing", "camera",
					"encoder",           "orientation", "video"};
	static const char *booleans[] = {"charging", "visible", "wakeLock", "economy", "muted"};

	json t = json::object();
	for (const char *k : numeric) {
		auto it = message.find(k);
		if (it != message.end() && it->is_number())
			t[k] = *it;
	}
	for (const char *k : strings) {
		auto it = message.find(k);
		if (it != message.end() && it->is_string())
			t[k] = it->get<std::string>().substr(0, 64);
	}
	for (const char *k : booleans) {
		auto it = message.find(k);
		if (it != message.end() && it->is_boolean())
			t[k] = *it;
	}

	// The browser measures the round-trip time from our receiver reports; libdatachannel has no
	// RTT estimate without a data channel. It drives the NACK retry interval.
	if (t.contains("rtt") && t["rtt"].get<double>() > 0)
		rtt_ms_ = static_cast<int>(std::clamp(t["rtt"].get<double>(), 1.0, 2000.0));

	std::lock_guard<std::mutex> lock(mutex_);
	telemetry_ = std::move(t);
	telemetry_ns_ = now_ns();
	if (telemetry_.contains("visible"))
		hidden_ = !telemetry_["visible"].get<bool>();
}

namespace {

// Copies a JSON value from the phone keeping only bounded, simple data.
json sanitize_value(const json &v, int depth)
{
	if (v.is_boolean() || v.is_number())
		return v;
	if (v.is_string())
		return v.get<std::string>().substr(0, 128);
	if (depth <= 0)
		return nullptr;
	if (v.is_array()) {
		json out = json::array();
		for (const auto &item : v) {
			if (out.size() >= 16)
				break;
			out.push_back(sanitize_value(item, depth - 1));
		}
		return out;
	}
	if (v.is_object()) {
		json out = json::object();
		for (auto it = v.begin(); it != v.end(); ++it) {
			if (out.size() >= 24)
				break;
			out[it.key().substr(0, 32)] = sanitize_value(it.value(), depth - 1);
		}
		return out;
	}
	return nullptr;
}

} // namespace

void CameraSession::handle_capabilities(const json &message)
{
	auto it = message.find("caps");
	if (it == message.end() || !it->is_object())
		return;
	json caps = sanitize_value(*it, 3);
	const std::string mode = json_get<std::string>(caps, "video", "camera");
	bool clear = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		caps_ = std::move(caps);
		clear = mode == "none" && video_mode_ != "none";
		video_mode_ = mode;
	}
	// Audio only: do not leave the last picture frozen in OBS.
	if (clear)
		channel_->clear();
}

json CameraSession::capabilities() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return caps_;
}

void CameraSession::send_control(const json &set)
{
	send({{"type", "control"}, {"set", set}});
}

void CameraSession::set_tally(int tally)
{
	if (tally_.exchange(tally) == tally)
		return;
	send({{"type", "tally"}, {"state", Tally::name(tally)}});
}

void CameraSession::handle_offer(const std::string &sdp)
{
	if (sdp.empty() || sdp.size() > 64 * 1024)
		return;

	// A new offer replaces the previous peer connection (renegotiation after a network change).
	teardown_peer();

	rtc::Configuration cfg;
	cfg.portRangeBegin = static_cast<uint16_t>(config_.udp_port_min);
	cfg.portRangeEnd = static_cast<uint16_t>(config_.udp_port_max);
	cfg.disableAutoNegotiation = false;

	std::shared_ptr<rtc::PeerConnection> pc;
	try {
		pc = std::make_shared<rtc::PeerConnection>(cfg);
	} catch (const std::exception &e) {
		obs_log(LOG_ERROR, "[%s] Cannot create the peer connection: %s", name_.c_str(), e.what());
		send({{"type", "error"}, {"code", "webrtc"}});
		return;
	}

	auto pipeline = std::make_shared<MediaPipeline>(channel_, config_.hw_decode);
	std::weak_ptr<CameraSession> weak = weak_from_this();
	pipeline->set_keyframe_requester([weak] {
		if (auto self = weak.lock())
			self->request_keyframe();
	});

	{
		std::lock_guard<std::mutex> lock(video_rx_mutex_);
		video_rx_ = VideoReceiver();
	}
	{
		std::lock_guard<std::mutex> lock(audio_rx_mutex_);
		audio_rx_ = AudioReceiver();
	}

	pc->onTrack([weak](std::shared_ptr<rtc::Track> track) {
		if (auto self = weak.lock())
			self->setup_track(track);
	});

	const int max_kbps = config_.max_bitrate_kbps;
	const bool opus_10ms = config_.opus_10ms;
	pc->onLocalDescription([weak, max_kbps, opus_10ms](rtc::Description description) {
		auto self = weak.lock();
		if (!self)
			return;
		const std::string answer = tune_answer(std::string(description), max_kbps, opus_10ms);
		self->send({{"type", "answer"}, {"sdp", answer}});
	});

	pc->onLocalCandidate([weak](rtc::Candidate candidate) {
		if (auto self = weak.lock())
			self->send(
				{{"type", "candidate"},
				 {"candidate", {{"candidate", candidate.candidate()}, {"sdpMid", candidate.mid()}}}});
	});

	pc->onStateChange([weak, max_kbps](rtc::PeerConnection::State state) {
		auto self = weak.lock();
		if (!self || self->closed_)
			return;
		switch (state) {
		case rtc::PeerConnection::State::Connecting:
			self->set_state("connecting");
			break;
		case rtc::PeerConnection::State::Connected: {
			self->set_state("live");
			obs_log(LOG_INFO, "[%s] connected (%s)", self->name_.c_str(), self->ip_.c_str());
			std::shared_ptr<rtc::Track> video;
			{
				std::lock_guard<std::mutex> lock(self->mutex_);
				video = self->video_track_;
			}
			if (video) {
				try {
					video->requestBitrate(static_cast<unsigned int>(max_kbps) * 1000u);
					video->requestKeyframe();
				} catch (...) {
				}
			}
			self->channel_->set_live(true, now_ns());
			self->send({{"type", "peer-state"}, {"state", "connected"}});
			break;
		}
		case rtc::PeerConnection::State::Disconnected:
			self->set_state("unstable");
			self->send({{"type", "peer-state"}, {"state", "disconnected"}});
			break;
		case rtc::PeerConnection::State::Failed:
			self->set_state("failed");
			obs_log(LOG_WARNING, "[%s] connection failed", self->name_.c_str());
			self->channel_->set_live(false, now_ns());
			self->send({{"type", "peer-state"}, {"state", "failed"}});
			break;
		case rtc::PeerConnection::State::Closed:
			break;
		default:
			break;
		}
	});

	{
		std::lock_guard<std::mutex> lock(mutex_);
		pc_ = pc;
		pipeline_ = pipeline;
		negotiations_++;
		state_ = "connecting";
		state_since_ns_ = now_ns();
	}
	pipeline->start();

	try {
		const std::string filtered = filter_offer(sdp, config_.preferred_codec);
		pc->setRemoteDescription(rtc::Description(filtered, rtc::Description::Type::Offer));
	} catch (const std::exception &e) {
		obs_log(LOG_WARNING, "[%s] Invalid offer: %s", name_.c_str(), e.what());
		send({{"type", "error"}, {"code", "bad_offer"}});
		teardown_peer();
	}
}

void CameraSession::handle_candidate(const json &message)
{
	std::shared_ptr<rtc::PeerConnection> pc;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pc = pc_;
	}
	if (!pc)
		return;
	auto it = message.find("candidate");
	if (it == message.end() || !it->is_object())
		return;
	const std::string cand = json_get<std::string>(*it, "candidate", "");
	const std::string mid = json_get<std::string>(*it, "sdpMid", "0");
	if (cand.empty())
		return; // end of candidates
	try {
		pc->addRemoteCandidate(rtc::Candidate(cand, mid));
	} catch (const std::exception &e) {
		obs_log(LOG_DEBUG, "[%s] Ignored candidate: %s", name_.c_str(), e.what());
	}
}

void CameraSession::setup_track(const std::shared_ptr<rtc::Track> &track)
{
	const rtc::Description::Media desc = track->description();
	const std::string kind = desc.type();
	std::weak_ptr<CameraSession> weak = weak_from_this();

	std::shared_ptr<MediaPipeline> pipeline;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pipeline = pipeline_;
	}
	if (!pipeline)
		return;
	std::weak_ptr<MediaPipeline> weak_pipeline = pipeline;

	if (kind == "video") {
		std::vector<int> rtx_pts;
		{
			std::lock_guard<std::mutex> lock(video_rx_mutex_);
			rtx_apt_.clear();
			for (int pt : desc.payloadTypes()) {
				const auto *map = desc.rtpMap(pt);
				if (!map)
					continue;
				const std::string fmt = lower(map->format);
				if (fmt == "h264") {
					video_rx_.set_payload_codec(pt, VideoCodec::H264);
				} else if (fmt == "h265") {
					video_rx_.set_payload_codec(pt, VideoCodec::H265);
				} else if (fmt == "vp8") {
					video_rx_.set_payload_codec(pt, VideoCodec::VP8);
				} else if (fmt == "rtx") {
					for (const auto &f : map->fmtps) {
						const size_t pos = f.find("apt=");
						if (pos != std::string::npos) {
							rtx_apt_[pt] = atoi(f.c_str() + pos + 4);
							rtx_pts.push_back(pt);
						}
					}
				}
			}
		}
		auto handler = std::make_shared<ReceiverHandler>(
			[weak](RtpPacket &&packet) {
				auto self = weak.lock();
				return self ? self->on_video_packet(std::move(packet)) : ReceiverHandler::Feedback{};
			},
			[weak_pipeline](const SenderReport &sr, int64_t) {
				if (auto p = weak_pipeline.lock())
					p->clock().on_sender_report(StreamKind::Video, sr.ntp, sr.rtp_timestamp);
			});
		handler->set_rtx_payload_types(rtx_pts);
		track->setMediaHandler(handler);
		track->onMessage([](rtc::message_variant) {});
		std::lock_guard<std::mutex> lock(mutex_);
		video_track_ = track;
		video_handler_ = handler;
	} else if (kind == "audio") {
		{
			std::lock_guard<std::mutex> lock(audio_rx_mutex_);
			for (int pt : desc.payloadTypes()) {
				const auto *map = desc.rtpMap(pt);
				if (map && lower(map->format) == "opus")
					audio_rx_.set_payload_type(pt);
			}
		}
		auto handler = std::make_shared<ReceiverHandler>(
			[weak](RtpPacket &&packet) {
				auto self = weak.lock();
				return self ? self->on_audio_packet(std::move(packet)) : ReceiverHandler::Feedback{};
			},
			[weak_pipeline](const SenderReport &sr, int64_t) {
				if (auto p = weak_pipeline.lock())
					p->clock().on_sender_report(StreamKind::Audio, sr.ntp, sr.rtp_timestamp);
			});
		track->setMediaHandler(handler);
		track->onMessage([](rtc::message_variant) {});
		std::lock_guard<std::mutex> lock(mutex_);
		audio_track_ = track;
		audio_handler_ = handler;
	}
}

ReceiverHandler::Feedback CameraSession::on_video_packet(RtpPacket &&packet)
{
	ReceiverHandler::Feedback fb;
	VideoReceiver::Result result;
	const int buffer_ms = channel_->buffer_ms();
	{
		std::lock_guard<std::mutex> lock(video_rx_mutex_);
		auto rtx = rtx_apt_.find(packet.payload_type);
		if (rtx != rtx_apt_.end()) {
			// RFC 4588: original sequence number in the first two bytes, original payload type in apt.
			// Payload-less RTX packets are bandwidth probes.
			if (packet.payload.size() <= 2)
				return fb;
			packet.seq = static_cast<uint16_t>((packet.payload[0] << 8) | packet.payload[1]);
			packet.payload.erase(packet.payload.begin(), packet.payload.begin() + 2);
			packet.payload_type = static_cast<uint8_t>(rtx->second);
		}
		// Wait for a retransmission as long as the buffer allows it (and at least a few RTTs).
		video_rx_.set_max_wait_ms(std::max(buffer_ms + 40, 4 * rtt_ms_.load() + 30));
		video_rx_.set_rtt_ms(rtt_ms_.load());
		result = video_rx_.insert(std::move(packet));
	}
	std::shared_ptr<MediaPipeline> pipeline;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pipeline = pipeline_;
	}
	if (pipeline) {
		for (auto &frame : result.frames)
			pipeline->push_video_frame(std::move(frame));
	}
	fb.nacks = std::move(result.nacks);
	fb.keyframe = result.request_keyframe;
	return fb;
}

ReceiverHandler::Feedback CameraSession::on_audio_packet(RtpPacket &&packet)
{
	bool accepted;
	{
		std::lock_guard<std::mutex> lock(audio_rx_mutex_);
		accepted = audio_rx_.accept(packet);
	}
	if (accepted) {
		std::shared_ptr<MediaPipeline> pipeline;
		{
			std::lock_guard<std::mutex> lock(mutex_);
			pipeline = pipeline_;
		}
		if (pipeline)
			pipeline->push_audio_packet(std::move(packet));
	}
	return {};
}

void CameraSession::request_keyframe()
{
	std::shared_ptr<rtc::Track> video;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		video = video_track_;
	}
	if (!video)
		return;
	try {
		video->requestKeyframe();
	} catch (...) {
	}
}

void CameraSession::teardown_peer()
{
	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<MediaPipeline> pipeline;
	std::shared_ptr<rtc::Track> video, audio;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pc = std::move(pc_);
		pipeline = std::move(pipeline_);
		video = std::move(video_track_);
		audio = std::move(audio_track_);
		video_handler_.reset();
		audio_handler_.reset();
	}
	if (pc) {
		try {
			pc->resetCallbacks();
			pc->close();
		} catch (...) {
		}
	}
	if (pipeline)
		pipeline->stop();
}

void CameraSession::close(const std::string &reason, bool notify_client)
{
	if (closed_.exchange(true))
		return;
	obs_log(LOG_INFO, "[%s] session closed (%s)", name_.c_str(), reason.c_str());
	if (notify_client && ws_)
		ws_->send_text(json{{"type", "bye"}, {"reason", reason}}.dump());
	teardown_peer();
	set_state("closed");
	channel_->set_live(false, now_ns());
	if (ws_)
		ws_->close(1000, reason);
}

void CameraSession::tick(int64_t now)
{
	if (closed_)
		return;

	std::shared_ptr<rtc::PeerConnection> pc;
	std::shared_ptr<MediaPipeline> pipeline;
	std::shared_ptr<ReceiverHandler> vh, ah;
	std::string state;
	int64_t state_since;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		pc = pc_;
		pipeline = pipeline_;
		vh = video_handler_;
		ah = audio_handler_;
		state = state_;
		state_since = state_since_ns_;
	}

	if (pc) {
		try {
			if (auto rtt = pc->rtt())
				rtt_ms_ = static_cast<int>(std::clamp<long long>(rtt->count(), 1, 2000));
		} catch (...) {
		}
	}

	// Packet loss over the last second.
	uint64_t packets = (vh ? vh->packets_received() : 0) + (ah ? ah->packets_received() : 0);
	uint64_t lost;
	{
		std::lock_guard<std::mutex> lock(video_rx_mutex_);
		lost = video_rx_.stats().lost_packets;
	}
	{
		std::lock_guard<std::mutex> lock(audio_rx_mutex_);
		lost += audio_rx_.lost_packets();
	}
	if (packets >= last_packets_ && lost >= last_lost_) {
		const double dp = static_cast<double>(packets - last_packets_);
		const double dl = static_cast<double>(lost - last_lost_);
		const double pct = dp + dl > 0 ? 100.0 * dl / (dp + dl) : 0;
		loss_pct_ = loss_pct_ * 0.6 + pct * 0.4;
	}
	last_packets_ = packets;
	last_lost_ = lost;

	if (state == "signaling" && !pc && now - state_since > kConnectTimeoutNs) {
		// Joined but never sent an offer: free the slot.
		close("timeout", true);
		return;
	}
	if ((state == "connecting" || state == "signaling") && now - state_since > kConnectTimeoutNs && pc) {
		obs_log(LOG_WARNING, "[%s] WebRTC connection timed out (firewall or Wi-Fi isolation?)", name_.c_str());
		set_state("failed");
		send({{"type", "peer-state"}, {"state", "failed"}});
	}

	// Feedback for the phone screen, every 2 seconds.
	if (pipeline && state == "live" && now - last_stats_sent_ns_ >= 2 * kSec) {
		last_stats_sent_ns_ = now;
		PipelineStats st = pipeline->stats();
		send({{"type", "stats"},
		      {"fps", st.fps},
		      {"kbps", st.video_kbps + st.audio_kbps},
		      {"width", st.width},
		      {"height", st.height},
		      {"bufferMs", st.buffer_ms},
		      {"delayMs", st.delay_ms},
		      {"rttMs", rtt_ms_.load()},
		      {"lossPct", loss_pct_},
		      {"codec", st.codec},
		      {"watchers", channel_->sink_count()}});
	}
}

json CameraSession::status() const
{
	json j;
	std::shared_ptr<MediaPipeline> pipeline;
	std::shared_ptr<ReceiverHandler> vh;
	const int64_t now = now_ns();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		j["id"] = id_;
		j["name"] = name_;
		j["ip"] = ip_;
		j["userAgent"] = user_agent_;
		j["state"] = state_;
		j["stateFor"] = static_cast<double>(now - state_since_ns_) / 1e9;
		j["uptime"] = live_since_ns_ ? static_cast<double>(now - live_since_ns_) / 1e9 : 0.0;
		j["negotiations"] = negotiations_;
		j["hidden"] = hidden_;
		j["telemetry"] = telemetry_;
		j["telemetryAge"] = telemetry_ns_ ? static_cast<double>(now - telemetry_ns_) / 1e9 : -1.0;
		j["caps"] = caps_;
		j["video"] = video_mode_;
		pipeline = pipeline_;
		vh = video_handler_;
	}
	j["rttMs"] = rtt_ms_.load();
	j["lossPct"] = loss_pct_;
	j["watchers"] = channel_->sink_count();
	j["tally"] = Tally::name(channel_->tally());
	if (pipeline) {
		PipelineStats st = pipeline->stats();
		j["fps"] = st.fps;
		j["videoKbps"] = st.video_kbps;
		j["audioKbps"] = st.audio_kbps;
		j["width"] = st.width;
		j["height"] = st.height;
		j["codec"] = st.codec;
		j["hardware"] = st.hardware;
		j["framesDecoded"] = st.frames_decoded;
		j["framesSkipped"] = st.frames_skipped;
		j["decodeErrors"] = st.decode_errors;
		j["audioDropped"] = st.audio_dropped;
		j["delayMs"] = st.delay_ms;
		j["bufferMs"] = st.buffer_ms;
		j["avSynced"] = st.av_synced;
	}
	{
		std::lock_guard<std::mutex> lock(video_rx_mutex_);
		const auto s = video_rx_.stats();
		j["lostPackets"] = s.lost_packets;
		j["recoveredPackets"] = s.recovered_packets;
		j["droppedFrames"] = s.dropped_frames;
		j["keyframes"] = s.keyframes;
	}
	if (vh) {
		j["nacks"] = vh->nacks_sent();
		j["plis"] = vh->plis_sent();
	}
	return j;
}

} // namespace camdrop
