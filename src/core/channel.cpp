#include "core/channel.hpp"

#include <algorithm>

namespace camdrop {

void Channel::add_sink(ChannelSink *sink)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (std::find(sinks_.begin(), sinks_.end(), sink) == sinks_.end())
		sinks_.push_back(sink);
}

void Channel::remove_sink(ChannelSink *sink)
{
	std::lock_guard<std::mutex> lock(mutex_);
	sinks_.erase(std::remove(sinks_.begin(), sinks_.end(), sink), sinks_.end());
}

bool Channel::has_sinks() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return !sinks_.empty();
}

size_t Channel::sink_count() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return sinks_.size();
}

void Channel::output_video(const obs_source_frame *frame)
{
	std::lock_guard<std::mutex> lock(mutex_);
	cleared_ = false;
	for (ChannelSink *sink : sinks_)
		sink->channel_video(frame);
}

void Channel::output_audio(const obs_source_audio *audio)
{
	std::lock_guard<std::mutex> lock(mutex_);
	for (ChannelSink *sink : sinks_)
		sink->channel_audio(audio);
}

void Channel::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	cleared_ = true;
	for (ChannelSink *sink : sinks_)
		sink->channel_clear();
}

int Channel::buffer_ms() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (sinks_.empty())
		return kDefaultBufferMs;
	int value = 0;
	for (const ChannelSink *sink : sinks_)
		value = std::max(value, sink->sink_buffer_ms());
	return value;
}

int Channel::tally() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	int value = Tally::Off;
	for (const ChannelSink *sink : sinks_)
		value = std::max(value, sink->sink_tally());
	return value;
}

const char *Tally::name(int tally)
{
	switch (tally) {
	case Tally::Program:
		return "program";
	case Tally::Preview:
		return "preview";
	default:
		return "off";
	}
}

void Channel::set_live(bool live, int64_t now_ns)
{
	live_ = live;
	if (!live)
		offline_since_ns_ = now_ns;
}

} // namespace camdrop
