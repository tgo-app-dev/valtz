#include "valtz/cache/cache-store.h"

#include "valtz/base/log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <vector>

namespace valtz::cache {

namespace fs = std::filesystem;

namespace {

std::int64_t
now_ns()
{
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

std::int64_t
mtime_ns(const fs::path& p)
{
  struct stat st;
  if (stat(p.c_str(), &st) != 0) {
    return 0;
  }
  return std::int64_t{st.st_mtimespec.tv_sec} * 1'000'000'000 +
         st.st_mtimespec.tv_nsec;
}

// Mark an entry used on disk too, so LRU order survives a relaunch.
// Throttled: at most one metadata write per entry per hour.
void
touch(const fs::path& p, std::int64_t prev_ns)
{
  constexpr std::int64_t kHour = 3600LL * 1'000'000'000;
  if (now_ns() - prev_ns < kHour) {
    return;
  }
  utimes(p.c_str(), nullptr);
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
  CFBooleanRef v = nullptr;
  bool internal = true;
  if (CFURLCopyResourcePropertyForKey(url, kCFURLVolumeIsInternalKey, &v,
                                      nullptr) &&
      v) {
    internal = CFBooleanGetValue(v);
    CFRelease(v);
  }
  CFRelease(url);
  return internal;
}

}

std::string
CacheStore::key_for(const ContentHash& h, std::string_view variant)
{
  return std::format("{}.{}", h.hex(), variant);
}

bool
CacheStore::valid_key(std::string_view k)
{
  if (k.empty() || k.size() > 200 || k[0] == '.') {
    return false;
  }
  return std::all_of(k.begin(), k.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
  });
}

Result<std::unique_ptr<CacheStore>>
CacheStore::open(const fs::path& root, const Options& opts)
{
  std::error_code ec;
  fs::create_directories(root / "objects", ec);
  if (ec) {
    return make_error(Code::Io, std::format("cache {}: {}", root.string(),
                                            ec.message()));
  }
  // Scratch never survives a launch: whatever a crashed run left there
  // is garbage by definition.
  fs::remove_all(root / "scratch", ec);
  fs::create_directories(root / "scratch", ec);

  std::unique_ptr<CacheStore> c(new CacheStore());
  c->_root = root;
  c->_internal = volume_is_internal(root);
  if (!c->_internal) {
    VALTZ_LOG_WARN("cache", "{} is not on an internal volume; cached "
                   "proxies and thumbnails will be slow", root.string());
  }

  c->_budget = opts.budget_bytes;
  if (c->_budget == 0) {
    struct statfs st;
    std::uint64_t cap = 0;
    if (statfs(root.c_str(), &st) == 0) {
      cap = static_cast<std::uint64_t>(st.f_blocks) * st.f_bsize;
    }
    c->_budget = std::min<std::uint64_t>(std::uint64_t{20} << 30,
                                         cap ? cap / 10 : 0);
    if (c->_budget == 0) {
      c->_budget = std::uint64_t{20} << 30;
    }
  }

  for (auto it = fs::recursive_directory_iterator(root / "objects", ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const std::string name = it->path().filename().string();
    if (name.ends_with(".staging")) {
      fs::remove(it->path(), ec);  // an interrupted write
      continue;
    }
    Entry e;
    e.bytes = it->file_size(ec);
    e.last_used_ns = mtime_ns(it->path());
    c->_bytes += e.bytes;
    c->_entries[name] = e;
  }
  if (c->_bytes > c->_budget) {
    c->evict_to(c->_budget * 9 / 10);
  }
  VALTZ_LOG_INFO("cache", "{}: {} entries, {} MB of {} MB budget",
                 root.string(), c->_entries.size(), c->_bytes >> 20,
                 c->_budget >> 20);
  return c;
}

std::uint64_t
CacheStore::size_bytes() const
{
  std::lock_guard lk(_mu);
  return _bytes;
}

Result<fs::path>
CacheStore::scratch_dir(std::string_view owner)
{
  if (!valid_key(owner)) {
    return make_error(Code::InvalidArgument, "bad scratch owner name");
  }
  fs::path p = _root / "scratch" / std::string(owner);
  std::error_code ec;
  fs::create_directories(p, ec);
  if (ec) {
    return make_error(Code::Io, ec.message());
  }
  return p;
}

fs::path
CacheStore::object_path_(std::string_view key) const
{
  return _root / "objects" / std::string(key.substr(0, 2)) /
         std::string(key);
}

std::optional<fs::path>
CacheStore::find(std::string_view key)
{
  if (!valid_key(key)) {
    return std::nullopt;
  }
  std::int64_t prev = 0;
  fs::path p = object_path_(key);
  {
    std::lock_guard lk(_mu);
    auto it = _entries.find(std::string(key));
    if (it == _entries.end()) {
      return std::nullopt;
    }
    prev = it->second.last_used_ns;
    it->second.last_used_ns = now_ns();
  }
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) {
    // Deleted underneath us (by the user, or a cleaner): forget it.
    std::lock_guard lk(_mu);
    if (auto it = _entries.find(std::string(key)); it != _entries.end()) {
      _bytes -= it->second.bytes;
      _entries.erase(it);
    }
    return std::nullopt;
  }
  touch(p, prev);
  return p;
}

fs::path
CacheStore::staging_path(std::string_view key) const
{
  fs::path p = object_path_(key);
  p += ".staging";
  std::error_code ec;
  fs::create_directories(p.parent_path(), ec);
  return p;
}

Result<fs::path>
CacheStore::commit(std::string_view key)
{
  if (!valid_key(key)) {
    return make_error(Code::InvalidArgument,
                      std::format("bad cache key '{}'", key));
  }
  fs::path staged = staging_path(key);
  fs::path dst = object_path_(key);
  std::error_code ec;
  auto bytes = fs::file_size(staged, ec);
  if (ec) {
    return make_error(Code::NotFound, "nothing staged for cache entry");
  }
  fs::rename(staged, dst, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cache commit: {}",
                                            ec.message()));
  }
  std::lock_guard lk(_mu);
  auto& e = _entries[std::string(key)];
  _bytes -= e.bytes;
  e.bytes = bytes;
  e.last_used_ns = now_ns();
  _bytes += bytes;
  if (_bytes > _budget) {
    evict_locked_(_budget * 9 / 10);
  }
  return dst;
}

CacheStore::Pin
CacheStore::pin(std::string_view key)
{
  std::lock_guard lk(_mu);
  auto it = _entries.find(std::string(key));
  if (it == _entries.end()) {
    return Pin();
  }
  ++it->second.pins;
  return Pin(this, std::string(key));
}

void
CacheStore::unpin_(const std::string& key)
{
  std::lock_guard lk(_mu);
  if (auto it = _entries.find(key); it != _entries.end() &&
                                    it->second.pins > 0) {
    --it->second.pins;
  }
}

CacheStore::Pin::Pin(Pin&& o) noexcept
  : _store(o._store), _key(std::move(o._key))
{
  o._store = nullptr;
}

CacheStore::Pin&
CacheStore::Pin::operator=(Pin&& o) noexcept
{
  if (this != &o) {
    if (_store) {
      _store->unpin_(_key);
    }
    _store = o._store;
    _key = std::move(o._key);
    o._store = nullptr;
  }
  return *this;
}

CacheStore::Pin::~Pin()
{
  if (_store) {
    _store->unpin_(_key);
  }
}

void
CacheStore::evict_to(std::uint64_t target_bytes)
{
  std::lock_guard lk(_mu);
  evict_locked_(target_bytes);
}

void
CacheStore::evict_locked_(std::uint64_t target)
{
  if (_bytes <= target) {
    return;
  }
  std::vector<std::pair<std::int64_t, std::string>> order;
  order.reserve(_entries.size());
  for (const auto& [k, e] : _entries) {
    if (e.pins == 0) {
      order.emplace_back(e.last_used_ns, k);
    }
  }
  std::sort(order.begin(), order.end());
  std::error_code ec;
  for (const auto& [_, k] : order) {
    if (_bytes <= target) {
      break;
    }
    fs::remove(object_path_(k), ec);
    auto it = _entries.find(k);
    _bytes -= it->second.bytes;
    _entries.erase(it);
  }
}

}
