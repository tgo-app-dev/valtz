#include "valtz/models/model-check.h"

#include "valtz/base/message.h"
#include "valtz/models/drawthings.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <map>
#include <set>

namespace valtz::models {

namespace fs = std::filesystem;

namespace {

using Level = ModelCheck::Level;

std::string
lower(std::string s)
{
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

// Hidden: a tool's own bookkeeping (.cache, .git), never the model.
bool
hidden(const fs::path& rel)
{
  for (const auto& part : rel) {
    const std::string s = part.string();
    if (!s.empty() && s[0] == '.' && s != "." && s != "..") {
      return true;
    }
  }
  return false;
}

std::string
joined(const std::vector<std::string>& v, std::size_t most = 3)
{
  std::string out;
  for (std::size_t i = 0; i < v.size() && i < most; ++i) {
    out += (i ? ", " : "") + v[i];
  }
  if (v.size() > most) {
    out += std::format(" and {} more", v.size() - most);
  }
  return out;
}

int
dtype_bytes(std::string_view t)
{
  if (t == "F32" || t == "I32" || t == "U32") {
    return 4;
  }
  if (t == "F64" || t == "I64" || t == "U64") {
    return 8;
  }
  if (t.starts_with("F8") || t == "I8" || t == "U8" || t == "BOOL") {
    return 1;
  }
  return 2;
}

// A safetensors file's header: its tensors, and the bytes the file
// should hold -- 8, the header, the data to its furthest tensor's end.
struct StHeader {
  bool          readable = false;
  std::uint64_t want = 0;
  std::uint64_t have = 0;
  Json          tensors = Json::object();
};

StHeader
read_safetensors(const fs::path& p)
{
  StHeader h;
  std::error_code ec;
  h.have = fs::file_size(p, ec);
  if (ec || h.have < 8) {
    return h;
  }
  std::ifstream in(p, std::ios::binary);
  unsigned char b[8] = {};
  if (!in.read(reinterpret_cast<char*>(b), 8)) {
    return h;
  }
  std::uint64_t n = 0;
  for (int i = 7; i >= 0; --i) {
    n = (n << 8) | b[i];
  }
  // The format's own limit is 100 MB of header.
  if (n == 0 || n > (std::uint64_t{100} << 20) || n > h.have - 8) {
    return h;
  }
  std::string text(n, '\0');
  if (!in.read(text.data(), static_cast<std::streamsize>(n))) {
    return h;
  }
  Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    return h;
  }
  j.erase("__metadata__");
  std::uint64_t end = 0;
  for (const auto& t : j) {
    const Json off = jget(t, "data_offsets", Json());
    if (!off.is_array() || off.size() != 2 || !off[1].is_number()) {
      return h;
    }
    end = std::max(end, off[1].get<std::uint64_t>());
  }
  h.want = 8 + n + end;
  h.tensors = std::move(j);
  h.readable = true;
  return h;
}

std::string
head_bytes(const fs::path& p, std::size_t n, std::uint64_t at = 0)
{
  std::ifstream in(p, std::ios::binary);
  std::string s(n, '\0');
  in.seekg(static_cast<std::streamoff>(at));
  in.read(s.data(), static_cast<std::streamsize>(n));
  s.resize(static_cast<std::size_t>(std::max<std::streamsize>(0,
                                                              in.gcount())));
  return s;
}

// A PyTorch archive (.ckpt, .pth, .pt, a zip .bin) ends with a zip's
// end-of-directory record; one cut short does not. A legacy pickle has
// no such end: not judged.
bool
zip_whole(const fs::path& p, std::uint64_t size)
{
  if (head_bytes(p, 4) != std::string("PK\x03\x04", 4)) {
    return true;
  }
  const std::uint64_t tail = std::min<std::uint64_t>(size, 65557);
  const std::string end = head_bytes(p, tail, size - tail);
  return end.find(std::string("PK\x05\x06", 4)) != std::string::npos;
}

// What a JSON file names its model: a class, a model type, the
// architectures.
std::vector<std::string>
names_in(const fs::path& f)
{
  std::ifstream in(f);
  const Json j = in ? Json::parse(in, nullptr, false) : Json();
  std::vector<std::string> out;
  if (!j.is_object()) {
    return out;
  }
  for (const char* k : {"_class_name", "model_type"}) {
    if (const auto s = jget<std::string>(j, k, ""); !s.empty()) {
      out.push_back(s);
    }
  }
  for (const auto& a : jget(j, "architectures", Json::array())) {
    if (a.is_string()) {
      out.push_back(a.get<std::string>());
    }
  }
  return out;
}

std::vector<std::string>
strings(const Json& v)
{
  std::vector<std::string> out;
  if (v.is_string()) {
    out.push_back(v.get<std::string>());
  }
  for (const auto& s : v.is_array() ? v : Json::array()) {
    if (s.is_string()) {
      out.push_back(s.get<std::string>());
    }
  }
  return out;
}

// The catalog's `is` against a folder: the JSON files of it there, and
// what they name.
struct Match {
  int         compared = 0;
  int         matched = 0;
  std::string identity;  // the first name matched
  std::string found;     // the first one that differs
  std::string wanted;    // ... and what it should be
};

Match
match_at(const Json& is, const fs::path& dir)
{
  Match m;
  std::error_code ec;
  for (auto it = is.begin(); it != is.end(); ++it) {
    const fs::path f = dir / it.key();
    if (!fs::is_regular_file(f, ec)) {
      continue;
    }
    const auto wants = strings(*it);
    const auto names = names_in(f);
    ++m.compared;
    const auto hit = std::ranges::find_if(names, [&](const auto& n) {
      return std::ranges::find(wants, n) != wants.end();
    });
    if (hit != names.end()) {
      ++m.matched;
      if (m.identity.empty()) {
        m.identity = *hit;
      }
    } else if (m.found.empty()) {
      m.found = names.empty() ? it.key() : names.front();
      m.wanted = wants.empty() ? std::string() : wants.front();
    }
  }
  return m;
}

bool
fits(const Json& is, const fs::path& dir)
{
  const Match m = match_at(is, dir);
  return m.compared > 0 && m.matched == m.compared;
}

// The folders inside `dir`, `depth` levels down, that are the model. A
// folder that is one is not looked into; at most `budget` folders are
// visited (a whole models folder chosen).
void
find_inside(const Json& is, const fs::path& dir, int depth, int& budget,
            std::vector<fs::path>& out)
{
  if (depth == 0) {
    return;
  }
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(dir, ec)) {
    if (--budget < 0) {
      return;
    }
    if (!d.is_directory(ec) || hidden(d.path().filename())) {
      continue;
    }
    if (fits(is, d.path())) {
      out.push_back(d.path());
    } else {
      find_inside(is, d.path(), depth - 1, budget, out);
    }
  }
}

// Laid out as a Comfy-Org repack: one file a component, by role.
bool
repack(const fs::path& dir)
{
  std::error_code ec;
  return fs::is_directory(dir / "diffusion_models", ec) ||
         fs::is_directory(dir / "text_encoders", ec);
}

void
note(ModelCheck& c, Level l, const Message& m, const MessageArgs& args)
{
  c.notes.push_back({l, message_fields(m, args)});
}

bool
has_error(const ModelCheck& c)
{
  return std::ranges::any_of(c.notes, [](const ModelCheck::Note& n) {
    return n.level == Level::Error;
  });
}

std::string
rel_text(const fs::path& p, const fs::path& base)
{
  const fs::path r = p.lexically_relative(base);
  return r.empty() || *r.begin() == ".." ? p.string() : r.string();
}

// One weight file: whole, readable as what its name says. Its tensor
// names, for an index to be checked against.
void
check_weight_file(const fs::path& f, const fs::path& base, ModelCheck& c,
                  std::map<std::string, std::set<std::string>>* names)
{
  std::error_code ec;
  const std::string ext = lower(f.extension().string());
  const std::uint64_t size = fs::file_size(f, ec);
  const std::string file = rel_text(f, base);
  if (ext == ".safetensors") {
    ++c.weight_files;
    const StHeader h = read_safetensors(f);
    if (!h.readable) {
      note(c, Level::Error, msg::kCheckUnreadable,
           {{"file", file}, {"format", "safetensors"}});
      return;
    }
    if (h.have < h.want) {
      note(c, Level::Error, msg::kCheckCutShort,
           {{"file", file}, {"have", size_text(h.have)},
            {"want", size_text(h.want)}});
    }
    c.tensors += h.tensors.size();
    if (names) {
      auto& set = (*names)[f.lexically_normal().string()];
      for (auto it = h.tensors.begin(); it != h.tensors.end(); ++it) {
        set.insert(it.key());
      }
    }
  } else if (ext == ".ckpt" || ext == ".pth" || ext == ".pt" ||
             ext == ".bin") {
    ++c.weight_files;
    if (!zip_whole(f, size)) {
      note(c, Level::Error, msg::kCheckUnreadable,
           {{"file", file}, {"format", "PyTorch"}});
    }
  } else if (ext == ".gguf") {
    ++c.weight_files;
    if (head_bytes(f, 4) != "GGUF") {
      note(c, Level::Error, msg::kCheckUnreadable,
           {{"file", file}, {"format", "GGUF"}});
    }
  } else if (ext == ".tar") {
    // A CoreML package, archived (vpipe-supplement's).
    ++c.weight_files;
    if (head_bytes(f, 5, 257) != "ustar") {
      note(c, Level::Error, msg::kCheckUnreadable,
           {{"file", file}, {"format", "tar"}});
    }
  }
}

// A folder's files: each whole, every part an index lists there and
// holding its tensors, every component its pipeline lists.
void
check_folder_files(const fs::path& dir, ModelCheck& c)
{
  std::error_code ec;
  std::map<std::string, std::set<std::string>> names;
  std::vector<fs::path> indexes;
  bool unfinished = false;
  for (auto it = fs::recursive_directory_iterator(
           dir, fs::directory_options::follow_directory_symlink |
                    fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    const fs::path& p = it->path();
    const fs::path rel = p.lexically_relative(dir);
    const std::string ext = lower(p.extension().string());
    if (it->is_directory(ec)) {
      if (p.filename() == ".git") {
        it.disable_recursion_pending();
      } else if (ext == ".mlpackage" || ext == ".mlmodelc") {
        ++c.weight_files;
        it.disable_recursion_pending();
      }
      continue;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    c.bytes += it->file_size(ec);
    // A download still running, or stopped: its file is not whole.
    if (ext == ".part" || ext == ".incomplete" || ext == ".download") {
      if (!unfinished) {
        note(c, Level::Error, msg::kCheckUnfinished,
             {{"path", dir.string()}, {"file", rel.string()}});
        unfinished = true;
      }
      continue;
    }
    if (hidden(rel)) {
      continue;
    }
    if (p.filename().string().ends_with(".index.json")) {
      indexes.push_back(p);
      continue;
    }
    check_weight_file(p, dir, c, &names);
  }
  for (const auto& idx : indexes) {
    std::ifstream in(idx);
    const Json j = in ? Json::parse(in, nullptr, false) : Json();
    const Json map = jget(j, "weight_map", Json::object());
    if (!map.is_object()) {
      continue;
    }
    std::vector<std::string> missing;
    std::uint64_t lost = 0;
    for (auto it = map.begin(); it != map.end(); ++it) {
      if (!it->is_string()) {
        continue;
      }
      const std::string shard = it->get<std::string>();
      const fs::path sp = (idx.parent_path() / shard).lexically_normal();
      if (!fs::exists(sp, ec)) {
        if (std::ranges::find(missing, shard) == missing.end()) {
          missing.push_back(shard);
        }
        continue;
      }
      const auto n = names.find(sp.string());
      if (n != names.end() && !n->second.contains(it.key())) {
        ++lost;
      }
    }
    const std::string index = rel_text(idx, dir);
    if (!missing.empty()) {
      note(c, Level::Error, msg::kCheckPartsMissing,
           {{"count", std::to_string(missing.size())}, {"index", index},
            {"files", joined(missing)}});
    }
    if (lost > 0) {
      note(c, Level::Error, msg::kCheckTensorsMissing,
           {{"count", std::to_string(lost)}, {"index", index}});
    }
  }
  // A diffusers pipeline lists its components: each a folder of it.
  {
    std::ifstream in(dir / "model_index.json");
    Json j = in ? Json::parse(in, nullptr, false) : Json();
    if (!j.is_object()) {
      j = Json::object();
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      const Json& v = *it;
      if (it.key().starts_with("_") || !v.is_array() || v.size() != 2 ||
          !v[1].is_string()) {
        continue;
      }
      if (!fs::exists(dir / it.key(), ec)) {
        note(c, Level::Error, msg::kCheckComponentMissing,
             {{"path", dir.string()}, {"component", it.key()}});
      }
    }
  }
  if (c.weight_files == 0) {
    note(c, Level::Error, msg::kCheckNoWeights, {{"path", dir.string()}});
  }
}

// What the catalog's `is` asks for, for a person: "model_index.json:
// Krea2Pipeline".
std::string
wanted_text(const Json& is)
{
  std::vector<std::string> parts;
  for (auto it = is.begin(); it != is.end(); ++it) {
    const auto w = strings(*it);
    parts.push_back(it.key() + ": " + (w.empty() ? "?" : w.front()));
  }
  return joined(parts, 4);
}

std::string
files_text(const Json& is)
{
  std::vector<std::string> parts;
  for (auto it = is.begin(); it != is.end(); ++it) {
    parts.push_back(it.key());
  }
  return joined(parts, 4);
}

void
check_folder(const ModelEntry& e, ModelCheck& c, const Catalog* cat)
{
  std::error_code ec;
  fs::path dir = fs::is_regular_file(c.chosen, ec) ? c.chosen.parent_path()
                                                   : c.chosen;
  Json is = jget(e.check, "is", Json::object());
  if (!is.is_object()) {
    is = Json::object();
  }
  const auto files = strings(jget(e.check, "files", Json::array()));
  // A repo of several models: its own folder in it.
  if (!e.subdir.empty() && fs::is_directory(dir / e.subdir, ec)) {
    dir /= e.subdir;
    c.moved = "down";
  }
  Match m = match_at(is, dir);
  if (!is.empty() && m.compared == 0) {
    // Nothing here to tell: the model inside the folder chosen, or the
    // one around it.
    std::vector<fs::path> inside;
    int budget = 4000;
    find_inside(is, dir, 3, budget, inside);
    if (inside.size() > 1) {
      // Several: the one named as the repo is, if one is.
      const std::string repo = lower(fs::path(e.hf_path).filename()
                                         .string());
      std::vector<fs::path> named;
      for (const auto& p : inside) {
        if (lower(p.filename().string()) == repo ||
            (!e.subdir.empty() &&
             lower(p.filename().string()) == lower(e.subdir))) {
          named.push_back(p);
        }
      }
      if (named.size() == 1) {
        inside = named;
      }
    }
    if (inside.size() == 1) {
      dir = inside.front();
      c.moved = "down";
    } else if (inside.size() > 1) {
      std::vector<std::string> paths;
      for (const auto& p : inside) {
        paths.push_back(rel_text(p, dir));
      }
      c.path = dir;
      note(c, Level::Error, msg::kCheckSeveral,
           {{"path", dir.string()}, {"count", std::to_string(inside.size())},
            {"model", e.name}, {"paths", joined(paths)}});
      return;
    } else {
      fs::path up = dir;
      for (int i = 0; i < 2 && up.has_parent_path() &&
                      up.parent_path() != up; ++i) {
        up = up.parent_path();
        if (fits(is, up)) {
          dir = up;
          c.moved = "up";
          break;
        }
      }
    }
    m = match_at(is, dir);
  }
  c.path = dir;
  if (c.moved == "down" && dir != c.chosen) {
    note(c, Level::Info, msg::kCheckFoundInside,
         {{"model", e.name}, {"path", dir.string()}});
  } else if (c.moved == "up") {
    note(c, Level::Info, msg::kCheckFoundAbove,
         {{"model", e.name}, {"path", dir.string()}});
  } else {
    c.moved.clear();
  }
  // Another model of its repo -- MiniMax H3's Ref2VA for its FL2VA: the
  // two are byte for byte alike but in what they were trained to do.
  if (cat && !e.subdir.empty()) {
    const std::string here = lower(dir.filename().string());
    for (const auto& o : cat->models()) {
      if (o.id != e.id && o.hf_path == e.hf_path && !o.subdir.empty() &&
          lower(o.subdir) == here && lower(e.subdir) != here) {
        note(c, Level::Error, msg::kCheckWrongPart,
             {{"path", dir.string()}, {"found", o.name},
              {"model", e.name}});
        return;
      }
    }
  }
  if (m.compared > m.matched) {
    note(c, Level::Error, msg::kCheckWrongModel,
         {{"path", dir.string()}, {"found", m.found}, {"model", e.name},
          {"wanted", m.wanted}});
    return;
  }
  // Nothing of it here, nor laid out as a repack (which runs as it is):
  // not the model -- and a whole models folder is not walked for it.
  if (!is.empty() && m.compared == 0 && !repack(dir)) {
    note(c, Level::Error, msg::kCheckNotThere,
         {{"path", dir.string()}, {"model", e.name},
          {"wanted", files_text(is)}});
    return;
  }
  check_folder_files(dir, c);
  bool all_files = true;
  for (const auto& f : files) {
    if (!fs::exists(dir / f, ec)) {
      all_files = false;
      note(c, Level::Error, msg::kCheckFileMissing,
           {{"file", f}, {"path", dir.string()}});
    }
  }
  if (m.matched > 0) {
    c.confirmed = true;
    c.identity = m.identity;
  } else if (is.empty() && !files.empty() && all_files) {
    c.confirmed = true;
    c.identity = files.front();
  } else if (!is.empty()) {
    // A repack: not as its download is laid out, run as it is.
    note(c, Level::Warning, msg::kCheckUnconfirmed,
         {{"model", e.name}, {"path", dir.string()},
          {"wanted", wanted_text(is)}});
  }
}

void
check_file(const ModelEntry& e, ModelCheck& c)
{
  std::error_code ec;
  const std::string want_ext = lower(fs::path(e.file).extension().string());
  fs::path pick;
  if (fs::is_regular_file(c.chosen, ec)) {
    pick = c.chosen;
  } else if (fs::is_regular_file(c.chosen / e.file, ec)) {
    pick = c.chosen / e.file;
  } else {
    // A folder of its kind of file (a LoRA's): the largest; else the
    // file of its name inside it.
    std::uintmax_t most = 0;
    for (const auto& f : fs::directory_iterator(c.chosen, ec)) {
      if (f.is_regular_file(ec) &&
          lower(f.path().extension().string()) == want_ext) {
        const auto n = f.file_size(ec);
        if (!ec && n > most) {
          most = n;
          pick = f.path();
        }
      }
    }
    if (pick.empty()) {
      std::vector<fs::path> found;
      for (auto it = fs::recursive_directory_iterator(
               c.chosen, fs::directory_options::skip_permission_denied, ec);
           it != fs::recursive_directory_iterator() && found.size() < 8;
           it.increment(ec)) {
        if (ec) {
          break;
        }
        if (it.depth() > 2) {
          it.disable_recursion_pending();
        }
        if (it->path().filename() == e.file &&
            it->is_regular_file(ec)) {
          found.push_back(it->path());
        }
      }
      if (found.size() > 1) {
        std::vector<std::string> paths;
        for (const auto& p : found) {
          paths.push_back(rel_text(p, c.chosen));
        }
        c.path = c.chosen;
        note(c, Level::Error, msg::kCheckSeveral,
             {{"path", c.chosen.string()},
              {"count", std::to_string(found.size())},
              {"model", e.name}, {"paths", joined(paths)}});
        return;
      }
      if (found.size() == 1) {
        pick = found.front();
        c.moved = "down";
        note(c, Level::Info, msg::kCheckFoundInside,
             {{"model", e.name}, {"path", pick.string()}});
      }
    }
  }
  if (pick.empty()) {
    c.path = c.chosen;
    note(c, Level::Error, msg::kCheckNoWeights,
         {{"path", c.chosen.string()}});
    return;
  }
  c.path = pick;
  c.bytes = fs::file_size(pick, ec);
  const std::size_t before = c.notes.size();
  check_weight_file(pick, pick.parent_path(), c, nullptr);
  if (c.weight_files == 0) {
    // Not weights (a prompt guide's text): there, and not empty.
    c.weight_files = c.bytes > 0 ? 1 : 0;
  }
  if (c.notes.size() > before) {
    return;  // not whole
  }
  const auto words = strings(jget(e.check, "tensors", Json::array()));
  if (!words.empty() && lower(pick.extension().string()) ==
                            ".safetensors") {
    const StHeader h = read_safetensors(pick);
    for (const auto& w : words) {
      const std::string lw = lower(w);
      bool any = false;
      for (auto it = h.tensors.begin(); it != h.tensors.end() && !any;
           ++it) {
        any = lower(it.key()).find(lw) != std::string::npos;
      }
      if (!any) {
        note(c, Level::Error, msg::kCheckWrongTensors,
             {{"file", pick.filename().string()}, {"model", e.name},
              {"wanted", joined(words)}});
        return;
      }
    }
    c.confirmed = true;
    c.identity = pick.filename().string();
  } else if (pick.filename() == e.file) {
    // Named as its download names it.
    c.confirmed = true;
    c.identity = e.file;
  } else {
    note(c, Level::Warning, msg::kCheckUnconfirmed,
         {{"model", e.name}, {"path", pick.string()}, {"wanted", e.file}});
  }
}

const char*
level_text(Level l)
{
  switch (l) {
  case Level::Info:    return "info";
  case Level::Warning: return "warning";
  case Level::Error:   return "error";
  }
  return "info";
}

}

std::string
size_text(std::uint64_t b)
{
  const double d = static_cast<double>(b);
  if (d >= 1e9) {
    return std::format("{:.1f} GB", d / 1e9);
  }
  if (d >= 1e6) {
    return std::format("{:.0f} MB", d / 1e6);
  }
  if (d >= 1e3) {
    return std::format("{:.0f} KB", d / 1e3);
  }
  return std::format("{} bytes", b);
}

Json
ModelCheck::to_json() const
{
  Json ns = Json::array();
  for (const auto& n : notes) {
    Json m = n.message;
    m["level"] = level_text(n.level);
    ns.push_back(std::move(m));
  }
  return {{"chosen", chosen.string()}, {"path", path.string()},
          {"moved", moved}, {"confirmed", confirmed}, {"usable", usable},
          {"identity", identity}, {"parts", parts_json()}, {"bytes", bytes},
          {"files", weight_files}, {"tensors", tensors},
          {"notes", std::move(ns)}};
}

Json
ModelCheck::parts_json() const
{
  Json out = Json::object();
  for (const auto& [part, file] : parts) {
    out[part] = file.string();
  }
  return out;
}

Json
ModelCheck::error() const
{
  for (const auto& n : notes) {
    if (n.level == Level::Error) {
      return n.message;
    }
  }
  return Json::object();
}

namespace {

// What a Draw Things store vpipe reads is, as the person reads it.
std::string
drawthings_name(const DrawThingsStore& s)
{
  if (s.family == "qwen3-vl") {
    return s.width == 4096   ? "Qwen3-VL 8B text encoder"
           : s.width == 5120 ? "Qwen3-VL 32B text encoder"
                             : "Qwen3-VL text encoder";
  }
  return s.family == "qwen-image-21"       ? "Qwen-Image 2.1 DiT"
         : s.family == "minimax-h3"        ? "MiniMax H3 DiT"
         : s.family == "qwen-image-21-vae" ? "Qwen-Image 2.1 VAE"
         : s.family == "minimax-h3-vae"    ? "MiniMax H3 VAEs"
                                           : s.family;
}

// Whether `s` is `e`'s part, as its catalog `check.drawthings` names
// each ({part: {"family", "width", "not_named"}}): the family, the width
// a text encoder's must have, and not a file named as another release's
// (its tensors say the family, not the release).
bool
drawthings_fits(const Json& want, const DrawThingsStore& s)
{
  return want.is_object() && jget<std::string>(want, "family", "") ==
                                 s.family &&
         (!want.contains("width") || jget(want, "width", 0) == s.width);
}

std::string
drawthings_other_release(const Json& want, const std::string& file)
{
  for (const auto& w : strings(jget(want, "not_named", Json::array()))) {
    if (lower(file).find(lower(w)) != std::string::npos) {
      return w;
    }
  }
  return {};
}

// ...and, where a release is told only by its name (`named`: the Turbo's
// DiT, its tensors the base's), the word it lacks; "" when it has one.
std::string
drawthings_unnamed(const Json& want, const std::string& file)
{
  const auto words = strings(jget(want, "named", Json::array()));
  for (const auto& w : words) {
    if (lower(file).find(lower(w)) != std::string::npos) {
      return {};
    }
  }
  return words.empty() ? std::string() : words.front();
}

// `file`, a Draw Things store, as the part of `e` it is (models/
// drawthings.h): checked, and in `c.parts` when it can be used.
void
check_drawthings(const ModelEntry& e, ModelCheck& c, const fs::path& file)
{
  const std::string f = file.filename().string();
  const DrawThingsStore s = read_drawthings(file);
  c.bytes += s.bytes;
  c.weight_files += 1;
  c.tensors += s.tensors;
  const Json dt = jget(e.check, "drawthings", Json());
  if (!dt.is_object() || dt.empty()) {
    note(c, Level::Error, msg::kCheckDrawThingsNotForModel,
         {{"model", e.name}});
    return;
  }
  if (!s.store || !s.error.empty()) {
    note(c, Level::Error, msg::kCheckUnreadable,
         {{"file", f}, {"format", "Draw Things"}});
    return;
  }
  if (s.cut_short()) {
    note(c, Level::Error, msg::kCheckCutShort,
         {{"file", f}, {"have", size_text(s.bytes)},
          {"want", size_text(s.reach)}});
    return;
  }
  if (!s.unread.empty()) {
    note(c, Level::Error, msg::kCheckDrawThingsUnread,
         {{"file", f}, {"tensor", s.unread}});
    return;
  }
  if (s.rotated > 0) {
    note(c, Level::Error, msg::kCheckDrawThingsRotated, {{"file", f}});
    return;
  }
  if (s.part.empty()) {
    note(c, Level::Error, msg::kCheckDrawThingsOther, {{"file", f}});
    return;
  }
  const Json want = jget(dt, s.part.c_str(), Json());
  if (!drawthings_fits(want, s)) {
    note(c, Level::Error, msg::kCheckDrawThingsWrong,
         {{"file", f}, {"found", drawthings_name(s)}, {"model", e.name}});
    return;
  }
  if (const auto w = drawthings_other_release(want, f); !w.empty()) {
    note(c, Level::Error, msg::kCheckDrawThingsRelease,
         {{"file", f}, {"model", e.name}, {"word", w}});
    return;
  }
  if (const auto w = drawthings_unnamed(want, f); !w.empty()) {
    note(c, Level::Error, msg::kCheckDrawThingsUnnamed,
         {{"file", f}, {"model", e.name}, {"word", w}});
    return;
  }
  c.parts[s.part] = file;
  c.confirmed = true;
  c.identity = "Draw Things";
  const MessageArgs args = {{"file", f}, {"model", e.name},
                            {"size", size_text(s.bytes)}};
  note(c, Level::Info,
       s.part == "dit"       ? msg::kCheckDrawThingsDit
       : s.part == "encoder" ? msg::kCheckDrawThingsEncoder
                             : msg::kCheckDrawThingsVae,
       args);
  if (s.part == "dit" && s.eight_bit > 0) {
    note(c, Level::Info, msg::kCheckDrawThingsEightBit, {});
  }
  if (s.family == "qwen-image-21-vae") {
    note(c, Level::Info, msg::kCheckDrawThingsTextureFix, {});
  }
}

// A folder that holds no download of `e` -- Draw Things' Models -- looked
// through, at its top, for the Draw Things checkpoints of `e`'s parts:
// true when any is found, `c` then their checks. A part with one file is
// linked; one with several (Draw Things keeps a model's f16 beside its
// 8-bit) is named, for the one wanted to be chosen as a file -- and with
// none of a single file, which, asked.
bool
find_drawthings(const ModelEntry& e, ModelCheck& c)
{
  const Json dt = jget(e.check, "drawthings", Json());
  if (!dt.is_object() || dt.empty()) {
    return false;
  }
  std::map<std::string, std::vector<fs::path>> found;
  std::error_code ec;
  for (const auto& f : fs::directory_iterator(c.chosen, ec)) {
    const fs::path& p = f.path();
    if (!f.is_regular_file(ec) || lower(p.extension().string()) != ".ckpt" ||
        !looks_like_drawthings(p)) {
      continue;
    }
    const DrawThingsStore s = read_drawthings(p);
    const Json want = jget(dt, s.part.c_str(), Json());
    const std::string name = p.filename().string();
    if (!s.part.empty() && drawthings_fits(want, s) &&
        drawthings_other_release(want, name).empty() &&
        drawthings_unnamed(want, name).empty()) {
      found[s.part].push_back(p);
    }
  }
  if (found.empty()) {
    return false;
  }
  c.moved = "down";
  c.path = c.chosen;
  // The parts with several files: named, not linked.
  std::vector<std::string> one;
  for (auto& [part, files] : found) {
    if (files.size() == 1) {
      one.push_back(part);
      continue;
    }
    std::ranges::sort(files);
    std::vector<std::string> names;
    for (const auto& p : files) {
      names.push_back(p.filename().string());
    }
    note(c, Level::Warning, msg::kCheckDrawThingsAmbiguous,
         {{"model", e.name}, {"paths", joined(names, names.size())}});
  }
  if (one.empty()) {
    // Nothing to link without a choice: which, asked.
    for (auto& n : c.notes) {
      n.level = Level::Error;
    }
    return true;
  }
  // (Several: each part's own note names its file.)
  for (const char* part : {"dit", "encoder", "vae"}) {
    if (const auto it = found.find(part);
        it != found.end() && it->second.size() == 1) {
      if (found.size() == 1) {
        note(c, Level::Info, msg::kCheckFoundInside,
             {{"model", e.name}, {"path", it->second.front().string()}});
      }
      check_drawthings(e, c, it->second.front());
    }
  }
  // One part alone: that file is what was found.
  if (found.size() == 1) {
    c.path = found.begin()->second.front();
  }
  return true;
}

}

ModelCheck
check_model(const ModelEntry& e, const fs::path& target, const Catalog* cat)
{
  ModelCheck c;
  std::error_code ec;
  c.chosen = fs::absolute(target, ec).lexically_normal();
  if (c.chosen.has_filename() == false && c.chosen.has_parent_path()) {
    c.chosen = c.chosen.parent_path();  // a trailing slash
  }
  if (fs::is_regular_file(c.chosen, ec) &&
      looks_like_drawthings(c.chosen)) {
    c.path = c.chosen;
    check_drawthings(e, c, c.chosen);
  } else if (e.file.empty()) {
    check_folder(e, c, cat);
    // No download there: a Draw Things checkpoint of its DiT may be.
    if (has_error(c) && fs::is_directory(c.chosen, ec)) {
      ModelCheck d;
      d.chosen = c.chosen;
      if (find_drawthings(e, d)) {
        c = std::move(d);
      }
    }
  } else {
    check_file(e, c);
  }
  c.usable = !has_error(c);
  // Draw Things' parts: each said; what they lack, said once.
  if (c.usable && !c.parts.empty()) {
    note(c, Level::Info, msg::kCheckDrawThingsConfigs, {{"model", e.name}});
  }
  if (c.usable && c.parts.empty()) {
    // The headline, first: what it is.
    const MessageArgs args = {
        {"model", e.name}, {"identity", c.identity},
        {"files", std::to_string(c.weight_files)},
        {"size", size_text(c.bytes)}};
    const bool warned = std::ranges::any_of(
        c.notes, [](const ModelCheck::Note& n) {
          return n.level == Level::Warning;
        });
    if (c.confirmed) {
      c.notes.insert(c.notes.begin(),
                     {Level::Info,
                      message_fields(msg::kCheckConfirmed, args)});
    } else if (!warned) {
      c.notes.insert(c.notes.begin(),
                     {Level::Warning, message_fields(msg::kCheckWhole,
                                                     args)});
    }
  }
  std::ranges::stable_sort(
      c.notes, [](const ModelCheck::Note& a, const ModelCheck::Note& b) {
        return static_cast<int>(a.level) > static_cast<int>(b.level);
      });
  return c;
}

std::string
compress_part_folder(const fs::path& dir, const std::string& target)
{
  std::error_code ec;
  const std::string t = lower(target);
  if (t == "dit" || t == "transformer") {
    return "transformer";
  }
  if (t == "text_encoder" || t == "text-encoder" || t == "te") {
    return fs::is_directory(dir / "text_encoder", ec) ||
                   !fs::is_directory(dir / "mllm", ec)
               ? "text_encoder"
               : "mllm";
  }
  return fs::is_directory(dir / target, ec) ? target : std::string();
}

CompressEstimate
estimate_compressed(const fs::path& dir,
                    const std::vector<ModelEntry::CompressPart>& parts,
                    int bits)
{
  CompressEstimate est;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(
           dir, fs::directory_options::follow_directory_symlink |
                    fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    if (it->is_regular_file(ec)) {
      est.original += it->file_size(ec);
    }
  }
  std::uint64_t parts_bytes = 0;
  const double per_weight = bits / 8.0 + 4.0 / 64.0;
  for (const auto& part : parts) {
    const std::string sub = compress_part_folder(dir, part.target);
    if (sub.empty()) {
      continue;
    }
    for (auto it = fs::recursive_directory_iterator(dir / sub, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      if (!it->is_regular_file(ec)) {
        continue;
      }
      const std::uint64_t size = it->file_size(ec);
      parts_bytes += size;
      if (lower(it->path().extension().string()) != ".safetensors") {
        est.written += size;
        continue;
      }
      const StHeader h = read_safetensors(it->path());
      for (auto t = h.tensors.begin(); t != h.tensors.end(); ++t) {
        const auto dtype = jget<std::string>(*t, "dtype", "BF16");
        const Json shape = jget(*t, "shape", Json::array());
        std::uint64_t n = 1;
        for (const auto& d : shape) {
          n *= d.is_number() ? d.get<std::uint64_t>() : 1;
        }
        const std::string name = lower(t.key());
        const bool floaty = dtype.starts_with("F") || dtype == "BF16";
        // A text encoder's head stays as it is too (it predicts no token).
        const bool quantized = shape.size() == 2 && floaty && n >= 4096 &&
                               name.find("embed") == std::string::npos &&
                               name.find("norm") == std::string::npos &&
                               name.find("lm_head") == std::string::npos;
        est.written += quantized
            ? static_cast<std::uint64_t>(static_cast<double>(n) *
                                         per_weight)
            : n * static_cast<std::uint64_t>(dtype_bytes(dtype));
      }
    }
  }
  est.total = est.original - std::min(est.original, parts_bytes) +
              est.written;
  return est;
}

}
