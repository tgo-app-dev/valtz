#include "valtz/base/id.h"

#include <chrono>
#include <cstdlib>

namespace valtz {

namespace {

int
hex_val(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

}

Uuid
Uuid::v7()
{
  Uuid u;
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  auto t = static_cast<std::uint64_t>(ms);
  for (int i = 0; i < 6; ++i) {
    u.bytes[i] = static_cast<std::uint8_t>(t >> (8 * (5 - i)));
  }
  arc4random_buf(u.bytes.data() + 6, 10);
  u.bytes[6] = static_cast<std::uint8_t>(0x70 | (u.bytes[6] & 0x0f));
  u.bytes[8] = static_cast<std::uint8_t>(0x80 | (u.bytes[8] & 0x3f));
  return u;
}

std::optional<Uuid>
Uuid::parse(std::string_view s)
{
  if (s.size() != 36) {
    return std::nullopt;
  }
  Uuid u;
  std::size_t bi = 0;
  for (std::size_t i = 0; i < s.size();) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-') {
        return std::nullopt;
      }
      ++i;
      continue;
    }
    int hi = hex_val(s[i]);
    int lo = hex_val(s[i + 1]);
    if (hi < 0 || lo < 0 || bi >= 16) {
      return std::nullopt;
    }
    u.bytes[bi++] = static_cast<std::uint8_t>((hi << 4) | lo);
    i += 2;
  }
  return bi == 16 ? std::optional<Uuid>(u) : std::nullopt;
}

bool
Uuid::is_nil() const noexcept
{
  for (auto b : bytes) {
    if (b) {
      return false;
    }
  }
  return true;
}

std::string
Uuid::str() const
{
  static const char* kHex = "0123456789abcdef";
  std::string s;
  s.reserve(36);
  for (std::size_t i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      s.push_back('-');
    }
    s.push_back(kHex[bytes[i] >> 4]);
    s.push_back(kHex[bytes[i] & 0xf]);
  }
  return s;
}

std::uint64_t
Uuid::unix_ms() const noexcept
{
  std::uint64_t t = 0;
  for (int i = 0; i < 6; ++i) {
    t = (t << 8) | bytes[i];
  }
  return t;
}

}
