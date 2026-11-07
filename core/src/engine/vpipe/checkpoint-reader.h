// A safetensors file as vpipe's loader reads it (DESIGN §8b): its
// tensors of 1 MB and up -- those MetalLlamaWeights::load() and a
// streamed block's refill read with pread_into, the smaller ones copied
// from its map -- read with pread_into, uncached. A model's read speed
// is measured through it (models/read-speed.h), so the figure follows
// vpipe's reader as it changes: its pieces in flight, the 4 KB grid, a
// tensor off the grid staged.

#ifndef VALTZ_ENGINE_VPIPE_CHECKPOINT_READER_H
#define VALTZ_ENGINE_VPIPE_CHECKPOINT_READER_H

#include "valtz/models/checkpoint-reader.h"

#include <filesystem>
#include <memory>

namespace valtz::engine::vp {

// Null for a file that is not safetensors, or that vpipe does not open.
std::unique_ptr<models::CheckpointReader>
open_checkpoint(const std::filesystem::path& file);

}  // namespace valtz::engine::vp

#endif
