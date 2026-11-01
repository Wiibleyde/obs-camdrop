#include "core/sdp-utils.hpp"

#include "util/strings.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <vector>

namespace camdrop {

namespace {

struct Section {
	std::vector<std::string> lines; // first line is "m=..." (except for the session section)
};

std::vector<Section> split_sections(const std::string &sdp)
{
	std::vector<Section> sections(1);
	for (std::string line : split(sdp, '\n')) {
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;
		if (starts_with(line, "m="))
			sections.emplace_back();
		sections.back().lines.push_back(line);
	}
	return sections;
}

std::string join_sections(const std::vector<Section> &sections)
{
	std::string out;
	for (const auto &s : sections)
		for (const auto &l : s.lines)
			out += l + "\r\n";
	return out;
}

// "a=rtpmap:96 H264/90000" -> 96, returns -1 when the line is not for a payload type.
int line_pt(const std::string &line, const std::string &prefix)
{
	if (!starts_with(line, prefix))
		return -1;
	size_t start = prefix.size();
	size_t end = start;
	while (end < line.size() && isdigit(static_cast<unsigned char>(line[end])))
		end++;
	if (end == start)
		return -1;
	return std::stoi(line.substr(start, end - start));
}

std::string media_kind(const Section &s)
{
	if (s.lines.empty() || !starts_with(s.lines[0], "m="))
		return {};
	size_t sp = s.lines[0].find(' ');
	return s.lines[0].substr(2, sp == std::string::npos ? std::string::npos : sp - 2);
}

void rewrite_mline(Section &s, const std::vector<int> &pts)
{
	auto parts = split(s.lines[0], ' ');
	if (parts.size() < 3)
		return;
	std::string m = parts[0] + " " + parts[1] + " " + parts[2];
	for (int pt : pts)
		m += " " + std::to_string(pt);
	s.lines[0] = m;
}

void filter_section(Section &s, const std::vector<int> &keep)
{
	const std::set<int> keep_set(keep.begin(), keep.end());
	std::vector<std::string> out;
	out.push_back(s.lines[0]);
	for (size_t i = 1; i < s.lines.size(); i++) {
		const std::string &l = s.lines[i];
		int pt = line_pt(l, "a=rtpmap:");
		if (pt < 0)
			pt = line_pt(l, "a=fmtp:");
		if (pt < 0)
			pt = line_pt(l, "a=rtcp-fb:");
		if (pt >= 0 && !keep_set.count(pt))
			continue;
		if (starts_with(l, "a=rtcp-fb:") && l.find("transport-cc") != std::string::npos)
			continue;
		if (starts_with(l, "a=extmap:") && l.find("transport-wide-cc") != std::string::npos)
			continue;
		// Without the coordination of video orientation (CVO) extension, browsers rotate the frames
		// themselves: a phone held in portrait sends portrait pictures instead of a rotation flag.
		if (starts_with(l, "a=extmap:") && l.find("video-orientation") != std::string::npos)
			continue;
		out.push_back(l);
	}
	s.lines = std::move(out);
	rewrite_mline(s, keep);
}

} // namespace

std::string filter_offer(const std::string &sdp, const std::string &preferred_codec)
{
	auto sections = split_sections(sdp);

	for (auto &s : sections) {
		const std::string kind = media_kind(s);
		if (kind != "video" && kind != "audio")
			continue;

		std::map<int, std::string> codec;  // pt -> lower-case codec name
		std::map<int, std::string> params; // pt -> fmtp parameters (lower case)
		std::vector<int> order;
		for (const auto &l : s.lines) {
			int pt = line_pt(l, "a=rtpmap:");
			if (pt >= 0) {
				size_t sp = l.find(' ');
				std::string enc = sp == std::string::npos ? "" : to_lower(l.substr(sp + 1));
				codec[pt] = enc.substr(0, enc.find('/'));
				order.push_back(pt);
				continue;
			}
			pt = line_pt(l, "a=fmtp:");
			if (pt >= 0) {
				size_t sp = l.find(' ');
				params[pt] = sp == std::string::npos ? "" : to_lower(l.substr(sp + 1));
			}
		}

		std::vector<int> keep;
		if (kind == "audio") {
			for (int pt : order)
				if (codec[pt] == "opus")
					keep.push_back(pt);
		} else {
			std::vector<int> h264_cb, h264_other, vp8;
			for (int pt : order) {
				const std::string &c = codec[pt];
				const std::string &p = params[pt];
				if (c == "h264") {
					if (p.find("packetization-mode=1") == std::string::npos)
						continue;
					size_t pos = p.find("profile-level-id=");
					std::string profile = pos == std::string::npos ? "" : p.substr(pos + 17, 4);
					if (profile == "42e0" || profile == "4200" || profile == "42c0")
						h264_cb.push_back(pt);
					else
						h264_other.push_back(pt);
				} else if (c == "vp8") {
					vp8.push_back(pt);
				}
			}
			std::vector<int> h264 = h264_cb.empty() ? h264_other : h264_cb;
			if (h264.size() > 2)
				h264.resize(2);
			if (preferred_codec == "vp8") {
				keep = vp8;
				keep.insert(keep.end(), h264.begin(), h264.end());
			} else {
				keep = h264;
				keep.insert(keep.end(), vp8.begin(), vp8.end());
			}
			// Retransmission streams (RFC 4588) of the kept codecs.
			const std::vector<int> media = keep;
			for (int pt : order) {
				if (codec[pt] != "rtx")
					continue;
				const std::string &p = params[pt];
				const size_t pos = p.find("apt=");
				if (pos == std::string::npos)
					continue;
				const int apt = atoi(p.c_str() + pos + 4);
				if (std::find(media.begin(), media.end(), apt) != media.end())
					keep.push_back(pt);
			}
		}

		if (!keep.empty())
			filter_section(s, keep);
	}
	return join_sections(sections);
}

std::string tune_answer(const std::string &sdp, int max_bitrate_kbps, bool opus_10ms)
{
	auto sections = split_sections(sdp);
	for (auto &s : sections) {
		const std::string kind = media_kind(s);
		if (kind == "video") {
			std::vector<std::string> out;
			std::set<int> pts_with_fmtp;
			std::vector<int> pts;
			for (const auto &l : s.lines) {
				if (starts_with(l, "b="))
					continue;
				int pt = line_pt(l, "a=rtpmap:");
				if (pt >= 0)
					pts.push_back(pt);
				pt = line_pt(l, "a=fmtp:");
				if (pt >= 0) {
					pts_with_fmtp.insert(pt);
					const int start = std::max(300, max_bitrate_kbps * 6 / 10);
					out.push_back(l + ";x-google-start-bitrate=" + std::to_string(start) +
						      ";x-google-max-bitrate=" + std::to_string(max_bitrate_kbps) +
						      ";x-google-min-bitrate=300");
					continue;
				}
				out.push_back(l);
				if (starts_with(l, "c="))
					out.push_back("b=AS:" + std::to_string(max_bitrate_kbps));
			}
			for (int pt : pts) {
				if (pts_with_fmtp.count(pt))
					continue;
				const int start = std::max(300, max_bitrate_kbps * 6 / 10);
				out.push_back("a=fmtp:" + std::to_string(pt) +
					      " x-google-start-bitrate=" + std::to_string(start) +
					      ";x-google-max-bitrate=" + std::to_string(max_bitrate_kbps) +
					      ";x-google-min-bitrate=300");
			}
			if (std::none_of(out.begin(), out.end(),
					 [](const std::string &l) { return starts_with(l, "b=AS"); }))
				out.insert(out.begin() + 1, "b=AS:" + std::to_string(max_bitrate_kbps));
			s.lines = std::move(out);
		} else if (kind == "audio" && opus_10ms) {
			for (auto &l : s.lines) {
				if (starts_with(l, "a=fmtp:") && l.find("minptime") == std::string::npos)
					l += ";minptime=10";
				else if (starts_with(l, "a=fmtp:"))
					l = l.substr(0, l.find("minptime")) + "minptime=10" +
					    (l.find(';', l.find("minptime")) == std::string::npos
						     ? ""
						     : l.substr(l.find(';', l.find("minptime"))));
			}
			s.lines.push_back("a=ptime:10");
		}
	}
	return join_sections(sections);
}

} // namespace camdrop
