// valtz-sink: every beat that reaches it goes to Valtz, in place.
//
// WHY A SINK AND NOT vpipe's external-tap. external-tap is a passthrough:
// the beat it exposes is about to travel downstream, so it must HOLD the
// pipeline while the caller reads. Valtz reads a preview on the UI's
// schedule, and a denoise loop must never wait for a window to redraw. A
// sink has no downstream: once it replies, nothing else will ever see
// the beat, so it hands the bytes over (the reply's owner keeps them)
// and moves on at once. What bounds memory is the queue, not a hold:
//
//   queue   keep every beat; at `depth` the sink stops reading, so the
//           producer backs up and nothing is lost.
//   latest  keep the newest `depth`; an older beat is dropped as a newer
//           one arrives, and the producer never waits.
//
// Serving. `next` commands wait in FIFO order for beats; beats wait in
// FIFO order for `next`. The stage sleeps on both at once (read_any with
// commands), so a beat and a command are matched the moment the second
// of them arrives.
//
// The end is a HANDSHAKE (`drain`, the default): queued beats are still
// handed out, and the stage ends only once a `next` has been ANSWERED
// with `eos`. Ending as soon as the queue emptied would race the
// reader's following `next`, which would then be refused by a stage
// that is gone -- indistinguishable, for the reader, from a failure.
// The price: a draining sink nobody reads ends only at stop. A graph
// no host reads sets drain = false and the sink ends with its input.

#include "stages.h"

#include "beat-buffer.h"
#include "valtz-vpipe/exchange.h"

#include "apple-silicon/tensor-beat.h"
#include "common/flex-data.h"
#include "common/job.h"
#include "common/vpipe-format.h"
#include "pipeline/runtime-context.h"
#include "pipeline/stage-command.h"
#include "pipeline/typed-stage.h"
#include "plugin/plugin-context.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace valtz::plugin {

namespace {

namespace ex = valtz::exchange;
using vpipe::BufferSpec;
using vpipe::CommandSpec;
using vpipe::ConfigKey;
using vpipe::ConfigType;
using vpipe::FlexData;
using vpipe::PortSpec;
using vpipe::StageSpec;

const PortSpec kIports[] = {
  {.name = "in",
   .doc = "any beat: a tensor is handed to Valtz in place, anything "
          "else as JSON",
   .type = nullptr},
};

const ConfigKey kAttrs[] = {
  {.key = ex::kSinkPolicy, .type = ConfigType::String,
   .doc = "\"queue\": keep every beat, and stop reading at `depth` so the "
          "producer backs up. \"latest\": keep the newest `depth`, "
          "dropping older ones; the producer never waits",
   .def_str = ex::kSinkPolicyQueue},
  {.key = ex::kSinkDepth, .type = ConfigType::Uint,
   .doc = "beats queued at most (at least 1)", .def_uint = 4},
  {.key = ex::kSinkDrain, .type = ConfigType::Bool,
   .doc = "at end of stream: hand out what is queued and end once a "
          "`next` has been answered `eos` (true), or drop the queue and "
          "end with the input (false: for a graph no host reads)",
   .def_bool = true},
};

const ConfigKey kNextResults[] = {
  {.key = ex::kEos, .type = ConfigType::Bool,
   .doc = "the stream ended and the queue is empty; nothing else is set"},
  {.key = ex::kSeq, .type = ConfigType::Uint,
   .doc = "the beat's 1-based index into the sink this run"},
  {.key = ex::kDropped, .type = ConfigType::Uint,
   .doc = "beats dropped (policy latest) since the previous reply"},
  {.key = ex::kKind, .type = ConfigType::String,
   .doc = "\"tensor\" (buffer `data`), \"data\" (`data`) or \"other\""},
  {.key = ex::kType, .type = ConfigType::String, .doc = "element type"},
  {.key = ex::kShape, .type = ConfigType::Array, .doc = "the shape"},
  {.key = ex::kStrides, .type = ConfigType::Array,
   .doc = "byte strides; [] when C-contiguous"},
  {.key = ex::kSideband, .type = ConfigType::Any,
   .doc = "the tensor's sideband (null when none)"},
  {.key = ex::kStorage, .type = ConfigType::String,
   .doc = "\"shared\" (a Metal buffer) or \"cpu\""},
  {.key = ex::kMtlBuffer, .type = ConfigType::Uint,
   .doc = "in-process: the MTL::Buffer* holding the bytes, 0 if none; "
          "valid while buffer `data`'s owner is"},
  {.key = ex::kMtlOffset, .type = ConfigType::Uint,
   .doc = "byte offset of buffer `data` in that MTL::Buffer"},
  {.key = ex::kData, .type = ConfigType::Any,
   .doc = "a data beat's value"},
  {.key = ex::kDescribe, .type = ConfigType::String,
   .doc = "what any other beat is"},
};
const BufferSpec kNextOut[] = {
  {.name = ex::kBufData,
   .doc = "the tensor beat's own bytes, read-only, never reused: they "
          "stay as replied while any copy of this buffer's owner lives",
   .type = kTensorTypes, .optional = true},
};

const ConfigKey kStatsResults[] = {
  {.key = ex::kReceived, .type = ConfigType::Uint, .doc = "beats read"},
  {.key = ex::kDelivered, .type = ConfigType::Uint,
   .doc = "beats handed out"},
  {.key = ex::kDropped, .type = ConfigType::Uint, .doc = "beats dropped"},
  {.key = ex::kQueued, .type = ConfigType::Uint, .doc = "beats waiting"},
  {.key = ex::kEos, .type = ConfigType::Bool,
   .doc = "the input has ended (queued beats may remain)"},
};

const CommandSpec kCommands[] = {
  {.name = ex::kCmdNext,
   .doc = "the oldest queued beat, in place, waiting for one if the queue "
          "is empty; `eos` once the stream has ended and the queue is "
          "drained. Never holds the pipeline",
   .results = kNextResults, .out = kNextOut},
  {.name = ex::kCmdStats,
   .doc = "counters: received, delivered, dropped, queued, eos",
   .results = kStatsResults},
};

const StageSpec kSpec = {
  .type_name = ex::kSinkType,
  .doc = "Sink: hands every beat to the host app (Valtz) in place through "
         "the `next` command. Never holds the pipeline: the queue policy "
         "decides between back-pressure (queue) and dropping the oldest "
         "(latest).",
  .display_name = "Valtz Sink",
  .category = vpipe::StageCategory::Control,
  .iports = kIports,
  .oports = {},
  .attrs = kAttrs,
  .commands = kCommands,
};

class SinkStage final : public vpipe::TypedStage<SinkStage> {
public:
  static constexpr const char* kTypeName = ex::kSinkType;

  SinkStage(const vpipe::SessionContextIntf* s, std::string id,
            std::vector<vpipe::InEdge> iports, FlexData config)
    : vpipe::TypedStage<SinkStage>(s, std::move(id), std::move(iports),
                                   std::move(config))
  {
    allocate_oports(0);
    const std::string policy = attr_str(ex::kSinkPolicy);
    if (policy == ex::kSinkPolicyLatest) {
      _latest = true;
    } else if (policy != ex::kSinkPolicyQueue) {
      fail_config(vpipe::fmt("{}('{}'): policy '{}' is neither \"queue\" "
                             "nor \"latest\"", kTypeName, this->id(),
                             policy));
    }
    _depth = std::max<std::uint64_t>(1, attr_uint(ex::kSinkDepth));
    _drain = attr_bool(ex::kSinkDrain);
  }

  const StageSpec& spec() const noexcept override { return kSpec; }

  void
  reset_run_state() override
  {
    _queue.clear();
    _waiting.clear();
    _received = _delivered = _dropped = _dropped_since = 0;
    _told_eos = false;
  }

  vpipe::Job
  process(vpipe::RuntimeContext& ctx) override
  {
    if (ctx.stop_requested()) {
      ctx.signal_done();
      co_return;
    }
    // Already over (drain = false with the input ended): do not wait.
    if (finished_(ctx)) {
      take_commands_(ctx);
      end_();
      ctx.signal_done();
      co_return;
    }
    // Wait for a beat -- only while there is room for one, and only on a
    // port still open (a port at EOS always reads as ready) -- or for a
    // command, whichever comes first.
    const bool room = _latest || _queue.size() < _depth;
    std::vector<unsigned> ports;
    if (room && !ctx.eos(0)) {
      ports.push_back(0);
    }
    co_await ctx.read_any(std::move(ports), /*commands=*/true);

    if (room) {
      for (std::uint32_t n = ctx.backlog(0); n > 0; --n) {
        if (!_latest && _queue.size() >= _depth) {
          break;
        }
        auto beat = co_await ctx.read(0);
        if (!beat) {
          break;
        }
        accept_(std::move(beat));
      }
    }
    take_commands_(ctx);
    serve_();
    if (ctx.eos(0) && _queue.empty()) {
      tell_eos_();
    }

    if (finished_(ctx)) {
      end_();
      ctx.signal_done();
    } else if (ctx.stop_requested()) {
      ctx.signal_done();
    }
  }

private:
  bool
  finished_(vpipe::RuntimeContext& ctx) const
  {
    return ctx.eos(0) && (!_drain || (_queue.empty() && _told_eos));
  }

  void
  take_commands_(vpipe::RuntimeContext& ctx)
  {
    while (auto cmd = ctx.try_command()) {
      if (cmd->name() == ex::kCmdStats) {
        cmd->reply(stats_(ctx));
      } else {
        _waiting.push_back(std::move(cmd));
      }
    }
  }

  struct Held {
    std::uint64_t                           seq = 0;
    std::shared_ptr<vpipe::BeatPayloadIntf> beat;
  };

  void
  accept_(std::unique_ptr<vpipe::BeatPayloadIntf> beat)
  {
    if (_latest && _queue.size() >= _depth) {
      _queue.pop_front();
      ++_dropped;
      ++_dropped_since;
    }
    _queue.push_back({++_received, std::move(beat)});
  }

  // Match queued beats with waiting commands, oldest with oldest.
  void
  serve_()
  {
    while (!_waiting.empty()) {
      auto& cmd = _waiting.front();
      if (cmd->closed()) {  // the caller gave up waiting
        _waiting.pop_front();
        continue;
      }
      if (_queue.empty()) {
        break;
      }
      Held h = std::move(_queue.front());
      _queue.pop_front();
      if (answer_(*cmd, h)) {
        ++_delivered;
        _dropped_since = 0;
      } else if (cmd->closed()) {
        _queue.push_front(std::move(h));  // closed as we replied: keep it
      } else {
        // The host refused our own reply against our own spec: a bug
        // here, and the command is already Failed with the reason.
        session()->warn(vpipe::fmt("{}('{}'): reply to `next` refused; "
                                   "beat {} dropped", kTypeName, id(),
                                   h.seq));
        ++_dropped;
      }
      _waiting.pop_front();
    }
  }

  bool
  answer_(vpipe::StageCommand& cmd, const Held& h)
  {
    FlexData r = FlexData::make_object();
    auto ro = r.as_object();
    ro.insert(ex::kEos, FlexData::make_bool(false));
    ro.insert(ex::kSeq, FlexData::make_uint(h.seq));
    ro.insert(ex::kDropped, FlexData::make_uint(_dropped_since));
    std::vector<vpipe::DataBuffer> out;
    if (auto* tb = dynamic_cast<const vpipe::TensorBeatPayload*>(
            h.beat.get())) {
      ro.insert(ex::kKind, FlexData::make_string(ex::kKindTensor));
      ro.insert(ex::kType, FlexData::make_string(tb->dtype_name()));
      ro.insert(ex::kShape, int_array(tb->shape));
      std::vector<std::int64_t> strides;
      for (std::int64_t s : tb->strides) {
        strides.push_back(s * static_cast<std::int64_t>(
                                  tb->element_byte_size()));
      }
      ro.insert(ex::kStrides, int_array(strides));
      ro.insert(ex::kSideband, tb->sideband);
      describe_storage(*tb, r);
      // The owner IS the beat: whoever holds a copy of the buffer holds
      // the beat, and the sink lets go of it right after this reply.
      if (tb->byte_size() > 0) {
        out.push_back(tensor_view(*tb, ex::kBufData, h.beat, false));
      }
    } else if (auto* fd = dynamic_cast<const vpipe::FlexDataPayload*>(
                   h.beat.get())) {
      ro.insert(ex::kKind, FlexData::make_string(ex::kKindData));
      ro.insert(ex::kData, fd->data);
    } else {
      ro.insert(ex::kKind, FlexData::make_string(ex::kKindOther));
      ro.insert(ex::kDescribe, FlexData::make_string(h.beat->describe()));
    }
    return cmd.reply(std::move(r), std::move(out));
  }

  FlexData
  stats_(vpipe::RuntimeContext& ctx) const
  {
    FlexData r = FlexData::make_object();
    auto ro = r.as_object();
    ro.insert(ex::kReceived, FlexData::make_uint(_received));
    ro.insert(ex::kDelivered, FlexData::make_uint(_delivered));
    ro.insert(ex::kDropped, FlexData::make_uint(_dropped));
    ro.insert(ex::kQueued, FlexData::make_uint(_queue.size()));
    ro.insert(ex::kEos, FlexData::make_bool(ctx.eos(0)));
    return r;
  }

  // Answer every waiting `next`: the stream is over.
  void
  tell_eos_()
  {
    for (auto& cmd : _waiting) {
      if (cmd->closed()) {
        continue;
      }
      FlexData r = FlexData::make_object();
      r.as_object().insert(ex::kEos, FlexData::make_bool(true));
      r.as_object().insert(ex::kDropped, FlexData::make_uint(_dropped_since));
      if (cmd->reply(std::move(r))) {
        _told_eos = true;
      }
    }
    _waiting.clear();
  }

  // The stage ends: what is still queued is dropped (drain = false), and
  // whoever is waiting learns the end.
  void
  end_()
  {
    _dropped += _queue.size();
    _queue.clear();
    tell_eos_();
  }

  bool                                             _latest = false;
  std::uint64_t                                    _depth = 4;
  bool                                             _drain = true;
  std::deque<Held>                                 _queue;
  std::deque<std::shared_ptr<vpipe::StageCommand>> _waiting;
  std::uint64_t                                    _received = 0;
  std::uint64_t                                    _delivered = 0;
  std::uint64_t                                    _dropped = 0;
  std::uint64_t                                    _dropped_since = 0;
  bool                                             _told_eos = false;
};

}

void
register_sink(vpipe::VpipePluginContext* ctx)
{
  ctx->register_stage<SinkStage>(&kSpec);
}

}
