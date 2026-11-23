#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace plgen {

// An HTTP/1.1 request, already parsed. Example raw request:
//
//   GET /similar?song=abc123&k=20 HTTP/1.1\r\n
//   Host: localhost:8080\r\n
//   \r\n
struct HttpRequest {
  std::string method;  // "GET"
  std::string target;  // "/similar?song=abc123&k=20"
  std::string path;    // "/similar"
  std::string version; // "HTTP/1.1"
  std::unordered_map<std::string, std::string> query;    // {"song": "abc123", "k": "20"}
  std::unordered_map<std::string, std::string> headers;  // names lowercased: {"host": ...}
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

enum class ParseResult {
  kOk,          // a full request was parsed into `out`
  kIncomplete,  // valid so far, but we need more bytes from the socket
  kBad,         // malformed: reply 400
};

// Parses one request from the bytes received so far. Data arrives from the
// network in arbitrary chunks, so "not finished yet" is a normal outcome.
[[nodiscard]] ParseResult parse_request(std::string_view raw, HttpRequest& out);

// "rainy%20day+study" -> "rainy day study". Invalid %-escapes are kept as-is.
[[nodiscard]] std::string url_decode(std::string_view s);

// "a=1&b=two%20words" -> {"a": "1", "b": "two words"}
[[nodiscard]] std::unordered_map<std::string, std::string> parse_query(std::string_view qs);

// Status line + headers + body, ready to write to the socket.
[[nodiscard]] std::string serialize(const HttpResponse& r);

[[nodiscard]] std::string_view status_text(int status) noexcept;

}  // namespace plgen
