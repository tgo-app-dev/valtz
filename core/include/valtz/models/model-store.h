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

#ifndef VALTZ_MODELS_MODEL_STORE_H
#define VALTZ_MODELS_MODEL_STORE_H

#include "valtz/base/result.h"
#include "valtz/models/catalog.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace valtz::models {

enum class InstallState : std::uint8_t {
  Missing,
  Partial,    // a download was interrupted (*.part files present)
  Installed,
};

const char* to_str(InstallState);

struct InstallInfo {
  InstallState          state = InstallState::Missing;
  std::filesystem::path dir;
  std::filesystem::path file;   // the entry's `file`, in `dir`; or empty
  std::uint64_t         bytes = 0;
  bool                  linked = false;  // where its link points

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

private:
  InstallInfo scan_(const ModelEntry&) const;
  InstallInfo scan_dir_(const ModelEntry&, const std::filesystem::path&,
                        InstallInfo best) const;
  InstallInfo scan_link_(const ModelEntry&,
                         const std::filesystem::path&) const;
  void save_links_() const;

  std::vector<std::filesystem::path>                   _roots;
  std::filesystem::path                                _links_file;
  std::unordered_map<std::string, std::filesystem::path> _links;
  mutable std::mutex                                   _mu;
  mutable std::unordered_map<std::string, InstallInfo> _cache;
};

}

#endif
