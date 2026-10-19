// A Draw Things checkpoint made for a test, laid out as s4nnc writes one
// (core models/drawthings.h): an SQLite database whose `tensors` table is
// the table of contents, every payload EXTERNAL -- after the database's
// pages, from the base in header bytes 60..63 (SQLite's user_version,
// big-endian) -- its row's `data` the payload's offset and length from
// the base.

#ifndef VALTZ_TESTS_DRAWTHINGS_FIXTURE_H
#define VALTZ_TESTS_DRAWTHINGS_FIXTURE_H

#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace valtz::test {

struct DrawThingsFixture {
  // Codecs (libccv / s4nnc): raw f16, i8x 8-bit codes and row scales.
  static constexpr std::uint32_t kRaw = 0;
  static constexpr std::uint32_t kI8x = 0x8a1e9b;
  static constexpr std::uint32_t kExternal = 0x10000000u;
  static constexpr std::uint32_t kHadamard = 0x01000000u;

  std::vector<std::string> names;
  std::uint32_t            codec = kI8x;
  bool                     rotated = false;
  // Bytes left off the end: a copy cut short.
  std::uint64_t            short_by = 0;
  // Each weight's columns, as its row's `dim` says: a text encoder's
  // width.
  std::int32_t             cols = 64;

  // Qwen-Image 2.1's DiT, as vpipe tells it.
  static DrawThingsFixture qwen_image_21()
  {
    return {{"__dit__[t-x_ada_ln_0-0-0]", "__dit__[t-context_norm-0-0]",
             "__dit__[t-ffn_gate_proj-0-0]", "__dit__[t-x_q-3-0]"}};
  }
  // MiniMax H3's.
  static DrawThingsFixture minimax_h3()
  {
    return {{"__dit__[t-adaln_0_0-0-0]", "__dit__[t-refiner_q-0-0]",
             "__dit__[t-context_embedder-0-0]"}};
  }
  // A Qwen3-VL text encoder, `cols` wide: Qwen-Image 2.1's 8B is 4096,
  // MiniMax H3's 32B 5120.
  static DrawThingsFixture qwen3_vl(std::int32_t width)
  {
    DrawThingsFixture f{{"__text_model__[t-q_proj-0-0]",
                         "__text_model__[t-k_proj-0-0]",
                         "__vision_model__[t-q_proj-0-0]"}};
    f.cols = width;
    return f;
  }
  // Qwen-Image 2.1's VAE.
  static DrawThingsFixture qwen_image_21_vae()
  {
    return {{"__decoder__[t-decoder_conv_in-0-0]",
             "__encoder__[t-encoder_conv_in-0-0]"}};
  }
  // MiniMax H3's two VAEs, in one file.
  static DrawThingsFixture minimax_h3_vae()
  {
    return {{"__video_decoder__[t-conv_in-0-0]",
             "__audio_decoder__[t-conv_in-0-0]"}};
  }
  // A store of nothing vpipe reads.
  static DrawThingsFixture unknown()
  {
    return {{"__unet__[t-conv_in-0-0]"}};
  }

  // Written to `file`; false when SQLite would not.
  bool write(const std::filesystem::path& file) const
  {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(file, ec);
    sqlite3* db = nullptr;
    if (sqlite3_open(file.c_str(), &db) != SQLITE_OK) {
      return false;
    }
    bool ok = sqlite3_exec(db,
                           "CREATE TABLE tensors (name TEXT, type INTEGER, "
                           "format INTEGER, datatype INTEGER, dim BLOB, "
                           "data BLOB)",
                           nullptr, nullptr, nullptr) == SQLITE_OK;
    sqlite3_stmt* st = nullptr;
    ok = ok && sqlite3_prepare_v2(db,
                                  "INSERT INTO tensors VALUES "
                                  "(?, ?, 0, ?, ?, ?)",
                                  -1, &st, nullptr) == SQLITE_OK;
    // Each a [16, 64] weight: its payload 16 x 64 bytes of codes (and,
    // i8x, 16 f16 row scales at the next 128).
    const std::uint64_t each = codec == kI8x ? 16 * 64 + 128 : 16 * 64 * 2;
    std::uint64_t at = 0;
    for (const auto& n : names) {
      std::uint32_t ident = codec | kExternal;
      if (rotated) {
        ident |= kHadamard;
      }
      const auto type = static_cast<std::int64_t>(std::uint64_t{ident}
                                                  << 32);
      std::int32_t dim[12] = {16, cols};
      std::uint64_t span[2] = {at, each};
      at += each;
      sqlite3_bind_text(st, 1, n.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 2, type);
      sqlite3_bind_int64(st, 3, 0x20000);  // f16
      sqlite3_bind_blob(st, 4, dim, sizeof dim, SQLITE_TRANSIENT);
      sqlite3_bind_blob(st, 5, span, sizeof span, SQLITE_TRANSIENT);
      ok = ok && sqlite3_step(st) == SQLITE_DONE;
      sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    sqlite3_close(db);
    if (!ok) {
      return false;
    }
    // The base: where the database's pages end.
    const auto base = static_cast<std::uint32_t>(fs::file_size(file, ec));
    {
      std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
      const unsigned char b[4] = {
          static_cast<unsigned char>(base >> 24),
          static_cast<unsigned char>(base >> 16),
          static_cast<unsigned char>(base >> 8),
          static_cast<unsigned char>(base)};
      f.seekp(60);
      f.write(reinterpret_cast<const char*>(b), 4);
      f.seekp(0, std::ios::end);
      const std::uint64_t n = at > short_by ? at - short_by : 0;
      f << std::string(n, '\x01');
    }
    return true;
  }
};

}

#endif
