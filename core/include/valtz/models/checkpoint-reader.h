// A checkpoint as the ENGINE reads it (DESIGN §8b): the tensors a load
// of it reads, and the engine's own read of each -- vpipe's pread_into,
// pieces in flight on the file's grid, past the memory cache, staged
// where the tensor lies off it. A model's read speed (read-speed.h) is
// measured through it, so the figure is what a load of the model gets,
// not what a read of Valtz's shape would.
//
// Here, apart from the engine's header, so the measure and the engine
// share it without either knowing the other.

#ifndef VALTZ_MODELS_CHECKPOINT_READER_H
#define VALTZ_MODELS_CHECKPOINT_READER_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace valtz::models {

class CheckpointReader {
public:
  struct Tensor {
    std::uint64_t offset = 0;   // in the file
    std::uint64_t bytes = 0;
  };

  virtual ~CheckpointReader() = default;
  // The tensors a load reads this way, in the file's order -- not the
  // small ones it takes another way (vpipe: under 1 MB, from its map).
  virtual const std::vector<Tensor>& tensors() const = 0;
  // Tensor `index` into `dst` (`cap` bytes, page-aligned, as a Metal
  // buffer is), past the memory cache. False: it could not.
  virtual bool read(std::size_t index, void* dst, std::size_t cap) = 0;
};

// The engine's reader of a file, or null where it reads it no such way
// (not a checkpoint; one it converts as it loads: GGUF, Draw Things').
using CheckpointOpener = std::function<std::unique_ptr<CheckpointReader>(
    const std::filesystem::path&)>;

}  // namespace valtz::models

#endif
