#include "net/rate-limiter.hpp"

#include <algorithm>
#include <chrono>

namespace camdrop {

namespace {
constexpr int kMaxFailuresPerIp = 5;
constexpr int64_t kWindowMs = 60 * 1000;
constexpr int64_t kBaseBanMs = 60 * 1000;
constexpr int64_t kMaxBanMs = 30 * 60 * 1000;
constexpr int kMaxGlobalFailures = 30;
} // namespace

int64_t RateLimiter::now_ms()
{
	using namespace std::chrono;
	return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void RateLimiter::cleanup(int64_t now)
{
	if (entries_.size() < 256)
		return;
	for (auto it = entries_.begin(); it != entries_.end();) {
		if (it->second.blocked_until < now && now - it->second.window_start > kWindowMs)
			it = entries_.erase(it);
		else
			++it;
	}
}

bool RateLimiter::allowed(const std::string &ip)
{
	return retry_after(ip) == 0;
}

int RateLimiter::retry_after(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const int64_t now = now_ms();
	int64_t until = global_blocked_until_;
	auto it = entries_.find(ip);
	if (it != entries_.end())
		until = std::max(until, it->second.blocked_until);
	if (until <= now)
		return 0;
	return static_cast<int>((until - now + 999) / 1000);
}

void RateLimiter::record_failure(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(mutex_);
	const int64_t now = now_ms();
	cleanup(now);

	Entry &e = entries_[ip];
	if (now - e.window_start > kWindowMs) {
		e.window_start = now;
		e.failures = 0;
	}
	if (++e.failures >= kMaxFailuresPerIp) {
		const int64_t ban = std::min(kMaxBanMs, kBaseBanMs << std::min(e.strikes, 5));
		e.blocked_until = now + ban;
		e.strikes++;
		e.failures = 0;
	}

	if (now - global_window_start_ > kWindowMs) {
		global_window_start_ = now;
		global_failures_ = 0;
	}
	if (++global_failures_ >= kMaxGlobalFailures) {
		global_blocked_until_ = now + kBaseBanMs;
		global_failures_ = 0;
	}
}

void RateLimiter::record_success(const std::string &ip)
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = entries_.find(ip);
	if (it != entries_.end()) {
		it->second.failures = 0;
		it->second.strikes = 0;
	}
}

} // namespace camdrop
