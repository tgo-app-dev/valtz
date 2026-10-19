// Where models are on disk and whether they are complete.
//
// Checkpoints use vpipe's model-fetch layout, <root>/<org>/<repo>,
// mirroring the hub path, so a directory vpipe fetched is one Valtz can
// use and vice versa. Several roots are searched in order; downloads go
// to the first (the app's own, in Application Support).
//
// A model can also be LINKED to a file or folder anywhere -- a ComfyUI
// or Draw Things models folder, a download kept elsewhere -- which is
// then where it is, ahead of the roots (Settings > Capabilities). The
// links are kept in a small JSON file beside the models folder, by
// catalog id.
//
// A download can be replaced by a COMPRESSED copy of itself (DESIGN
// §8b): <download root>/compressed/<id>-<bits>bit, marked by the file
// Valtz writes in it (kCompressedMarker). Where one is, the model is
// there -- after a link, before the roots -- until its download is
// fetched again. A copy made with its original KEPT (to try it first)
// is in use beside it until its mark sets it aside ("in_use": false);
// the original is used then.
//
// A model's PARTS can be linked apart -- its DiT, text encoder, VAE, each
// a Draw Things checkpoint (models/drawthings.h) -- the rest of it (its
// configs, its tokenizer) where the model is: kept in the same file,
// under "part_links".

#ifndef VALTZ_MODELS_MODEL_STORE_H
#define VALTZ_MODELS_MODEL_STORE_H

#include "valtz/base/result.h"
#include "valtz/models/catalog.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace valtz::models {

enum class InstallState : std::uint8_t {
  Missing,
  Partial,    // a download paused or interrupted (*.part files present)
  Installed,
};

const char* to_str(InstallState);

struct InstallInfo {
  InstallState          state = InstallState::Missing;
  std::filesystem::path dir;
  std::filesystem::path file;   // the entry's `file`, in `dir`; or empty
  std::uint64_t         bytes = 0;   // on disk (Partial: so far)
  bool                  linked = false;  // where its link points
  int                   compressed_bits = 0;  // a compressed copy: 8, 4
  // Its OTHER weights, kept beside these (a compress that kept the
  // original): while a compressed copy is in use, the original it was
  // made from (`other_bits` 0); while the original is -- the copy set
  // aside -- the copy. `other_dir` empty: none.
  std::filesystem::path other_dir;
  int                   other_bits = 0;
  std::uint64_t         other_bytes = 0;
  // Its parts linked apart (link_part), by part, and whether each file
  // is there.
  struct PartLink {
    std::filesystem::path file;
    bool                  present = false;
  };
  std::map<std::string, PartLink> parts;
  // The part's file when linked and there; empty otherwise.
  std::filesystem::path part(const std::string& name) const;

  // What the engine is given: the entry's file when it names one, else
  // the folder.
  std::filesystem::path path() const;
};

class ModelStore {
public:
  // `roots` must not be empty; the first is the download root.
  // `links_file`: where links are kept (none: links are not kept).
  explicit ModelStore(std::vector<std::filesystem::path> roots,
                      std::filesystem::path links_file = {});

  // `primary` (the app's models dir, where downloads go), then
  // ~/vpipe/models and ~/dump/vpipe-test/models when present -- a
  // developer box that already holds vpipe's checkpoints.
  static std::vector<std::filesystem::path>
  default_roots(const std::filesystem::path& primary);

  const std::vector<std::filesystem::path>& roots() const noexcept
  {
    return _roots;
  }
  const std::filesystem::path& download_root() const
  {
    return _roots.front();
  }

  // Scans lazily and caches; rescan() after a download or delete.
  InstallInfo info(const ModelEntry&) const;
  void rescan();

  // `e` linked to `target`, a file or folder that exists: for a model
  // that names a `file`, that file -- or a folder holding it (or, for a
  // LoRA, its largest .safetensors); for a folder model, its folder.
  Status link(const ModelEntry& e, const std::filesystem::path& target);
  void unlink(const std::string& id);
  // Where `id` is linked to; empty when it is not.
  std::filesystem::path link_of(const std::string& id) const;

  // `e`'s `part` ("dit", "encoder", "vae") taken from `file` (checked
  // first: models/model-check.h), in place of the one where the model
  // is; unlink_parts gives every one back.
  Status link_part(const ModelEntry& e, const std::string& part,
                   const std::filesystem::path& file);
  void unlink_parts(const std::string& id);
  std::map<std::string, std::filesystem::path>
  part_links_of(const std::string& id) const;

  // Where `id`'s copy compressed to `bits` is kept.
  std::filesystem::path compressed_dir(const std::string& id,
                                       int bits) const;
  static constexpr const char* kCompressedMarker = "valtz-compressed.json";

private:
  InstallInfo scan_(const ModelEntry&) const;
  InstallInfo scan_roots_(const ModelEntry&) const;
  InstallInfo scan_dir_(const ModelEntry&, const std::filesystem::path&,
                        InstallInfo best) const;
  InstallInfo scan_link_(const ModelEntry&,
                         const std::filesystem::path&) const;
  void save_links_() const;

  std::vector<std::filesystem::path>                   _roots;
  std::filesystem::path                                _links_file;
  std::unordered_map<std::string, std::filesystem::path> _links;
  std::unordered_map<std::string,
                     std::map<std::string, std::filesystem::path>>
                                                       _part_links;
  mutable std::mutex                                   _mu;
  mutable std::unordered_map<std::string, InstallInfo> _cache;
};

}

#endif
