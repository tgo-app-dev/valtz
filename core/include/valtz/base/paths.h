// Where Valtz keeps things on disk. One definition, used by the
// controller, the model store, the engine and the app.
//
//   ~/Movies/Valtz/                         projects (user documents)
//     Default.valtz
//   ~/Library/Application Support/com.tgous.valtz/
//     models/                               default model download root
//     cache/                                the MANAGED cache (below)
//     engine/                               engine-private state (vpipe DB)
//
// The cache is in Application Support, not ~/Library/Caches, on purpose:
// it is on the internal SSD next to the models, it is size-managed by
// Valtz (LRU under a budget), and the system must not purge it behind
// our back while an entry is in use. See cache/cache-store.h.

#ifndef VALTZ_BASE_PATHS_H
#define VALTZ_BASE_PATHS_H

#include <filesystem>

namespace valtz {

inline constexpr const char* kBundleId = "com.tgous.valtz";

struct AppPaths {
  std::filesystem::path support;   // .../Application Support/com.tgous.valtz
  std::filesystem::path models;    // support/models
  std::filesystem::path cache;     // support/cache
  std::filesystem::path engine;    // support/engine
  std::filesystem::path projects;  // ~/Movies/Valtz

  // The standard layout under $HOME. `support_override` (tests, CI)
  // relocates everything but `projects`.
  static AppPaths standard(const std::filesystem::path& support_override
                           = {});

  std::filesystem::path default_project() const
  {
    return projects / "Default.valtz";
  }
};

}

#endif
