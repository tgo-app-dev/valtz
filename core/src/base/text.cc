#include "valtz/base/text.h"

#include "valtz/base/json.h"

#include <vector>

namespace valtz {

namespace {

constexpr std::string_view kReplacement = "\xef\xbf\xbd";  // U+FFFD

// A code point in the string's own bytes, or U+FFFD for an invalid byte.
void
append_cp(std::string& out, std::string_view s, std::size_t from,
          std::size_t to, char32_t cp)
{
  if (cp == 0xfffd && !(to - from == 3 && s.substr(from, 3) == kReplacement)) {
    out += kReplacement;
  } else {
    out.append(s.substr(from, to - from));
  }
}

bool
is_space(char32_t cp)
{
  return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' ||
         cp == '\v' || cp == '\f' || cp == 0x3000;
}

}

std::string
utf8_truncate(std::string_view s, std::size_t max_bytes)
{
  if (s.size() <= max_bytes) {
    return std::string(s);
  }
  if (max_bytes < kEllipsis.size() + 1) {
    return std::string(utf8_prefix(s, max_bytes));
  }
  std::string_view keep = utf8_prefix(s, max_bytes - kEllipsis.size());
  while (!keep.empty() && keep.back() == ' ') {
    keep.remove_suffix(1);
  }
  return std::string(keep) + std::string(kEllipsis);
}

std::string
one_line(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  bool gap = false;
  for (std::size_t i = 0; i < s.size();) {
    const std::size_t at = i;
    const char32_t cp = utf8_next(s, i);
    if (is_space(cp)) {
      gap = !out.empty();
      continue;
    }
    if (gap) {
      out += ' ';
      gap = false;
    }
    out.append(s.substr(at, i - at));
  }
  return out;
}

char32_t
utf8_next(std::string_view s, std::size_t& i)
{
  const auto b0 = static_cast<unsigned char>(s[i]);
  std::size_t len = 0;
  char32_t cp = 0;
  if (b0 < 0x80) {
    ++i;
    return b0;
  } else if ((b0 & 0xe0) == 0xc0) {
    len = 2;
    cp = b0 & 0x1f;
  } else if ((b0 & 0xf0) == 0xe0) {
    len = 3;
    cp = b0 & 0x0f;
  } else if ((b0 & 0xf8) == 0xf0) {
    len = 4;
    cp = b0 & 0x07;
  } else {
    ++i;
    return 0xfffd;
  }
  if (i + len > s.size()) {
    ++i;
    return 0xfffd;
  }
  for (std::size_t k = 1; k < len; ++k) {
    const auto b = static_cast<unsigned char>(s[i + k]);
    if ((b & 0xc0) != 0x80) {
      ++i;
      return 0xfffd;
    }
    cp = (cp << 6) | (b & 0x3f);
  }
  // Overlong forms, surrogates and values past U+10FFFF are invalid.
  static constexpr char32_t kMin[] = {0, 0, 0x80, 0x800, 0x10000};
  if (cp < kMin[len] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
    ++i;
    return 0xfffd;
  }
  i += len;
  return cp;
}

int
display_width(char32_t cp)
{
  if (cp == 0) {
    return 0;
  }
  // Combining marks, zero-width characters, variation selectors.
  if ((cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
      (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x200b && cp <= 0x200f) ||
      (cp >= 0x20d0 && cp <= 0x20ff) || (cp >= 0xfe00 && cp <= 0xfe0f) ||
      (cp >= 0xfe20 && cp <= 0xfe2f)) {
    return 0;
  }
  // East Asian wide and fullwidth (Unicode EastAsianWidth W / F), and
  // the emoji blocks terminals draw two columns wide.
  if ((cp >= 0x1100 && cp <= 0x115f) || (cp >= 0x2e80 && cp <= 0x303e) ||
      (cp >= 0x3041 && cp <= 0x33ff) || (cp >= 0x3400 && cp <= 0x4dbf) ||
      (cp >= 0x4e00 && cp <= 0x9fff) || (cp >= 0xa000 && cp <= 0xa4cf) ||
      (cp >= 0xac00 && cp <= 0xd7a3) || (cp >= 0xf900 && cp <= 0xfaff) ||
      (cp >= 0xfe30 && cp <= 0xfe4f) || (cp >= 0xff00 && cp <= 0xff60) ||
      (cp >= 0xffe0 && cp <= 0xffe6) || (cp >= 0x1f300 && cp <= 0x1f64f) ||
      (cp >= 0x1f900 && cp <= 0x1f9ff) ||
      (cp >= 0x20000 && cp <= 0x3fffd)) {
    return 2;
  }
  return 1;
}

int
display_width(std::string_view s)
{
  int w = 0;
  for (std::size_t i = 0; i < s.size();) {
    w += display_width(utf8_next(s, i));
  }
  return w;
}

std::string
fit_columns(std::string_view s, int cols, bool middle)
{
  std::string out;
  if (cols <= 0) {
    return out;
  }
  // The characters, each with its bytes (U+FFFD for a broken one) and
  // its width.
  struct Ch {
    std::string bytes;
    int w;
  };
  std::vector<Ch> chs;
  int total = 0;
  for (std::size_t i = 0; i < s.size();) {
    const std::size_t at = i;
    const char32_t cp = utf8_next(s, i);
    Ch c{{}, display_width(cp)};
    append_cp(c.bytes, s, at, i, cp);
    total += c.w;
    chs.push_back(std::move(c));
  }
  if (total <= cols) {
    for (const auto& c : chs) {
      out += c.bytes;
    }
    out.append(static_cast<std::size_t>(cols - total), ' ');
    return out;
  }
  // Too wide: whole characters beside the one-column "…". Cut at the
  // end, or keep a tail as wide as the head (give or take a column).
  const int room = cols - 1;
  const int head_room = middle ? (room + 1) / 2 : room;
  int w = 0;
  std::size_t h = 0;
  while (h < chs.size() && w + chs[h].w <= head_room) {
    w += chs[h++].w;
  }
  std::size_t t = chs.size();
  if (middle) {
    int tw = 0;
    while (t > h && w + tw + chs[t - 1].w <= room) {
      tw += chs[--t].w;
    }
    w += tw;
  }
  for (std::size_t k = 0; k < h; ++k) {
    out += chs[k].bytes;
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
    --w;
  }
  out += kEllipsis;
  ++w;
  for (std::size_t k = t; k < chs.size(); ++k) {
    out += chs[k].bytes;
  }
  out.append(static_cast<std::size_t>(cols - w), ' ');
  return out;
}

std::string
base64(std::span<const std::uint8_t> bytes)
{
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((bytes.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= bytes.size(); i += 3) {
    const std::uint32_t v = (std::uint32_t(bytes[i]) << 16) |
                            (std::uint32_t(bytes[i + 1]) << 8) |
                            bytes[i + 2];
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (const std::size_t rest = bytes.size() - i; rest > 0) {
    std::uint32_t v = std::uint32_t(bytes[i]) << 16;
    if (rest == 2) {
      v |= std::uint32_t(bytes[i + 1]) << 8;
    }
    out += kAlphabet[(v >> 18) & 63];
    out += kAlphabet[(v >> 12) & 63];
    out += rest == 2 ? kAlphabet[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

std::optional<std::vector<std::uint8_t>>
unbase64(std::string_view s)
{
  auto value = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::vector<std::uint8_t> out;
  out.reserve(s.size() / 4 * 3);
  std::uint32_t acc = 0;
  int bits = 0;
  bool ended = false;
  for (char c : s) {
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
      continue;
    }
    if (c == '=') {
      ended = true;
      continue;
    }
    const int v = value(c);
    if (v < 0 || ended) {
      return std::nullopt;
    }
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<std::uint8_t>((acc >> bits) & 0xff));
    }
  }
  return out;
}

}

namespace valtz {

std::string
text_in(const Json& v, std::string_view lang)
{
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (!v.is_object() || v.empty()) {
    return {};
  }
  auto pick = [&](std::string_view tag) -> const Json* {
    if (tag.empty()) {
      return nullptr;
    }
    auto it = v.find(std::string(tag));
    return it != v.end() && it->is_string() ? &*it : nullptr;
  };
  const std::string_view alone = lang.substr(0, lang.find('-'));
  for (const std::string_view tag : {lang, alone, std::string_view("en")}) {
    if (const Json* s = pick(tag)) {
      return s->get<std::string>();
    }
  }
  for (const auto& s : v) {
    if (s.is_string()) {
      return s.get<std::string>();
    }
  }
  return {};
}

}
