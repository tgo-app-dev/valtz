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

#ifndef VALTZ_MODELS_MODEL_CHECK_H
#define VALTZ_MODELS_MODEL_CHECK_H

#include "valtz/base/json.h"
#include "valtz/models/catalog.h"

#include <cstdint>
#include <filesystem>
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
  std::uint64_t         bytes = 0;
  int                   weight_files = 0;
  std::uint64_t         tensors = 0;
  // The headline first: an error, else what could not be confirmed,
  // else that it is confirmed; then the rest.
  std::vector<Note>     notes;

  Json to_json() const;
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

}

#endif
