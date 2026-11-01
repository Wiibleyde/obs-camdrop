#pragma once

#include "media/rtp.hpp"

#include <rtc/rtc.hpp>

#include <atomic>
#include <functional>
#include <vector>

namespace camdrop {

// libdatachannel media handler placed on each received track. It keeps the standard receiver
// behaviour (receiver reports, PLI, REMB) and adds what the plugin needs: raw RTP packets, sender
// reports for audio/video sync, and NACK requests for lost video packets.
class ReceiverHandler : public rtc::RtcpReceivingSession {
public:
	struct Feedback {
		std::vector<uint16_t> nacks; // sequence numbers to ask again right away
		bool keyframe = false;       // send a picture loss indication
	};
	// Called for each RTP packet.
	using PacketCallback = std::function<Feedback(RtpPacket &&packet)>;
	using SenderReportCallback = std::function<void(const SenderReport &report, int64_t arrival_ns)>;

	ReceiverHandler(PacketCallback on_packet, SenderReportCallback on_sr);

	void incoming(rtc::message_vector &messages, const rtc::message_callback &send) override;

	// Retransmission payload types (RFC 4588): their packets come with another SSRC.
	void set_rtx_payload_types(std::vector<int> pts) { rtx_pts_ = std::move(pts); }

	uint64_t bytes_received() const { return bytes_.load(); }
	uint64_t packets_received() const { return packets_.load(); }
	uint64_t nacks_sent() const { return nacks_.load(); }
	uint64_t plis_sent() const { return plis_.load(); }
	uint32_t media_ssrc() const { return media_ssrc_.load(); }

private:
	PacketCallback on_packet_;
	SenderReportCallback on_sr_;
	std::atomic<uint64_t> bytes_{0};
	std::atomic<uint64_t> packets_{0};
	std::atomic<uint64_t> nacks_{0};
	std::atomic<uint64_t> plis_{0};
	std::atomic<uint32_t> media_ssrc_{0};
	uint32_t own_ssrc_;
	std::vector<int> rtx_pts_;
	int simulated_loss_pct_ = 0; // CAMDROP_SIMULATED_LOSS (testing only)
};

} // namespace camdrop
