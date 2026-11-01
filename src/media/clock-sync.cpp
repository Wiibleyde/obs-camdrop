#include "media/clock-sync.hpp"

#include "media/rtp.hpp"

#include <algorithm>

namespace camdrop {

void ClockSync::MinWindow::add(int64_t now_ns, int64_t value)
{
	if (current_ >= 0 && now_ns - starts_[current_] > kBuckets * kBucketNs)
		reset(); // no sample for a whole window: start over
	if (current_ < 0 || now_ns - starts_[current_] >= kBucketNs) {
		current_ = (current_ + 1) % kBuckets;
		starts_[current_] = now_ns;
		mins_[current_] = value;
		used_[current_] = true;
		return;
	}
	mins_[current_] = std::min(mins_[current_], value);
}

bool ClockSync::MinWindow::get(int64_t &out) const
{
	bool found = false;
	for (int i = 0; i < kBuckets; i++) {
		if (!used_[i])
			continue;
		out = found ? std::min(out, mins_[i]) : mins_[i];
		found = true;
	}
	return found;
}

void ClockSync::MinWindow::reset()
{
	used_.fill(false);
	current_ = -1;
}

void ClockSync::set_clock_rate(StreamKind kind, int rate)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (rate > 0)
		streams_[static_cast<int>(kind)].rate = rate;
}

void ClockSync::on_sender_report(StreamKind kind, uint64_t ntp, uint32_t rtp_timestamp)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Stream &s = streams_[static_cast<int>(kind)];
	s.has_sr = true;
	s.sr_ntp_ns = ntp_to_ns(ntp);
	s.sr_rtp = rtp_timestamp;
}

bool ClockSync::send_time_ns(const Stream &s, uint32_t rtp, int64_t &out, bool &ntp_domain) const
{
	if (s.has_sr) {
		const int32_t diff = static_cast<int32_t>(rtp - s.sr_rtp);
		out = s.sr_ntp_ns + static_cast<int64_t>(diff) * 1000000000LL / s.rate;
		ntp_domain = true;
		return true;
	}
	if (s.has_anchor) {
		const int32_t diff = static_cast<int32_t>(rtp - s.anchor_rtp);
		out = s.anchor_ns + static_cast<int64_t>(diff) * 1000000000LL / s.rate;
		ntp_domain = false;
		return true;
	}
	return false;
}

void ClockSync::observe(StreamKind kind, uint32_t rtp_timestamp, int64_t arrival_ns)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Stream &s = streams_[static_cast<int>(kind)];
	if (!s.has_sr && !s.has_anchor) {
		s.has_anchor = true;
		s.anchor_ns = arrival_ns;
		s.anchor_rtp = rtp_timestamp;
	}
	int64_t send = 0;
	bool ntp = false;
	if (!send_time_ns(s, rtp_timestamp, send, ntp))
		return;
	if (ntp)
		ntp_offset_.add(arrival_ns, arrival_ns - send);
	else
		s.local_offset.add(arrival_ns, arrival_ns - send);
}

bool ClockSync::base_playout_ns(StreamKind kind, uint32_t rtp_timestamp, int64_t &out)
{
	std::lock_guard<std::mutex> lock(mutex_);
	Stream &s = streams_[static_cast<int>(kind)];
	int64_t send = 0;
	bool ntp = false;
	if (!send_time_ns(s, rtp_timestamp, send, ntp))
		return false;
	int64_t offset = 0;
	if (ntp && ntp_offset_.get(offset)) {
		out = send + offset;
		return true;
	}
	if (!s.has_anchor)
		return false;
	// Sender report received but no arrival observed with it yet: use the local anchor.
	const int32_t diff = static_cast<int32_t>(rtp_timestamp - s.anchor_rtp);
	send = s.anchor_ns + static_cast<int64_t>(diff) * 1000000000LL / s.rate;
	if (!s.local_offset.get(offset))
		offset = 0;
	out = send + offset;
	return true;
}

bool ClockSync::synced() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return streams_[0].has_sr && streams_[1].has_sr;
}

} // namespace camdrop
