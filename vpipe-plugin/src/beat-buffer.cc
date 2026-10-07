#include "beat-buffer.h"

#include "valtz-vpipe/exchange.h"

#include "apple-silicon/metal-compute/metal-compute.h"
#include "apple-silicon/metal-compute/shared-buffer.h"

#include <CoreFoundation/CoreFoundation.h>

#include <cstring>

namespace valtz::plugin {

namespace ex = valtz::exchange;
using vpipe::DataBuffer;
using vpipe::ElementType;
using vpipe::FlexData;
using vpipe::TensorBeat;

namespace {

// An ExternalStorageHandle deleter takes only the buffer, so the beat
// holds a retain of its own (CFRetain: an MTLBuffer is an Objective-C
// object) and this drops it.
void
release_retained(void* mtl_buffer)
{
  if (mtl_buffer) {
    CFRelease(mtl_buffer);
  }
}

}

ElementType
element_type_of(TensorBeat::DType d) noexcept
{
  switch (d) {
  case TensorBeat::DType::U8:   return ElementType::U8;
  case TensorBeat::DType::I8:   return ElementType::I8;
  case TensorBeat::DType::Bf16: return ElementType::BF16;
  case TensorBeat::DType::F16:  return ElementType::F16;
  case TensorBeat::DType::F32:  return ElementType::F32;
  }
  return ElementType::U8;
}

bool
tensor_dtype_of(ElementType t, TensorBeat::DType* out) noexcept
{
  TensorBeat::DType d{};
  switch (t) {
  case ElementType::U8:    d = TensorBeat::DType::U8;   break;
  case ElementType::Bytes: d = TensorBeat::DType::U8;   break;
  case ElementType::I8:    d = TensorBeat::DType::I8;   break;
  case ElementType::BF16:  d = TensorBeat::DType::Bf16; break;
  case ElementType::F16:   d = TensorBeat::DType::F16;  break;
  case ElementType::F32:   d = TensorBeat::DType::F32;  break;
  default: return false;
  }
  if (out) {
    *out = d;
  }
  return true;
}

DataBuffer
tensor_view(const TensorBeat& tb, std::string name,
            std::shared_ptr<void> owner, bool writable)
{
  const std::size_t es = tb.element_byte_size();
  DataBuffer b;
  b.name = std::move(name);
  b.layout.type = element_type_of(tb.dtype);
  b.layout.shape.assign(tb.shape.begin(), tb.shape.end());
  // TensorBeat strides are in elements; a DataBuffer's are in bytes.
  for (std::int64_t s : tb.strides) {
    b.layout.strides.push_back(s * static_cast<std::int64_t>(es));
  }
  const std::size_t off = static_cast<std::size_t>(tb.storage_offset) * es;
  const std::size_t total = tb.byte_size();
  b.data = const_cast<std::uint8_t*>(tb.bytes_()) + off;
  b.size = total > off ? total - off : 0;
  b.writable = writable;
  b.owner = std::move(owner);
  return b;
}

bool
allocate_storage(TensorBeat& tb, vpipe::metal_compute::MetalCompute* mc,
                 bool shared)
{
  tb.strides.clear();
  tb.storage_offset = 0;
  tb.external.reset();
  const std::size_t n = tb.element_count() * tb.element_byte_size();
  if (shared && n > 0 && mc && mc->valid()) {
    vpipe::metal_compute::SharedBuffer sb = mc->make_shared_buffer(n);
    if (!sb.empty()) {
      MTL::Buffer* buf = sb.mtl_buffer();
      CFRetain(buf);  // the beat's own reference; sb drops its one below
      auto h = std::make_unique<vpipe::ExternalStorageHandle>();
      h->mtl_buffer = buf;
      h->contents = static_cast<std::uint8_t*>(sb.contents());
      h->byte_size = n;
      h->deleter = &release_retained;
      tb.data.clear();
      tb.external = std::move(h);
      return true;
    }
  }
  tb.resize_contiguous(tb.element_count());
  return tb.data.size() == n;
}

void
gather(const DataBuffer& b, std::uint8_t* dst)
{
  const auto n = static_cast<std::size_t>(b.layout.element_count());
  const std::size_t es = vpipe::element_size(b.layout.type);
  const auto* src = static_cast<const std::uint8_t*>(b.data);
  if (n == 0) {
    return;
  }
  if (b.layout.is_contiguous() || b.layout.shape.empty()) {
    std::memcpy(dst, src, n * es);
    return;
  }
  // Row by row along the innermost dimension: one memcpy per row when
  // it is packed, one per element when it is not.
  const auto& shape = b.layout.shape;
  const auto& st = b.layout.strides;
  const std::size_t rank = shape.size();
  const auto inner = static_cast<std::size_t>(shape[rank - 1]);
  const bool packed = st[rank - 1] == static_cast<std::int64_t>(es);
  std::vector<std::int64_t> idx(rank, 0);
  for (std::size_t row = 0; row < n / inner; ++row) {
    std::int64_t at = 0;
    for (std::size_t k = 0; k + 1 < rank; ++k) {
      at += idx[k] * st[k];
    }
    if (packed) {
      std::memcpy(dst, src + at, inner * es);
      dst += inner * es;
    } else {
      for (std::size_t j = 0; j < inner; ++j) {
        std::memcpy(dst, src + at + static_cast<std::int64_t>(j) *
                                        st[rank - 1], es);
        dst += es;
      }
    }
    for (std::size_t k = rank - 1; k-- > 0;) {
      if (++idx[k] < shape[k]) {
        break;
      }
      idx[k] = 0;
    }
  }
}

void
describe_storage(const TensorBeat& tb, FlexData& out)
{
  auto o = out.as_object();
  const bool shared = tb.external && tb.external->mtl_buffer;
  o.insert_or_assign(ex::kStorage, FlexData::make_string(
      shared ? ex::kStorageShared : ex::kStorageCpu));
  o.insert_or_assign(ex::kMtlBuffer, FlexData::make_uint(
      shared ? reinterpret_cast<std::uintptr_t>(tb.external->mtl_buffer)
             : 0));
  o.insert_or_assign(ex::kMtlOffset, FlexData::make_uint(
      shared ? static_cast<std::uint64_t>(tb.storage_offset) *
                   tb.element_byte_size()
             : 0));
}

FlexData
int_array(const std::vector<std::int64_t>& v)
{
  FlexData a = FlexData::make_array();
  for (std::int64_t x : v) {
    a.as_array().push_back(FlexData::make_int(x));
  }
  return a;
}

}
