#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace plgen {

// Appends `s` as a JSON string literal (with quotes) to `out`.
// Escapes quotes, backslashes and control characters; UTF-8 passes through,
// so song titles like "Áries" or "吳青峰" come out intact.
inline void append_json_string(std::string& out, std::string_view s) {
  out.push_back('"');
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  out.push_back('"');
}

[[nodiscard]] inline std::string json_string(std::string_view s) {
  std::string out;
  append_json_string(out, s);
  return out;
}

}  // namespace plgen
