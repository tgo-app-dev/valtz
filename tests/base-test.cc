#include "testing.h"

#include "valtz/base/hash.h"
#include "valtz/base/id.h"
#include "valtz/base/json.h"
#include "valtz/base/rational.h"
#include "valtz/base/text.h"

#include <set>
#include <thread>

using namespace valtz;

TEST(base, uuid_v7_roundtrip_and_order)
{
  Uuid a = Uuid::v7();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  Uuid b = Uuid::v7();
  CHECK(a < b);                           // time-ordered
  CHECK((a.bytes[6] >> 4) == 7);          // version
  CHECK((a.bytes[8] & 0xc0) == 0x80);     // variant
  auto p = Uuid::parse(a.str());
  REQUIRE(p.has_value());
  CHECK(*p == a);
  CHECK(!Uuid::parse("not-a-uuid").has_value());
}

TEST(base, uuid_unique_under_burst)
{
  std::set<Uuid> seen;
  for (int i = 0; i < 10000; ++i) {
    seen.insert(Uuid::v7());
  }
  CHECK(seen.size() == 10000);
}

TEST(base, sha256_known_vector)
{
  // FIPS 180-2 test vector.
  auto h = ContentHash::of(std::string_view("abc"));
  CHECK(h.hex() ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  auto p = ContentHash::parse(h.hex());
  REQUIRE(p.has_value());
  CHECK(*p == h);
}

TEST(base, rational_normalizes)
{
  Rational r(60000, 2002);
  CHECK(r.num == 30000);
  CHECK(r.den == 1001);
  Rational n(1, -2);
  CHECK(n.num == -1);
  CHECK(n.den == 2);
}

// A prompt cut to a name's byte budget must not split a character: the
// cut once left half a character in a name, and every asset list after
// it failed to serialize.
TEST(base, utf8_prefix_cuts_on_a_character)
{
  const std::string ascii = "a red fox in snow";
  CHECK(utf8_prefix(ascii, 5) == "a red");
  CHECK(utf8_prefix(ascii, 100) == ascii);

  // "fox " then CJK (3 bytes each): 4 + 3k never lands on 6.
  const std::string mixed = "fox \xe5\x9c\xa8\xe9\x9b\xaa\xe5\x9c\xb0";
  CHECK(utf8_prefix(mixed, 6) == "fox ");
  CHECK(utf8_prefix(mixed, 7) == "fox \xe5\x9c\xa8");
  CHECK(utf8_prefix(mixed, 9) == "fox \xe5\x9c\xa8");
  CHECK(utf8_prefix(mixed, 10) == "fox \xe5\x9c\xa8\xe9\x9b\xaa");

  // A 4-byte character (an emoji) is kept whole or left out whole.
  const std::string emoji = "ok\xf0\x9f\xa6\x8a";
  CHECK(utf8_prefix(emoji, 5) == "ok");
  CHECK(utf8_prefix(emoji, 6) == emoji);

  // Every cut of the mixed string is valid UTF-8 (to_text round-trips it
  // without replacement characters).
  for (std::size_t n = 0; n <= mixed.size(); ++n) {
    const Json j = std::string(utf8_prefix(mixed, n));
    CHECK(to_text(j).find("\xef\xbf\xbd") == std::string::npos);
  }
}

// Text for the app never throws on bad bytes: they become U+FFFD, and the
// rest of the document survives.
TEST(base, to_text_replaces_invalid_utf8)
{
  Json j = {{"name", std::string("fox \xe5\x9c")}, {"ok", true}};
  bool threw = false;
  try {
    (void)j.dump();
  } catch (const Json::type_error&) {
    threw = true;
  }
  CHECK(threw);  // what the strict dump did
  const std::string t = to_text(j);
  CHECK(t.find("\xef\xbf\xbd") != std::string::npos);
  CHECK(t.find("\"ok\":true") != std::string::npos);
}

// A cut name says it was cut: whole characters, then "…", all within the
// budget.
TEST(base, utf8_truncate_marks_the_cut)
{
  CHECK(utf8_truncate("a red fox", 48) == "a red fox");
  // 10 bytes: 7 of text, then the 3-byte "…".
  CHECK(utf8_truncate("a red fox in snow", 10) == "a red f\xe2\x80\xa6");
  // A cut after a space drops it: "a red " + "…" reads "a red…".
  CHECK(utf8_truncate("a red fox in snow", 9) == "a red\xe2\x80\xa6");
  // "fox " + 3 CJK (13 bytes); 10 bytes leaves 7 for text: "fox " + one
  // character, then "…".
  const std::string mixed = "fox \xe5\x9c\xa8\xe9\x9b\xaa\xe5\x9c\xb0";
  const std::string cut = utf8_truncate(mixed, 10);
  CHECK(cut == "fox \xe5\x9c\xa8\xe2\x80\xa6");
  CHECK(cut.size() <= 10);
  for (std::size_t n = 0; n <= mixed.size() + 1; ++n) {
    const std::string t = utf8_truncate(mixed, n);
    CHECK(t.size() <= std::max<std::size_t>(n, 0));
    CHECK(to_text(Json(t)).find("\xef\xbf\xbd") == std::string::npos);
  }
}

TEST(base, one_line_folds_whitespace)
{
  CHECK(one_line("  a red\n\n fox\tin \xe3\x80\x80snow ") == "a red fox in snow");
  CHECK(one_line("") == "");
  CHECK(one_line("\n\t ") == "");
}

// Terminal columns: CJK is two wide, so a name column is cut and padded
// by width, not bytes.
TEST(base, fit_columns_by_display_width)
{
  CHECK(display_width("fox") == 3);
  CHECK(display_width("\xe7\x8b\x90\xe7\x8b\xb8") == 4);  // 狐狸
  CHECK(display_width("e\xcc\x81") == 1);                // e + combining acute

  CHECK(fit_columns("fox", 6) == "fox   ");
  CHECK(fit_columns("\xe7\x8b\x90\xe7\x8b\xb8", 6) == "\xe7\x8b\x90\xe7\x8b\xb8  ");
  // Too wide: whole characters beside the one-column "…", then padding.
  // 5 columns: 狐 (2) + 狸 (2) + … (1).
  const std::string four = "\xe7\x8b\x90\xe7\x8b\xb8\xe5\x9c\xa8\xe9\x9b\xaa";
  CHECK(fit_columns(four, 5) == "\xe7\x8b\x90\xe7\x8b\xb8\xe2\x80\xa6");
  // 6 columns: the third character would need 7 with "…": pad instead.
  CHECK(fit_columns(four, 6) == "\xe7\x8b\x90\xe7\x8b\xb8\xe2\x80\xa6 ");
  CHECK(display_width(fit_columns(four, 6)) == 6);
  CHECK(display_width(fit_columns("a red fox in snow at dawn", 10)) == 10);
  // Middle: the end survives -- the " (2)" that tells two names apart.
  const std::string m = fit_columns("a red fox in the snow at dawn (2)", 16,
                                    /*middle=*/true);
  CHECK(display_width(m) == 16);
  CHECK(m.ends_with(" (2)"));
  CHECK(m.find("\xe2\x80\xa6") != std::string::npos);
  CHECK(fit_columns("fox (2)", 10, true) == "fox (2)   ");
  // A broken byte shows as U+FFFD and the column still lines up.
  const std::string bad = fit_columns("fox \xe5\x9c", 8);
  CHECK(bad.find("\xef\xbf\xbd") != std::string::npos);
  CHECK(display_width(bad) == 8);
}

TEST(base, base64_is_rfc4648)
{
  auto b64 = [](std::string_view s) {
    return base64(std::span(reinterpret_cast<const std::uint8_t*>(s.data()),
                            s.size()));
  };
  CHECK(b64("") == "");
  CHECK(b64("f") == "Zg==");
  CHECK(b64("fo") == "Zm8=");
  CHECK(b64("foo") == "Zm9v");
  CHECK(b64("foobar") == "Zm9vYmFy");
}
