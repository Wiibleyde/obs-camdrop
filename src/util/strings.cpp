#include "util/strings.hpp"

#include <mbedtls/base64.h>

#include <algorithm>
#include <cctype>

namespace camdrop {

std::string to_lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string trim(const std::string &s)
{
	size_t b = 0, e = s.size();
	while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
		b++;
	while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
		e--;
	return s.substr(b, e - b);
}

std::vector<std::string> split(const std::string &s, char sep)
{
	std::vector<std::string> out;
	size_t start = 0;
	for (;;) {
		size_t pos = s.find(sep, start);
		if (pos == std::string::npos) {
			out.push_back(s.substr(start));
			break;
		}
		out.push_back(s.substr(start, pos - start));
		start = pos + 1;
	}
	return out;
}

bool starts_with(const std::string &s, const std::string &prefix)
{
	return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool ends_with(const std::string &s, const std::string &suffix)
{
	return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

std::string url_decode(const std::string &s)
{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); i++) {
		char c = s[i];
		if (c == '+') {
			out.push_back(' ');
		} else if (c == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 && hex_value(s[i + 2]) >= 0) {
			out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
			i += 2;
		} else {
			out.push_back(c);
		}
	}
	return out;
}

std::map<std::string, std::string> parse_query(const std::string &query)
{
	std::map<std::string, std::string> out;
	for (const auto &part : split(query, '&')) {
		if (part.empty())
			continue;
		size_t eq = part.find('=');
		if (eq == std::string::npos)
			out[url_decode(part)] = "";
		else
			out[url_decode(part.substr(0, eq))] = url_decode(part.substr(eq + 1));
	}
	return out;
}

std::string sanitize_name(const std::string &name, size_t max_len)
{
	std::string out;
	out.reserve(name.size());
	bool last_space = false;
	for (unsigned char c : name) {
		if (c < 0x20 || c == 0x7f) {
			c = ' ';
		}
		if (c == ' ') {
			if (last_space || out.empty())
				continue;
			last_space = true;
		} else {
			last_space = false;
		}
		out.push_back(static_cast<char>(c));
	}
	out = trim(out);
	if (out.size() > max_len) {
		size_t cut = max_len;
		// Do not cut in the middle of a UTF-8 sequence.
		while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80)
			cut--;
		out.resize(cut);
		out = trim(out);
	}
	return out;
}

bool secure_equals(const std::string &a, const std::string &b)
{
	if (a.size() != b.size())
		return false;
	unsigned char diff = 0;
	for (size_t i = 0; i < a.size(); i++)
		diff |= static_cast<unsigned char>(a[i] ^ b[i]);
	return diff == 0;
}

std::string base64_encode(const unsigned char *data, size_t len)
{
	size_t olen = 0;
	mbedtls_base64_encode(nullptr, 0, &olen, data, len);
	std::string out(olen, '\0');
	if (mbedtls_base64_encode(reinterpret_cast<unsigned char *>(out.data()), out.size(), &olen, data, len) != 0)
		return {};
	out.resize(olen);
	return out;
}

} // namespace camdrop
