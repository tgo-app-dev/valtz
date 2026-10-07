// JSON / CBOR glue.
//
// nlohmann::json is the in-memory document type wherever a value is
// open-ended: recipe parameters, model manifests, pipeline specs, and
// event payloads crossing to Swift. Stored records are the same
// documents encoded as CBOR (compact, binary-safe, self-describing), so
// a schema can grow a field without a migration and any record can be
// dumped as JSON for debugging.

#ifndef VALTZ_BASE_JSON_H
#define VALTZ_BASE_JSON_H

#include "valtz/base/hash.h"
#include "valtz/base/id.h"
#include "valtz/base/rational.h"
#include "valtz/base/result.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace valtz {

using Json = nlohmann::json;

// A document as text for the app or a terminal. Strings from outside --
// file names, prompts, and names cut from them -- are not always valid
// UTF-8, and a strict dump throws at the first bad byte, losing the whole
// reply (base/text.h tells how that once broke every asset list). Here
// invalid bytes become U+FFFD instead. Not for hashing: fingerprints keep
// the strict, canonical dump.
inline std::string
to_text(const Json& j, int indent = -1)
{
  return j.dump(indent, ' ', false, Json::error_handler_t::replace);
}

inline std::vector<std::uint8_t>
to_cbor(const Json& j)
{
  return Json::to_cbor(j);
}

inline Result<Json>
from_cbor(std::span<const std::uint8_t> bytes)
{
  Json j = Json::from_cbor(bytes.begin(), bytes.end(),
                           /*strict=*/true, /*allow_exceptions=*/false);
  if (j.is_discarded()) {
    return make_error(Code::Corrupt, "record failed to decode (CBOR)");
  }
  return j;
}

// The fields a bridge reply or an event carries for user-facing text
// (base/message.h): "message" (English), and "key" + "args" (an object)
// for the app to localize. An Error without a key gives only "message".
inline Json
message_fields(std::string english, std::string_view key,
               const MessageArgs& args)
{
  Json j = {{"message", std::move(english)}};
  if (!key.empty()) {
    Json a = Json::object();
    for (const auto& [k, v] : args) {
      a[k] = v;
    }
    j["key"] = std::string(key);
    j["args"] = std::move(a);
  }
  return j;
}

inline Json
message_fields(const Error& e)
{
  return message_fields(e.message, e.key, e.args);
}

inline Json
message_fields(const Message& m, const MessageArgs& args = {})
{
  return message_fields(render_message(m.english, args), m.key, args);
}

// Typed getters that never throw: a missing or mistyped field yields the
// fallback. Stored records are read with these so an older or newer
// schema degrades instead of failing.
template <class T>
T
jget(const Json& j, const char* key, T fallback)
{
  if (!j.is_object()) {
    return fallback;
  }
  auto it = j.find(key);
  if (it == j.end() || it->is_null()) {
    return fallback;
  }
  try {
    return it->template get<T>();
  } catch (...) {
    return fallback;
  }
}

// Text a document shows a person, where it may come in several
// languages (an extension's manifest, DESIGN §8a): a string, or
// {lang: string} picked for `lang` (BCP-47) -- the tag itself, then its
// language alone ("zh-Hans": "zh"), then English, then any.
std::string text_in(const Json& v, std::string_view lang);

}

// ADL serializers for the base types, in their canonical text forms.
namespace valtz {

template <class Tag>
void
to_json(Json& j, const Id<Tag>& id)
{
  j = id.is_nil() ? Json(nullptr) : Json(id.str());
}

template <class Tag>
void
from_json(const Json& j, Id<Tag>& id)
{
  id = Id<Tag>{};
  if (j.is_string()) {
    if (auto p = Id<Tag>::parse(j.template get<std::string>())) {
      id = *p;
    }
  }
}

inline void
to_json(Json& j, const ContentHash& h)
{
  j = h.is_zero() ? Json(nullptr) : Json(h.hex());
}

inline void
from_json(const Json& j, ContentHash& h)
{
  h = ContentHash{};
  if (j.is_string()) {
    if (auto p = ContentHash::parse(j.get<std::string>())) {
      h = *p;
    }
  }
}

inline void
to_json(Json& j, const Rational& r)
{
  j = Json::array({r.num, r.den});
}

inline void
from_json(const Json& j, Rational& r)
{
  if (j.is_array() && j.size() == 2) {
    r = Rational(j[0].get<std::int64_t>(), j[1].get<std::int64_t>());
  } else {
    r = Rational{};
  }
}

}

#endif
