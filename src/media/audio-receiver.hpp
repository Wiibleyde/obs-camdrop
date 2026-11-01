#pragma once

#include "media/rtp.hpp"

#include <cstdint>
#include <deque>

namespace camdrop {

// Opus packets need no reassembly: this only unwraps sequence numbers and drops duplicates.
// Ordering and timing are handled by the playout stage of the pipeline.
class AudioReceiver {
public:
	void set_payload_type(int pt) { payload_type_ = pt; }
	// Returns false if the packet must be ignored.
	bool accept(RtpPacket &packet);

	uint64_t lost_packets() const { return lost_; }

private:
	int payload_type_ = -1;
	SeqUnwrapper unwrapper_;
	std::deque<int64_t> recent_;
	int64_t highest_ = -1;
	uint64_t lost_ = 0;
};

} // namespace camdrop
