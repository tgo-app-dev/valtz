#include "valtz/models/model-store.h"

#include "valtz/base/json.h"

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

ModelStore::ModelStore(std::vector<fs::path> roots, fs::path links_file)
  : _roots(std::move(roots)), _links_file(std::move(links_file))
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
ModelStore::default_roots(const fs::path& primary)
{
  std::vector<fs::path> out{primary};
  const char* home = std::getenv("HOME");
  fs::path h = home ? fs::path(home) : fs::path("/tmp");
  std::error_code ec;
  for (auto p : {h / "vpipe/models", h / "dump/vpipe-test/models"}) {
    if (fs::is_directory(p, ec)) {
      out.push_back(p);
    }
  }
  return out;
}

fs::path
InstallInfo::path() const
{
  return file.empty() ? dir : dir / file;
}

InstallInfo
ModelStore::scan_(const ModelEntry& e) const
{
  // Linked: there, and nowhere else.
  if (const fs::path l = link_of(e.id); !l.empty()) {
    return scan_link_(e, l);
  }
  InstallInfo best;
  for (const auto& root : _roots) {
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
