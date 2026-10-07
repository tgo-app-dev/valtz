#include "valtz/engine/tensor.h"

namespace valtz::engine {

std::size_t
Tensor::element_size(DType d)
{
  switch (d) {
  case DType::U8:   return 1;
  case DType::F16:  return 2;
  case DType::BF16: return 2;
  case DType::F32:  return 4;
  }
  return 0;
}

std::size_t
Tensor::element_count() const
{
  std::size_t n = 1;
  for (auto d : shape) {
    n *= static_cast<std::size_t>(d);
  }
  return shape.empty() ? 0 : n;
}

bool
Tensor::contiguous() const
{
  if (strides.empty()) {
    return true;
  }
  std::int64_t running = 1;
  for (std::size_t i = shape.size(); i-- > 0;) {
    if (strides[i] != running) {
      return false;
    }
    running *= shape[i];
  }
  return true;
}

TensorPtr
make_owned_tensor(Tensor::DType dt, std::vector<std::int64_t> shape,
                  std::vector<std::uint8_t> bytes, Json meta)
{
  auto store = std::make_shared<std::vector<std::uint8_t>>(std::move(bytes));
  auto t = std::make_shared<Tensor>();
  t->dtype = dt;
  t->shape = std::move(shape);
  t->data = store->data();
  t->byte_size = store->size();
  t->meta = std::move(meta);
  t->owner = std::move(store);
  return t;
}

bool
dtype_from_str(std::string_view s, Tensor::DType& out)
{
  if (s == "u8") {
    out = Tensor::DType::U8;
  } else if (s == "f16") {
    out = Tensor::DType::F16;
  } else if (s == "bf16") {
    out = Tensor::DType::BF16;
  } else if (s == "f32") {
    out = Tensor::DType::F32;
  } else {
    return false;
  }
  return true;
}

}
