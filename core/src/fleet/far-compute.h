#ifndef VALTZ_FLEET_FAR_COMPUTE_H
#define VALTZ_FLEET_FAR_COMPUTE_H

// EXPERTS RUN WHERE THEY ARE LENT (DESIGN §11e "Run there"; PROTOTYPE).
// A far expert read back costs its bytes over the cables -- 18 MB for
// DeepSeek V4.1 Flash, ~0.8 ms -- where running it here and handing back
// its output costs the row's activation and its output row, 10 KB each
// way. So the lender runs a decode row's far experts on its GPU, over the
// lent memory in place (Metal buffers on the session's pages, no copy),
// with the same arithmetic as the model's own decode kernels (vpipe's
// dsv41_moe_swiglu_fp4 / dsv41_moe_down_fp4, restated here, and its
// dsv41_moe_*_iq5_s4_r2 for a GGUF's own blocks, copied: the lender needs
// nothing of vpipe), each expert's output row rounded to the element type
// as there and handed back as it is (vpipe's far-compute/2) -- so the
// model's combine is the one it makes of its own rows.
//
// MEASURED (vpipe's deepseek_v41.far_expert_compute_bench, M5 Max): a GPU
// idle for a millisecond answers 2-4x slower -- its clocks drop -- so
// while calls come (one in the last second), a small dispatch keeps it
// busy every VALTZ_FAR_GPU_WARM_US (20; 0 off).

#include "valtz-vpipe/far-wire.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace valtz::fleet {

class FarCompute {
public:
  // Over the session's memory (page-aligned, a whole number of pages).
  // Null with `err` where this Mac's GPU cannot (no Metal, the kernels
  // did not build, the memory not mappable).
  static std::unique_ptr<FarCompute> create(std::uint8_t* mem,
                                            std::uint64_t bytes,
                                            std::string* err);
  ~FarCompute();
  FarCompute(const FarCompute&) = delete;
  FarCompute& operator=(const FarCompute&) = delete;

  // One row: x [hidden] in, out [n, hidden]: each call's expert's
  // weighted output, in call order. False with `err` on a form it does
  // not run (a GGUF's tables whose head is not version 1's) or a GPU
  // error; the caller has checked every part lies in the memory.
  bool run(const far_wire::ComputeForm& form,
           const far_wire::ComputeCall* calls, std::size_t n,
           const void* x, void* out, std::string* err);

  // Where a call's time goes, summed over the calls since made (Metal's
  // own timestamps, in microseconds): LAG from the commit to the GPU's
  // start, RUN its kernels, NOTICE from the GPU's end to this thread
  // seeing it, WALL the whole call.
  struct Timing {
    std::uint64_t calls = 0;
    double lag_us = 0, run_us = 0, notice_us = 0, wall_us = 0;
  };
  Timing timing() const;

  struct Impl;

private:
  explicit FarCompute(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> _impl;
};

}

#endif
