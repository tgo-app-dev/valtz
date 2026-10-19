#include "engine/vpipe/encoding-cache.h"

#include "engine/vpipe/host-exchange.h"

#include "valtz/base/hash.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>

namespace valtz::engine::vp {

namespace fs = std::filesystem;

namespace {

constexpr char          kMagic[8] = {'V', 'A', 'L', 'T', 'Z', 'E', 'N', 'C'};
constexpr std::uint32_t kVersion = 1;
// Each tensor's bytes start on a 64-byte boundary of the blob.
constexpr std::size_t   kAlign = 64;

const char*
dtype_name(Tensor::DType d)
{
  switch (d) {
  case Tensor::DType::U8:   return "u8";
  case Tensor::DType::F16:  return "f16";
  case Tensor::DType::BF16: return "bf16";
  case Tensor::DType::F32:  return "f32";
  }
  return "u8";
}

// `t`'s elements in row-major order, whatever its strides.
void
append_contiguous(const Tensor& t, std::vector<std::uint8_t>& out)
{
  const std::size_t es = t.element_size();
  const std::size_t n = t.element_count();
  if (n == 0 || !t.data) {
    return;
  }
  if (t.contiguous()) {
    out.insert(out.end(), t.data, t.data + n * es);
    return;
  }
  const std::size_t rank = t.shape.size();
  std::vector<std::int64_t> idx(rank, 0);
  for (std::size_t k = 0; k < n; ++k) {
    std::int64_t at = 0;
    for (std::size_t d = 0; d < rank; ++d) {
      at += idx[d] * t.strides[d];
    }
    const std::uint8_t* p = t.data + at * static_cast<std::int64_t>(es);
    out.insert(out.end(), p, p + es);
    for (std::size_t d = rank; d-- > 0;) {
      if (++idx[d] < t.shape[d]) {
        break;
      }
      idx[d] = 0;
    }
  }
}

void
pad_to(std::vector<std::uint8_t>& blob, std::size_t align)
{
  blob.resize((blob.size() + align - 1) / align * align, 0);
}

// [offset, length] of `bytes` appended to the blob.
Json
put(std::vector<std::uint8_t>& blob, std::string_view bytes)
{
  const std::size_t at = blob.size();
  blob.insert(blob.end(), bytes.begin(), bytes.end());
  return Json::array({at, bytes.size()});
}

bool
span_of(const Json& j, std::size_t size, std::size_t* at, std::size_t* n)
{
  if (!j.is_array() || j.size() != 2 || !j[0].is_number_unsigned() ||
      !j[1].is_number_unsigned()) {
    return false;
  }
  *at = j[0].get<std::size_t>();
  *n = j[1].get<std::size_t>();
  return *at <= size && *n <= size - *at;
}

}

std::uint64_t
Kept::bytes() const
{
  std::uint64_t n = 0;
  for (const auto& p : ports) {
    for (const auto& t : p.tensors) {
      n += t ? t->byte_size : 0;
    }
  }
  return n;
}

namespace {

// A tensor's bytes copied, contiguous: what is kept must not be the
// beat's own storage, which its producer may go on to reuse.
TensorPtr
owned_copy(const TensorPtr& t)
{
  if (!t) {
    return t;
  }
  std::vector<std::uint8_t> bytes;
  append_contiguous(*t, bytes);
  return make_owned_tensor(t->dtype, t->shape, std::move(bytes), t->meta);
}

}

Kept::Port
kept_port(std::string name, const SinkBeat& beat)
{
  Kept::Port p;
  p.name = std::move(name);
  p.list = beat.is_list;
  if (beat.is_list) {
    p.list_sideband = beat.sideband_bin;
    for (const auto& item : beat.items) {
      p.tensors.push_back(owned_copy(item.tensor));
      p.sidebands.push_back(item.sideband_bin);
      p.shared.push_back(item.tensor && item.tensor->mtl_buffer);
    }
  } else {
    p.tensors.push_back(owned_copy(beat.tensor));
    p.sidebands.push_back(beat.sideband_bin);
    p.shared.push_back(beat.tensor && beat.tensor->mtl_buffer);
  }
  return p;
}

std::string
encoding_key(std::string_view what, const Json& made_from)
{
  // to_text: never throws on a byte that is not UTF-8 (a prompt from
  // outside), and an object's keys come sorted -- the same document is
  // always the same text.
  const std::string text = to_text(made_from);
  return std::format("enc-{}-{}", what, ContentHash::of(text).hex());
}

std::string
model_stamp(const fs::path& dir)
{
  std::error_code ec;
  if (dir.empty() || !fs::is_directory(dir, ec)) {
    return {};
  }
  std::vector<std::string> lines;
  auto it = fs::recursive_directory_iterator(
      dir, fs::directory_options::follow_directory_symlink |
               fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    return {};
  }
  for (const auto end = fs::recursive_directory_iterator(); it != end;
       it.increment(ec)) {
    if (ec) {
      return {};
    }
    const fs::path& p = it->path();
    const std::string name = p.filename().string();
    // A Finder's .DS_Store, a download's .cache: not the model.
    if (!name.empty() && name[0] == '.') {
      if (it->is_directory(ec)) {
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const auto size = it->file_size(ec);
    const auto time = it->last_write_time(ec);
    lines.push_back(std::format(
        "{}\t{}\t{}", p.lexically_relative(dir).generic_string(), size,
        time.time_since_epoch().count()));
  }
  std::ranges::sort(lines);
  Hasher h;
  for (const auto& l : lines) {
    h.update(l);
    h.update("\n");
  }
  return h.finish().hex();
}

std::string
file_stamp(const fs::path& file)
{
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  const auto time = fs::last_write_time(file, ec);
  return ContentHash::of(std::format("{}\t{}\t{}", file.string(), size,
                                     time.time_since_epoch().count()))
      .hex();
}

Status
write_kept(const fs::path& path, const Kept& k)
{
  std::vector<std::uint8_t> blob;
  Json ports = Json::array();
  for (const auto& p : k.ports) {
    Json items = Json::array();
    for (std::size_t i = 0; i < p.tensors.size(); ++i) {
      const TensorPtr& t = p.tensors[i];
      if (!t) {
        return make_error(Code::InvalidArgument, std::format(
            "{}: an item with no tensor", p.name));
      }
      pad_to(blob, kAlign);
      const std::size_t at = blob.size();
      append_contiguous(*t, blob);
      const bool shared = i < p.shared.size() && p.shared[i];
      Json item = {{"type", dtype_name(t->dtype)},
                   {"shape", t->shape},
                   {"storage", shared ? "shared" : "cpu"},
                   {"data", Json::array({at, blob.size() - at})}};
      if (i < p.sidebands.size() && !p.sidebands[i].empty()) {
        item["sideband"] = put(blob, p.sidebands[i]);
      }
      items.push_back(std::move(item));
    }
    Json port = {{"name", p.name}, {"list", p.list},
                 {"items", std::move(items)}};
    if (!p.list_sideband.empty()) {
      port["list_sideband"] = put(blob, p.list_sideband);
    }
    ports.push_back(std::move(port));
  }
  const std::string header = to_text(Json{{"ports", std::move(ports)}});
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  const std::uint32_t version = kVersion;
  const std::uint64_t hn = header.size();
  f.write(kMagic, sizeof(kMagic));
  f.write(reinterpret_cast<const char*>(&version), sizeof(version));
  f.write(reinterpret_cast<const char*>(&hn), sizeof(hn));
  f.write(header.data(), static_cast<std::streamsize>(header.size()));
  f.write(reinterpret_cast<const char*>(blob.data()),
          static_cast<std::streamsize>(blob.size()));
  f.close();
  if (!f) {
    return make_error(Code::Io, std::format("could not write {}",
                                            path.string()));
  }
  return ok_status();
}

Result<Kept>
read_kept(const fs::path& path)
{
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return make_error(Code::NotFound, path.string());
  }
  const auto size = static_cast<std::size_t>(f.tellg());
  auto file = std::make_shared<std::vector<std::uint8_t>>(size);
  f.seekg(0);
  f.read(reinterpret_cast<char*>(file->data()),
         static_cast<std::streamsize>(size));
  if (!f) {
    return make_error(Code::Io, std::format("could not read {}",
                                            path.string()));
  }
  auto bad = [&](std::string_view why) {
    return make_error(Code::Corrupt, std::format("{}: {}", path.string(),
                                                 why));
  };
  constexpr std::size_t kHead = sizeof(kMagic) + 4 + 8;
  if (size < kHead || std::memcmp(file->data(), kMagic, sizeof(kMagic))) {
    return bad("not a kept encoding");
  }
  std::uint32_t version = 0;
  std::uint64_t hn = 0;
  std::memcpy(&version, file->data() + sizeof(kMagic), 4);
  std::memcpy(&hn, file->data() + sizeof(kMagic) + 4, 8);
  if (version != kVersion) {
    return bad(std::format("version {}", version));
  }
  if (hn > size - kHead) {
    return bad("cut short");
  }
  const Json header = Json::parse(
      std::string_view(reinterpret_cast<const char*>(file->data()) + kHead,
                       static_cast<std::size_t>(hn)),
      nullptr, false);
  if (!header.is_object()) {
    return bad("its header is not a document");
  }
  const std::uint8_t* blob = file->data() + kHead + hn;
  const std::size_t blob_size = size - kHead - static_cast<std::size_t>(hn);
  auto bytes_of = [&](const Json& span, std::string& out) {
    std::size_t at = 0, n = 0;
    if (!span_of(span, blob_size, &at, &n)) {
      return false;
    }
    out.assign(reinterpret_cast<const char*>(blob) + at, n);
    return true;
  };
  Kept k;
  for (const auto& jp : jget(header, "ports", Json::array())) {
    Kept::Port p;
    p.name = jget<std::string>(jp, "name", "");
    p.list = jget(jp, "list", false);
    if (jp.contains("list_sideband") &&
        !bytes_of(jp["list_sideband"], p.list_sideband)) {
      return bad("a list's sideband out of bounds");
    }
    for (const auto& ji : jget(jp, "items", Json::array())) {
      auto t = std::make_shared<Tensor>();
      if (!dtype_from_str(jget<std::string>(ji, "type", ""), t->dtype)) {
        return bad("an unknown element type");
      }
      for (const auto& d : jget(ji, "shape", Json::array())) {
        if (!d.is_number_integer() || d.get<std::int64_t>() < 0) {
          return bad("a shape not of sizes");
        }
        t->shape.push_back(d.get<std::int64_t>());
      }
      std::size_t at = 0, n = 0;
      if (!span_of(jget(ji, "data", Json()), blob_size, &at, &n) ||
          n != t->element_count() * t->element_size()) {
        return bad("a tensor's bytes out of bounds");
      }
      t->data = n > 0 ? blob + at : nullptr;
      t->byte_size = n;
      t->owner = file;
      std::string sb;
      if (ji.contains("sideband") && !bytes_of(ji["sideband"], sb)) {
        return bad("a sideband out of bounds");
      }
      p.tensors.push_back(std::move(t));
      p.sidebands.push_back(std::move(sb));
      p.shared.push_back(jget<std::string>(ji, "storage", "cpu") ==
                         "shared");
    }
    if (!p.list && p.tensors.size() != 1) {
      return bad("a port of no tensor");
    }
    k.ports.push_back(std::move(p));
  }
  return k;
}

}
