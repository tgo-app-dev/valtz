#ifndef VALTZ_FLEET_FAR_LEND_H
#define VALTZ_FLEET_FAR_LEND_H

// MEMORY LENT TO A MEMBER (DESIGN §11e). A language model too large for
// one Mac -- DeepSeek V4.1 Flash, 432 GB -- keeps its routed experts in a
// cache refilled from disk on a miss. A paired member with memory to spare
// LENDS some of it: the model's experts selected most often after those
// its Mac holds are kept here and read back over the Thunderbolt cables
// in place of the drive. This side needs no model and no files: it holds
// bytes where it is told and hands them back, and the Mac using it
// decides what goes where.
//
// HOW MUCH is this Mac's to say, when a Mac starts using it (a SESSION:
// its first connection, the memory taken; its last closed, given back):
// what is free then, less a reserve for its own work, at most three
// quarters of its memory -- or less, as Settings › Fleet's Lend Memory
// says. One Mac at a time.
//
// The wire is valtz-vpipe/far-wire.h (TCP, several connections a cable,
// the fleet's key proved both ways); the plugin's valtz-far transport is
// the other end.

#include "valtz/base/result.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace valtz::fleet {

// What this Mac can lend now: free memory less `reserve`, at most three
// quarters of all of it, in whole GB.
std::uint64_t lendable_bytes(std::uint64_t reserve = 8ull << 30);

// What Lend Memory says ("off", "auto", a number of GB) lends now: 0, or
// lendable_bytes() at most that.
std::uint64_t lend_bytes_for(const std::string& setting);

class FarLender {
public:
  struct Options {
    // The bytes a session takes, asked as it starts (0: none to lend).
    std::function<std::uint64_t()> size;
    // The key a user proves (empty: none asked), asked at each
    // connection: the fleet's may change.
    std::function<std::vector<std::uint8_t>()> key;
    int port = 0;   // 0: any
    // A folder of model files this Mac keeps copies of (PROTOTYPE,
    // far-wire.h's kOpenFile): read for the user off this Mac's drive
    // ("" none).
    std::string files;
  };

  // Listens on every interface; no memory is taken until a session.
  static Result<std::unique_ptr<FarLender>> start(Options o);
  ~FarLender();
  FarLender(const FarLender&) = delete;
  FarLender& operator=(const FarLender&) = delete;

  int port() const { return _port; }

  struct Traffic {
    std::uint64_t read = 0, written = 0;   // bytes handed back / stored
    std::uint64_t lent = 0;                // the session's, 0 none
    int connections = 0;
    // Experts run here (kCompute): the calls, and the experts in them.
    std::uint64_t compute_calls = 0, computed = 0;
    std::uint64_t compute_us = 0;          // ...their time on the GPU
    // ...split by Metal's timestamps (the session's, summed):
    // committed to started, the kernels, ended to seen
    std::uint64_t gpu_calls = 0;
    double gpu_lag_us = 0, gpu_run_us = 0, gpu_notice_us = 0;
  };
  Traffic traffic() const;

private:
  FarLender() = default;
  void accept_();
  void serve_(int fd);

  Options _o;
  int _port = 0;
  int _listen = -1;
  std::atomic<bool> _stop{false};
  std::atomic<std::uint64_t> _read{0}, _written{0};
  std::atomic<std::uint64_t> _compute_calls{0}, _computed{0};
  std::atomic<std::uint64_t> _compute_us{0};
  // The session: its user, connections, memory.
  mutable std::mutex _smu;
  std::uint64_t _user = 0;
  int _conns = 0;
  std::uint8_t* _mem = nullptr;
  std::uint64_t _bytes = 0;
  struct GpuTiming {
    std::uint64_t calls = 0;
    double lag_us = 0, run_us = 0, notice_us = 0, wall_us = 0;
    GpuTiming& operator=(const auto& t)
    {
      calls = t.calls;
      lag_us = t.lag_us;
      run_us = t.run_us;
      notice_us = t.notice_us;
      wall_us = t.wall_us;
      return *this;
    }
  } _gpu;
  struct Threads;
  std::unique_ptr<Threads> _threads;
};

}

#endif
