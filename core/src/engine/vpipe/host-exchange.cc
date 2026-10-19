#include "engine/vpipe/host-exchange.h"

#include "valtz/base/log.h"

#include "engine/vpipe/flex-json.h"
#include "valtz-vpipe/exchange.h"

#include "plugin/plugin-abi.h"
#include "plugin/plugin-context.h"

#include <mach-o/dyld.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <format>
#include <optional>

namespace valtz::engine::vp {

namespace fs = std::filesystem;
namespace ex = valtz::exchange;

namespace {

fs::path
executable_dir()
{
  char buf[PATH_MAX];
  std::uint32_t n = sizeof(buf);
  if (_NSGetExecutablePath(buf, &n) != 0) {
    return {};
  }
  std::error_code ec;
  fs::path p = fs::canonical(buf, ec);
  return ec ? fs::path(buf).parent_path() : p.parent_path();
}

const char*
dtype_str(Tensor::DType d)
{
  switch (d) {
  case Tensor::DType::U8:   return "u8";
  case Tensor::DType::F16:  return "f16";
  case Tensor::DType::BF16: return "bf16";
  case Tensor::DType::F32:  return "f32";
  }
  return "u8";
}

vpipe::ElementType
element_type(Tensor::DType d)
{
  switch (d) {
  case Tensor::DType::U8:   return vpipe::ElementType::U8;
  case Tensor::DType::F16:  return vpipe::ElementType::F16;
  case Tensor::DType::BF16: return vpipe::ElementType::BF16;
  case Tensor::DType::F32:  return vpipe::ElementType::F32;
  }
  return vpipe::ElementType::U8;
}

using vpipe::FlexData;

// The stage's reply, as the document it sent (an object; {} if none).
FlexData
reply_of(const vpipe::CommandHandle& cmd)
{
  FlexData r = cmd.result();
  return r.is_object() ? r : FlexData::make_object();
}

// Wait for the stage's answer; an error names the command and the cause.
Result<FlexData>
answer(const vpipe::CommandHandle& cmd, std::string_view what)
{
  const vpipe::CommandState st = cmd.wait(-1);
  if (st != vpipe::CommandState::Replied) {
    return make_error(Code::Engine, std::format(
        "{}: {} ({})", what, cmd.error(),
        vpipe::command_state_name(st)));
  }
  return reply_of(cmd);
}

// A sideband: set when the caller has one.
void
add_sideband(FlexData& args, const Json& sideband)
{
  if (sideband.is_object() && !sideband.empty()) {
    args.as_object().insert_or_assign(ex::kSideband, to_flex(sideband));
  }
}

}

fs::path
find_plugin()
{
  if (const char* env = std::getenv("VALTZ_VPIPE_PLUGIN"); env && *env) {
    return env;
  }
  std::vector<fs::path> candidates;
  if (fs::path dir = executable_dir(); !dir.empty()) {
    candidates.push_back(dir / ".." / "PlugIns" / ex::kPluginFile);
    candidates.push_back(dir / ex::kPluginFile);
  }
#ifdef VALTZ_VPIPE_PLUGIN_BUILD_PATH
  candidates.push_back(VALTZ_VPIPE_PLUGIN_BUILD_PATH);
#endif
  for (const auto& c : candidates) {
    std::error_code ec;
    if (fs::is_regular_file(c, ec)) {
      fs::path p = fs::canonical(c, ec);
      return ec ? c : p;
    }
  }
  return {};
}

std::string
backend_refusal(int abi, const std::vector<std::string>& required,
                Json* detail)
{
  if (abi < static_cast<int>(VPIPE_PLUGIN_ABI_OLDEST) ||
      abi > static_cast<int>(VPIPE_PLUGIN_ABI_VERSION)) {
    *detail = {{"abi", abi},
               {"oldest", VPIPE_PLUGIN_ABI_OLDEST},
               {"current", VPIPE_PLUGIN_ABI_VERSION}};
    return "abi";
  }
  for (const auto& f : required) {
    if (!vpipe::host_has_feature(f)) {
      *detail = {{"feature", f}};
      return "feature";
    }
  }
  *detail = Json::object();
  return {};
}

namespace {

// A sideband in FlexData's binary form; "" when there is none.
std::string
sideband_bin(const FlexData& sb)
{
  return sb.is_null() ? std::string() : sb.to_binary();
}

// The tensor a reply describes, into `b`: its bytes in place (a view of
// the beat), or -- no buffer -- its type and shape alone.
Status
tensor_from(const vpipe::CommandHandle& cmd, const FlexData& r, SinkBeat& b)
{
  const FlexData sb = field(r, ex::kSideband);
  b.meta = from_flex(sb);
  b.sideband_bin = sideband_bin(sb);
  Tensor::DType dt;
  if (!dtype_from_str(field_str(r, ex::kType), dt)) {
    return ok_status();  // one Tensor cannot describe (i8)
  }
  vpipe::DataBuffer buf;
  if (!cmd.buffer(ex::kBufData, &buf)) {
    // No bytes: [0, 96], a clip's reference rows when none carries video.
    auto t = std::make_shared<Tensor>();
    t->dtype = dt;
    const FlexData shape = field(r, ex::kShape);
    if (shape.is_array()) {
      const auto a = shape.as_array();
      for (std::size_t i = 0; i < a.size(); ++i) {
        t->shape.push_back(a.at(i).as_int(0));
      }
    }
    t->meta = b.meta;
    b.tensor = std::move(t);
    return ok_status();
  }
  auto t = std::make_shared<Tensor>();
  t->dtype = dt;
  t->shape = buf.layout.shape;
  if (!buf.layout.is_contiguous()) {
    const auto esz = static_cast<std::int64_t>(t->element_size());
    for (std::int64_t s : buf.layout.strides) {
      if (s % esz != 0) {
        return make_error(Code::Engine, std::format(
            "valtz-sink: stride {} is not a multiple of the element size",
            s));
      }
      t->strides.push_back(s / esz);
    }
  }
  t->data = static_cast<const std::uint8_t*>(buf.data);
  // The tensor's extent, not the whole allocation: consumers find the
  // last frame of [F,C,H,W] from the end of it.
  t->byte_size = std::min(buf.size, buf.layout.extent_bytes());
  t->mtl_buffer = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
      field_uint(r, ex::kMtlBuffer)));
  t->mtl_offset = field_uint(r, ex::kMtlOffset);
  t->meta = b.meta;
  t->owner = std::move(buf.owner);  // the beat itself
  b.tensor = std::move(t);
  return ok_status();
}

}

Result<SinkBeat>
sink_beat_from(const vpipe::CommandHandle& cmd, bool* eos)
{
  const FlexData r = reply_of(cmd);
  *eos = field_bool(r, ex::kEos);
  SinkBeat b;
  if (*eos) {
    return b;
  }
  b.seq = field_uint(r, ex::kSeq);
  b.dropped = field_uint(r, ex::kDropped);
  const std::string kind = field_str(r, ex::kKind);
  if (kind == ex::kKindData) {
    b.meta = from_flex(field(r, ex::kData));
    return b;
  }
  if (kind == ex::kKindList) {
    // An item of a list: the item here, the list's own sideband beside.
    b.is_list = true;
    b.item = field_uint(r, ex::kItem);
    b.item_count = field_uint(r, ex::kItems);
    SinkBeat whole;
    const FlexData lsb = field(r, ex::kListSideband);
    whole.meta = from_flex(lsb);
    whole.sideband_bin = sideband_bin(lsb);
    b.items.push_back(std::move(whole));
    if (b.item_count > 0) {
      VALTZ_TRY(tensor_from(cmd, r, b));
    }
    return b;
  }
  if (kind != ex::kKindTensor) {
    b.meta = from_flex(r);  // an unknown beat: pass its description along
    return b;
  }
  VALTZ_TRY(tensor_from(cmd, r, b));
  return b;
}

// ---- SinkReader ---------------------------------------------------------

SinkReader::SinkReader(vpipe::StageHandle stage, Fn fn)
  : _stage(stage), _fn(std::move(fn))
{
  _thread = std::thread([this] { run_(); });
}

SinkReader::~SinkReader()
{
  join();
}

void
SinkReader::join()
{
  if (_thread.joinable()) {
    _thread.join();
  }
}

std::string
SinkReader::stage_id() const
{
  return std::string(_stage.id());
}

void
SinkReader::run_()
{
  struct Ended {
    std::atomic<bool>& flag;
    ~Ended() { flag = true; }
  } ended{_ended};
  // A list beat being put back together, an item a reply.
  std::optional<SinkBeat> list;
  for (;;) {
    vpipe::CommandHandle cmd = _stage.command(ex::kCmdNext, FlexData());
    // A stop cancels the command, so this wait always ends.
    const vpipe::CommandState st = cmd.wait(-1);
    if (st != vpipe::CommandState::Replied) {
      _why = std::format("{}: {}", vpipe::command_state_name(st),
                         cmd.error());
      VALTZ_LOG_DEBUG("engine", "{} stopped reading: {}", _stage.id(),
                      _why);
      return;
    }
    bool eos = false;
    Result<SinkBeat> b = sink_beat_from(cmd, &eos);
    // The beat's owner lives on in the tensor; the command's own
    // references go now, and the sink never waited for them anyway.
    cmd.close();
    if (eos) {
      _eos = true;
      return;
    }
    if (!b.ok()) {
      VALTZ_LOG_WARN("engine", "{} ({})", b.error().message, _stage.id());
      continue;
    }
    if (!b->is_list) {
      _fn(std::move(*b));
      continue;
    }
    // A list's item: gathered, the list handed on whole after its last.
    if (!list || b->item == 0) {
      list.emplace();
      list->seq = b->seq;
      list->dropped = b->dropped;
      list->is_list = true;
      list->item_count = b->item_count;
      if (!b->items.empty()) {
        list->meta = std::move(b->items[0].meta);
        list->sideband_bin = std::move(b->items[0].sideband_bin);
      }
    }
    if (b->item_count > 0) {
      SinkBeat item;
      item.seq = b->seq;
      item.tensor = std::move(b->tensor);
      item.meta = std::move(b->meta);
      item.sideband_bin = std::move(b->sideband_bin);
      list->items.push_back(std::move(item));
    }
    if (list->items.size() >= list->item_count) {
      _fn(std::move(*list));
      list.reset();
    }
  }
}

// ---- SourceWriter -------------------------------------------------------

Result<SourceWriter::Lease>
SourceWriter::lease(Tensor::DType dt, const std::vector<std::int64_t>& shape,
                    const Json& sideband, bool shared)
{
  FlexData dims = FlexData::make_array();
  for (std::int64_t d : shape) {
    dims.as_array().push_back(d);
  }
  FlexData args = flex_object({
    {ex::kType, flex_str(dtype_str(dt))},
    {ex::kShape, std::move(dims)},
    {ex::kStorage, flex_str(shared ? ex::kStorageShared : ex::kStorageCpu)},
  });
  add_sideband(args, sideband);
  Lease l;
  l._cmd = _stage.command(ex::kCmdLease, args);
  VALTZ_ASSIGN(FlexData r, answer(l._cmd, "valtz-source lease"));
  vpipe::DataBuffer buf;
  if (!l._cmd.buffer(ex::kBufData, &buf)) {
    return make_error(Code::Engine, "valtz-source lease: no buffer");
  }
  // Keep the pointer, not the DataBuffer: a view kept past the close
  // would make the stage copy the beat instead of emitting it.
  l._data = static_cast<std::uint8_t*>(buf.data);
  l._size = buf.size;
  l._mtl = reinterpret_cast<void*>(static_cast<std::uintptr_t>(
      field_uint(r, ex::kMtlBuffer)));
  l._mtl_offset = field_uint(r, ex::kMtlOffset);
  l._seq = field_uint(r, ex::kSeq);
  return l;
}

Status
SourceWriter::push(const Tensor& t, const Json& sideband)
{
  vpipe::BufferLayout layout;
  layout.type = element_type(t.dtype);
  layout.shape = t.shape;
  for (std::int64_t s : t.strides) {
    layout.strides.push_back(s * static_cast<std::int64_t>(
                                     t.element_size()));
  }
  // The stage reads the bytes in place and copies them; `owner` keeps
  // them alive for as long as vpipe holds the view.
  vpipe::DataBuffer b = vpipe::DataBuffer::view(
      ex::kBufData, t.data, std::move(layout),
      std::const_pointer_cast<void>(t.owner));
  FlexData args = FlexData::make_object();
  add_sideband(args, sideband);
  vpipe::CommandHandle cmd = _stage.command(ex::kCmdPush, args,
                                            {std::move(b)});
  VALTZ_TRY(answer(cmd, "valtz-source push"));
  return ok_status();
}

namespace {

// A push's arguments for a kept tensor: its sideband as vpipe made it,
// and -- no bytes -- its type and shape (the buffer then absent).
FlexData
kept_args(const Tensor& t, std::string_view sideband, bool shared)
{
  FlexData args = FlexData::make_object();
  args.as_object().insert_or_assign(
      ex::kStorage, flex_str(shared ? ex::kStorageShared : ex::kStorageCpu));
  if (!sideband.empty()) {
    FlexData sb = FlexData::from_binary(sideband);
    if (sb.is_object()) {
      args.as_object().insert_or_assign(ex::kSideband, std::move(sb));
    }
  }
  if (t.byte_size == 0) {
    FlexData dims = FlexData::make_array();
    for (std::int64_t d : t.shape) {
      dims.as_array().push_back(d);
    }
    args.as_object().insert_or_assign(ex::kType, flex_str(dtype_str(t.dtype)));
    args.as_object().insert_or_assign(ex::kShape, std::move(dims));
  }
  return args;
}

std::vector<vpipe::DataBuffer>
kept_buffer(const Tensor& t)
{
  std::vector<vpipe::DataBuffer> out;
  if (t.byte_size == 0) {
    return out;
  }
  vpipe::BufferLayout layout;
  layout.type = element_type(t.dtype);
  layout.shape = t.shape;
  for (std::int64_t s : t.strides) {
    layout.strides.push_back(s * static_cast<std::int64_t>(
                                     t.element_size()));
  }
  out.push_back(vpipe::DataBuffer::view(
      ex::kBufData, t.data, std::move(layout),
      std::const_pointer_cast<void>(t.owner)));
  return out;
}

}

Status
SourceWriter::push_kept(const Tensor& t, std::string_view sideband_bin,
                        bool shared)
{
  vpipe::CommandHandle cmd = _stage.command(
      ex::kCmdPush, kept_args(t, sideband_bin, shared), kept_buffer(t));
  VALTZ_TRY(answer(cmd, "valtz-source push"));
  return ok_status();
}

Status
SourceWriter::push_kept_list(const std::vector<TensorPtr>& items,
                             const std::vector<std::string>& sideband_bins,
                             const std::vector<bool>& shared,
                             std::string_view list_sideband_bin)
{
  FlexData lsb = FlexData::make_null();
  if (!list_sideband_bin.empty()) {
    lsb = FlexData::from_binary(list_sideband_bin);
  }
  const auto n = static_cast<std::uint64_t>(items.size());
  auto as_item = [&](FlexData args, std::uint64_t i) {
    auto o = args.as_object();
    o.insert_or_assign(ex::kList, FlexData::make_bool(true));
    o.insert_or_assign(ex::kItems, FlexData::make_uint(n));
    o.insert_or_assign(ex::kItem, FlexData::make_uint(i));
    if (lsb.is_object()) {
      o.insert_or_assign(ex::kListSideband, lsb);
    }
    return args;
  };
  if (items.empty()) {
    vpipe::CommandHandle cmd = _stage.command(
        ex::kCmdPush, as_item(FlexData::make_object(), 0));
    VALTZ_TRY(answer(cmd, "valtz-source push"));
    return ok_status();
  }
  for (std::uint64_t i = 0; i < n; ++i) {
    const Tensor& t = *items[i];
    const std::string_view sb = i < sideband_bins.size()
                                    ? std::string_view(sideband_bins[i])
                                    : std::string_view();
    const bool sh = i < shared.size() && shared[i];
    vpipe::CommandHandle cmd = _stage.command(
        ex::kCmdPush, as_item(kept_args(t, sb, sh), i), kept_buffer(t));
    VALTZ_TRY(answer(cmd, "valtz-source push"));
  }
  return ok_status();
}

Status
SourceWriter::emit(const Json& value)
{
  FlexData args = flex_object({{ex::kData, to_flex(value)}});
  vpipe::CommandHandle cmd = _stage.command(ex::kCmdEmit, args);
  VALTZ_TRY(answer(cmd, "valtz-source emit"));
  return ok_status();
}

Result<TapCount>
tap_wait(vpipe::StageHandle tap, std::uint64_t frames,
         const std::function<bool()>& tick)
{
  vpipe::CommandHandle cmd = tap.command(
      ex::kCmdWait, flex_object({{ex::kFrames, vpipe::FlexData::make_uint(
                                                   frames)}}));
  for (;;) {
    const vpipe::CommandState st = cmd.wait(100);
    if (st != vpipe::CommandState::Pending &&
        st != vpipe::CommandState::Active) {
      break;
    }
    if (tick && !tick()) {
      cmd.close();
      return make_error(Code::Cancelled, "cancelled");
    }
  }
  VALTZ_ASSIGN(vpipe::FlexData r, answer(cmd, "valtz-tap wait"));
  return TapCount{field_uint(r, ex::kPassed), field_uint(r, ex::kCut),
                  field_bool(r, ex::kEos)};
}

Status
SourceWriter::finish()
{
  vpipe::CommandHandle cmd = _stage.command(ex::kCmdFinish, FlexData());
  VALTZ_TRY(answer(cmd, "valtz-source finish"));
  return ok_status();
}

}
