#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace camdrop {

struct HttpRequest {
	std::string method;
	std::string target; // raw request target
	std::string path;   // decoded path, without query
	std::string query;  // raw query string
	std::string version;
	std::map<std::string, std::string> headers; // lower-case names
	std::string body;

	std::string header(const std::string &name) const;
	std::map<std::string, std::string> query_params() const;
	bool wants_websocket() const;
	bool keep_alive() const;
};

struct HttpResponse {
	int status = 200;
	std::vector<std::pair<std::string, std::string>> headers;
	std::string body;

	HttpResponse &set(const std::string &name, const std::string &value);

	static HttpResponse text(int status, const std::string &body);
	static HttpResponse json(int status, const std::string &body);
	static HttpResponse redirect(const std::string &location);
};

enum class ParseResult { Complete, NeedMore, Error };

// Parses one request from `buffer`. On Complete, `consumed` bytes can be dropped from the buffer.
ParseResult parse_http_request(const std::string &buffer, HttpRequest &req, size_t &consumed,
			       size_t max_header = 16 * 1024, size_t max_body = 256 * 1024);

std::string serialize_response(const HttpResponse &res, bool keep_alive, bool head_only = false);
const char *status_reason(int status);
std::string mime_type_for(const std::string &path);

} // namespace camdrop
