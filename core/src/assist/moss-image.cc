#include "valtz/assist/moss-image.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>
#include <vector>

namespace valtz::assist {

namespace {

std::string
trim(std::string_view s)
{
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return std::string(s);
}

// Words, not blank.
std::optional<std::string>
words(const Json& v)
{
  if (!v.is_string()) {
    return std::nullopt;
  }
  std::string t = trim(v.get<std::string>());
  if (t.empty()) {
    return std::nullopt;
  }
  return t;
}

// A string as JSON text, its characters kept (the script's
// ensure_ascii=False).
std::string
json_text(const std::string& s)
{
  return to_text(Json(s));
}

// A list of strings as the script keeps one -- its nonempty strings --
// or nothing when none is left.
std::optional<std::string>
string_list(const Json& v)
{
  if (!v.is_array()) {
    return std::nullopt;
  }
  std::string out;
  for (const auto& e : v) {
    if (auto w = words(e)) {
      out += (out.empty() ? "[" : ",") + json_text(*w);
    }
  }
  if (out.empty()) {
    return std::nullopt;
  }
  return out + "]";
}

// The script's round(v, 9): an edge a hair past a whole pixel, from
// binary fractions, still encloses that pixel and no more.
double
round9(double v)
{
  return std::round(v * 1e9) / 1e9;
}

std::optional<std::array<double, 4>>
box_of(const Json& b)
{
  if (!b.is_array() || b.size() != 4) {
    return std::nullopt;
  }
  std::array<double, 4> out{};
  for (std::size_t i = 0; i < 4; ++i) {
    if (!b[i].is_number() || !std::isfinite(b[i].get<double>())) {
      return std::nullopt;
    }
    out[i] = b[i].get<double>();
  }
  return out;
}

}

Result<std::string>
moss_image_prompt(const Json& draft, int width, int height)
{
  if (!draft.is_object()) {
    return make_error(Code::InvalidArgument, "the draft is not an object");
  }
  if (width <= 0 || height <= 0 || width % 16 != 0 || height % 16 != 0) {
    return make_error(Code::InvalidArgument, std::format(
        "a MOSS-Image canvas is a multiple of 16 a side, not {} x {}",
        width, height));
  }
  // A caption draft: the caption is the prompt.
  if (!draft.contains("items") && !draft.contains("summary")) {
    if (auto c = words(jget(draft, "caption", Json()))) {
      return *c;
    }
    return make_error(Code::InvalidArgument,
                      "the reply is neither a caption nor a layout");
  }

  // Its summary, in the skill's order: meta, canvas, caption, palette.
  Json summary = jget(draft, "summary", Json::object());
  if (summary.is_string()) {
    summary = Json{{"caption", summary}};
  }
  const auto caption = words(jget(summary, "caption", Json()));
  if (!caption) {
    return make_error(Code::InvalidArgument,
                      "the layout has no summary caption");
  }
  std::vector<std::string> sum;
  if (auto m = string_list(jget(summary, "meta", Json()))) {
    sum.push_back("\"meta\":" + *m);
  }
  if (const auto c = jget<std::string>(summary, "canvas", "");
      c == "single" || c == "multi_panel" || c == "collage") {
    sum.push_back("\"canvas\":" + json_text(c));
  }
  sum.push_back("\"caption\":" + json_text(*caption));
  if (auto p = string_list(jget(summary, "palette", Json()))) {
    sum.push_back("\"palette\":" + *p);
  }

  // The canvas its boxes were drawn on: the draft's own size, or this
  // one -- or, every edge within 0..1, fractions of it.
  const Json items = jget(draft, "items", Json::array());
  bool fractions = false;
  {
    bool any = false, unit = true;
    for (const auto& it : items.is_array() ? items : Json::array()) {
      if (auto b = box_of(jget(it, "bbox", Json()))) {
        any = true;
        unit = unit && std::ranges::all_of(*b, [](double v) {
          return v >= 0.0 && v <= 1.0;
        });
      }
    }
    fractions = any && unit;
  }
  const double dw = jget(draft, "width", 0.0) > 0
                        ? jget(draft, "width", 0.0) : width;
  const double dh = jget(draft, "height", 0.0) > 0
                        ? jget(draft, "height", 0.0) : height;
  const double sx = fractions ? width : width / dw;
  const double sy = fractions ? height : height / dh;

  std::string list;
  for (const auto& it : items.is_array() ? items : Json::array()) {
    if (!it.is_object()) {
      continue;
    }
    auto b = box_of(jget(it, "bbox", Json()));
    const auto text = words(jget(it, "text", Json()));
    auto cap = words(jget(it, "caption", Json()));
    if (!cap) {
      cap = text;
    }
    if (!b || !cap) {
      continue;
    }
    std::string label = jget<std::string>(it, "label", "object");
    if (label != "object" && label != "character" && label != "text") {
      label = "object";
    }
    if (label == "text" && !text) {
      label = "object";
    }
    // On this canvas, edges in order, clipped to it.
    double x0 = (*b)[0] * sx, y0 = (*b)[1] * sy;
    double x1 = (*b)[2] * sx, y1 = (*b)[3] * sy;
    if (x0 > x1) {
      std::swap(x0, x1);
    }
    if (y0 > y1) {
      std::swap(y0, y1);
    }
    x0 = std::clamp(x0, 0.0, static_cast<double>(width));
    x1 = std::clamp(x1, 0.0, static_cast<double>(width));
    y0 = std::clamp(y0, 0.0, static_cast<double>(height));
    y1 = std::clamp(y1, 0.0, static_cast<double>(height));
    // Enclosed in pixel edges, then about the centre, y before x.
    const auto top = static_cast<std::int64_t>(std::floor(round9(y0)));
    const auto left = static_cast<std::int64_t>(std::floor(round9(x0)));
    const auto bottom = static_cast<std::int64_t>(std::ceil(round9(y1)));
    const auto right = static_cast<std::int64_t>(std::ceil(round9(x1)));
    if (!(top < bottom && left < right)) {
      continue;
    }
    std::string item = "{\"label\":" + json_text(label) +
                       ",\"caption\":" + json_text(*cap);
    if (text) {
      item += ",\"text\":" + json_text(*text);
    }
    item += std::format(",\"bbox\":[{},{},{},{}]", top - height / 2,
                        left - width / 2, bottom - height / 2,
                        right - width / 2);
    for (const char* k : {"tags", "palette"}) {
      if (auto l = string_list(jget(it, k, Json()))) {
        item += std::format(",\"{}\":{}", k, *l);
      }
    }
    list += (list.empty() ? "" : ",") + item + "}";
  }
  if (list.empty()) {
    return make_error(Code::InvalidArgument,
                      "the layout has no item with a caption and a box");
  }
  std::string out = "{\"summary\":{";
  for (std::size_t i = 0; i < sum.size(); ++i) {
    out += (i ? "," : "") + sum[i];
  }
  out += "},\"items\":[" + list +
         "],\"bbox_coord_space\":\"abs_centralized\","
         "\"bbox_format\":\"yxyx\"}";
  return out;
}

std::string
moss_layout_caption(std::string_view prompt)
{
  const std::string t = trim(prompt);
  if (t.empty() || t.front() != '{') {
    return {};
  }
  const Json j = Json::parse(t, nullptr, false);
  if (!j.is_object() ||
      jget<std::string>(j, "bbox_coord_space", "") != "abs_centralized") {
    return {};
  }
  return words(jget(jget(j, "summary", Json::object()), "caption", Json()))
      .value_or(std::string());
}

}
