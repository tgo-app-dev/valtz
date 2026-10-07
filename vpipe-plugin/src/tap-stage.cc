// valtz-tap: frames go by and are counted; the host waits on the count.
//
// WHY A TAP. Valtz feeds an upscaler (FlashVSR) a clip it restores a
// group of frames at a time, and what a group becomes inside the graph --
// frames resampled to the output size, the projection's rows, the
// denoiser's window, the decoder's working set -- is many times the
// source frames it came from. So the host must not feed the next group
// before the last has come OUT: a feeder at full speed, or paced only by
// its own port, fills every edge between it and the denoiser with
// upscaled frames. Valtz knows exactly how many frames each group takes
// and gives; what it cannot see from outside is how far the graph has
// got. The tap says so, by a stage command: `wait` replies once that
// many frames have gone by. Past `max_frames` it CUTS: a group padded
// to the model's length gives frames the clip never had, and they go no
// further.
//
// Serving: like the sink, the stage sleeps on its port and its commands
// at once, so a `wait` is answered the moment the frame it waits for has
// been written on.

#include "stages.h"

#include "valtz-vpipe/exchange.h"

#include "apple-silicon/tensor-beat.h"
#include "common/flex-data.h"
#include "common/job.h"
#include "common/vpipe-format.h"
#include "pipeline/runtime-context.h"
#include "pipeline/stage-command.h"
#include "pipeline/typed-stage.h"
#include "plugin/plugin-context.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace valtz::plugin {

namespace {

namespace ex = valtz::exchange;
using vpipe::CommandSpec;
using vpipe::ConfigKey;
using vpipe::ConfigType;
using vpipe::FlexData;
using vpipe::PortSpec;
using vpipe::StageSpec;

const PortSpec kIports[] = {
  {.name = "in", .doc = "any beat; tensors are counted as frames",
   .type = nullptr},
};
const PortSpec kOports[] = {
  {.name = "out", .doc = "the same beats, by reference, up to the cut",
   .type = nullptr},
};

const ConfigKey kAttrs[] = {
  {.key = ex::kTapMaxFrames, .type = ConfigType::Uint,
   .doc = "frames passed at most; a beat past them is dropped (0: all)",
   .def_uint = 0},
};

const ConfigKey kWaitArgs[] = {
  {.key = ex::kFrames, .type = ConfigType::Uint, .required = true,
   .doc = "reply once this many frames have gone by"},
};
const ConfigKey kCountResults[] = {
  {.key = ex::kPassed, .type = ConfigType::Uint,
   .doc = "frames passed on so far"},
  {.key = ex::kCut, .type = ConfigType::Uint,
   .doc = "frames dropped past max_frames"},
  {.key = ex::kEos, .type = ConfigType::Bool,
   .doc = "the input has ended: no more frames will pass"},
};

const CommandSpec kCommands[] = {
  {.name = ex::kCmdWait,
   .doc = "reply once `frames` frames have gone by, or at the end of the "
          "stream (eos); never holds the pipeline",
   .args = kWaitArgs, .results = kCountResults},
  {.name = ex::kCmdStats, .doc = "the counts now: passed, cut, eos",
   .results = kCountResults},
};

const StageSpec kSpec = {
  .type_name = ex::kTapType,
  .doc = "Passthrough: counts the frames going by, so the host (Valtz) can "
         "feed a graph by what has come out of it -- `wait` replies once a "
         "count is reached -- and cuts the stream at max_frames.",
  .display_name = "Valtz Tap",
  .category = vpipe::StageCategory::Control,
  .iports = kIports,
  .oports = kOports,
  .attrs = kAttrs,
  .commands = kCommands,
};

// Frames in a beat: a [F, C, H, W] tensor is F; any other tensor is one
// picture; anything else (data, end markers) is none.
std::uint64_t
frames_in(const vpipe::BeatPayloadIntf& beat)
{
  const auto* tb = dynamic_cast<const vpipe::TensorBeatPayload*>(&beat);
  if (tb == nullptr) {
    return 0;
  }
  if (tb->shape.size() == 4) {
    return tb->shape[0] > 0 ? static_cast<std::uint64_t>(tb->shape[0]) : 0;
  }
  return 1;
}

class TapStage final : public vpipe::TypedStage<TapStage> {
public:
  static constexpr const char* kTypeName = ex::kTapType;

  TapStage(const vpipe::SessionContextIntf* s, std::string id,
           std::vector<vpipe::InEdge> iports, FlexData config)
    : vpipe::TypedStage<TapStage>(s, std::move(id), std::move(iports),
                                  std::move(config))
  {
    allocate_oports(1);
    _max = attr_uint(ex::kTapMaxFrames);
  }

  const StageSpec& spec() const noexcept override { return kSpec; }

  void
  reset_run_state() override
  {
    _passed = _cut = 0;
    _waiting.clear();
  }

  vpipe::Job
  process(vpipe::RuntimeContext& ctx) override
  {
    if (ctx.stop_requested() || ctx.eos(0)) {
      take_commands_(ctx);
      serve_(/*ended=*/true);
      ctx.signal_done();
      co_return;
    }
    co_await ctx.read_any({0}, /*commands=*/true);
    for (std::uint32_t n = ctx.backlog(0); n > 0; --n) {
      auto beat = co_await ctx.read(0);
      if (!beat) {
        break;
      }
      const std::uint64_t f = frames_in(*beat);
      if (_max > 0 && f > 0 && _passed >= _max) {
        _cut += f;  // past the cut: the clip has no such frames
        continue;
      }
      co_await ctx.write(0, std::move(beat));
      _passed += f;
      // Answered as each frame goes on, not after the whole backlog: a
      // host waiting for the group's last frame feeds the next at once.
      take_commands_(ctx);
      serve_(false);
    }
    take_commands_(ctx);
    serve_(false);
    if (ctx.eos(0) || ctx.stop_requested()) {
      serve_(true);
      ctx.signal_done();
    }
  }

private:
  void
  take_commands_(vpipe::RuntimeContext& ctx)
  {
    while (auto cmd = ctx.try_command()) {
      if (cmd->name() == ex::kCmdStats) {
        cmd->reply(counts_(false));
      } else {
        _waiting.push_back(std::move(cmd));
      }
    }
  }

  // Reply to every `wait` its count has reached -- and, at the end, to
  // every one: no more frames will come.
  void
  serve_(bool ended)
  {
    for (auto it = _waiting.begin(); it != _waiting.end();) {
      auto& cmd = *it;
      if (cmd->closed()) {
        it = _waiting.erase(it);
        continue;
      }
      const std::uint64_t want =
          cmd->args().as_object().at(ex::kFrames).as_uint(0);
      if (_passed >= want || ended) {
        cmd->reply(counts_(ended));
        it = _waiting.erase(it);
      } else {
        ++it;
      }
    }
  }

  FlexData
  counts_(bool ended) const
  {
    FlexData r = FlexData::make_object();
    auto ro = r.as_object();
    ro.insert(ex::kPassed, FlexData::make_uint(_passed));
    ro.insert(ex::kCut, FlexData::make_uint(_cut));
    ro.insert(ex::kEos, FlexData::make_bool(ended));
    return r;
  }

  std::uint64_t                                    _max = 0;
  std::uint64_t                                    _passed = 0;
  std::uint64_t                                    _cut = 0;
  std::deque<std::shared_ptr<vpipe::StageCommand>> _waiting;
};

}

void
register_tap(vpipe::VpipePluginContext* ctx)
{
  ctx->register_stage<TapStage>(&kSpec);
}

}
