#include "valtz/media/refmod.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <thread>

namespace valtz::media {

namespace fs = std::filesystem;

namespace {

constexpr int kVisualChannels = 24;
constexpr int kAudioChannels = 32;
// An audio latent's rate: the codec's 32 kHz over its 800-sample hop.
constexpr double kAudioLatentsPerSecond = 40.0;

// A safetensors file's header: its tensors and its metadata.
struct Header {
  Json          tensors = Json::object();
  Json          meta = Json::object();
  std::uint64_t data_start = 0;
  std::uint64_t size = 0;
};

Result<Header>
read_header(const fs::path& file)
{
  std::error_code ec;
  const std::uint64_t size = fs::file_size(file, ec);
  if (ec || size < 8) {
    return make_error(Code::Io, std::format("{}: not readable",
                                            file.string()));
  }
  std::ifstream in(file, std::ios::binary);
  unsigned char b[8] = {};
  if (!in.read(reinterpret_cast<char*>(b), 8)) {
    return make_error(Code::Io, std::format("{}: not readable",
                                            file.string()));
  }
  std::uint64_t n = 0;
  for (int i = 7; i >= 0; --i) {
    n = (n << 8) | b[i];
  }
  // The format's own limit is 100 MB of header.
  if (n == 0 || n > (std::uint64_t{100} << 20) || n > size - 8) {
    return make_error(Code::InvalidArgument, "not a safetensors file");
  }
  std::string text(n, '\0');
  if (!in.read(text.data(), static_cast<std::streamsize>(n))) {
    return make_error(Code::Io, std::format("{}: not readable",
                                            file.string()));
  }
  Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return make_error(Code::InvalidArgument, "not a safetensors file");
  }
  Header h;
  h.meta = jget(j, "__metadata__", Json::object());
  j.erase("__metadata__");
  h.tensors = std::move(j);
  h.data_start = 8 + n;
  h.size = size;
  return h;
}

// The refmod metadata, parsed: null where there is none.
Json
refmod_meta(const Header& h)
{
  for (const char* k : {"refmod_meta", "audio_refmod_meta"}) {
    const auto text = jget<std::string>(h.meta, k, "");
    if (!text.empty()) {
      Json j = Json::parse(text, nullptr, false);
      if (!j.is_discarded() && j.is_object()) {
        return j;
      }
    }
  }
  return Json();
}

std::size_t
dtype_size(const std::string& d)
{
  return d == "F32" ? 4 : (d == "F16" || d == "BF16") ? 2 : 0;
}

// One member's tensor, its layout checked against its kind.
Status
take_tensor(const Header& h, const std::string& key, RefModMember& m)
{
  const Json t = jget(h.tensors, key.c_str(), Json());
  if (!t.is_object()) {
    return make_error(Code::Corrupt,
                      std::format("its tensor {} is missing", key));
  }
  m.tensor = key;
  m.dtype = jget<std::string>(t, "dtype", "");
  const Json shape = jget(t, "shape", Json::array());
  const Json off = jget(t, "data_offsets", Json::array());
  if (dtype_size(m.dtype) == 0) {
    return make_error(Code::Corrupt,
                      std::format("{} is {}, not a float", key, m.dtype));
  }
  for (const auto& d : shape) {
    if (!d.is_number_integer() || d.get<std::int64_t>() <= 0) {
      return make_error(Code::Corrupt,
                        std::format("{} has an empty shape", key));
    }
    m.shape.push_back(d.get<std::int64_t>());
  }
  if (!off.is_array() || off.size() != 2 || !off[0].is_number() ||
      !off[1].is_number()) {
    return make_error(Code::Corrupt,
                      std::format("{} has no data offsets", key));
  }
  const auto a = off[0].get<std::uint64_t>(), b = off[1].get<std::uint64_t>();
  std::uint64_t count = 1;
  for (auto d : m.shape) {
    count *= static_cast<std::uint64_t>(d);
  }
  m.offset = h.data_start + a;
  m.bytes = b - a;
  if (b < a || m.bytes != count * dtype_size(m.dtype) ||
      m.offset + m.bytes > h.size) {
    return make_error(Code::Corrupt,
                      std::format("{} is shorter than its shape", key));
  }
  const auto& s = m.shape;
  if (m.kind == "audio") {
    if (s.size() != 4 || s[0] != 1 || s[1] != kAudioChannels || s[2] != 2) {
      return make_error(Code::Corrupt, std::format(
          "{} is not an H3 audio latent [1, 32, 2, T]", key));
    }
    return ok_status();
  }
  if (s.size() != 5 || s[0] != 1 || s[1] != kVisualChannels ||
      s[3] % 2 != 0 || s[4] % 2 != 0) {
    return make_error(Code::Corrupt, std::format(
        "{} is not an H3 video latent [1, 24, T, H, W] of even H and W",
        key));
  }
  if (m.kind == "image" && s[2] != 1) {
    return make_error(Code::Corrupt,
                      std::format("{} is a picture of {} frames", key, s[2]));
  }
  return ok_status();
}

bool
known_kind(const std::string& k)
{
  return k == "image" || k == "video" || k == "audio";
}

// --- numbers --------------------------------------------------------

float
from_half(std::uint16_t h)
{
  _Float16 f;
  std::memcpy(&f, &h, 2);
  return static_cast<float>(f);
}

std::uint16_t
to_half(float v)
{
  const _Float16 f = static_cast<_Float16>(v);
  std::uint16_t h;
  std::memcpy(&h, &f, 2);
  return h;
}

float
from_bf16(std::uint16_t b)
{
  const std::uint32_t u = static_cast<std::uint32_t>(b) << 16;
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// One axis of a trilinear enlargement (PyTorch's align_corners false):
// each output index's two inputs and their weights.
struct Axis {
  std::vector<int>   i0, i1;
  std::vector<float> l0, l1;
};

Axis
axis_weights(int in, int out)
{
  Axis a;
  a.i0.resize(out);
  a.i1.resize(out);
  a.l0.resize(out);
  a.l1.resize(out);
  const float scale = static_cast<float>(in) / static_cast<float>(out);
  for (int o = 0; o < out; ++o) {
    if (in == out) {
      a.i0[o] = a.i1[o] = o;
      a.l0[o] = 1;
      a.l1[o] = 0;
      continue;
    }
    const float src = std::max(0.0f, scale * (static_cast<float>(o) + 0.5f)
                                         - 0.5f);
    const int i0 = static_cast<int>(src);
    a.i0[o] = i0;
    a.i1[o] = i0 + (i0 < in - 1 ? 1 : 0);
    a.l1[o] = std::clamp(src - static_cast<float>(i0), 0.0f, 1.0f);
    a.l0[o] = 1.0f - a.l1[o];
  }
  return a;
}

// One channel's [t, h, w] enlarged to [T, H, W], an axis at a time.
void
enlarge_channel(const float* src, int t, int h, int w, const Axis& at,
                const Axis& ah, const Axis& aw, int T, int H, int W,
                std::vector<float>& s1, std::vector<float>& s2, float* dst)
{
  s1.assign(static_cast<std::size_t>(t) * h * W, 0.0f);
  for (int i = 0; i < t * h; ++i) {
    const float* row = src + static_cast<std::size_t>(i) * w;
    float* o = s1.data() + static_cast<std::size_t>(i) * W;
    for (int x = 0; x < W; ++x) {
      o[x] = aw.l0[x] * row[aw.i0[x]] + aw.l1[x] * row[aw.i1[x]];
    }
  }
  s2.assign(static_cast<std::size_t>(t) * H * W, 0.0f);
  for (int k = 0; k < t; ++k) {
    for (int y = 0; y < H; ++y) {
      const float* r0 = s1.data() + (static_cast<std::size_t>(k) * h +
                                     ah.i0[y]) * W;
      const float* r1 = s1.data() + (static_cast<std::size_t>(k) * h +
                                     ah.i1[y]) * W;
      float* o = s2.data() + (static_cast<std::size_t>(k) * H + y) * W;
      for (int x = 0; x < W; ++x) {
        o[x] = ah.l0[y] * r0[x] + ah.l1[y] * r1[x];
      }
    }
  }
  const std::size_t plane = static_cast<std::size_t>(H) * W;
  for (int z = 0; z < T; ++z) {
    const float* p0 = s2.data() + at.i0[z] * plane;
    const float* p1 = s2.data() + at.i1[z] * plane;
    float* o = dst + z * plane;
    for (std::size_t i = 0; i < plane; ++i) {
      o[i] = at.l0[z] * p0[i] + at.l1[z] * p1[i];
    }
  }
}

// Its adjoint: a gradient over [T, H, W] gathered back onto [t, h, w].
void
gather_channel(const float* g, int T, int H, int W, const Axis& at,
               const Axis& ah, const Axis& aw, int t, int h, int w,
               std::vector<float>& s1, std::vector<float>& s2, float* dst)
{
  const std::size_t plane = static_cast<std::size_t>(H) * W;
  s2.assign(static_cast<std::size_t>(t) * plane, 0.0f);
  for (int z = 0; z < T; ++z) {
    const float* gi = g + z * plane;
    float* p0 = s2.data() + at.i0[z] * plane;
    float* p1 = s2.data() + at.i1[z] * plane;
    for (std::size_t i = 0; i < plane; ++i) {
      p0[i] += at.l0[z] * gi[i];
      p1[i] += at.l1[z] * gi[i];
    }
  }
  s1.assign(static_cast<std::size_t>(t) * h * W, 0.0f);
  for (int k = 0; k < t; ++k) {
    for (int y = 0; y < H; ++y) {
      const float* gi = s2.data() + (static_cast<std::size_t>(k) * H + y) * W;
      float* r0 = s1.data() + (static_cast<std::size_t>(k) * h +
                               ah.i0[y]) * W;
      float* r1 = s1.data() + (static_cast<std::size_t>(k) * h +
                               ah.i1[y]) * W;
      for (int x = 0; x < W; ++x) {
        r0[x] += ah.l0[y] * gi[x];
        r1[x] += ah.l1[y] * gi[x];
      }
    }
  }
  std::fill(dst, dst + static_cast<std::size_t>(t) * h * w, 0.0f);
  for (int i = 0; i < t * h; ++i) {
    const float* gi = s1.data() + static_cast<std::size_t>(i) * W;
    float* row = dst + static_cast<std::size_t>(i) * w;
    for (int x = 0; x < W; ++x) {
      row[aw.i0[x]] += aw.l0[x] * gi[x];
      row[aw.i1[x]] += aw.l1[x] * gi[x];
    }
  }
}

std::string
grid_text(const Latent& z)
{
  return std::format("{}x{}x{}", z.shape[1], z.shape[2], z.shape[3]);
}

// The latent's frames `keep`, in order.
Latent
frames_of(const Latent& z, const std::vector<int>& keep)
{
  Latent out;
  out.shape = z.shape;
  out.shape[1] = static_cast<std::int64_t>(keep.size());
  const std::size_t plane = static_cast<std::size_t>(z.shape[2] * z.shape[3]);
  const auto T = static_cast<std::size_t>(z.shape[1]);
  out.data.resize(out.count());
  for (std::int64_t c = 0; c < z.shape[0]; ++c) {
    for (std::size_t k = 0; k < keep.size(); ++k) {
      const float* s = z.data.data() +
                       (static_cast<std::size_t>(c) * T + keep[k]) * plane;
      std::copy(s, s + plane,
                out.data.data() +
                    (static_cast<std::size_t>(c) * keep.size() + k) * plane);
    }
  }
  return out;
}

// Latents of one channel count and grid laid end to end in time.
Latent
stack(const std::vector<Latent>& parts)
{
  Latent out;
  out.shape = parts.front().shape;
  std::int64_t T = 0;
  for (const auto& p : parts) {
    T += p.shape[1];
  }
  out.shape[1] = T;
  out.data.reserve(out.count());
  const std::size_t plane =
      static_cast<std::size_t>(out.shape[2] * out.shape[3]);
  for (std::int64_t c = 0; c < out.shape[0]; ++c) {
    for (const auto& p : parts) {
      const std::size_t n = static_cast<std::size_t>(p.shape[1]) * plane;
      const float* s = p.data.data() + static_cast<std::size_t>(c) * n;
      out.data.insert(out.data.end(), s, s + n);
    }
  }
  return out;
}

Latent
halved(Latent z)
{
  for (auto& v : z.data) {
    v = refmod::half(v);
  }
  return z;
}

// A safetensors file of F16 tensors, written whole then moved in.
Status
write_safetensors(const fs::path& out,
                  const std::vector<std::pair<std::string, Latent>>& ts,
                  const Json& metadata)
{
  Json head = Json::object();
  head["__metadata__"] = metadata;
  std::uint64_t at = 0;
  for (const auto& [key, t] : ts) {
    const std::uint64_t n = t.count() * 2;
    head[key] = {{"dtype", "F16"}, {"shape", t.shape},
                 {"data_offsets", {at, at + n}}};
    at += n;
  }
  std::string text = to_text(head);
  text.append((8 - text.size() % 8) % 8, ' ');
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  const fs::path tmp = out.parent_path() /
                       ("." + out.filename().string() + ".part");
  {
    std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
    std::uint64_t n = text.size();
    unsigned char b[8];
    for (int i = 0; i < 8; ++i) {
      b[i] = static_cast<unsigned char>(n >> (8 * i));
    }
    f.write(reinterpret_cast<const char*>(b), 8);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    std::vector<std::uint16_t> h;
    for (const auto& [key, t] : ts) {
      h.resize(t.count());
      for (std::size_t i = 0; i < h.size(); ++i) {
        h[i] = to_half(t.data[i]);
      }
      f.write(reinterpret_cast<const char*>(h.data()),
              static_cast<std::streamsize>(h.size() * 2));
    }
    if (!f) {
      fs::remove(tmp, ec);
      return make_error(Code::Io, std::format("{}: could not be written",
                                              out.string()));
    }
  }
  fs::rename(tmp, out, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return make_error(Code::Io, std::format("{}: could not be written",
                                            out.string()));
  }
  return ok_status();
}

}  // namespace

// --- a member ---------------------------------------------------------

int
RefModMember::frames() const
{
  // [1, 24, T, H, W] or [1, 32, 2, T].
  const std::size_t at = visual() ? 2 : 3;
  return shape.size() > at ? static_cast<int>(shape[at]) : 0;
}

int
RefModMember::height() const
{
  return visual() && shape.size() == 5 ? static_cast<int>(shape[3]) : 0;
}

int
RefModMember::width() const
{
  return visual() && shape.size() == 5 ? static_cast<int>(shape[4]) : 0;
}

std::uint64_t
RefModMember::tokens() const
{
  const auto t = static_cast<std::uint64_t>(frames());
  return visual() ? t * static_cast<std::uint64_t>(height() / 2) *
                        static_cast<std::uint64_t>(width() / 2)
                  : 2 * t;
}

double
RefModMember::seconds() const
{
  return visual() ? 0.0 : frames() / kAudioLatentsPerSecond;
}

std::uint64_t
RefMod::tokens() const
{
  std::uint64_t n = 0;
  for (const auto& m : members) {
    n += m.tokens();
  }
  return n;
}

int
RefMod::visuals() const
{
  return static_cast<int>(std::ranges::count_if(
      members, [](const RefModMember& m) { return m.visual(); }));
}

int
RefMod::sounds() const
{
  return static_cast<int>(members.size()) - visuals();
}

Json
RefMod::to_json() const
{
  Json ms = Json::array();
  for (const auto& m : members) {
    ms.push_back({{"name", m.name}, {"kind", m.kind},
                  {"frames", m.frames()}, {"height", m.height()},
                  {"width", m.width()}, {"tokens", m.tokens()},
                  {"seconds", m.seconds()},
                  {"description", jget<std::string>(m.meta, "description",
                                                    "")},
                  {"concept_type", jget<std::string>(m.meta, "concept_type",
                                                     "")},
                  {"mode", jget<std::string>(m.meta, "mode", "")}});
  }
  return {{"name", name}, {"version", version}, {"tokens", tokens()},
          {"members", std::move(ms)}};
}

std::size_t
Latent::count() const
{
  std::size_t n = shape.empty() ? 0 : 1;
  for (auto d : shape) {
    n *= static_cast<std::size_t>(d);
  }
  return n;
}

// --- reading ------------------------------------------------------------

Result<RefMod>
read_refmod(const fs::path& file)
{
  VALTZ_ASSIGN(const Header h, read_header(file));
  const Json meta = refmod_meta(h);
  if (meta.is_null()) {
    return make_error(Code::InvalidArgument, "no RefMod metadata");
  }
  RefMod r;
  r.meta = meta;
  r.version = jget(meta, "_format_version", 0);
  r.name = jget<std::string>(meta, "name", file.stem().string());
  if (jget<std::string>(meta, "kind", "") == "bundle") {
    if (r.version != 5) {
      return make_error(Code::Unsupported, std::format(
          "a RefMod bundle of version {}, not 5", r.version));
    }
    const Json ms = jget(meta, "members", Json::array());
    if (!ms.is_array() || ms.empty() || ms.size() > 256) {
      return make_error(Code::Corrupt,
                        "a RefMod bundle holds 1 to 256 references");
    }
    for (std::size_t i = 0; i < ms.size(); ++i) {
      RefModMember m;
      m.meta = ms[i];
      m.kind = jget<std::string>(ms[i], "kind", "");
      if (!ms[i].is_object() || !known_kind(m.kind)) {
        return make_error(Code::Corrupt, std::format(
            "member {} is not a picture, a clip or a sound", i));
      }
      m.name = jget<std::string>(ms[i], "name",
                                 std::format("{} {}", r.name, i + 1));
      VALTZ_TRY(take_tensor(h, std::format("ref_{}", i), m));
      r.members.push_back(std::move(m));
    }
    return r;
  }
  // One reference (versions 2-4; H3AudioMod's audio files).
  RefModMember m;
  m.meta = meta;
  m.kind = jget<std::string>(meta, "kind", "");
  if (!known_kind(m.kind)) {
    return make_error(Code::Corrupt,
                      "not a picture, a clip or a sound");
  }
  m.name = r.name;
  VALTZ_TRY(take_tensor(h, "latent", m));
  r.members.push_back(std::move(m));
  return r;
}

bool
is_refmod(const fs::path& file)
{
  return read_refmod(file).ok();
}

Result<Latent>
read_member(const fs::path& file, const RefModMember& m)
{
  std::ifstream in(file, std::ios::binary);
  in.seekg(static_cast<std::streamoff>(m.offset));
  std::string raw(m.bytes, '\0');
  if (!in.read(raw.data(), static_cast<std::streamsize>(m.bytes))) {
    return make_error(Code::Io, std::format("{}: could not read {}",
                                            file.string(), m.tensor));
  }
  const std::size_t n = m.bytes / dtype_size(m.dtype);
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    if (m.dtype == "F32") {
      std::memcpy(&v[i], raw.data() + i * 4, 4);
    } else {
      std::uint16_t b;
      std::memcpy(&b, raw.data() + i * 2, 2);
      v[i] = m.dtype == "F16" ? from_half(b) : from_bf16(b);
    }
  }
  Latent out;
  if (m.visual()) {
    out.shape.assign(m.shape.begin() + 1, m.shape.end());
    out.data = std::move(v);
    return out;
  }
  // [1, 32, 2, T] -> [2, 32, T].
  const auto C = m.shape[1], S = m.shape[2], T = m.shape[3];
  out.shape = {S, C, T};
  out.data.resize(n);
  for (std::int64_t c = 0; c < C; ++c) {
    for (std::int64_t s = 0; s < S; ++s) {
      std::copy(v.begin() + (c * S + s) * T, v.begin() + (c * S + s + 1) * T,
                out.data.begin() + (s * C + c) * T);
    }
  }
  return out;
}

// --- vpipe's tensor file --------------------------------------------------

namespace {
constexpr char kVpMagic[8] = {'V', 'P', 'T', 'E', 'N', 'S', 'O', 'R'};
constexpr std::uint32_t kVpF32 = 3;   // TensorBeat::DType::F32
}  // namespace

Status
write_vptensor(const fs::path& file, const Latent& t, const Json& sideband)
{
  std::ofstream f(file, std::ios::binary | std::ios::trunc);
  auto u32 = [&](std::uint32_t v) {
    f.write(reinterpret_cast<const char*>(&v), 4);
  };
  f.write(kVpMagic, 8);
  u32(1);
  u32(kVpF32);
  u32(static_cast<std::uint32_t>(t.shape.size()));
  u32(0);
  const std::uint64_t count = t.count();
  f.write(reinterpret_cast<const char*>(&count), 8);
  for (auto d : t.shape) {
    f.write(reinterpret_cast<const char*>(&d), 8);
  }
  f.write(reinterpret_cast<const char*>(t.data.data()),
          static_cast<std::streamsize>(t.data.size() * sizeof(float)));
  if (!sideband.is_null()) {
    const std::string js = to_text(sideband);
    f.write(js.data(), static_cast<std::streamsize>(js.size()));
  }
  if (!f) {
    return make_error(Code::Io, std::format("{}: could not be written",
                                            file.string()));
  }
  return ok_status();
}

Result<Latent>
read_vptensor(const fs::path& file)
{
  std::ifstream f(file, std::ios::binary);
  char magic[8] = {};
  std::uint32_t v[4] = {};
  std::uint64_t count = 0;
  if (!f.read(magic, 8) || std::memcmp(magic, kVpMagic, 8) != 0 ||
      !f.read(reinterpret_cast<char*>(v), sizeof v) || v[0] != 1 ||
      v[1] != kVpF32 || v[2] == 0 || v[2] > 8 ||
      !f.read(reinterpret_cast<char*>(&count), 8)) {
    return make_error(Code::Corrupt, std::format(
        "{}: not an f32 tensor file", file.string()));
  }
  Latent t;
  t.shape.resize(v[2]);
  if (!f.read(reinterpret_cast<char*>(t.shape.data()),
              static_cast<std::streamsize>(v[2] * 8)) ||
      t.count() != count) {
    return make_error(Code::Corrupt, std::format(
        "{}: its shape disagrees with its count", file.string()));
  }
  t.data.resize(count);
  if (!f.read(reinterpret_cast<char*>(t.data.data()),
              static_cast<std::streamsize>(count * sizeof(float)))) {
    return make_error(Code::Corrupt, std::format(
        "{}: shorter than its shape", file.string()));
  }
  return t;
}

// --- the pack's arithmetic -----------------------------------------------

namespace refmod {

float
half(float v)
{
  return static_cast<float>(static_cast<_Float16>(v));
}

std::pair<int, int>
aspect_grid(int pool_h, int pool_w, double aspect)
{
  const double edge = std::max(pool_h, pool_w);
  double h = edge, w = edge;
  if (aspect >= 1.0) {
    w = edge / aspect;
  } else {
    h = edge * aspect;
  }
  // Python's round: half to even, as nearbyint rounds by default.
  const int gh = std::max(2, static_cast<int>(std::nearbyint(h / 2)) * 2);
  const int gw = std::max(2, static_cast<int>(std::nearbyint(w / 2)) * 2);
  return {gh, gw};
}

Latent
pool(const Latent& z, int t, int h, int w)
{
  const int C = static_cast<int>(z.shape[0]), T = static_cast<int>(z.shape[1]),
            H = static_cast<int>(z.shape[2]), W = static_cast<int>(z.shape[3]);
  Latent out;
  out.shape = {C, t, h, w};
  out.data.resize(out.count());
  auto lo = [](int a, int out_n, int in_n) { return a * in_n / out_n; };
  auto hi = [](int a, int out_n, int in_n) {
    return ((a + 1) * in_n + out_n - 1) / out_n;
  };
  for (int c = 0; c < C; ++c) {
    for (int k = 0; k < t; ++k) {
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          double sum = 0;
          int n = 0;
          for (int kk = lo(k, t, T); kk < hi(k, t, T); ++kk) {
            for (int yy = lo(y, h, H); yy < hi(y, h, H); ++yy) {
              const float* row = z.data.data() +
                  ((static_cast<std::size_t>(c) * T + kk) * H + yy) * W;
              for (int xx = lo(x, w, W); xx < hi(x, w, W); ++xx) {
                sum += row[xx];
                ++n;
              }
            }
          }
          out.data[((static_cast<std::size_t>(c) * t + k) * h + y) * w + x] =
              static_cast<float>(sum / std::max(n, 1));
        }
      }
    }
  }
  return out;
}

Latent
enlarge(const Latent& small, int T, int H, int W)
{
  const int C = static_cast<int>(small.shape[0]);
  const int t = static_cast<int>(small.shape[1]),
            h = static_cast<int>(small.shape[2]),
            w = static_cast<int>(small.shape[3]);
  const Axis at = axis_weights(t, T), ah = axis_weights(h, H),
             aw = axis_weights(w, W);
  Latent out;
  out.shape = {C, T, H, W};
  out.data.resize(out.count());
  std::vector<float> s1, s2;
  const std::size_t in_n = static_cast<std::size_t>(t) * h * w,
                    out_n = static_cast<std::size_t>(T) * H * W;
  for (int c = 0; c < C; ++c) {
    enlarge_channel(small.data.data() + c * in_n, t, h, w, at, ah, aw, T, H,
                    W, s1, s2, out.data.data() + c * out_n);
  }
  return out;
}

Latent
refine(const Latent& small, const Latent& full, int steps,
       const std::function<bool()>& stop)
{
  if (steps <= 0) {
    return small;
  }
  const int C = static_cast<int>(small.shape[0]);
  const int t = static_cast<int>(small.shape[1]),
            h = static_cast<int>(small.shape[2]),
            w = static_cast<int>(small.shape[3]);
  const int T = static_cast<int>(full.shape[1]),
            H = static_cast<int>(full.shape[2]),
            W = static_cast<int>(full.shape[3]);
  const Axis at = axis_weights(t, T), ah = axis_weights(h, H),
             aw = axis_weights(w, W);
  const std::size_t in_n = static_cast<std::size_t>(t) * h * w,
                    out_n = static_cast<std::size_t>(T) * H * W;
  // The loss is a mean over every channel's values; Adam works value by
  // value -- so each channel's refinement is its own, run side by side.
  const float scale = 2.0f / static_cast<float>(out_n * C);
  constexpr float kLr = 0.02f, kB1 = 0.9f, kB2 = 0.999f, kEps = 1e-8f;
  Latent out = small;
  std::atomic<bool> halt{false};
  auto channel = [&](int c) {
    float* p = out.data.data() + c * in_n;
    const float* target = full.data.data() + c * out_n;
    std::vector<float> m(in_n, 0.0f), v(in_n, 0.0f), g(in_n), up(out_n),
        s1, s2;
    for (int step = 1; step <= steps && !halt; ++step) {
      enlarge_channel(p, t, h, w, at, ah, aw, T, H, W, s1, s2, up.data());
      for (std::size_t i = 0; i < out_n; ++i) {
        up[i] = scale * (up[i] - target[i]);
      }
      gather_channel(up.data(), T, H, W, at, ah, aw, t, h, w, s1, s2,
                     g.data());
      const float bc1 = 1.0f - std::pow(kB1, static_cast<float>(step));
      const float bc2 = 1.0f - std::pow(kB2, static_cast<float>(step));
      const float step_size = kLr / bc1, root_bc2 = std::sqrt(bc2);
      for (std::size_t i = 0; i < in_n; ++i) {
        m[i] = kB1 * m[i] + (1.0f - kB1) * g[i];
        v[i] = kB2 * v[i] + (1.0f - kB2) * g[i] * g[i];
        p[i] -= step_size * m[i] / (std::sqrt(v[i]) / root_bc2 + kEps);
      }
    }
  };
  const int workers = std::clamp(
      static_cast<int>(std::thread::hardware_concurrency()), 1, C);
  std::atomic<int> next{0}, finished{0};
  std::vector<std::thread> pool;
  for (int k = 0; k < workers; ++k) {
    pool.emplace_back([&] {
      for (int c; (c = next.fetch_add(1)) < C;) {
        channel(c);
        ++finished;
      }
    });
  }
  // Asked on this thread alone, the caller's.
  while (stop && finished.load() < C) {
    if (stop()) {
      halt = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  for (auto& th : pool) {
    th.join();
  }
  return out;
}

std::vector<int>
distinct_frames(const Latent& z)
{
  const int C = static_cast<int>(z.shape[0]), T = static_cast<int>(z.shape[1]);
  const std::size_t plane = static_cast<std::size_t>(z.shape[2] * z.shape[3]);
  std::vector<int> kept;
  if (T <= 1) {
    for (int i = 0; i < T; ++i) {
      kept.push_back(i);
    }
    return kept;
  }
  auto at = [&](int c, int k) {
    return z.data.data() + (static_cast<std::size_t>(c) * T + k) * plane;
  };
  auto mean_abs = [&](int k) {
    double s = 0;
    for (int c = 0; c < C; ++c) {
      for (std::size_t i = 0; i < plane; ++i) {
        s += std::fabs(at(c, k)[i]);
      }
    }
    return s / static_cast<double>(C * plane);
  };
  kept.push_back(0);
  int prev = 0;
  for (int k = 1; k < T; ++k) {
    double d = 0;
    for (int c = 0; c < C; ++c) {
      for (std::size_t i = 0; i < plane; ++i) {
        d += std::fabs(at(c, k)[i] - at(c, prev)[i]);
      }
    }
    d /= static_cast<double>(C * plane);
    const double denom = (mean_abs(k) + mean_abs(prev)) / 2 + 1e-6;
    if (d / denom >= 0.02) {
      kept.push_back(k);
      prev = k;
    }
  }
  return kept;
}

Result<Latent>
fit_tokens(const Latent& z, std::int64_t budget)
{
  if (budget <= 0) {
    return z;
  }
  const std::int64_t per = (z.shape[2] / 2) * (z.shape[3] / 2);
  if (per > budget) {
    return make_error(Code::Unsupported, std::format(
        "one latent frame costs {} tokens, over the budget of {}", per,
        budget));
  }
  if (per * z.shape[1] <= budget) {
    return z;
  }
  Latent out = frames_of(z, distinct_frames(z));
  const std::int64_t t = out.shape[1];
  if (per * t > budget) {
    const std::int64_t fit = std::max<std::int64_t>(1, budget / per);
    std::vector<int> idx;
    for (std::int64_t i = 0; i < fit; ++i) {
      const double x = fit == 1 ? 0.0
                                : static_cast<double>(i) * (t - 1) /
                                      static_cast<double>(fit - 1);
      idx.push_back(static_cast<int>(std::nearbyint(x)));
    }
    out = frames_of(out, idx);
  }
  return out;
}

}  // namespace refmod

// --- making one -----------------------------------------------------------

Result<RefMod>
make_refmod(const RefModRecipe& r, const std::vector<RefModSource>& visual,
            const std::vector<Latent>& sounds, const fs::path& out,
            const std::function<bool()>& stop,
            const std::function<void(int, int)>& progress)
{
  if (visual.empty() && sounds.empty()) {
    return make_error(Code::InvalidArgument, "nothing to make a RefMod of");
  }
  const std::string name = r.name.empty() ? std::string("refmod") : r.name;
  std::vector<std::pair<std::string, Latent>> tensors;
  Json members = Json::array();
  if (!visual.empty()) {
    for (const auto& s : visual) {
      if (s.z.shape.size() != 4 || s.z.shape[0] != kVisualChannels) {
        return make_error(Code::InvalidArgument,
                          "a source is not an H3 video latent");
      }
    }
    const auto& z0 = visual.front().z;
    const auto [gh, gw] = refmod::aspect_grid(
        r.pool_h, r.pool_w,
        static_cast<double>(z0.shape[2]) / static_cast<double>(z0.shape[3]));
    std::vector<Latent> parts;
    std::string shapes;
    int n_img = 0, n_vid = 0, done = 0;
    const int total = r.compressed
        ? r.refine_steps * static_cast<int>(visual.size()) : 0;
    for (const auto& s : visual) {
      (s.video ? n_vid : n_img) += 1;
      shapes += (shapes.empty() ? "" : " +") + grid_text(s.z);
      if (!r.compressed) {
        if (s.z.shape[2] != z0.shape[2] || s.z.shape[3] != z0.shape[3]) {
          return make_error(Code::InvalidArgument,
                            "Full Reference sources are not on one canvas");
        }
        parts.push_back(halved(s.z));
        continue;
      }
      const int t = s.video ? std::min<int>(std::max(r.latent_frames, 1),
                                            static_cast<int>(s.z.shape[1]))
                            : 1;
      Latent small = halved(refmod::pool(s.z, t, gh, gw));
      if (r.refine_steps > 0) {
        small = halved(refmod::refine(small, s.z, r.refine_steps, stop));
        if (stop && stop()) {
          return make_error(Code::Cancelled, "stopped");
        }
      }
      done += r.refine_steps;
      if (progress) {
        progress(done, total);
      }
      parts.push_back(std::move(small));
    }
    VALTZ_ASSIGN(Latent z, refmod::fit_tokens(stack(parts), r.max_tokens));
    const auto T = z.shape[1], H = z.shape[2], W = z.shape[3];
    const bool several = visual.size() > 1;
    Json m = {
        {"name", name},
        {"kind", T > 1 ? "video" : "image"},
        {"latent_h", H}, {"latent_w", W}, {"latent_t", T},
        {"mode", r.compressed ? "training" : "encode"},
        {"source", several ? "stack" : visual.front().video ? "video"
                                                              : "image"},
        {"source_shape", shapes},
        {"pool", r.compressed
                     ? std::format("{}x{}x{}", T, gh, gw)
                     : std::format("full-res {}x{}px (short-edge cap {}px)",
                                   W * 16, H * 16, r.resolution)},
        {"optimize_steps", r.compressed ? r.refine_steps : 0},
        {"tags", Json::array({std::format("{} img, {} vid", n_img, n_vid)})},
        {"description", r.description},
        {"concept_type", r.concept_type},
        {"_format_version", 4},
        {"sample_rate", 32000}};
    members.push_back(std::move(m));
    z.shape.insert(z.shape.begin(), 1);
    tensors.emplace_back(std::format("ref_{}", tensors.size()),
                         std::move(z));
  }
  for (std::size_t k = 0; k < sounds.size(); ++k) {
    const auto& s = sounds[k];
    if (s.shape.size() != 3 || s.shape[0] != 2 ||
        s.shape[1] != kAudioChannels || s.shape[2] <= 0) {
      return make_error(Code::InvalidArgument,
                        "a sound is not an H3 audio latent [2, 32, T]");
    }
    // [2, 32, T] -> [1, 32, 2, T].
    const auto C = s.shape[1], T = s.shape[2];
    Latent z;
    z.shape = {1, C, 2, T};
    z.data.resize(s.count());
    for (std::int64_t c = 0; c < C; ++c) {
      for (std::int64_t st = 0; st < 2; ++st) {
        std::copy(s.data.begin() + (st * C + c) * T,
                  s.data.begin() + (st * C + c + 1) * T,
                  z.data.begin() + (c * 2 + st) * T);
      }
    }
    const std::string base = visual.empty() ? name : name + "_audio";
    members.push_back(
        {{"name", k == 0 ? base : std::format("{}_{}", base, k + 1)},
         {"kind", "audio"},
         {"latent_h", 0}, {"latent_w", 0}, {"latent_t", T},
         {"mode", "encode"}, {"source", "audio"}, {"source_shape", ""},
         {"pool", ""}, {"optimize_steps", 0}, {"tags", Json::array()},
         {"description", r.description},
         {"concept_type", visual.empty() ? r.concept_type : "generic"},
         {"_format_version", 4}, {"sample_rate", 32000}});
    tensors.emplace_back(std::format("ref_{}", tensors.size()),
                         std::move(z));
  }
  const Json bundle = {{"_format_version", 5}, {"kind", "bundle"},
                       {"name", name}, {"members", members}};
  VALTZ_TRY(write_safetensors(out, tensors,
                              {{"refmod_meta", to_text(bundle)}}));
  return read_refmod(out);
}

}  // namespace valtz::media
