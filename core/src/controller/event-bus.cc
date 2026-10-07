#include "valtz/controller/event-bus.h"

#include <chrono>

namespace valtz {

std::string
Event::to_json() const
{
  Json j = data.is_object() ? data : Json::object();
  j["seq"] = seq;
  j["kind"] = kind;
  if (!job.is_nil()) {
    j["job"] = job.str();
  }
  if (tensor) {
    j["tensor"] = {{"shape", tensor->shape}};
  }
  return to_text(j);
}

EventBus::EventBus(std::size_t capacity) : _cap(capacity) {}

void
EventBus::post(Event ev)
{
  {
    std::lock_guard lk(_mu);
    if (_closed) {
      return;
    }
    ev.seq = ++_seq;
    // Coalesced per job, newest wins: a frame, and a count that moves
    // ten times a second (a reader that falls behind skips to now).
    if (ev.kind == "job.preview" || ev.kind == "job.progress") {
      for (auto& q : _q) {
        if (q.kind == ev.kind && q.job == ev.job) {
          q = std::move(ev);
          return;
        }
      }
    }
    if (_q.size() >= _cap) {
      // Drop the oldest non-terminal event rather than block a producer
      // that may be an engine thread.
      _q.pop_front();
      ++_dropped;
    }
    _q.push_back(std::move(ev));
  }
  _cv.notify_one();
}

bool
EventBus::wait(Event& out, int timeout_ms)
{
  std::unique_lock lk(_mu);
  auto ready = [&] { return _closed || !_q.empty(); };
  if (timeout_ms < 0) {
    _cv.wait(lk, ready);
  } else if (!_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                           ready)) {
    return false;
  }
  if (_q.empty()) {
    return false;
  }
  out = std::move(_q.front());
  _q.pop_front();
  return true;
}

void
EventBus::close()
{
  {
    std::lock_guard lk(_mu);
    _closed = true;
  }
  _cv.notify_all();
}

std::uint64_t
EventBus::dropped() const
{
  std::lock_guard lk(_mu);
  return _dropped;
}

}
