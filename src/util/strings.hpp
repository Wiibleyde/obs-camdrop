#pragma once

#include <map>
#include <string>
#include <vector>

namespace camdrop {

std::string to_lower(std::string s);
std::string trim(const std::string &s);
std::vector<std::string> split(const std::string &s, char sep);
bool starts_with(const std::string &s, const std::string &prefix);
bool ends_with(const std::string &s, const std::string &suffix);
std::string url_decode(const std::string &s);
std::map<std::string, std::string> parse_query(const std::string &query);

// Keeps a display name safe and short: trims, collapses control characters, max `max_len` UTF-8 bytes.
std::string sanitize_name(const std::string &name, size_t max_len = 32);

// Constant-time comparison (for access codes).
bool secure_equals(const std::string &a, const std::string &b);

std::string base64_encode(const unsigned char *data, size_t len);

} // namespace camdrop
