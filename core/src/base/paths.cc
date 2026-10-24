#include "valtz/base/paths.h"

#include "valtz/base/json.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/stat.h>

#include <cstdlib>
#include <format>
#include <fstream>

namespace valtz {

namespace fs = std::filesystem;

AppPaths
AppPaths::defaults(const fs::path& support_override)
{
  const char* h = std::getenv("HOME");
  fs::path home = h ? h : "/tmp";
  AppPaths p;
  p.support = support_override.empty()
                  ? home / "Library/Application Support" / kBundleId
                  : support_override;
  p.models = p.support / "models";
  p.cache = p.support / "cache";
  p.engine = p.support / "engine";
  p.projects = home / "Movies/Valtz";
  return p;
}

AppPaths
AppPaths::standard(const fs::path& support_override)
{
  AppPaths p = defaults(support_override);
  const Locations l = read_locations(p.support);
  if (!l.projects.empty()) {
    p.projects = l.projects;
  }
  if (!l.models.empty()) {
    p.models = l.models;
  }
  if (!l.cache.empty()) {
    p.cache = l.cache;
  }
  p.models_kept = l.models_kept;
  return p;
}

namespace {

fs::path
path_of(const Json& j, const char* key)
{
  return fs::path(jget<std::string>(j, key, ""));
}

std::vector<fs::path>
paths_of(const Json& j, const char* key)
{
  std::vector<fs::path> out;
  for (const auto& x : jget(j, key, Json::array())) {
    if (x.is_string() && !x.get_ref<const std::string&>().empty()) {
      out.emplace_back(x.get<std::string>());
    }
  }
  return out;
}

}

Locations
read_locations(const fs::path& support)
{
  std::ifstream in(support / "locations.json");
  const Json j = in ? Json::parse(in, nullptr, /*allow_exceptions=*/false)
                    : Json();
  Locations l;
  if (!j.is_object()) {
    return l;
  }
  l.projects = path_of(j, "projects");
  l.models = path_of(j, "models");
  l.cache = path_of(j, "cache");
  l.models_kept = paths_of(j, "models_kept");
  return l;
}

Status
write_locations(const fs::path& support, const Locations& l)
{
  Json j = Json::object();
  auto put = [&j](const char* key, const fs::path& p) {
    if (!p.empty()) {
      j[key] = p.string();
    }
  };
  auto put_all = [&j](const char* key, const std::vector<fs::path>& ps) {
    if (ps.empty()) {
      return;
    }
    Json a = Json::array();
    for (const auto& p : ps) {
      a.push_back(p.string());
    }
    j[key] = a;
  };
  put("projects", l.projects);
  put("models", l.models);
  put("cache", l.cache);
  put_all("models_kept", l.models_kept);
  const fs::path file = support / "locations.json";
  const fs::path tmp = file.string() + ".tmp";
  std::error_code ec;
  fs::create_directories(support, ec);
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << to_text(j, 2) << "\n";
    if (!out) {
      return make_error(Code::Io, std::format("cannot write {}",
                                              tmp.string()));
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot write {}: {}",
                                            file.string(), ec.message()));
  }
  return {};
}

bool
volume_is_internal(const fs::path& p)
{
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(p.c_str()),
      static_cast<CFIndex>(p.native().size()), true);
  if (!url) {
    return true;
  }
  // A network share is no internal drive either, whatever it says.
  bool internal = true;
  for (CFStringRef key : {kCFURLVolumeIsInternalKey, kCFURLVolumeIsLocalKey}) {
    CFBooleanRef v = nullptr;
    if (CFURLCopyResourcePropertyForKey(url, key, &v, nullptr) && v) {
      internal = internal && CFBooleanGetValue(v);
      CFRelease(v);
    }
  }
  CFRelease(url);
  return internal;
}

bool
same_volume(const fs::path& a, const fs::path& b)
{
  // Each as far up as it exists: a folder not made yet is where its
  // parent is.
  auto device = [](fs::path p) -> dev_t {
    struct stat st;
    while (!p.empty()) {
      if (stat(p.c_str(), &st) == 0) {
        return st.st_dev;
      }
      if (p == p.parent_path()) {
        break;
      }
      p = p.parent_path();
    }
    return static_cast<dev_t>(-1);
  };
  const dev_t da = device(a);
  return da != static_cast<dev_t>(-1) && da == device(b);
}

bool
path_within(const fs::path& inner, const fs::path& outer)
{
  const fs::path rel = inner.lexically_normal().lexically_relative(
      outer.lexically_normal());
  return !rel.empty() && *rel.begin() != "..";
}

}
