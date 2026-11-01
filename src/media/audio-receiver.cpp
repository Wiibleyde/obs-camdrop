#include "media/audio-receiver.hpp"

#include <algorithm>

namespace camdrop {

bool AudioReceiver::accept(RtpPacket &packet)
{
	if (payload_type_ >= 0 && packet.payload_type != payload_type_)
		return false;
	if (packet.payload.empty())
		return false;
	const int64_t ext = unwrapper_.unwrap(packet.seq);
	packet.ext_seq = ext;

	if (std::find(recent_.begin(), recent_.end(), ext) != recent_.end())
		return false;
	recent_.push_back(ext);
	if (recent_.size() > 64)
		recent_.pop_front();

	if (highest_ >= 0 && ext > highest_ + 1 && ext - highest_ < 1000)
		lost_ += static_cast<uint64_t>(ext - highest_ - 1);
	if (ext > highest_)
		highest_ = ext;
	else if (lost_ > 0)
		lost_--; // a reordered packet filled a gap
	return true;
}

} // namespace camdrop
