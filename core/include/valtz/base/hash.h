// Content hashing.
//
// Blobs are content-addressed: the store names a file by the SHA-256 of
// its bytes, a derived asset records the hashes of the inputs it was
// built from, and a peer is sent only the blobs whose hashes it does not
// already hold. SHA-256 comes from CommonCrypto, which uses the ARMv8
// SHA-2 instructions (~2 GB/s per core on M-series) and needs no
// third-party code.

#ifndef VALTZ_BASE_HASH_H
#define VALTZ_BASE_HASH_H

#include "valtz/base/result.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace valtz {

struct ContentHash {
  std::array<std::uint8_t, 32> bytes{};

  static ContentHash of(const void* data, std::size_t n);
  static ContentHash of(std::string_view s)
  {
    return of(s.data(), s.size());
  }
  static std::optional<ContentHash> parse(std::string_view hex);

  bool is_zero() const noexcept;
  std::string hex() const;         // 64 lowercase hex chars
  std::string short_hex() const;   // first 12, for logs and UI

  auto operator<=>(const ContentHash&) const = default;
};

// Incremental SHA-256.
class Hasher {
public:
  Hasher();
  ~Hasher();
  Hasher(const Hasher&) = delete;
  Hasher& operator=(const Hasher&) = delete;

  void update(const void* data, std::size_t n);
  void update(std::string_view s) { update(s.data(), s.size()); }
  ContentHash finish();

private:
  struct State;
  std::unique_ptr<State> _s;
};

// Hash a file by streaming it (no mmap: a multi-GB video must not become
// resident just to be named).
Result<ContentHash> hash_file(const std::filesystem::path&);

}

template <>
struct std::hash<valtz::ContentHash> {
  std::size_t
  operator()(const valtz::ContentHash& h) const noexcept
  {
    std::size_t v = 0;
    for (int i = 0; i < 8; ++i) {
      v = (v << 8) | h.bytes[i];
    }
    return v;
  }
};

#endif
