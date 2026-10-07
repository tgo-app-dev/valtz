// Key encodings and CBOR record helpers shared by the project sources.

#ifndef VALTZ_PROJECT_KEYS_H
#define VALTZ_PROJECT_KEYS_H

#include "valtz/base/id.h"
#include "valtz/base/json.h"
#include "valtz/db/lmdb.h"

#include <array>
#include <cstring>
#include <optional>

namespace valtz::project {

template <class Tag>
db::Bytes
id_key(const Id<Tag>& id)
{
  return {id.data(), Id<Tag>::size()};
}

// AssetId followed by the version number, big-endian so versions sort
// numerically under a cursor.
inline std::array<std::uint8_t, 20>
version_key(const AssetId& id, std::uint32_t n)
{
  std::array<std::uint8_t, 20> k{};
  std::memcpy(k.data(), id.data(), 16);
  k[16] = static_cast<std::uint8_t>(n >> 24);
  k[17] = static_cast<std::uint8_t>(n >> 16);
  k[18] = static_cast<std::uint8_t>(n >> 8);
  k[19] = static_cast<std::uint8_t>(n);
  return k;
}

template <class IdT>
IdT
id_from_bytes(db::Bytes b)
{
  IdT id;
  if (b.size() >= 16) {
    std::memcpy(id.uuid.bytes.data(), b.data(), 16);
  }
  return id;
}

inline Status
put_cbor(db::Txn& txn, db::Dbi dbi, db::Bytes key, const Json& j)
{
  auto bytes = to_cbor(j);
  return txn.put(dbi, key, {bytes.data(), bytes.size()});
}

inline Result<std::optional<Json>>
get_cbor(const db::Txn& txn, db::Dbi dbi, db::Bytes key)
{
  VALTZ_ASSIGN(auto v, txn.get(dbi, key));
  if (!v) {
    return std::optional<Json>{};
  }
  VALTZ_ASSIGN(Json j, from_cbor(*v));
  return std::optional<Json>{std::move(j)};
}

}

#endif
