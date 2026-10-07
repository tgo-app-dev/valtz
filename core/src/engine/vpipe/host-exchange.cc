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
  if (kind != ex::kKindTensor) {
    b.meta = from_flex(r);  // an unknown beat: pass its description along
    return b;
  }
  b.meta = from_flex(field(r, ex::kSideband));
  vpipe::DataBuffer buf;
  Tensor::DType dt;
  if (!cmd.buffer(ex::kBufData, &buf) ||
      !dtype_from_str(field_str(r, ex::kType), dt)) {
    return b;  // an empty tensor, or one Tensor cannot describe
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

void
SinkReader::run_()
{
  for (;;) {
    vpipe::CommandHandle cmd = _stage.command(ex::kCmdNext, FlexData());
    // A stop cancels the command, so this wait always ends.
    const vpipe::CommandState st = cmd.wait(-1);
    if (st != vpipe::CommandState::Replied) {
      _why = std::format("{}: {}", vpipe::command_state_name(st),
                         cmd.error());
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
    _fn(std::move(*b));
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
