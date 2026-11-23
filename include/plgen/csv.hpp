#pragma once

#include <cstddef>
#include <istream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace plgen {

// Reads RFC 4180-style CSV records from a stream:
//  - fields may be wrapped in double quotes
//  - quoted fields may contain commas, newlines and "" (an escaped quote)
//  - trailing \r (Windows line endings) is stripped
class CsvReader {
 public:
  explicit CsvReader(std::istream& in) : in_(in) {}

  // Reads the next record into `fields` (cleared first). Returns false at EOF.
  // Throws std::runtime_error on an unterminated quoted field.
  bool next(std::vector<std::string>& fields);

  // Physical line number of the last line consumed (1-based), for error messages.
  [[nodiscard]] std::size_t line_number() const noexcept { return line_; }

 private:
  std::istream& in_;  // reference member: the reader does not own the stream
  std::size_t line_ = 0;
};

// Convenience for tests: parse a single record from a string.
[[nodiscard]] std::vector<std::string> split_csv_record(std::string_view text);

// Maps column names to positions so the loader never hard-codes column order.
class CsvHeader {
 public:
  explicit CsvHeader(const std::vector<std::string>& names);

  // Throws std::runtime_error if the column is missing.
  [[nodiscard]] std::size_t index_of(const std::string& name) const;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  std::unordered_map<std::string, std::size_t> index_;
  std::size_t size_;
};

}  // namespace plgen
