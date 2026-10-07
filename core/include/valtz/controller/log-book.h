// The LOG the app's Log view shows: what the engine (vpipe) reports and
// what Valtz logs, a line a row, kept in a ring of the newest 16 384.
//
// Not the event bus: a busy pipeline can report thousands of lines a
// minute, and the bus is bounded and carries the jobs' events -- a burst
// of log there would crowd them out. The book is read by sequence number
// instead (since()), by a view while it is on screen; lines older than
// the ring are gone, so a log that runs on does not grow without end.

#ifndef VALTZ_CONTROLLER_LOG_BOOK_H
#define VALTZ_CONTROLLER_LOG_BOOK_H

#include "valtz/base/json.h"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>

namespace valtz {

class LogBook {
public:
  explicit LogBook(std::size_t capacity = 16384) : _capacity(capacity) {}

  // `text` added, a row per line of it. `level`: "error", "warn",
  // "info", "debug"; `source`: "vpipe" or "valtz". Thread-safe.
  void add(std::string_view level, std::string_view source,
           std::string_view text);

  // The rows after `seq`, at most `max`, oldest first:
  //   {"rows": [{"seq", "time" (ms since 1970), "level", "source",
  //              "text"}], "next": the last seq given (or `seq`),
  //    "first": the oldest seq still kept}
  // A reader that asked for less than `first` - 1 missed rows that fell
  // out of the ring.
  Json since(std::uint64_t seq, std::size_t max = 4096) const;

  // Every row gone; sequence numbers carry on.
  void clear();

private:
  struct Row {
    std::uint64_t seq;
    std::int64_t  time_ms;
    std::string   level;
    std::string   source;
    std::string   text;
  };
  std::size_t        _capacity;
  mutable std::mutex _mu;
  std::deque<Row>    _rows;
  std::uint64_t      _next = 1;
};

}

#endif
