// Where Valtz keeps things on disk. One definition, used by the
// controller, the model store, the engine and the app.
//
//   ~/Movies/Valtz/                         projects (user documents)
//     Default.valtz
//   ~/Library/Application Support/com.tgous.valtz/
//     models/                               default model download root
//     cache/                                the MANAGED cache (below)
//     engine/                               engine-private state (vpipe DB)
//     locations.json                        the three above, moved
//
// The cache is in Application Support, not ~/Library/Caches, on purpose:
// it is on the internal SSD next to the models, it is size-managed by
// Valtz (LRU under a budget), and the system must not purge it behind
// our back while an entry is in use. See cache/cache-store.h.
//
// The projects, models and cache folders can be put ELSEWHERE (Settings ›
// General › Storage, `valtzctl locations`): kept in locations.json, read
// by every Valtz that starts -- the app and valtzctl alike. A models
// folder left behind with its models in it (Keep, not Move) stays one
// Valtz looks in and owns: `models_kept`.

#ifndef VALTZ_BASE_PATHS_H
#define VALTZ_BASE_PATHS_H

#include "valtz/base/result.h"

#include <filesystem>
#include <vector>

namespace valtz {

inline constexpr const char* kBundleId = "com.tgous.valtz";

struct AppPaths {
  std::filesystem::path support;   // .../Application Support/com.tgous.valtz
  std::filesystem::path models;    // support/models, or where it was put
  std::filesystem::path cache;     // support/cache, or where it was put
  std::filesystem::path engine;    // support/engine
  std::filesystem::path projects;  // ~/Movies/Valtz, or where it was put
  // Models folders left where they were, their models still used, after
  // `models`.
  std::vector<std::filesystem::path> models_kept;

  // The standard layout under $HOME, the locations chosen laid over it.
  // `support_override` (tests, CI) relocates everything but `projects`.
  static AppPaths standard(const std::filesystem::path& support_override
                           = {});
  // The standard layout alone: where "Use Default" puts each.
  static AppPaths defaults(const std::filesystem::path& support_override
                           = {});

  std::filesystem::path default_project() const
  {
    return projects / "Default.valtz";
  }
};

// What locations.json holds: a folder moved from its default (empty: the
// default), and the models folders kept.
struct Locations {
  std::filesystem::path              projects;
  std::filesystem::path              models;
  std::filesystem::path              cache;
  std::vector<std::filesystem::path> models_kept;
};

Locations read_locations(const std::filesystem::path& support);
Status write_locations(const std::filesystem::path& support,
                       const Locations&);

// On this Mac's internal drive -- not an external one, not a network
// share. Unknown (not there yet): internal.
bool volume_is_internal(const std::filesystem::path&);
// The two on one volume: a move is a rename.
bool same_volume(const std::filesystem::path&, const std::filesystem::path&);
// `inner` is `outer` or somewhere in it (lexically, normalized).
bool path_within(const std::filesystem::path& inner,
                 const std::filesystem::path& outer);

}

#endif
