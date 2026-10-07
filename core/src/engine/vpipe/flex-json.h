// Valtz documents (Json) <-> vpipe documents (FlexData), tree to tree.
//
// Valtz's own documents stay Json (base/json.h: records, catalog, recipes,
// the bridge). What goes INTO vpipe -- the session config, pipeline specs,
// command arguments -- and what comes back -- command results, sidebands
// -- crosses as FlexData through vpipe's in-memory host API, so no JSON
// text is written or parsed at the boundary. These convert node by node
// and keep the number kinds: Json's integer / unsigned / float are
// FlexData's Int / Uint / Real, which text would blur into "a number".
//
// Json binary values have no FlexData kind; they become Null (Valtz sends
// none to vpipe).

#ifndef VALTZ_ENGINE_VPIPE_FLEX_JSON_H
#define VALTZ_ENGINE_VPIPE_FLEX_JSON_H

#include "valtz/base/json.h"

#include "common/flex-data.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace valtz::engine::vp {

vpipe::FlexData to_flex(const Json&);
Json from_flex(const vpipe::FlexData&);

// {key: value, ...} built directly as a FlexData object, for the small
// documents Valtz makes for vpipe (a session config, command arguments).
vpipe::FlexData flex_object(
    std::initializer_list<std::pair<std::string_view, vpipe::FlexData>>);
inline vpipe::FlexData flex_str(std::string_view s)
{
  return vpipe::FlexData::make_string(s);
}

// Reading a FlexData object that may lack a key or hold another kind:
// the value, or `fallback` (like jget for Json). Never throws.
vpipe::FlexData field(const vpipe::FlexData& obj, std::string_view key);
std::string field_str(const vpipe::FlexData& obj, std::string_view key,
                      std::string_view fallback = {});
std::uint64_t field_uint(const vpipe::FlexData& obj, std::string_view key,
                         std::uint64_t fallback = 0);
bool field_bool(const vpipe::FlexData& obj, std::string_view key,
                bool fallback = false);

}

#endif
