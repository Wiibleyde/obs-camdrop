#include "media/receiver-handler.hpp"

#include "util/random.hpp"

#include <util/platform.h>

#include <algorithm>
#include <cstdlib>

namespace camdrop {

ReceiverHandler::ReceiverHandler(PacketCallback on_packet, SenderReportCallback on_sr)
	: on_packet_(std::move(on_packet)),
	  on_sr_(std::move(on_sr))
{
	random_bytes(reinterpret_cast<uint8_t *>(&own_ssrc_), sizeof(own_ssrc_));
	own_ssrc_ |= 1;

	// Test hook: drop a percentage of incoming RTP packets to exercise NACK and recovery.
	if (const char *loss = getenv("CAMDROP_SIMULATED_LOSS"))
		simulated_loss_pct_ = std::clamp(atoi(loss), 0, 50);
}

void ReceiverHandler::incoming(rtc::message_vector &messages, const rtc::message_callback &send)
{
	const int64_t now = static_cast<int64_t>(os_gettime_ns());

	rtc::message_vector for_base;
	for_base.reserve(messages.size());

	for (const auto &message : messages) {
		if (!message || message->empty())
			continue;
		const auto *data = reinterpret_cast<const uint8_t *>(message->data());
		const size_t len = message->size();

		if (is_rtcp(data, len)) {
			if (on_sr_) {
				const uint32_t ssrc = media_ssrc_.load();
				for (const auto &sr : parse_sender_reports(data, len)) {
					// RTCP of a bundled connection may reach every track: keep our stream's.
					if (ssrc == 0 || sr.ssrc == ssrc)
						on_sr_(sr, now);
				}
			}
			for_base.push_back(message);
			continue;
		}

		const bool rtx = len >= 2 &&
				 std::find(rtx_pts_.begin(), rtx_pts_.end(), data[1] & 0x7F) != rtx_pts_.end();
		if (!rtx)
			for_base.push_back(message);

		if (simulated_loss_pct_ > 0 && random_uniform(100) < static_cast<uint32_t>(simulated_loss_pct_))
			continue;

		RtpPacket packet;
		if (!parse_rtp(data, len, packet))
			continue;
		packet.arrival_ns = now;
		bytes_ += len;
		packets_++;

		if (!rtx)
			media_ssrc_ = packet.ssrc;
		const uint32_t ssrc = media_ssrc_.load();
		Feedback fb = on_packet_ ? on_packet_(std::move(packet)) : Feedback{};
		if (!send || ssrc == 0)
			continue;
		if (!fb.nacks.empty()) {
			auto rtcp = build_nack(own_ssrc_, ssrc, fb.nacks);
			auto bytes = reinterpret_cast<const std::byte *>(rtcp.data());
			send(rtc::make_message(bytes, bytes + rtcp.size(), rtc::Message::Control));
			nacks_ += fb.nacks.size();
		}
		if (fb.keyframe) {
			auto rtcp = build_pli(own_ssrc_, ssrc);
			auto bytes = reinterpret_cast<const std::byte *>(rtcp.data());
			send(rtc::make_message(bytes, bytes + rtcp.size(), rtc::Message::Control));
			plis_++;
		}
	}

	// Receiver reports, REMB and PLI bookkeeping of the base class.
	rtc::RtcpReceivingSession::incoming(for_base, send);

	// Packets were consumed above; nothing needs to reach the track's message queue.
	messages.clear();
}

} // namespace camdrop
