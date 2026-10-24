#include "valtz/models/model-store.h"

#include "valtz/base/json.h"
#include "valtz/base/paths.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <fstream>

namespace valtz::models {

namespace fs = std::filesystem;

const char*
to_str(InstallState s)
{
  switch (s) {
  case InstallState::Missing:   return "missing";
  case InstallState::Partial:   return "partial";
  case InstallState::Installed: return "installed";
  }
  return "?";
}

ModelStore::ModelStore(std::vector<fs::path> roots, fs::path links_file,
                       std::size_t owned)
  : _roots(std::move(roots)),
    _owned(std::clamp<std::size_t>(owned, 1, std::max<std::size_t>(
                                                 _roots.size(), 1))),
    _links_file(std::move(links_file))
{
  if (_links_file.empty()) {
    return;
  }
  std::ifstream in(_links_file);
  const Json j = in ? Json::parse(in, nullptr, false) : Json();
  if (j.is_object()) {
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it->is_string()) {
        _links[it.key()] = fs::path(it->get<std::string>());
      }
    }
    const Json parts = jget(j, "part_links", Json::object());
    for (auto it = parts.begin(); parts.is_object() && it != parts.end();
         ++it) {
      for (auto p = it->begin(); it->is_object() && p != it->end(); ++p) {
        if (p->is_string()) {
          _part_links[it.key()][p.key()] = fs::path(p->get<std::string>());
        }
      }
    }
  }
}

void
ModelStore::save_links_() const
{
  if (_links_file.empty()) {
    return;
  }
  Json j = Json::object();
  for (const auto& [id, p] : _links) {
    j[id] = p.string();
  }
  // (An object: a reader from before them takes strings alone.)
  if (!_part_links.empty()) {
    Json parts = Json::object();
    for (const auto& [id, ps] : _part_links) {
      for (const auto& [part, p] : ps) {
        parts[id][part] = p.string();
      }
    }
    j["part_links"] = std::move(parts);
  }
  std::error_code ec;
  fs::create_directories(_links_file.parent_path(), ec);
  const fs::path tmp = _links_file.string() + ".tmp";
  {
    std::ofstream out(tmp);
    out << to_text(j, 2) << "\n";
  }
  fs::rename(tmp, _links_file, ec);
}

Status
ModelStore::link(const ModelEntry& e, const fs::path& target)
{
  std::error_code ec;
  if (!fs::exists(target, ec)) {
    return make_error(Code::NotFound,
                      std::format("{} is not there", target.string()));
  }
  if (scan_link_(e, target).state != InstallState::Installed) {
    return make_error(Code::InvalidArgument, std::format(
        "{} holds no weights for {}", target.string(), e.name));
  }
  std::lock_guard lk(_mu);
  _links[e.id] = fs::absolute(target, ec);
  _cache.erase(e.id);
  save_links_();
  return ok_status();
}

void
ModelStore::unlink(const std::string& id)
{
  std::lock_guard lk(_mu);
  _links.erase(id);
  _cache.erase(id);
  save_links_();
}

fs::path
ModelStore::link_of(const std::string& id) const
{
  std::lock_guard lk(_mu);
  const auto it = _links.find(id);
  return it == _links.end() ? fs::path() : it->second;
}

Status
ModelStore::link_part(const ModelEntry& e, const std::string& part,
                      const fs::path& file)
{
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    return make_error(Code::NotFound,
                      std::format("{} is not there", file.string()));
  }
  std::lock_guard lk(_mu);
  _part_links[e.id][part] = fs::absolute(file, ec);
  _cache.erase(e.id);
  save_links_();
  return ok_status();
}

void
ModelStore::unlink_parts(const std::string& id)
{
  std::lock_guard lk(_mu);
  _part_links.erase(id);
  _cache.erase(id);
  save_links_();
}

std::map<std::string, fs::path>
ModelStore::part_links_of(const std::string& id) const
{
  std::lock_guard lk(_mu);
  const auto it = _part_links.find(id);
  return it == _part_links.end() ? std::map<std::string, fs::path>()
                                 : it->second;
}

fs::path
InstallInfo::part(const std::string& name) const
{
  const auto it = parts.find(name);
  return it != parts.end() && it->second.present ? it->second.file
                                                 : fs::path();
}

// A linked file or folder, as the entry takes it.
InstallInfo
ModelStore::scan_link_(const ModelEntry& e, const fs::path& target) const
{
  std::error_code ec;
  InstallInfo info;
  info.linked = true;
  if (fs::is_regular_file(target, ec)) {
    info.dir = target.parent_path();
    // A folder model linked to one of its files: the folder.
    info.file = e.file.empty() ? fs::path() : target.filename();
    if (e.file.empty()) {
      return scan_dir_(e, info.dir, info);
    }
    info.state = InstallState::Installed;
    info.bytes = fs::file_size(target, ec);
    return info;
  }
  if (!fs::is_directory(target, ec)) {
    return info;
  }
  if (e.file.empty()) {
    InstallInfo i = scan_dir_(e, target, info);
    i.linked = true;
    return i;
  }
  // A folder for a one-file model: the file it names, or -- a LoRA's
  // folder -- the largest .safetensors in it.
  fs::path pick = target / e.file;
  if (!fs::is_regular_file(pick, ec)) {
    pick.clear();
    std::uintmax_t most = 0;
    for (const auto& f : fs::directory_iterator(target, ec)) {
      if (f.path().extension() == ".safetensors") {
        const auto n = f.file_size(ec);
        if (!ec && n > most) {
          most = n;
          pick = f.path();
        }
      }
    }
  }
  if (!pick.empty()) {
    info.dir = pick.parent_path();
    info.file = pick.filename();
    info.state = InstallState::Installed;
    info.bytes = fs::file_size(pick, ec);
  }
  return info;
}

std::vector<fs::path>
ModelStore::default_roots(const fs::path& primary,
                          const std::vector<fs::path>& kept)
{
  std::vector<fs::path> out{primary};
  auto listed = [&out](const fs::path& p) {
    return std::ranges::any_of(out, [&p](const fs::path& q) {
      return q.lexically_normal() == p.lexically_normal();
    });
  };
  for (const auto& k : kept) {
    if (!listed(k)) {
      out.push_back(k);
    }
  }
  const char* home = std::getenv("HOME");
  fs::path h = home ? fs::path(home) : fs::path("/tmp");
  std::error_code ec;
  for (auto p : {h / "vpipe/models", h / "dump/vpipe-test/models"}) {
    if (fs::is_directory(p, ec) && !listed(p)) {
      out.push_back(p);
    }
  }
  return out;
}

std::vector<fs::path>
ModelStore::roots() const
{
  std::lock_guard lk(_mu);
  return _roots;
}

fs::path
ModelStore::download_root() const
{
  std::lock_guard lk(_mu);
  return _roots.front();
}

std::vector<fs::path>
ModelStore::owned_roots() const
{
  std::lock_guard lk(_mu);
  return {_roots.begin(), _roots.begin() + static_cast<std::ptrdiff_t>(
                                               _owned)};
}

fs::path
ModelStore::own_root_of(const fs::path& p) const
{
  for (const auto& r : owned_roots()) {
    if (path_within(p, r)) {
      return r;
    }
  }
  return {};
}

void
ModelStore::set_roots(std::vector<fs::path> roots, std::size_t owned)
{
  if (roots.empty()) {
    return;
  }
  std::lock_guard lk(_mu);
  _roots = std::move(roots);
  _owned = std::clamp<std::size_t>(owned, 1, _roots.size());
  _cache.clear();
}

void
ModelStore::rebase(const fs::path& from, const fs::path& to)
{
  auto moved = [&](fs::path& p) {
    if (!path_within(p, from)) {
      return false;
    }
    p = to / p.lexically_normal().lexically_relative(
                 from.lexically_normal());
    return true;
  };
  std::lock_guard lk(_mu);
  bool any = false;
  for (auto& [id, p] : _links) {
    any = moved(p) || any;
  }
  for (auto& [id, parts] : _part_links) {
    for (auto& [part, p] : parts) {
      any = moved(p) || any;
    }
  }
  if (any) {
    save_links_();
  }
  _cache.clear();
}

fs::path
InstallInfo::path() const
{
  return file.empty() ? dir : dir / file;
}

fs::path
ModelStore::compressed_dir(const std::string& id, int bits) const
{
  return download_root() / "compressed" / std::format("{}-{}bit", id, bits);
}

namespace {

fs::path
compressed_in(const fs::path& root, const std::string& id, int bits)
{
  return root / "compressed" / std::format("{}-{}bit", id, bits);
}

}

InstallInfo
ModelStore::scan_(const ModelEntry& e) const
{
  // Linked: there, and nowhere else.
  if (const fs::path l = link_of(e.id); !l.empty()) {
    return scan_link_(e, l);
  }
  // Compressed: the copy that replaced its download, whole (marked last,
  // once it passed its check).
  // In any folder of Valtz's own: the models folder, or one kept after
  // it moved.
  std::vector<fs::path> roots;
  std::size_t owned = 1;
  {
    std::lock_guard lk(_mu);
    roots = _roots;
    owned = _owned;
  }
  InstallInfo copy;
  Json mark;
  for (std::size_t r = 0; r < owned && copy.compressed_bits == 0; ++r) {
    for (const int bits : {8, 4}) {
      const fs::path d = compressed_in(roots[r], e.id, bits);
      std::ifstream in(d / kCompressedMarker);
      const Json m = in ? Json::parse(in, nullptr, false) : Json();
      if (!m.is_object() || jget<std::string>(m, "model", "") != e.id) {
        continue;
      }
      InstallInfo info = scan_dir_(e, d, InstallInfo{});
      if (info.state == InstallState::Installed) {
        info.compressed_bits = bits;
        copy = info;
        mark = m;
        break;
      }
    }
  }
  InstallInfo orig = scan_roots_(e, roots);
  if (copy.compressed_bits == 0) {
    return orig;
  }
  // Its original KEPT beside it -- the download it was made from, still
  // whole where it was (not another copy found in vpipe's folders): the
  // copy in use unless its mark set it aside.
  const fs::path from = jget<std::string>(mark, "from", "");
  const bool kept = orig.state == InstallState::Installed &&
                    !from.empty() &&
                    orig.dir.lexically_normal() == from.lexically_normal();
  if (!kept) {
    return copy;
  }
  if (!jget(mark, "in_use", true)) {
    orig.other_dir = copy.dir;
    orig.other_bits = copy.compressed_bits;
    orig.other_bytes = copy.bytes;
    return orig;
  }
  copy.other_dir = orig.dir;
  copy.other_bytes = orig.bytes;
  return copy;
}

// The roots in order: the first whole copy, else the first partial one.
InstallInfo
ModelStore::scan_roots_(const ModelEntry& e,
                        const std::vector<fs::path>& roots) const
{
  InstallInfo best;
  for (const auto& root : roots) {
    fs::path dir = root / e.hf_path;
    if (!e.subdir.empty()) {
      dir /= e.subdir;
    }
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
      continue;
    }
    InstallInfo info;
    info.dir = dir;
    if (!e.file.empty()) {
      // One file of a repo that holds several: that file, complete.
      info.file = e.file;
      const fs::path f = dir / e.file;
      fs::path part = f;
      part += ".part";
      if (fs::is_regular_file(f, ec)) {
        info.state = InstallState::Installed;
        info.bytes = fs::file_size(f, ec);
        return info;
      }
      if (fs::exists(part, ec) && best.state == InstallState::Missing) {
        info.state = InstallState::Partial;
        // What has come down: a paused download's row says how far.
        info.bytes = fs::file_size(part, ec);
        if (ec) {
          info.bytes = 0;
        }
        best = info;
      }
      continue;
    }
    info = scan_dir_(e, dir, info);
    if (info.state == InstallState::Installed) {
      return info;
    }
    if (best.state == InstallState::Missing) {
      best = info;
    }
  }
  return best;
}

// A folder model's folder: installed when it holds weights and no
// partial download.
InstallInfo
ModelStore::scan_dir_(const ModelEntry& e, const fs::path& dir,
                      InstallInfo info) const
{
  (void)e;
  std::error_code ec;
  info.dir = dir;
    bool weights = false;
    bool partial = false;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::follow_directory_symlink |
                      fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      if (!it->is_regular_file(ec)) {
        continue;
      }
      auto ext = it->path().extension().string();
      if (ext == ".part") {
        partial = true;
      }
      if (ext == ".safetensors" || ext == ".gguf" || ext == ".mlpackage" ||
          ext == ".pth" || ext == ".bin") {
        weights = true;
      }
      info.bytes += it->file_size(ec);
    }
    info.state = partial   ? InstallState::Partial
                 : weights ? InstallState::Installed
                           : InstallState::Partial;
  return info;
}

InstallInfo
ModelStore::info(const ModelEntry& e) const
{
  {
    std::lock_guard lk(_mu);
    if (auto it = _cache.find(e.id); it != _cache.end()) {
      return it->second;
    }
  }
  InstallInfo i = scan_(e);
  for (const auto& [part, file] : part_links_of(e.id)) {
    std::error_code ec;
    i.parts[part] = {file, fs::is_regular_file(file, ec)};
  }
  std::lock_guard lk(_mu);
  _cache[e.id] = i;
  return i;
}

void
ModelStore::rescan()
{
  std::lock_guard lk(_mu);
  _cache.clear();
}

}
