#include "valtz/controller/asset-ops.h"

#include "valtz/engine/engine.h"

#include <algorithm>
#include <array>

namespace valtz {

namespace {

using models::Capability;
using project::AssetKind;

// A sound, or a clip's sound: its speech and the sounds heard (§4h).
constexpr std::array<AssetOpTake, 2> kTranscribe = {{
  {AssetKind::Audio, engine::kOpTranscribeAudio, Capability::Transcribe},
  {AssetKind::Video, engine::kOpTranscribeAudio, Capability::Transcribe},
}};
constexpr std::array<std::string_view, 1> kTranscript = {"transcript"};

// A clip, told scene by scene and whole (§4i).
constexpr std::array<AssetOpTake, 1> kSummarize = {{
  {AssetKind::Video, engine::kOpSummarizeVideo, Capability::VideoSummary},
}};
constexpr std::array<std::string_view, 1> kSummary = {"summary"};

// A layer's picture (VOSR) or clip (FlashVSR), at its scale (§4f).
constexpr std::array<AssetOpTake, 2> kUpscale = {{
  {AssetKind::Image, engine::kOpUpscaleImage, Capability::UpscaleImage},
  {AssetKind::Video, engine::kOpUpscaleVideo, Capability::UpscaleVideo},
}};
constexpr std::array<std::string_view, 1> kUpscaled = {"upscaled"};

// A sound -- a song -- split into its voice and the rest (§4j).
constexpr std::array<AssetOpTake, 1> kSeparate = {{
  {AssetKind::Audio, engine::kOpSeparateAudio, Capability::AudioSeparate},
}};
constexpr std::array<std::string_view, 2> kStems = {"vocals",
                                                    "accompaniment"};

constexpr std::array<AssetOperation, 4> kOperations = {{
  {"transcribe", "Transcribe", kTranscribe, false, kTranscript},
  {"summarize", "Summarize Video", kSummarize, false, kSummary},
  {"separate", "Separate Vocals and Accompaniment", kSeparate, false,
   kStems},
  {"upscale", "Render Upscaled", kUpscale, true, kUpscaled},
}};

}

std::span<const AssetOperation>
asset_operations()
{
  return kOperations;
}

const AssetOperation*
find_asset_operation(std::string_view name)
{
  const auto it = std::ranges::find(kOperations, name,
                                    &AssetOperation::name);
  return it == kOperations.end() ? nullptr : &*it;
}

const AssetOperation*
asset_operation_of(std::string_view op)
{
  for (const auto& o : kOperations) {
    for (const auto& t : o.takes) {
      if (t.op == op) {
        return &o;
      }
    }
  }
  return nullptr;
}

const AssetOpTake*
asset_op_take(const AssetOperation& op, project::AssetKind kind)
{
  for (const auto& t : op.takes) {
    if (t.kind == kind) {
      return &t;
    }
  }
  return nullptr;
}

}
