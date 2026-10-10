// UTF-8 text helpers.
//
// Names are cut from prompts to a byte budget. A cut that lands inside a
// multi-byte character leaves bytes no JSON encoder accepts, and one such
// name once made every asset list the app asked for fail to serialize --
// each new result then showed as "Failed". Cut on a character boundary,
// and say so: a cut name ends with "…".
//
// A terminal is the other place text is cut, to a column: there a CJK
// character is two columns wide, so a byte count (printf's %-32s) both
// splits characters and misaligns the table. Cut and pad by columns.

#ifndef VALTZ_BASE_TEXT_H
#define VALTZ_BASE_TEXT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace valtz {

// What a cut ends with: U+2026, three bytes, one column.
inline constexpr std::string_view kEllipsis = "\xe2\x80\xa6";

// The longest prefix of `s` of at most `max_bytes` bytes that ends on a
// UTF-8 code point boundary. (A grapheme may still be split -- an accent
// from its letter -- which is cosmetic; the text stays valid.)
inline std::string_view
utf8_prefix(std::string_view s, std::size_t max_bytes)
{
  if (s.size() <= max_bytes) {
    return s;
  }
  std::size_t n = max_bytes;
  // s[n] is the first byte left out; while it continues a character
  // (10xxxxxx), that character straddles the cut: leave it out whole.
  while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) {
    --n;
  }
  return s.substr(0, n);
}

// `s` in at most `max_bytes` bytes: whole when it fits; otherwise cut at
// the last full character that leaves room for "…", trailing spaces
// dropped, then "…". Fewer than 4 bytes of budget gives a bare cut.
std::string utf8_truncate(std::string_view s, std::size_t max_bytes);

// `s` on one line: runs of whitespace (newlines, tabs, the ideographic
// space) become one space, and the ends are trimmed.
std::string one_line(std::string_view s);

// The code point at s[i], advancing `i` past it. A byte that does not
// start a valid sequence decodes as U+FFFD and advances one byte.
char32_t utf8_next(std::string_view s, std::size_t& i);

// Terminal columns a code point takes: 2 for East Asian wide and
// fullwidth characters (CJK, kana, hangul, fullwidth forms) and emoji,
// 0 for combining marks and zero-width characters, else 1.
int display_width(char32_t cp);
int display_width(std::string_view s);

// `s` in exactly `cols` columns: cut at whole characters with "…" when
// it is too wide, then padded with spaces. Invalid bytes show as U+FFFD.
// `middle` cuts from the middle, as Finder does with names, keeping the
// end -- a " (2)" that tells two names apart, a file's extension.
std::string fit_columns(std::string_view s, int cols, bool middle = false);

// Standard base64 (RFC 4648, padded): how bytes travel inside JSON.
std::string base64(std::span<const std::uint8_t> bytes);
// Back to bytes (padding optional, whitespace skipped); nullopt for
// anything else.
std::optional<std::vector<std::uint8_t>> unbase64(std::string_view s);

}

#endif
