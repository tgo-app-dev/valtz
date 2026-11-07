#include "checkpoint-reader.h"

#include "generative-models/llama3/metal-llama-weights.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace valtz::engine::vp {

namespace {

using vpipe::genai::MetalLlamaWeights;

// What load() reads with pread_into; the rest it copies from its map.
constexpr std::uint64_t kPreadAtLeast = 1ull << 20;

class VpipeCheckpoint final : public models::CheckpointReader {
public:
  VpipeCheckpoint(MetalLlamaWeights w, std::uint64_t data_start)
      : _w(std::move(w))
  {
    struct Entry {
      Tensor      t;
      std::string name;
    };
    std::vector<Entry> es;
    for (auto& name : _w.tensor_names()) {
      const MetalLlamaWeights::TensorInfo* ti = _w.info(name);
      if (!ti || ti->shard < 0 || ti->nbytes < kPreadAtLeast) {
        continue;
      }
      es.push_back({{data_start + ti->offset, ti->nbytes}, std::move(name)});
    }
    std::ranges::sort(es, {}, [](const Entry& e) { return e.t.offset; });
    for (auto& e : es) {
      _tensors.push_back(e.t);
      _names.push_back(std::move(e.name));
    }
  }

  const std::vector<Tensor>& tensors() const override { return _tensors; }

  bool read(std::size_t index, void* dst, std::size_t cap) override
  {
    return index < _names.size() &&
           _w.pread_into(_names[index], dst, cap, /*uncached=*/true);
  }

private:
  MetalLlamaWeights        _w;
  std::vector<Tensor>      _tensors;
  std::vector<std::string> _names;
};

// Where a safetensors file's data starts -- after its 8-byte length and
// the header -- which the tensors' offsets count from.
std::optional<std::uint64_t>
data_start(const std::filesystem::path& file)
{
  std::ifstream in(file, std::ios::binary);
  unsigned char b[8] = {};
  if (!in.read(reinterpret_cast<char*>(b), 8)) {
    return std::nullopt;
  }
  std::uint64_t n = 0;
  for (int i = 7; i >= 0; --i) {
    n = n << 8 | b[i];
  }
  return 8 + n;
}

}  // namespace

std::unique_ptr<models::CheckpointReader>
open_checkpoint(const std::filesystem::path& file)
{
  if (file.extension() != ".safetensors") {
    return nullptr;
  }
  const auto start = data_start(file);
  if (!start) {
    return nullptr;
  }
  auto w = MetalLlamaWeights::open(file.string());
  if (!w) {
    return nullptr;
  }
  return std::make_unique<VpipeCheckpoint>(std::move(*w), *start);
}

}  // namespace valtz::engine::vp
