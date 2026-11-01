#pragma once

#include <obs.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace camdrop {

// Receiver of a camera's frames (an OBS source).
class ChannelSink {
public:
	virtual ~ChannelSink() = default;
	virtual void channel_video(const obs_source_frame *frame) = 0;
	virtual void channel_audio(const obs_source_audio *audio) = 0;
	virtual void channel_clear() = 0;
	virtual int sink_buffer_ms() const = 0;
	// Tally of the OBS source: Tally::Program when shown in the program output, Tally::Preview when
	// shown elsewhere (studio mode preview, projector, properties window).
	virtual int sink_tally() const = 0;
};

namespace Tally {
constexpr int Off = 0;
constexpr int Preview = 1;
constexpr int Program = 2;
const char *name(int tally);
} // namespace Tally

// A named camera slot. Sources subscribe by name; whichever session currently uses that name
// (first connection, reconnection, another device with the same name later) feeds it. This is
// what makes sources rebind automatically after a reconnection.
class Channel {
public:
	explicit Channel(std::string name) : name_(std::move(name)) {}

	const std::string &name() const { return name_; }

	void add_sink(ChannelSink *sink);
	void remove_sink(ChannelSink *sink);
	bool has_sinks() const;
	size_t sink_count() const;

	void output_video(const obs_source_frame *frame);
	void output_audio(const obs_source_audio *audio);
	void clear();

	// Jitter buffer target: the largest buffer requested by the sources showing this camera.
	int buffer_ms() const;
	// Highest tally among the sources showing this camera.
	int tally() const;
	static constexpr int kDefaultBufferMs = 60;

	// Bookkeeping used to blank sources a few seconds after a disconnection.
	void set_live(bool live, int64_t now_ns);
	bool live() const { return live_; }
	int64_t offline_since_ns() const { return offline_since_ns_; }
	bool cleared() const { return cleared_; }

private:
	std::string name_;
	mutable std::mutex mutex_;
	std::vector<ChannelSink *> sinks_;
	std::atomic<bool> live_{false};
	std::atomic<int64_t> offline_since_ns_{0};
	std::atomic<bool> cleared_{true};
};

} // namespace camdrop
