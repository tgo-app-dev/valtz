#include "valtz/controller/log-book.h"

#include "valtz/base/text.h"

#include <chrono>

namespace valtz {

void
LogBook::add(std::string_view level, std::string_view source,
             std::string_view text)
{
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  std::lock_guard lk(_mu);
  std::size_t start = 0;
  while (start <= text.size()) {
    auto nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      nl = text.size();
    }
    std::string_view line = text.substr(start, nl - start);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    // A trailing newline makes no empty row.
    if (!(line.empty() && nl == text.size() && start > 0)) {
      // A row is for reading: a runaway line is cut, at a whole
      // character.
      _rows.push_back(Row{_next++, now, std::string(level),
                          std::string(source),
                          std::string(utf8_prefix(line, 4096))});
      if (_rows.size() > _capacity) {
        _rows.pop_front();
      }
    }
    if (nl == text.size()) {
      break;
    }
    start = nl + 1;
  }
}

Json
LogBook::since(std::uint64_t seq, std::size_t max) const
{
  std::lock_guard lk(_mu);
  Json rows = Json::array();
  std::uint64_t last = seq;
  for (const auto& r : _rows) {
    if (r.seq <= seq) {
      continue;
    }
    if (rows.size() >= max) {
      break;
    }
    rows.push_back({{"seq", r.seq}, {"time", r.time_ms}, {"level", r.level},
                    {"source", r.source}, {"text", r.text}});
    last = r.seq;
  }
  return {{"rows", std::move(rows)}, {"next", last},
          {"first", _rows.empty() ? _next : _rows.front().seq}};
}

void
LogBook::clear()
{
  std::lock_guard lk(_mu);
  _rows.clear();
}

}
