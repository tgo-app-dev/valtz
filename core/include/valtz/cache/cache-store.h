// The managed cache: disposable, derived-from-content data on the
// internal SSD, shared by every project.
//
//   ~/Library/Application Support/com.tgous.valtz/cache/
//     objects/<k0k1>/<key>    evictable entries, LRU under a byte budget
//     scratch/<owner>/        transient working dirs, wiped at open
//
// KEYS COME FROM CONTENT. An entry derived from a blob is named by the
// blob's SHA-256 plus a variant ("thumb-256.jpg", later "proxy-1080.mov"),
// so it is valid in every project holding those bytes and can never be
// stale -- different bytes are a different key. Nothing in here is
// precious: losing the whole directory costs recomputation only.
//
// Why a managed cache on the internal SSD rather than ~/Library/Caches:
// linked originals often live on external drives that read at a fifth of
// the internal SSD's speed; thumbnails, proxies and decoded frames of
// them belong on the fast volume, bounded by a budget Valtz controls,
// and must not be purged by the system while in use. Entries in use are
// PINNED and never evicted.
//
// Thread-safe.

#ifndef VALTZ_CACHE_CACHE_STORE_H
#define VALTZ_CACHE_CACHE_STORE_H

#include "valtz/base/hash.h"
#include "valtz/base/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace valtz::cache {

class CacheStore {
public:
  struct Options {
    // 0 = default: min(20 GB, 10% of the volume's capacity).
    std::uint64_t budget_bytes = 0;
  };

  static Result<std::unique_ptr<CacheStore>>
  open(const std::filesystem::path& root, const Options& opts);
  static Result<std::unique_ptr<CacheStore>>
  open(const std::filesystem::path& root)
  {
    return open(root, Options{});
  }

  // "<sha256-hex>.<variant>" -- the canonical key for data derived from
  // a blob.
  static std::string key_for(const ContentHash&, std::string_view variant);
  // Keys are file names: [A-Za-z0-9._-], 1..200 chars.
  static bool valid_key(std::string_view);

  const std::filesystem::path& root() const noexcept { return _root; }
  bool on_internal_volume() const noexcept { return _internal; }
  std::uint64_t budget_bytes() const noexcept { return _budget; }
  std::uint64_t size_bytes() const;

  // A fresh, empty transient directory for `owner` (e.g. "vpipe").
  // Everything under scratch/ is deleted when the cache opens.
  Result<std::filesystem::path> scratch_dir(std::string_view owner);

  // The entry's path if present (and marks it recently used).
  std::optional<std::filesystem::path> find(std::string_view key);

  // Write a new entry: produce the file at staging_path(key), then
  // commit(key) moves it into place, accounts it and evicts others if
  // the budget is exceeded. Committing an existing key replaces it.
  std::filesystem::path staging_path(std::string_view key) const;
  Result<std::filesystem::path> commit(std::string_view key);

  // Keeps an entry from eviction while held. Movable, not copyable.
  class Pin {
  public:
    Pin() = default;
    Pin(Pin&&) noexcept;
    Pin& operator=(Pin&&) noexcept;
    ~Pin();
    explicit operator bool() const noexcept { return _store != nullptr; }

  private:
    friend class CacheStore;
    Pin(CacheStore* s, std::string k) : _store(s), _key(std::move(k)) {}
    CacheStore* _store = nullptr;
    std::string _key;
  };
  Pin pin(std::string_view key);

  // Evict least-recently-used, unpinned entries until size <= target.
  void evict_to(std::uint64_t target_bytes);

private:
  CacheStore() = default;
  std::filesystem::path object_path_(std::string_view key) const;
  void unpin_(const std::string& key);
  void evict_locked_(std::uint64_t target_bytes);

  struct Entry {
    std::uint64_t bytes = 0;
    std::int64_t  last_used_ns = 0;
    int           pins = 0;
  };

  std::filesystem::path                  _root;
  std::uint64_t                          _budget = 0;
  bool                                   _internal = true;
  mutable std::mutex                     _mu;
  std::unordered_map<std::string, Entry> _entries;
  std::uint64_t                          _bytes = 0;
};

}

#endif
