#include "plgen/csv.hpp"

#include <sstream>
#include <stdexcept>

namespace plgen {

bool CsvReader::next(std::vector<std::string>& fields) {
  fields.clear();
  std::string line;
  if (!std::getline(in_, line)) return false;
  ++line_;

  std::string field;
  bool in_quotes = false;
  std::size_t i = 0;

  // A small state machine: the only state is "inside quotes or not".
  while (true) {
    if (i == line.size()) {
      if (!in_quotes) break;
      // Quoted field continues onto the next physical line.
      if (!std::getline(in_, line)) {
        throw std::runtime_error("CSV: unterminated quoted field starting before line " +
                                 std::to_string(line_));
      }
      ++line_;
      field.push_back('\n');
      i = 0;
      continue;
    }

    const char c = line[i++];
    if (in_quotes) {
      if (c == '"') {
        if (i < line.size() && line[i] == '"') {  // "" -> literal quote
          field.push_back('"');
          ++i;
        } else {
          in_quotes = false;
        }
      } else {
        field.push_back(c);
      }
    } else if (c == '"') {
      in_quotes = true;
    } else if (c == ',') {
      fields.push_back(std::move(field));
      field.clear();  // moved-from strings are valid but unspecified: reset explicitly
    } else if (c == '\r' && i == line.size()) {
      // drop the \r of a \r\n line ending
    } else {
      field.push_back(c);
    }
  }
  fields.push_back(std::move(field));
  return true;
}

std::vector<std::string> split_csv_record(std::string_view text) {
  std::istringstream in{std::string(text)};
  CsvReader reader(in);
  std::vector<std::string> fields;
  reader.next(fields);
  return fields;
}

CsvHeader::CsvHeader(const std::vector<std::string>& names) : size_(names.size()) {
  for (std::size_t i = 0; i < names.size(); ++i) {
    std::string name = names[i];
    // Strip a UTF-8 byte-order mark that some tools put before the first column.
    if (i == 0 && name.starts_with("\xEF\xBB\xBF")) name.erase(0, 3);
    index_.emplace(std::move(name), i);
  }
}

std::size_t CsvHeader::index_of(const std::string& name) const {
  const auto it = index_.find(name);
  if (it == index_.end()) throw std::runtime_error("CSV: missing column '" + name + "'");
  return it->second;
}

}  // namespace plgen
