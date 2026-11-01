#include "net/http.hpp"

#include "util/strings.hpp"

#include <cstdlib>

namespace camdrop {

std::string HttpRequest::header(const std::string &name) const
{
	auto it = headers.find(to_lower(name));
	return it == headers.end() ? std::string() : it->second;
}

std::map<std::string, std::string> HttpRequest::query_params() const
{
	return parse_query(query);
}

bool HttpRequest::wants_websocket() const
{
	return to_lower(header("upgrade")) == "websocket" &&
	       to_lower(header("connection")).find("upgrade") != std::string::npos;
}

bool HttpRequest::keep_alive() const
{
	std::string c = to_lower(header("connection"));
	if (version == "HTTP/1.0")
		return c.find("keep-alive") != std::string::npos;
	return c.find("close") == std::string::npos;
}

HttpResponse &HttpResponse::set(const std::string &name, const std::string &value)
{
	for (auto &h : headers) {
		if (to_lower(h.first) == to_lower(name)) {
			h.second = value;
			return *this;
		}
	}
	headers.emplace_back(name, value);
	return *this;
}

HttpResponse HttpResponse::text(int status, const std::string &body)
{
	HttpResponse r;
	r.status = status;
	r.body = body;
	r.set("Content-Type", "text/plain; charset=utf-8");
	return r;
}

HttpResponse HttpResponse::json(int status, const std::string &body)
{
	HttpResponse r;
	r.status = status;
	r.body = body;
	r.set("Content-Type", "application/json; charset=utf-8");
	return r;
}

HttpResponse HttpResponse::redirect(const std::string &location)
{
	HttpResponse r;
	r.status = 302;
	r.set("Location", location);
	r.set("Content-Type", "text/plain; charset=utf-8");
	r.body = "Redirecting to " + location;
	return r;
}

ParseResult parse_http_request(const std::string &buffer, HttpRequest &req, size_t &consumed, size_t max_header,
			       size_t max_body)
{
	size_t header_end = buffer.find("\r\n\r\n");
	if (header_end == std::string::npos)
		return buffer.size() > max_header ? ParseResult::Error : ParseResult::NeedMore;
	if (header_end > max_header)
		return ParseResult::Error;

	req = HttpRequest{};
	const std::string head = buffer.substr(0, header_end);
	auto lines = split(head, '\n');
	if (lines.empty())
		return ParseResult::Error;

	// Request line
	std::string request_line = trim(lines[0]);
	auto parts = split(request_line, ' ');
	if (parts.size() != 3)
		return ParseResult::Error;
	req.method = parts[0];
	req.target = parts[1];
	req.version = parts[2];
	if (!starts_with(req.version, "HTTP/1."))
		return ParseResult::Error;
	if (req.target.empty() || req.target[0] != '/')
		return ParseResult::Error;

	size_t q = req.target.find('?');
	// Percent-decode the path ('+' is not a space in paths, so url_decode is applied per escape).
	req.path = q == std::string::npos ? req.target : req.target.substr(0, q);
	{
		std::string decoded;
		for (size_t i = 0; i < req.path.size(); i++) {
			if (req.path[i] == '%' && i + 2 < req.path.size()) {
				decoded += url_decode(req.path.substr(i, 3));
				i += 2;
			} else {
				decoded.push_back(req.path[i]);
			}
		}
		req.path = decoded;
	}
	if (q != std::string::npos)
		req.query = req.target.substr(q + 1);

	for (size_t i = 1; i < lines.size(); i++) {
		std::string line = lines[i];
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.empty())
			continue;
		size_t colon = line.find(':');
		if (colon == std::string::npos)
			return ParseResult::Error;
		std::string name = to_lower(trim(line.substr(0, colon)));
		std::string value = trim(line.substr(colon + 1));
		auto it = req.headers.find(name);
		if (it != req.headers.end())
			it->second += ", " + value;
		else
			req.headers.emplace(name, value);
	}

	size_t body_len = 0;
	std::string cl = req.header("content-length");
	if (!cl.empty()) {
		char *end = nullptr;
		unsigned long long v = strtoull(cl.c_str(), &end, 10);
		if (!end || *end != '\0' || v > max_body)
			return ParseResult::Error;
		body_len = static_cast<size_t>(v);
	}
	if (!req.header("transfer-encoding").empty())
		return ParseResult::Error; // chunked uploads are not needed

	const size_t total = header_end + 4 + body_len;
	if (buffer.size() < total)
		return ParseResult::NeedMore;
	req.body = buffer.substr(header_end + 4, body_len);
	consumed = total;
	return ParseResult::Complete;
}

const char *status_reason(int status)
{
	switch (status) {
	case 101:
		return "Switching Protocols";
	case 200:
		return "OK";
	case 204:
		return "No Content";
	case 301:
		return "Moved Permanently";
	case 302:
		return "Found";
	case 304:
		return "Not Modified";
	case 400:
		return "Bad Request";
	case 401:
		return "Unauthorized";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 413:
		return "Payload Too Large";
	case 429:
		return "Too Many Requests";
	case 500:
		return "Internal Server Error";
	case 503:
		return "Service Unavailable";
	default:
		return "Unknown";
	}
}

std::string serialize_response(const HttpResponse &res, bool keep_alive, bool head_only)
{
	std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + status_reason(res.status) + "\r\n";
	bool has_length = false;
	for (const auto &h : res.headers) {
		if (to_lower(h.first) == "content-length")
			has_length = true;
		out += h.first + ": " + h.second + "\r\n";
	}
	if (!has_length && res.status != 101)
		out += "Content-Length: " + std::to_string(res.body.size()) + "\r\n";
	if (res.status != 101)
		out += keep_alive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
	out += "\r\n";
	if (!head_only)
		out += res.body;
	return out;
}

std::string mime_type_for(const std::string &path)
{
	std::string p = to_lower(path);
	if (ends_with(p, ".html"))
		return "text/html; charset=utf-8";
	if (ends_with(p, ".js") || ends_with(p, ".mjs"))
		return "text/javascript; charset=utf-8";
	if (ends_with(p, ".css"))
		return "text/css; charset=utf-8";
	if (ends_with(p, ".json") || ends_with(p, ".webmanifest"))
		return "application/json; charset=utf-8";
	if (ends_with(p, ".svg"))
		return "image/svg+xml";
	if (ends_with(p, ".png"))
		return "image/png";
	if (ends_with(p, ".ico"))
		return "image/x-icon";
	if (ends_with(p, ".mp4"))
		return "video/mp4";
	if (ends_with(p, ".webm"))
		return "video/webm";
	if (ends_with(p, ".wav"))
		return "audio/wav";
	if (ends_with(p, ".txt"))
		return "text/plain; charset=utf-8";
	return "application/octet-stream";
}

} // namespace camdrop
