// TensorBeat <-> DataBuffer, and the engine memory a beat lives in.
//
// vpipe has the same conversions (stages/tensor-buffer.h) but does not
// install them: they are not part of the plugin SDK. These are ours,
// written against the installed headers only, so a vpipe refactor of its
// private helpers cannot break the plugin.
//
// Two directions, deliberately asymmetric (as in vpipe): a beat is
// VIEWED as a DataBuffer in place, while a caller's buffer becomes a beat
// by being GATHERED into memory the beat owns -- a TensorBeat owns its
// bytes outright, and a beat that aliased a caller's memory could be
// edited in place by a downstream stage, writing the caller's memory.
// The zero-copy way in is a lease: the caller fills the beat's own
// storage.

#ifndef VALTZ_VPIPE_PLUGIN_BEAT_BUFFER_H
#define VALTZ_VPIPE_PLUGIN_BEAT_BUFFER_H

#include "apple-silicon/tensor-beat.h"
#include "common/flex-data.h"
#include "vpipe/stage-command.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vpipe::metal_compute {
class MetalCompute;
}

namespace valtz::plugin {

// The element types a TensorBeat can carry, as a BufferSpec `type`.
inline constexpr char kTensorTypes[] = "u8,i8,f16,bf16,f32";

vpipe::ElementType element_type_of(vpipe::TensorBeat::DType) noexcept;
// False for the eight element types a TensorBeat has no dtype for.
bool tensor_dtype_of(vpipe::ElementType, vpipe::TensorBeat::DType*) noexcept;

// `tb` in place, named `name`, kept alive by `owner` (which must keep
// `tb` alive). Writable exactly when `writable`.
vpipe::DataBuffer tensor_view(const vpipe::TensorBeat& tb, std::string name,
                              std::shared_ptr<void> owner, bool writable);

// Give `tb` (dtype and shape set) contiguous zeroed storage for its
// elements. With `shared` and a Metal device, the storage is a Metal
// shared buffer -- the GPU binds it directly, and a GPU writer (Valtz's
// Metal converters) can fill a leased beat without a CPU pass. Falls
// back to CPU memory otherwise, and for an empty tensor. False only when
// even that fails.
bool allocate_storage(vpipe::TensorBeat& tb,
                      vpipe::metal_compute::MetalCompute* mc, bool shared);

// Copy `b`'s elements, through any strides, into `dst` in C order.
void gather(const vpipe::DataBuffer& b, std::uint8_t* dst);

// {storage, mtl_buffer, mtl_offset} for `tb`, into `out` (an object).
void describe_storage(const vpipe::TensorBeat& tb, vpipe::FlexData& out);

vpipe::FlexData int_array(const std::vector<std::int64_t>& v);

}

#endif
