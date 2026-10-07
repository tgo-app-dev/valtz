#include "valtz/base/hash.h"

#include <CommonCrypto/CommonDigest.h>

#include <cstdio>
#include <format>
#include <vector>

namespace valtz {

struct Hasher::State {
  CC_SHA256_CTX ctx;
};

Hasher::Hasher() : _s(std::make_unique<State>())
{
  CC_SHA256_Init(&_s->ctx);
}

Hasher::~Hasher() = default;

void
Hasher::update(const void* data, std::size_t n)
{
  // CC_LONG is 32-bit; feed large buffers in chunks.
  auto p = static_cast<const std::uint8_t*>(data);
  while (n > 0) {
    auto chunk = static_cast<CC_LONG>(std::min<std::size_t>(n, 1u << 30));
    CC_SHA256_Update(&_s->ctx, p, chunk);
    p += chunk;
    n -= chunk;
  }
}

ContentHash
Hasher::finish()
{
  ContentHash h;
  CC_SHA256_Final(h.bytes.data(), &_s->ctx);
  CC_SHA256_Init(&_s->ctx);
  return h;
}

ContentHash
ContentHash::of(const void* data, std::size_t n)
{
  Hasher h;
  h.update(data, n);
  return h.finish();
}

std::optional<ContentHash>
ContentHash::parse(std::string_view hex)
{
  if (hex.size() != 64) {
    return std::nullopt;
  }
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    return -1;
  };
  ContentHash h;
  for (std::size_t i = 0; i < 32; ++i) {
    int hi = nib(hex[2 * i]);
    int lo = nib(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) {
      return std::nullopt;
    }
    h.bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }
  return h;
}

bool
ContentHash::is_zero() const noexcept
{
  for (auto b : bytes) {
    if (b) {
      return false;
    }
  }
  return true;
}

std::string
ContentHash::hex() const
{
  static const char* kHex = "0123456789abcdef";
  std::string s(64, '0');
  for (std::size_t i = 0; i < 32; ++i) {
    s[2 * i] = kHex[bytes[i] >> 4];
    s[2 * i + 1] = kHex[bytes[i] & 0xf];
  }
  return s;
}

std::string
ContentHash::short_hex() const
{
  return hex().substr(0, 12);
}

Result<ContentHash>
hash_file(const std::filesystem::path& path)
{
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return make_error(Code::NotFound,
                      std::format("cannot open {}", path.string()));
  }
  Hasher h;
  std::vector<std::uint8_t> buf(std::size_t{4} << 20);
  for (;;) {
    std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
    if (n > 0) {
      h.update(buf.data(), n);
    }
    if (n < buf.size()) {
      break;
    }
  }
  bool err = std::ferror(f) != 0;
  std::fclose(f);
  if (err) {
    return make_error(Code::Io,
                      std::format("read failed: {}", path.string()));
  }
  return h.finish();
}

}
