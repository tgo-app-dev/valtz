#include "valtz/models/drawthings.h"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>

namespace valtz::models {

namespace fs = std::filesystem;

namespace {

// libccv / s4nnc's codecs and flags, as vpipe's reader spells them: a
// row's `type` packs the codec in bits 32..55, an external payload's flag
// and an i8x weight's rotation above it.
constexpr std::uint32_t kCodecRaw = 0;
constexpr std::uint32_t kCodecEzm7 = 0x511;
constexpr std::uint32_t kCodecQ8p = 0x8a1e8b;
constexpr std::uint32_t kCodecI8x = 0x8a1e9b;
constexpr std::uint32_t kExternal = 0x10000000u;
constexpr std::uint32_t kHadamard = 0x01000000u;
constexpr std::int64_t  kDtypeF16 = 0x20000;
constexpr std::int64_t  kDtypeF32 = 0x4000;

// The tensors vpipe tells each DiT by (map_to_diffusers): all three.
struct Family {
  const char* name;
  const char* tensors[3];
};
constexpr Family kFamilies[] = {
  {"qwen-image-21",
   {"__dit__[t-x_ada_ln_0-0-0]", "__dit__[t-context_norm-0-0]",
    "__dit__[t-ffn_gate_proj-0-0]"}},
  {"minimax-h3",
   {"__dit__[t-adaln_0_0-0-0]", "__dit__[t-refiner_q-0-0]",
    "__dit__[t-context_embedder-0-0]"}},
};
// A text encoder's first q_proj: its columns are the model's width.
constexpr char kTextQ[] = "__text_model__[t-q_proj-0-0]";

// A tensor's group: `__<group>__[t-...]`; "" for another name.
std::string
group_of(const std::string& n)
{
  if (n.size() < 8 || n.compare(0, 2, "__") != 0) {
    return {};
  }
  const auto ge = n.find("__[t-", 2);
  return ge == std::string::npos ? std::string() : n.substr(2, ge - 2);
}

// SQLite's URI form, so `immutable=1` can say the file will not change:
// no journal, no lock file beside it.
std::string
uri(const std::string& path)
{
  std::string u = "file:";
  for (const char c : path) {
    if (c == '%' || c == '?' || c == '#') {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", static_cast<unsigned char>(c));
      u += b;
    } else {
      u += c;
    }
  }
  return u + "?immutable=1";
}

}

bool
looks_like_drawthings(const fs::path& file)
{
  char m[16] = {};
  std::ifstream in(file, std::ios::binary);
  return in.read(m, sizeof m) &&
         std::memcmp(m, "SQLite format 3", sizeof m) == 0;
}

DrawThingsStore
read_drawthings(const fs::path& file)
{
  DrawThingsStore s;
  std::error_code ec;
  s.bytes = fs::file_size(file, ec);
  std::uint8_t h[64] = {};
  {
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(h), sizeof h) ||
        std::memcmp(h, "SQLite format 3", 16) != 0) {
      s.error = "not an SQLite database";
      return s;
    }
  }
  const std::uint64_t base = (std::uint64_t{h[60]} << 24) |
                             (std::uint64_t{h[61]} << 16) |
                             (std::uint64_t{h[62]} << 8) | h[63];
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(uri(file.string()).c_str(), &raw,
                      SQLITE_OPEN_READONLY | SQLITE_OPEN_URI,
                      nullptr) != SQLITE_OK) {
    if (raw) {
      sqlite3_close(raw);
    }
    s.error = "not an SQLite database";
    return s;
  }
  std::unique_ptr<sqlite3, int (*)(sqlite3*)> db(raw, sqlite3_close);
  sqlite3_stmt* rs = nullptr;
  if (sqlite3_prepare_v2(db.get(),
                         "SELECT name, type, datatype, data, dim "
                         "FROM tensors",
                         -1, &rs, nullptr) != SQLITE_OK) {
    s.error = "no tensors table";
    return s;
  }
  std::unique_ptr<sqlite3_stmt, int (*)(sqlite3_stmt*)> st(
      rs, sqlite3_finalize);
  s.store = true;
  int seen[std::size(kFamilies)] = {};
  std::set<std::string> groups;
  bool vae_conv_in = false;
  int rc;
  while ((rc = sqlite3_step(st.get())) == SQLITE_ROW) {
    ++s.tensors;
    const auto* nm = sqlite3_column_text(st.get(), 0);
    const std::string name = nm ? reinterpret_cast<const char*>(nm) : "";
    for (std::size_t f = 0; f < std::size(kFamilies); ++f) {
      for (const char* t : kFamilies[f].tensors) {
        seen[f] += name == t ? 1 : 0;
      }
    }
    if (std::string g = group_of(name); !g.empty()) {
      groups.insert(std::move(g));
    }
    vae_conv_in = vae_conv_in ||
                  name == "__decoder__[t-decoder_conv_in-0-0]" ||
                  name == "__encoder__[t-encoder_conv_in-0-0]";
    // Its shape: twelve int32s, ending at the first 0 (a row may carry
    // its byte count after it). A weight [N, K]: K its width.
    if (name == kTextQ) {
      const auto* dim = static_cast<const std::uint8_t*>(
          sqlite3_column_blob(st.get(), 4));
      const int n = sqlite3_column_bytes(st.get(), 4);
      for (int i = 0; dim && i + 4 <= n && i < 48; i += 4) {
        std::int32_t d = 0;
        std::memcpy(&d, dim + i, 4);
        if (d == 0) {
          break;
        }
        s.width = d;
      }
    }
    const auto type =
        static_cast<std::uint64_t>(sqlite3_column_int64(st.get(), 1));
    const auto ident = static_cast<std::uint32_t>(type >> 32);
    const std::uint32_t codec = ident & 0x00ffffffu;
    const std::int64_t dtype = sqlite3_column_int64(st.get(), 2);
    const bool known = codec == kCodecRaw || codec == kCodecEzm7 ||
                       codec == kCodecQ8p || codec == kCodecI8x;
    if ((!known || (dtype != kDtypeF16 && dtype != kDtypeF32)) &&
        s.unread.empty()) {
      s.unread = name;
    }
    s.eight_bit += codec == kCodecI8x ? 1 : 0;
    s.palette += codec == kCodecQ8p ? 1 : 0;
    s.rotated += codec == kCodecI8x && (ident & kHadamard) ? 1 : 0;
    // An external payload: (u64 offset, u64 length) from the base; a
    // palette row heads them with its block size (u32, padded to 8).
    if (ident & kExternal) {
      const auto* data = static_cast<const std::uint8_t*>(
          sqlite3_column_blob(st.get(), 3));
      const int n = sqlite3_column_bytes(st.get(), 3);
      const int at = codec == kCodecQ8p ? 8 : 0;
      if (!data || n < at + 16) {
        s.reach = std::max(s.reach, s.bytes + 1);  // nowhere to read it
        continue;
      }
      std::uint64_t off = 0, len = 0;
      std::memcpy(&off, data + at, 8);
      std::memcpy(&len, data + at + 8, 8);
      s.reach = std::max(s.reach, base + off + len);
    }
  }
  if (rc != SQLITE_DONE) {
    s.error = sqlite3_errmsg(db.get());
    return s;
  }
  // In vpipe's order: a component by its groups, then a DiT.
  auto has = [&](std::initializer_list<const char*> gs) {
    return std::ranges::any_of(gs, [&](const char* g) {
      return groups.contains(g);
    });
  };
  if (has({"text_model"})) {
    s.part = "encoder";
    s.family = "qwen3-vl";
  } else if (has({"audio_decoder", "audio_encoder", "video_decoder",
                  "video_encoder"})) {
    s.part = "vae";
    s.family = "minimax-h3-vae";
  } else if (has({"encoder", "decoder"}) && vae_conv_in) {
    s.part = "vae";
    s.family = "qwen-image-21-vae";
  } else {
    for (std::size_t f = 0; f < std::size(kFamilies); ++f) {
      if (seen[f] == 3) {
        s.part = "dit";
        s.family = kFamilies[f].name;
      }
    }
  }
  return s;
}

}
