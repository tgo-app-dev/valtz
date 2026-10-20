// A model's FILES CHECKED, without loading them (DESIGN §8b): is a
// folder or file offered for a catalog model that model, and whole?
//
// Asked when a model is LINKED (Settings > Capabilities, `valtzctl
// link`): a wrong folder there failed only at the first generation, as
// a load error far from the choice that caused it -- or ran something
// else. Asked too of a COMPRESSED copy before the download it replaces
// is deleted, and of what is installed (`valtzctl check`).
//
// WHERE: the folder chosen, or -- the person chose its parent, or a
// part of it -- the one inside it (three levels down) or around it (two
// up) that is the model; a repo publishing several (MiniMax H3's
// partitions) is entered at the model's own `subdir`. Several copies
// inside are refused, one named after the repo preferred.
//
// THE RIGHT MODEL, by the catalog's `check`: the class, model type or
// architecture its JSON files name (`is`), the files it must have, the
// words its tensors are named with (a LoRA's "lora"). A name that
// differs is a wrong model; nothing to compare -- another layout, a
// Comfy-Org repack -- is said, and the folder used as it is.
//
// WHOLE: every .safetensors holds the bytes its header says (a copy or
// download cut short is the common failure), every part an index lists
// is there and holds the tensors listed for it, every component a
// pipeline lists has its folder, no unfinished download is in it, a
// PyTorch archive ends as a zip does. Hashes are not computed: a 60 GB
// model would take minutes, and the headers catch what goes wrong.
//
// A DRAW THINGS CHECKPOINT (a `.ckpt` that is an SQLite store) is checked
// as a PART of the model -- its DiT, text encoder or VAE -- for a model
// whose catalog `check.drawthings` names what vpipe reads from one, by
// part ({part: {"family", "width": a text encoder's, "not_named": [words
// of another release's file]}}): its table read, every payload in the
// file, its codecs vpipe's, the part the one named. A folder holding no
// download -- Draw Things' Models -- is looked through, at its top, for
// each of them.

#ifndef VALTZ_MODELS_MODEL_CHECK_H
#define VALTZ_MODELS_MODEL_CHECK_H

#include "valtz/base/json.h"
#include "valtz/models/catalog.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace valtz::models {

struct ModelCheck {
  enum class Level : std::uint8_t { Info, Warning, Error };
  struct Note {
    Level level = Level::Info;
    Json  message;  // message_fields: message, key, args
  };

  std::filesystem::path chosen;  // what was offered
  std::filesystem::path path;    // the model found: use this
  std::string           moved;   // "down" | "up" | "" (path vs chosen)
  // Its files say they are this model (its catalog `check`).
  bool                  confirmed = false;
  // Nothing found wrong: it can be used.
  bool                  usable = false;
  std::string           identity;  // what they name it ("Krea2Pipeline")
  // Not the model but PARTS of it: Draw Things checkpoints (models/
  // drawthings.h) of its "dit", "encoder", "vae", by part -- linked
  // beside the model, whose own files bring the rest (its configs, its
  // tokenizer). Empty: the model.
  std::map<std::string, std::filesystem::path> parts;
  std::uint64_t         bytes = 0;
  int                   weight_files = 0;
  std::uint64_t         tensors = 0;
  // The headline first: an error, else what could not be confirmed,
  // else that it is confirmed; then the rest.
  std::vector<Note>     notes;

  Json to_json() const;
  Json parts_json() const;  // {part: file}
  // The first error's message; none: an empty object.
  Json error() const;
};

// `target` (a file or folder that exists) as `e`. `cat`, when given,
// names the other partitions of `e`'s repo (MiniMax H3's FL2VA is not
// its Ref2VA).
ModelCheck check_model(const ModelEntry& e,
                       const std::filesystem::path& target,
                       const Catalog* cat = nullptr);

// What `dir` -- a download of a model with `compress` parts -- takes once
// they are quantized to `bits`: the copy's bytes, and of them the NEW
// ones (the parts quantized; vpipe hard-links the rest). About: from the
// safetensors headers, every 2-D float tensor of a part but its
// embeddings, norms and head at bits/8 + 4/64 bytes a weight (codes, a
// bf16 scale and bias per group of 64). Measured on FLUX.2 klein: 19.8
// GB told for 20.2 made at 8 bits, 11.8 for 12.4 at 4.
struct CompressEstimate {
  std::uint64_t original = 0;
  std::uint64_t total = 0;
  std::uint64_t written = 0;
};
CompressEstimate
estimate_compressed(const std::filesystem::path& dir,
                    const std::vector<ModelEntry::CompressPart>& parts,
                    int bits);

// The folder of a pipeline a model-quantize `target` names
// ("dit" -> "transformer").
std::string compress_part_folder(const std::filesystem::path& dir,
                                 const std::string& target);

// "33.0 GB", "512 MB": a size as the person reads it (decimal units).
std::string size_text(std::uint64_t bytes);

// A language model's context as its config.json in `dir` says: the
// tokens it holds (`max_position_embeddings`; its `text_config`'s for a
// model that also sees -- Qwen3.5's and Qwen3.8's 262,144) and a token's
// K/V (its full-attention layers' keys and values in f16: 64 KB for
// Qwen3.8 27B's 16 of 64 layers, 32 KB for Qwen3.5 9B's 8 of 32). 0
// where it says none.
struct LanguageContext {
  int           tokens = 0;
  std::uint64_t kv_bytes = 0;
};
LanguageContext language_context(const std::filesystem::path& dir);

// The tokens of it the helper is given on a Mac of `ram_bytes`, its
// weights `weight_bytes`: the model's own, as far as its K/V at the
// growth's peak -- twice its size, the pool doubling as it fills -- stays
// within a quarter of the memory the weights leave; whole pages of 16,
// never under 32,768 (an agent's head -- its guide and tools -- is
// ~15,000; past its context, a conversation is compacted). The 27B: all
// 262,144 from about 150 GB (tgo-m5-3's 256), 231,000 on 128, 100,000 on
// 64 and the floor on 24, where its whole context would want 16 GB of K/V
// beside 16 GB of weights; the 9B about 69,000 there. 0: the model says
// none.
int helper_context(const LanguageContext&, std::uint64_t ram_bytes,
                   std::uint64_t weight_bytes);

}

#endif
