#include "plgen/http.hpp"

#include <cctype>
#include <charconv>

namespace plgen {
namespace {

constexpr std::string_view kCrlf = "\r\n";
constexpr std::size_t kMaxBody = 1 << 20;  // 1 MiB: plenty for a playlist request

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::string url_decode(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out.push_back(' ');
    } else if (s[i] == '%' && i + 2 < s.size() && hex_value(s[i + 1]) >= 0 &&
               hex_value(s[i + 2]) >= 0) {
      out.push_back(static_cast<char>(hex_value(s[i + 1]) * 16 + hex_value(s[i + 2])));
      i += 2;
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::unordered_map<std::string, std::string> parse_query(std::string_view qs) {
  std::unordered_map<std::string, std::string> out;
  while (!qs.empty()) {
    const std::size_t amp = qs.find('&');
    const std::string_view pair = qs.substr(0, amp);
    if (!pair.empty()) {
      const std::size_t eq = pair.find('=');
      // First value wins if a key repeats; keys without '=' get an empty value.
      out.emplace(url_decode(pair.substr(0, eq)),
                  eq == std::string_view::npos ? "" : url_decode(pair.substr(eq + 1)));
    }
    if (amp == std::string_view::npos) break;
    qs.remove_prefix(amp + 1);
  }
  return out;
}

ParseResult parse_request(std::string_view raw, HttpRequest& out) {
  // Headers end at the first blank line. Until we've seen it, wait for more data.
  const std::size_t header_end = raw.find("\r\n\r\n");
  if (header_end == std::string_view::npos) return ParseResult::kIncomplete;

  std::string_view head = raw.substr(0, header_end);
  out = HttpRequest{};

  // Request line: METHOD SP TARGET SP VERSION
  const std::size_t line_end = head.find(kCrlf);
  const std::string_view request_line = head.substr(0, line_end);
  const std::size_t sp1 = request_line.find(' ');
  const std::size_t sp2 = request_line.find(' ', sp1 == std::string_view::npos ? sp1 : sp1 + 1);
  if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return ParseResult::kBad;
  out.method = request_line.substr(0, sp1);
  out.target = request_line.substr(sp1 + 1, sp2 - sp1 - 1);
  out.version = request_line.substr(sp2 + 1);
  if (out.method.empty() || out.target.empty() || out.target.front() != '/' ||
      !out.version.starts_with("HTTP/1.")) {
    return ParseResult::kBad;
  }

  const std::size_t qmark = out.target.find('?');
  out.path = url_decode(std::string_view(out.target).substr(0, qmark));
  if (qmark != std::string::npos) out.query = parse_query(std::string_view(out.target).substr(qmark + 1));

  // Header lines: "Name: value"
  head.remove_prefix(line_end == std::string_view::npos ? head.size() : line_end + kCrlf.size());
  while (!head.empty()) {
    const std::size_t end = head.find(kCrlf);
    const std::string_view line = head.substr(0, end);
    const std::size_t colon = line.find(':');
    if (colon == std::string_view::npos || colon == 0) return ParseResult::kBad;
    out.headers[lower(trim(line.substr(0, colon)))] = std::string(trim(line.substr(colon + 1)));
    if (end == std::string_view::npos) break;
    head.remove_prefix(end + kCrlf.size());
  }

  // Body: exactly Content-Length bytes after the blank line.
  std::size_t body_len = 0;
  if (const auto it = out.headers.find("content-length"); it != out.headers.end()) {
    const auto& v = it->second;
    const auto [ptr, ec] = std::from_chars(v.data(), v.data() + v.size(), body_len);
    if (ec != std::errc{} || ptr != v.data() + v.size() || body_len > kMaxBody) {
      return ParseResult::kBad;
    }
  }
  const std::size_t body_start = header_end + 4;
  if (raw.size() < body_start + body_len) return ParseResult::kIncomplete;
  out.body = raw.substr(body_start, body_len);
  return ParseResult::kOk;
}

std::string_view status_text(int status) noexcept {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Unknown";
  }
}

std::string serialize(const HttpResponse& r) {
  std::string out;
  out.reserve(128 + r.body.size());
  out += "HTTP/1.1 ";
  out += std::to_string(r.status);
  out += ' ';
  out += status_text(r.status);
  out += "\r\nContent-Type: ";
  out += r.content_type;
  out += "\r\nContent-Length: ";
  out += std::to_string(r.body.size());
  out += "\r\nConnection: close\r\n\r\n";  // one request per connection (simplest correct choice)
  out += r.body;
  return out;
}

}  // namespace plgen
