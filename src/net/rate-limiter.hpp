#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace camdrop {

// Limits access code guesses: per IP (5 failures per minute, then an exponential ban) and globally.
class RateLimiter {
public:
	// Returns false while `ip` (or everybody, after too many global failures) is blocked.
	bool allowed(const std::string &ip);
	void record_failure(const std::string &ip);
	void record_success(const std::string &ip);
	// Seconds before `ip` can retry (0 when allowed).
	int retry_after(const std::string &ip);

private:
	struct Entry {
		int failures = 0;
		int64_t window_start = 0;
		int64_t blocked_until = 0;
		int strikes = 0;
	};

	static int64_t now_ms();
	void cleanup(int64_t now);

	std::mutex mutex_;
	std::map<std::string, Entry> entries_;
	int global_failures_ = 0;
	int64_t global_window_start_ = 0;
	int64_t global_blocked_until_ = 0;
};

} // namespace camdrop
