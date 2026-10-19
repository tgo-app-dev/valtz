// A Draw Things checkpoint (`.ckpt`), read as far as a link's check needs
// (DESIGN §8b): s4nnc's TRAILER STORE -- an SQLite database whose one
// table, `tensors(name, type, format, datatype, dim, data)`, is the table
// of contents, the bulk of the bytes lying after its pages from a base in
// header bytes 60..63 (big-endian: SQLite's user_version).
//
// WHY Valtz reads it at all: vpipe loads one as a model's DiT, text
// encoder or VAE (the stages' `dit_dir`, `encoder_dir`, `vae_dir`; its
// shared/drawthings-store), but a wrong file -- another model's, another
// part's, one cut short, a layout vpipe does not read -- failed only at
// the first generation. So a link to one is checked as any link is: the
// table read (never the weights), each payload inside the file, the
// codecs vpipe reads, and WHAT IT IS told as vpipe tells it
// (map_to_diffusers): by its tensor groups -- `text_model` a Qwen3-VL
// text encoder (its width says which: 4096 the 8B, 5120 the 32B),
// `video_*` / `audio_*` MiniMax H3's two VAEs in one file, `encoder` /
// `decoder` with their conv_in Qwen-Image 2.1's VAE -- and a DiT by the
// three tensors vpipe tells it by. vpipe stays the judge at load.
//
// Draw Things keeps a model's parts in files of their own, and none holds
// a config or a tokenizer: a checkpoint stands in for a PART's weights,
// the model's own folder bringing the rest. A PyTorch `.ckpt` (a zip) is
// not one.

#ifndef VALTZ_MODELS_DRAWTHINGS_H
#define VALTZ_MODELS_DRAWTHINGS_H

#include <cstdint>
#include <filesystem>
#include <string>

namespace valtz::models {

struct DrawThingsStore {
  // An SQLite database holding a `tensors` table: a Draw Things store.
  bool          store = false;
  // The part it is: "dit", "encoder", "vae"; "" none vpipe reads.
  std::string   part;
  // What vpipe reads it as: a DiT's family ("qwen-image-21",
  // "minimax-h3"), "qwen3-vl", "qwen-image-21-vae", "minimax-h3-vae".
  std::string   family;
  // A text encoder's width (its q_proj's columns): 4096, 5120.
  std::int64_t  width = 0;
  std::uint64_t tensors = 0;
  std::uint64_t bytes = 0;      // the file's
  std::uint64_t reach = 0;      // where its last payload ends
  std::uint64_t eight_bit = 0;  // i8x weights: 8-bit codes, a row scale
  std::uint64_t palette = 0;    // q8p: a 256-entry palette a block
  // i8x weights stored Hadamard-rotated, Draw Things' own ordering, which
  // vpipe does not undo: refused.
  std::uint64_t rotated = 0;
  // The first tensor with a codec or element type vpipe does not read.
  std::string   unread;
  // Why it is not a store ("" when it is).
  std::string   error;

  bool cut_short() const { return reach > bytes; }
};

// The SQLite magic: worth reading the table. Cheap.
bool looks_like_drawthings(const std::filesystem::path& file);

// Its table of contents, read (immutable: nothing written beside the
// file -- it is the person's, in Draw Things' folder).
DrawThingsStore read_drawthings(const std::filesystem::path& file);

}

#endif
