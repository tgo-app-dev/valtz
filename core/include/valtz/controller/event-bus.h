// The one ordered stream of events from the controller to the UI.
//
// COMMANDS IN, EVENTS OUT. The UI calls Controller methods, which return
// immediately (a job id, or an error for a request that is invalid on
// its face); everything that happens afterwards -- progress, previews,
// streamed text, new assets, failures -- arrives here, in order. The UI
// never receives a callback on an engine thread, so C++ never calls into
// Swift, and the same event shape can later cross a process or network
// boundary unchanged.
//
// Bounded, with one exception to strict FIFO: live previews and progress
// COALESCE. If a job's previous preview (or count) has not been consumed,
// the new one replaces it in place, so a UI that falls behind shows the
// latest frame rather than queueing seconds of stale ones -- and the GPU
// never waits for the UI. Progress moves ten times a second.

#ifndef VALTZ_CONTROLLER_EVENT_BUS_H
#define VALTZ_CONTROLLER_EVENT_BUS_H

#include "valtz/base/id.h"
#include "valtz/base/json.h"
#include "valtz/engine/engine.h"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

namespace valtz {

struct Event {
  std::uint64_t                         seq = 0;
  std::string                           kind;  // "job.preview", ...
  JobId                                 job;
  Json                                  data = Json::object();
  engine::TensorPtr                     tensor;  // job.preview frames

  // {"seq", "kind", "job", ...data} -- what the UI decodes.
  std::string to_json() const;
};

class EventBus {
public:
  explicit EventBus(std::size_t capacity = 4096);

  void post(Event);
  // Blocks up to `timeout_ms` (negative = forever). False on timeout or
  // after close().
  bool wait(Event& out, int timeout_ms);
  void close();

  std::uint64_t dropped() const;

private:
  mutable std::mutex      _mu;
  std::condition_variable _cv;
  std::deque<Event>       _q;
  std::size_t             _cap;
  std::uint64_t           _seq = 0;
  std::uint64_t           _dropped = 0;
  bool                    _closed = false;
};

}

#endif
