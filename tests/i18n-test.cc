// Localization: the core's user-facing messages (base/message.h) and the
// app's String Catalog (app/macos/Resources/Localizable.xcstrings) must
// agree, and no translation may change a string's placeholders -- a
// translation with a %@ its source lacks crashes String(format:), and one
// missing a {name} silently drops an argument.

#include "testing.h"

#include "valtz/base/json.h"
#include "valtz/base/message.h"

#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>

using namespace valtz;

namespace {

Json
load_catalog()
{
  std::ifstream f(VALTZ_SOURCE_DIR
                  "/app/macos/Resources/Localizable.xcstrings");
  std::stringstream ss;
  ss << f.rdbuf();
  return Json::parse(ss.str(), nullptr, /*allow_exceptions=*/false);
}

// printf-style specifiers (positions dropped: %1$@ and %@ are the same
// argument type) and {name} placeholders, as a multiset.
std::multiset<std::string>
placeholders(const std::string& s)
{
  static const std::regex re(
      R"(%(?:\d+\$)?(?:ll|l)?[@dfsu]|\{[A-Za-z_]+\})");
  std::multiset<std::string> out;
  for (auto it = std::sregex_iterator(s.begin(), s.end(), re);
       it != std::sregex_iterator(); ++it) {
    std::string p = it->str();
    if (p[0] == '%') {
      p = std::regex_replace(p, std::regex(R"(%\d+\$)"), "%");
    }
    out.insert(p);
  }
  return out;
}

std::string
unit_value(const Json& loc)
{
  return jget<std::string>(jget(loc, "stringUnit", Json::object()), "value",
                           "");
}

}

TEST(i18n, render_message_fills_named_arguments)
{
  CHECK(render_message("{model} is not installed.", {{"model", "Krea 2"}}) ==
        "Krea 2 is not installed.");
  CHECK(render_message("{a} and {b}", {{"b", "2"}, {"a", "1"}}) ==
        "1 and 2");
  CHECK(render_message("{missing} stays", {}) == "{missing} stays");
  auto e = make_error(Code::NotFound, msg::kUnknownModel, {{"model", "x"}});
  CHECK(e.message == "Unknown model: x.");
  CHECK(e.key == "core.unknown_model");
}

TEST(i18n, every_core_message_is_in_the_catalog)
{
  const Json cat = load_catalog();
  REQUIRE(cat.is_object());
  const Json strings = jget(cat, "strings", Json::object());
  for (const Message& m : all_messages()) {
    const Json entry = jget(strings, std::string(m.key).c_str(), Json());
    if (!entry.is_object()) {
      test::record_failure(__FILE__, __LINE__,
                           "catalog lacks " + std::string(m.key));
      continue;
    }
    const Json en = jget(jget(entry, "localizations", Json::object()), "en",
                         Json());
    if (unit_value(en) != m.english) {
      test::record_failure(__FILE__, __LINE__,
                           std::string(m.key) + ": catalog English \"" +
                               unit_value(en) + "\" != \"" +
                               std::string(m.english) + "\"");
    }
  }
}

TEST(i18n, translations_keep_their_placeholders)
{
  const Json cat = load_catalog();
  REQUIRE(cat.is_object());
  int checked = 0;
  // Bound first: a range-for over a temporary's items() dangles.
  const Json strings = jget(cat, "strings", Json::object());
  for (const auto& [key, entry] : strings.items()) {
    const Json locs = jget(entry, "localizations", Json::object());
    // The source text: the catalog's English, or the key itself.
    std::string source = unit_value(jget(locs, "en", Json()));
    if (source.empty()) {
      source = key;
    }
    const auto want = placeholders(source);
    for (const auto& [lang, loc] : locs.items()) {
      const std::string v = unit_value(loc);
      if (v.empty()) {
        continue;   // plural / device variations: not used yet
      }
      ++checked;
      if (placeholders(v) != want) {
        test::record_failure(__FILE__, __LINE__,
                             lang + " \"" + key + "\": \"" + v +
                                 "\" changes the placeholders");
      }
    }
  }
  CHECK(checked > 0);
}
