// Identifiers.
//
// Every stored object is keyed by a UUIDv7: 48 bits of Unix-epoch
// milliseconds followed by random bits. Two properties drive the choice:
//
//   * Time-ordered. LMDB keys are sorted bytewise, so v7 ids cluster new
//     records at the tail of the B+tree (cheap appends, good page
//     locality) and a plain cursor walk lists objects in creation order.
//   * Globally unique without coordination. Projects move between
//     machines and peers derive assets for each other; ids minted on two
//     Macs must never collide, which rules out per-project counters.
//
// Id<Tag> wraps the 16 raw bytes in a distinct type per kind of object,
// so an AssetId cannot be passed where a RecipeId is expected.

#ifndef VALTZ_BASE_ID_H
#define VALTZ_BASE_ID_H

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace valtz {

struct Uuid {
  std::array<std::uint8_t, 16> bytes{};

  static Uuid v7();
  static std::optional<Uuid> parse(std::string_view);

  bool is_nil() const noexcept;
  std::string str() const;  // canonical 8-4-4-4-12 lowercase hex
  std::uint64_t unix_ms() const noexcept;

  auto operator<=>(const Uuid&) const = default;
};

template <class Tag>
struct Id {
  Uuid uuid;

  static Id make() { return Id{Uuid::v7()}; }
  static std::optional<Id>
  parse(std::string_view s)
  {
    auto u = Uuid::parse(s);
    if (!u) {
      return std::nullopt;
    }
    return Id{*u};
  }

  bool is_nil() const noexcept { return uuid.is_nil(); }
  std::string str() const { return uuid.str(); }
  const std::uint8_t* data() const noexcept { return uuid.bytes.data(); }
  static constexpr std::size_t size() noexcept { return 16; }

  auto operator<=>(const Id&) const = default;
};

struct ProjectTag;
struct AssetTag;
struct RecipeTag;
struct SubjectTag;
struct JobTag;
struct HistoryTag;

using ProjectId = Id<ProjectTag>;
using AssetId   = Id<AssetTag>;
using RecipeId  = Id<RecipeTag>;
using SubjectId = Id<SubjectTag>;
using JobId     = Id<JobTag>;
using HistoryId = Id<HistoryTag>;

}

template <>
struct std::hash<valtz::Uuid> {
  std::size_t
  operator()(const valtz::Uuid& u) const noexcept
  {
    // The tail is random in a v7 id; eight bytes of it are a fine hash.
    std::uint64_t h = 0;
    for (int i = 8; i < 16; ++i) {
      h = (h << 8) | u.bytes[i];
    }
    return static_cast<std::size_t>(h);
  }
};

template <class Tag>
struct std::hash<valtz::Id<Tag>> {
  std::size_t
  operator()(const valtz::Id<Tag>& id) const noexcept
  {
    return std::hash<valtz::Uuid>{}(id.uuid);
  }
};

#endif
