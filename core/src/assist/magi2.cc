#include "valtz/assist/magi2.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

// Compiled in (core/CMakeLists.txt, valtz_embed): MAGI-2-preview's prompt
// enhancement templates from 3rd-party/magi2, unmodified, under the
// Apache License 2.0 (its LICENSE and NOTICE).
namespace valtz::resources {
extern const char kMagi2T2vGuide[];
extern const char kMagi2I2vGuide[];
}

namespace valtz::assist {

namespace {

// The reply's own key order is the rendering's (Python's dicts keep it).
using OJson = nlohmann::ordered_json;

void
replace_all(std::string& s, std::string_view from, std::string_view to)
{
  for (std::size_t at = s.find(from); at != std::string::npos;
       at = s.find(from, at + to.size())) {
    s.replace(at, from.size(), to);
  }
}

// The first balanced object in `s` that parses (prose, a code fence or a
// model's preamble around it skipped).
bool
first_object(std::string_view s, OJson* out)
{
  for (std::size_t start = s.find('{'); start != std::string_view::npos;
       start = s.find('{', start + 1)) {
    int depth = 0;
    bool in_str = false, esc = false;
    for (std::size_t i = start; i < s.size(); ++i) {
      const char c = s[i];
      if (in_str) {
        if (esc) {
          esc = false;
        } else if (c == '\\') {
          esc = true;
        } else if (c == '"') {
          in_str = false;
        }
        continue;
      }
      if (c == '"') {
        in_str = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}' && --depth == 0) {
        OJson j = OJson::parse(s.substr(start, i - start + 1), nullptr,
                               /*allow_exceptions=*/false);
        if (j.is_object()) {
          *out = std::move(j);
          return true;
        }
        break;
      }
    }
  }
  return false;
}

// ---- pe_markdown.render_markdown, with enhancer.py's options:
// drop_empty, keep_empty_string false, omit_section_leaf_keys false,
// quote_strings false, collapse_string_whitespace false, " | ", six
// heading levels, scalar fields first.

bool
is_primitive(const OJson& v)
{
  return !v.is_object() && !v.is_array();
}

// Its newlines and tabs written as escapes (inline_string).
std::string
primitive_text(const OJson& v)
{
  if (v.is_string()) {
    std::string s = v.get<std::string>();
    replace_all(s, "\r\n", "\\n");
    replace_all(s, "\n", "\\n");
    replace_all(s, "\r", "\\r");
    replace_all(s, "\t", "\\t");
    return s;
  }
  if (v.is_null()) { return "null"; }
  if (v.is_boolean()) { return v.get<bool>() ? "true" : "false"; }
  return v.dump();
}

bool
blank(const std::string& s)
{
  for (const char c : s) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f' &&
        c != '\v') {
      return false;
    }
  }
  return true;
}

// filter_empty: null, "" (and whitespace), [] and {} dropped, and the
// containers they empty; false and 0 kept.
bool
filter_empty(const OJson& v, OJson* out)
{
  if (v.is_null()) { return false; }
  if (v.is_string()) {
    *out = v;
    return !blank(v.get<std::string>());
  }
  if (v.is_object()) {
    OJson o = OJson::object();
    for (const auto& [k, c] : v.items()) {
      OJson f;
      if (filter_empty(c, &f)) { o[k] = std::move(f); }
    }
    *out = std::move(o);
    return !out->empty();
  }
  if (v.is_array()) {
    OJson a = OJson::array();
    for (const auto& c : v) {
      OJson f;
      if (filter_empty(c, &f)) { a.push_back(std::move(f)); }
    }
    *out = std::move(a);
    return !out->empty();
  }
  *out = v;
  return true;
}

std::string
heading(int level, const std::string& title)
{
  if (level <= 6) {
    return std::string(static_cast<std::size_t>(std::max(1, level)), '#') +
           " " + title;
  }
  return "*" + title + "*";
}

std::string
inline_list(const OJson& values)
{
  std::string out;
  bool first = true;
  for (const auto& item : values) {
    std::string part;
    if (is_primitive(item)) {
      part = primitive_text(item);
    } else if (item.is_array()) {
      part = "[" + inline_list(item) + "]";
    } else {
      part = "{";
      bool f = true;
      for (const auto& [k, c] : item.items()) {
        if (!f) { part += ";"; }
        f = false;
        part += k + ":" + (is_primitive(c) ? primitive_text(c) : "...");
      }
      part += "}";
    }
    if (!first) { out += " | "; }
    first = false;
    out += part;
  }
  return out;
}

bool
all_primitive(const OJson& a)
{
  for (const auto& c : a) {
    if (!is_primitive(c)) { return false; }
  }
  return true;
}

void render_object(const OJson& obj, int level, std::vector<std::string>&);

// render_value. `key` empty: none (pe_markdown's None) -- a JSON key is
// never empty here, as an object's keys become headings.
void
render_value(const std::string* key, const OJson& value, int level,
             std::vector<std::string>& lines)
{
  if (is_primitive(value)) {
    const std::string t = primitive_text(value);
    lines.push_back(key ? *key + ":" + t : t);
    return;
  }
  if (value.is_array()) {
    if (value.empty()) { return; }
    if (key) {
      if (all_primitive(value)) {
        lines.push_back(*key + ":" + inline_list(value));
        return;
      }
      lines.push_back(heading(level, *key));
      lines.push_back("");
    }
    int idx = 0;
    for (const auto& item : value) {
      ++idx;
      const std::string title =
          (key ? *key : std::string("item")) + std::to_string(idx);
      if (is_primitive(item)) {
        lines.push_back(title + ":" + primitive_text(item));
      } else if (item.is_array()) {
        lines.push_back(heading(level + 1, title));
        lines.push_back("");
        render_value(nullptr, item, level + 2, lines);
      } else {
        lines.push_back(heading(level + 1, title));
        lines.push_back("");
        render_object(item, level + 2, lines);
        lines.push_back("");
      }
    }
    return;
  }
  if (key) {
    lines.push_back(heading(level, *key));
    lines.push_back("");
  }
  render_object(value, level + (key ? 1 : 0), lines);
}

bool
simple_for_line(const OJson& c)
{
  return is_primitive(c) || (c.is_array() && all_primitive(c));
}

// render_object_contents: the scalar and simple fields first, a blank
// line before each nested one but the first.
void
render_object(const OJson& obj, int level, std::vector<std::string>& lines)
{
  std::vector<std::pair<const std::string*, const OJson*>> order;
  for (int pass = 0; pass < 2; ++pass) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
      if (simple_for_line(it.value()) == (pass == 0)) {
        order.emplace_back(&it.key(), &it.value());
      }
    }
  }
  bool first = true;
  for (const auto& [k, c] : order) {
    if (!first && (c->is_object() || c->is_array()) && !lines.empty() &&
        !lines.back().empty()) {
      lines.push_back("");
    }
    render_value(k, *c, level, lines);
    first = false;
  }
}

// normalize_blank_lines: one blank line at most in a row, none at the end,
// each line's trailing spaces dropped, a newline after the last.
std::string
join_lines(const std::vector<std::string>& lines)
{
  std::vector<std::string> out;
  int blanks = 0;
  for (const auto& l : lines) {
    if (l.empty()) {
      if (++blanks <= 1) { out.push_back(l); }
      continue;
    }
    blanks = 0;
    std::size_t e = l.size();
    while (e > 0 && (l[e - 1] == ' ' || l[e - 1] == '\t' ||
                     l[e - 1] == '\n' || l[e - 1] == '\r' ||
                     l[e - 1] == '\f' || l[e - 1] == '\v')) {
      --e;
    }
    out.push_back(l.substr(0, e));
  }
  while (!out.empty() && out.back().empty()) { out.pop_back(); }
  std::string s;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (i) { s += "\n"; }
    s += out[i];
  }
  return s + "\n";
}

}

std::string
magi2_request(bool i2v, std::string_view request)
{
  std::string s = i2v ? resources::kMagi2I2vGuide : resources::kMagi2T2vGuide;
  replace_all(s, "{user_input_text}", request);
  replace_all(s, "{variation_seed}", "");
  return s;
}

Result<std::string>
magi2_prompt(std::string_view reply, bool i2v)
{
  OJson j;
  if (!first_object(reply, &j)) {
    return make_error(Code::InvalidArgument,
                      "the reply holds no JSON object");
  }
  // enhancer.py's required keys: some of them, at least, or it is not
  // this schema's reply at all.
  static const char* const kT2v[] = {"global_layer", "dynamic_layer"};
  static const char* const kI2v[] = {
      "global_caption", "reference_bank", "shot_timeline",
      "audio_event_timeline", "visible_text", "generation_requirements"};
  bool any = false;
  if (i2v) {
    for (const char* k : kI2v) { any = any || j.contains(k); }
  } else {
    for (const char* k : kT2v) { any = any || j.contains(k); }
  }
  if (!any) {
    return make_error(Code::InvalidArgument,
                      std::format("the reply is not MAGI-2's {} schema",
                                  i2v ? "i2v" : "t2v"));
  }
  OJson kept;
  if (!filter_empty(j, &kept)) {
    return make_error(Code::InvalidArgument, "the reply is empty");
  }
  std::vector<std::string> lines;
  bool first = true;
  for (auto it = kept.begin(); it != kept.end(); ++it) {
    if (!first) { lines.push_back(""); }
    first = false;
    render_value(&it.key(), it.value(), 1, lines);
  }
  return join_lines(lines);
}

}
