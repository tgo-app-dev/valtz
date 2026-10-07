#include "engine/vpipe/flex-json.h"

namespace valtz::engine::vp {

using vpipe::FlexData;

FlexData
to_flex(const Json& j)
{
  switch (j.type()) {
  case Json::value_t::null:
  case Json::value_t::discarded:
  case Json::value_t::binary:
    return FlexData::make_null();
  case Json::value_t::boolean:
    return FlexData::make_bool(j.get<bool>());
  case Json::value_t::number_integer:
    return FlexData::make_int(j.get<std::int64_t>());
  case Json::value_t::number_unsigned:
    return FlexData::make_uint(j.get<std::uint64_t>());
  case Json::value_t::number_float:
    return FlexData::make_real(j.get<double>());
  case Json::value_t::string:
    return FlexData::make_string(j.get_ref<const std::string&>());
  case Json::value_t::array: {
    FlexData a = FlexData::make_array();
    auto v = a.as_array();
    v.reserve(j.size());
    for (const auto& e : j) {
      v.push_back(to_flex(e));
    }
    return a;
  }
  case Json::value_t::object: {
    FlexData o = FlexData::make_object();
    auto v = o.as_object();
    for (auto it = j.begin(); it != j.end(); ++it) {
      v.insert_or_assign(it.key(), to_flex(it.value()));
    }
    return o;
  }
  }
  return FlexData::make_null();
}

Json
from_flex(const FlexData& f)
{
  if (f.is_null()) {
    return Json();
  }
  if (f.is_bool()) {
    return f.as_bool();
  }
  if (f.is_int()) {
    return f.as_int();
  }
  if (f.is_uint()) {
    return f.as_uint();
  }
  if (f.is_real()) {
    return f.as_real();
  }
  if (f.is_string()) {
    return std::string(f.as_string());
  }
  if (f.is_array()) {
    Json a = Json::array();
    for (const FlexData& e : f.as_array()) {
      a.push_back(from_flex(e));
    }
    return a;
  }
  if (f.is_object()) {
    Json o = Json::object();
    for (const auto& [k, v] : f.as_object()) {
      o[std::string(k)] = from_flex(v);
    }
    return o;
  }
  return Json();
}

FlexData
flex_object(std::initializer_list<std::pair<std::string_view, FlexData>> kv)
{
  FlexData o = FlexData::make_object();
  auto v = o.as_object();
  for (const auto& [k, x] : kv) {
    v.insert_or_assign(k, x);
  }
  return o;
}

FlexData
field(const FlexData& obj, std::string_view key)
{
  if (!obj.is_object()) {
    return FlexData();
  }
  const auto o = obj.as_object();
  const auto it = o.find(key);
  return it == o.end() ? FlexData() : (*it).second;
}

std::string
field_str(const FlexData& obj, std::string_view key,
          std::string_view fallback)
{
  const FlexData v = field(obj, key);
  return std::string(v.is_string() ? v.as_string() : fallback);
}

std::uint64_t
field_uint(const FlexData& obj, std::string_view key, std::uint64_t fallback)
{
  const FlexData v = field(obj, key);
  return v.is_int() || v.is_uint() || v.is_real() ? v.as_uint(fallback)
                                                  : fallback;
}

bool
field_bool(const FlexData& obj, std::string_view key, bool fallback)
{
  const FlexData v = field(obj, key);
  return v.is_bool() ? v.as_bool(fallback) : fallback;
}

}
