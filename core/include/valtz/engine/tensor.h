// A view of tensor memory produced by the engine, kept alive by `owner`.
//
// This is the seam for ZERO-COPY output. A tensor from the engine is a
// view of the vpipe beat itself, handed over by the valtz-vpipe plugin's
// `valtz-sink` (engine/vpipe/host-exchange.h): `data` points into the
// beat's storage and `owner` IS the beat, so the engine's memory lives
// exactly as long as some consumer holds this Tensor -- past the end of
// the pipeline, its unload, even the session -- and is freed by the
// last release, on whatever thread that is.
//
// The contract for consumers:
//   * READ-ONLY. The bytes may be the engine's own buffer.
//   * DROP REFERENCES PROMPTLY. An outstanding Tensor may be holding a
//     buffer the engine wants back. The event bus coalesces previews
//     (older frames are released as newer ones arrive) and the bridge
//     retains at most a handful, so the number of buffers Valtz holds is
//     bounded no matter how slowly the UI draws.
//   * `mtl_buffer` (an MTL::Buffer*, null when the producer used CPU
//     memory -- vpipe's TAE previews do, today) lets GPU consumers -- the
//     Metal viewer, color conversion to the working space -- bind the
//     memory directly instead of reading `data` on the CPU.

#ifndef VALTZ_ENGINE_TENSOR_H
#define VALTZ_ENGINE_TENSOR_H

#include "valtz/base/json.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace valtz::engine {

struct Tensor {
  enum class DType : std::uint8_t { U8, F16, BF16, F32 };

  DType                     dtype = DType::U8;
  std::vector<std::int64_t> shape;    // e.g. [C,H,W] or [F,C,H,W]
  std::vector<std::int64_t> strides;  // elements; empty = row-major
  const std::uint8_t*       data = nullptr;
  std::size_t               byte_size = 0;
  void*                     mtl_buffer = nullptr;  // MTL::Buffer*
  std::size_t               mtl_offset = 0;        // bytes into it
  Json                      meta;     // the beat's sideband
  // Keeps `data` valid; its deleter returns the memory to its producer.
  std::shared_ptr<const void> owner;

  static std::size_t element_size(DType);
  std::size_t element_size() const { return element_size(dtype); }
  std::size_t element_count() const;
  bool contiguous() const;
};

using TensorPtr = std::shared_ptr<const Tensor>;

// A Tensor owning a private copy of `bytes`.
TensorPtr make_owned_tensor(Tensor::DType, std::vector<std::int64_t> shape,
                            std::vector<std::uint8_t> bytes, Json meta);

// vpipe's dtype names ("u8", "f16", ...).
bool dtype_from_str(std::string_view, Tensor::DType&);

}

#endif
