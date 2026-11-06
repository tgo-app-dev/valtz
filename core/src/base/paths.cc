#include "valtz/base/paths.h"

#include "valtz/base/json.h"

#include <CoreFoundation/CoreFoundation.h>
#include <DiskArbitration/DiskArbitration.h>
#include <sys/mount.h>
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

namespace {

std::string
cf_text(CFTypeRef v)
{
  if (!v || CFGetTypeID(v) != CFStringGetTypeID()) {
    return {};
  }
  const auto s = static_cast<CFStringRef>(v);
  const CFIndex n = CFStringGetMaximumSizeForEncoding(
      CFStringGetLength(s), kCFStringEncodingUTF8) + 1;
  std::string out(static_cast<std::size_t>(n), '\0');
  if (!CFStringGetCString(s, out.data(), n, kCFStringEncodingUTF8)) {
    return {};
  }
  out.resize(std::char_traits<char>::length(out.c_str()));
  // A drive's model comes padded ("APPLE SSD AP1024Z   ").
  while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) {
    out.pop_back();
  }
  return out;
}

}  // namespace

VolumeInfo
volume_info(const fs::path& p)
{
  VolumeInfo v;
  v.internal = volume_is_internal(p);
  // The name the Finder shows: "Macintosh HD" for a path on the data
  // volume, whose own name is "Data".
  if (CFURLRef url = CFURLCreateFromFileSystemRepresentation(
          nullptr, reinterpret_cast<const UInt8*>(p.c_str()),
          static_cast<CFIndex>(p.native().size()), true)) {
    CFTypeRef name = nullptr;
    if (CFURLCopyResourcePropertyForKey(url, kCFURLVolumeLocalizedNameKey,
                                        &name, nullptr) && name) {
      v.name = cf_text(name);
      CFRelease(name);
    }
    CFRelease(url);
  }
  // How the drive is connected: DiskArbitration's word on the volume
  // mounted where the path lives.
  struct statfs st;
  if (statfs(p.c_str(), &st) != 0) {
    return v;
  }
  v.network = (st.f_flags & MNT_LOCAL) == 0;
  DASessionRef session = DASessionCreate(kCFAllocatorDefault);
  if (!session) {
    return v;
  }
  CFURLRef mount = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(st.f_mntonname),
      static_cast<CFIndex>(std::char_traits<char>::length(st.f_mntonname)),
      true);
  DADiskRef disk = mount ? DADiskCreateFromVolumePath(
                               kCFAllocatorDefault, session, mount)
                         : nullptr;
  if (disk) {
    if (CFDictionaryRef d = DADiskCopyDescription(disk)) {
      v.protocol = cf_text(
          CFDictionaryGetValue(d, kDADiskDescriptionDeviceProtocolKey));
      v.device = cf_text(
          CFDictionaryGetValue(d, kDADiskDescriptionDeviceModelKey));
      CFRelease(d);
    }
    CFRelease(disk);
  }
  if (mount) {
    CFRelease(mount);
  }
  CFRelease(session);
  return v;
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
