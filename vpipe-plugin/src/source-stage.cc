// valtz-source: beats from Valtz into a graph.
//
// WHY NOT vpipe's external-input. Its lease is CPU memory. Valtz's media
// path decodes and converts to a model's color space on the GPU (DESIGN
// §7), so the beat Valtz fills should be a Metal shared buffer that its
// kernels write and vpipe's kernels bind, with no CPU pass and no upload
// in between. That is the difference, and the reason the lease reply
// carries the MTL::Buffer. The source also emits JSON beats (`emit`),
// for stages that take structured data.
//
// One command at a time, in order: a lease holds the stage until it is
// closed, so commands queued behind it wait, and beats leave in the
// order Valtz sent them.

#include "stages.h"

#include "beat-buffer.h"
#include "valtz-vpipe/exchange.h"

#include "apple-silicon/tensor-beat.h"
#include "common/flex-data.h"
#include "common/job.h"
#include "common/vpipe-format.h"
#include "interfaces/session-context-intf.h"
#include "interfaces/session-services-intf.h"
#include "pipeline/runtime-context.h"
#include "pipeline/stage-command.h"
#include "pipeline/typed-stage.h"
#include "plugin/plugin-context.h"

#include <cstdint>
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
using vpipe::TensorBeat;

const PortSpec kOports[] = {
  {.name = "out",
   .doc = "one beat per closed lease, push or emit: TensorBeats, and "
          "FlexData beats from emit",
   .type = nullptr},
};

const ConfigKey kSeqResult[] = {
  {.key = ex::kSeq, .type = ConfigType::Uint,
   .doc = "the beat's 1-based index on the oport this run"},
};
const ConfigKey kLeaseArgs[] = {
  {.key = ex::kType, .type = ConfigType::String, .required = true,
   .doc = "element type: u8, i8, f16, bf16 or f32"},
  {.key = ex::kShape, .type = ConfigType::Array, .required = true,
   .doc = "the beat's shape, e.g. [3, 1024, 1024]"},
  {.key = ex::kSideband, .type = ConfigType::Object,
   .doc = "attached to the beat as its sideband"},
  {.key = ex::kStorage, .type = ConfigType::String,
   .doc = "\"shared\": a Metal shared buffer the GPU binds directly "
          "(falls back to CPU memory without a Metal device). \"cpu\"",
   .def_str = ex::kStorageShared},
};
const ConfigKey kLeaseResults[] = {
  {.key = ex::kSeq, .type = ConfigType::Uint,
   .doc = "the index the beat will have on the oport"},
  {.key = ex::kStorage, .type = ConfigType::String,
   .doc = "where the beat lives: \"shared\" or \"cpu\""},
  {.key = ex::kMtlBuffer, .type = ConfigType::Uint,
   .doc = "in-process: the MTL::Buffer* to write on the GPU; 0 for cpu"},
  {.key = ex::kMtlOffset, .type = ConfigType::Uint,
   .doc = "byte offset of buffer `data` in it"},
};
const BufferSpec kLeaseOut[] = {
  {.name = ex::kBufData,
   .doc = "the beat's own storage, C-contiguous: fill it, then close the "
          "command to emit it -- with no copy once every view is dropped",
   .type = kTensorTypes, .writable = true, .contiguous = true},
};

const ConfigKey kPushArgs[] = {
  {.key = ex::kSideband, .type = ConfigType::Object,
   .doc = "attached to the beat as its sideband"},
  {.key = ex::kStorage, .type = ConfigType::String,
   .doc = "\"shared\": a Metal shared buffer (falls back to CPU memory "
          "without a Metal device). \"cpu\": CPU memory -- as a beat "
          "made there is fed back, for a stage that reads its CPU bytes",
   .def_str = ex::kStorageShared},
  {.key = ex::kType, .type = ConfigType::String,
   .doc = "with no buffer: the element type of a tensor with no bytes"},
  {.key = ex::kShape, .type = ConfigType::Array,
   .doc = "with no buffer: its shape, one dimension 0 (e.g. [0, 96])"},
  {.key = ex::kList, .type = ConfigType::Bool,
   .doc = "the tensor is an item of a LIST, emitted as one beat once all "
          "its `items` are in", .def_bool = false},
  {.key = ex::kItems, .type = ConfigType::Uint,
   .doc = "with `list`: how many items (0: an empty list, emitted at "
          "once)"},
  {.key = ex::kItem, .type = ConfigType::Uint,
   .doc = "its place in the list: items come in order, from 0"},
  {.key = ex::kListSideband, .type = ConfigType::Object,
   .doc = "the list's own sideband (taken from its first item)"},
};
const BufferSpec kPushIn[] = {
  {.name = ex::kBufData,
   .doc = "the tensor to emit, any strides; copied into engine memory, "
          "so the caller's is free again once the reply arrives. "
          "Absent: a tensor with no bytes, its `type` and `shape` said",
   .type = kTensorTypes, .optional = true},
};

const ConfigKey kEmitArgs[] = {
  {.key = ex::kData, .type = ConfigType::Any, .required = true,
   .doc = "the value to emit as one FlexData beat"},
};

const CommandSpec kCommands[] = {
  {.name = ex::kCmdLease,
   .doc = "reply with a writable beat of the given type and shape in "
          "engine memory; closing the command emits it (dropped if the "
          "pipeline stops first)",
   .args = kLeaseArgs, .results = kLeaseResults, .out = kLeaseOut,
   .holds = true},
  {.name = ex::kCmdPush,
   .doc = "emit a copy of the caller's tensor -- or, with `items`, gather "
          "it into a list emitted whole after its last item; the reply "
          "comes once the oport has taken it, so a caller that waits is "
          "paced by the graph",
   .args = kPushArgs, .results = kSeqResult, .in = kPushIn},
  {.name = ex::kCmdEmit,
   .doc = "emit a JSON value as one FlexData beat",
   .args = kEmitArgs, .results = kSeqResult},
  {.name = ex::kCmdFinish, .doc = "end the stream: consumers see EOS",
   .results = kSeqResult},
};

const StageSpec kSpec = {
  .type_name = ex::kSourceType,
  .doc = "Source: beats from the host app (Valtz). `lease` hands it a "
         "writable beat in Metal shared memory, emitted with no copy when "
         "the command closes; `push` copies a tensor in; `emit` sends "
         "JSON; `finish` ends the stream.",
  .display_name = "Valtz Source",
  .category = vpipe::StageCategory::Control,
  .iports = {},
  .oports = kOports,
  .attrs = {},
  .commands = kCommands,
};

// The shape argument, checked. Bounded well below any allocation that
// could succeed, so the product cannot overflow on the way to refusing.
bool
parse_shape(const FlexData& v, std::vector<std::int64_t>* out)
{
  constexpr std::int64_t kMaxElems = std::int64_t{1} << 40;
  std::int64_t n = 1;
  const auto a = v.as_array();
  for (std::size_t i = 0; i < a.size(); ++i) {
    const FlexData d = a[i];
    if (!d.is_int() && !d.is_uint()) {
      return false;
    }
    const std::int64_t dim = d.is_uint()
                                 ? static_cast<std::int64_t>(d.as_uint(0))
                                 : d.as_int(-1);
    if (dim < 0 || (dim > 0 && n > kMaxElems / dim)) {
      return false;
    }
    n *= dim;
    out->push_back(dim);
  }
  return true;
}

class SourceStage final : public vpipe::TypedStage<SourceStage> {
public:
  static constexpr const char* kTypeName = ex::kSourceType;

  SourceStage(const vpipe::SessionContextIntf* s, std::string id,
              std::vector<vpipe::InEdge> iports, FlexData config)
    : vpipe::TypedStage<SourceStage>(s, std::move(id), std::move(iports),
                                     std::move(config))
  {
    allocate_oports(kSpec.oports.size());
    // A few beats ahead of the consumer, no more: a clip fed frame by
    // frame -- an hour's export, 50 MB a 4K frame -- outruns its encoder,
    // and the default ring takes a thousand before it pushes back. Full,
    // the write waits, the next lease waits with it, and so does the
    // host: paced by the graph.
    set_oport_policy(0, {kAhead, vpipe::OverrunPolicy::Backpressure});
  }

  static constexpr unsigned kAhead = 4;

  const StageSpec& spec() const noexcept override { return kSpec; }

  void
  reset_run_state() override
  {
    _emitted = 0;
    _list.reset();
  }

  vpipe::Job
  process(vpipe::RuntimeContext& ctx) override
  {
    auto cmd = co_await ctx.next_command();
    if (!cmd) {  // the inbox shut: the pipeline is stopping
      ctx.signal_done();
      co_return;
    }
    const std::string_view name = cmd->name();
    if (name == ex::kCmdLease) {
      co_await lease_(ctx, std::move(cmd));
    } else if (name == ex::kCmdPush) {
      co_await push_(ctx, std::move(cmd));
    } else if (name == ex::kCmdEmit) {
      FlexData v = cmd->args().as_object().at(ex::kData);
      co_await ctx.write(0, vpipe::make_payload<vpipe::FlexDataPayload>(
                                std::move(v)));
      cmd->reply(seq_result_(++_emitted));
    } else if (name == ex::kCmdFinish) {
      cmd->reply(seq_result_(_emitted));
      ctx.signal_done();
    }
  }

private:
  static FlexData
  seq_result_(std::uint64_t seq)
  {
    FlexData r = FlexData::make_object();
    r.as_object().insert(ex::kSeq, FlexData::make_uint(seq));
    return r;
  }

  vpipe::metal_compute::MetalCompute*
  metal_() const
  {
    auto* s = session();
    return s ? s->services()->metal_compute() : nullptr;
  }

  vpipe::Job
  lease_(vpipe::RuntimeContext& ctx, std::shared_ptr<vpipe::StageCommand> cmd)
  {
    const auto args = cmd->args().as_object();
    const std::string tname(args.at(ex::kType).as_string(""));
    vpipe::ElementType et{};
    TensorBeat::DType d{};
    if (!vpipe::parse_element_type(tname, &et) ||
        et == vpipe::ElementType::Bytes || !tensor_dtype_of(et, &d)) {
      cmd->fail(vpipe::fmt("`type` '{}' is not one of {}", tname,
                           kTensorTypes)());
      co_return;
    }
    // The beat is built first and SHARED with the reply. After the close
    // it is taken back whole when the caller dropped every view, and
    // copied when the caller kept one -- that memory stays the caller's.
    auto hold = std::make_shared<vpipe::TensorBeatPayload>();
    hold->dtype = d;
    if (!parse_shape(args.at(ex::kShape), &hold->shape)) {
      cmd->fail("`shape` must be non-negative integers of a sane size");
      co_return;
    }
    const bool shared = args.at(ex::kStorage).as_string("") !=
                        std::string_view(ex::kStorageCpu);
    if (!allocate_storage(*hold, metal_(), shared)) {
      cmd->fail("out of memory for the lease");
      co_return;
    }
    const FlexData sb = args.at(ex::kSideband);
    if (sb.is_object() && sb.as_object().size() > 0) {
      hold->sideband = sb;
    }

    const std::uint64_t seq = _emitted + 1;
    FlexData r = seq_result_(seq);
    describe_storage(*hold, r);
    if (!cmd->reply(std::move(r),
                    {tensor_view(*hold, ex::kBufData, hold, true)})) {
      co_return;
    }
    co_await cmd->until_closed();
    if (cmd->cancelled()) {
      co_return;
    }
    std::unique_ptr<vpipe::BeatPayloadIntf> out;
    if (hold.use_count() == 1) {
      out = vpipe::make_payload<vpipe::TensorBeatPayload>(std::move(*hold));
    } else {
      out = hold->clone();
    }
    hold.reset();
    co_await ctx.write(0, std::move(out));
    _emitted = seq;
  }

  vpipe::Job
  push_(vpipe::RuntimeContext& ctx, std::shared_ptr<vpipe::StageCommand> cmd)
  {
    const auto args = cmd->args().as_object();
    // (Every declared argument is there, at its default when not sent:
    // `list` says whether this is a list's item.)
    const bool in_list = args.at(ex::kList).as_bool(false);
    const std::uint64_t items =
        in_list ? args.at(ex::kItems).as_uint(0) : 0;
    if (in_list && items == 0) {
      // An empty list, at once.
      auto empty = std::make_unique<vpipe::TensorListPayload>();
      const FlexData lsb = args.at(ex::kListSideband);
      if (lsb.is_object()) {
        empty->sideband = lsb;
      }
      co_await ctx.write(0, std::move(empty));
      cmd->reply(seq_result_(++_emitted));
      co_return;
    }
    TensorBeat tb;
    if (std::string why = tensor_(*cmd, tb); !why.empty()) {
      cmd->fail(why);
      co_return;
    }
    const FlexData sb = args.at(ex::kSideband);
    if (sb.is_object() && sb.as_object().size() > 0) {
      tb.sideband = sb;
    }
    if (!in_list) {
      co_await ctx.write(0, vpipe::make_payload<vpipe::TensorBeatPayload>(
                                std::move(tb)));
      cmd->reply(seq_result_(++_emitted));
      co_return;
    }
    // An item: gathered in its place, the list emitted after its last.
    if (!_list) {
      _list = std::make_unique<vpipe::TensorListPayload>();
      const FlexData lsb = args.at(ex::kListSideband);
      if (lsb.is_object()) {
        _list->sideband = lsb;
      }
    }
    const std::uint64_t item = args.at(ex::kItem).as_uint(0);
    if (item != _list->items.size()) {
      cmd->fail(vpipe::fmt("list item {} pushed where {} was due", item,
                           _list->items.size())());
      _list.reset();
      co_return;
    }
    _list->items.push_back(std::move(tb));
    if (_list->items.size() < items) {
      cmd->reply(seq_result_(_emitted + 1));
      co_return;
    }
    co_await ctx.write(0, std::move(_list));
    _list.reset();
    cmd->reply(seq_result_(++_emitted));
  }

  // The tensor a `push` carries: a copy of buffer `data` in engine memory
  // (a Metal shared buffer), or -- no buffer -- one of no bytes, its type
  // and shape said. An error's words, else "".
  std::string
  tensor_(vpipe::StageCommand& cmd, TensorBeat& tb)
  {
    const vpipe::DataBuffer* in = cmd.input(ex::kBufData);
    if (in) {
      if (!tensor_dtype_of(in->layout.type, &tb.dtype)) {
        return "`data` has no tensor element type";
      }
      tb.shape.assign(in->layout.shape.begin(), in->layout.shape.end());
      const bool shared = cmd.args().as_object().at(ex::kStorage).as_string(
                              "") != std::string_view(ex::kStorageCpu);
      if (!allocate_storage(tb, metal_(), shared)) {
        return "out of memory for the beat";
      }
      gather(*in, tb.bytes_());
      return {};
    }
    const auto args = cmd.args().as_object();
    const std::string tname(args.at(ex::kType).as_string(""));
    vpipe::ElementType et{};
    if (!vpipe::parse_element_type(tname, &et) ||
        !tensor_dtype_of(et, &tb.dtype) ||
        !parse_shape(args.at(ex::kShape), &tb.shape)) {
      return "`data` is missing, and no `type` and `shape` say a tensor "
             "of no bytes";
    }
    if (tb.element_count() != 0) {
      return "a tensor with bytes needs buffer `data`";
    }
    allocate_storage(tb, nullptr, false);
    return {};
  }

  std::uint64_t                             _emitted = 0;
  std::unique_ptr<vpipe::TensorListPayload> _list;
};

}

void
register_source(vpipe::VpipePluginContext* ctx)
{
  ctx->register_stage<SourceStage>(&kSpec);
}

}
