// finder.h -- what the Finder shows of a file and lets one set: its
// TAGS (the Finder's, as Get Info and a document's title bar list them)
// and its LOCKED flag (the user-immutable flag Get Info's Locked sets: a
// locked package cannot be written into, so a project's Save fails --
// Valtz says so first). On a package -- a project -- they are the
// folder's own, as the Finder keeps them.
#pragma once

#include "valtz/base/result.h"

#include <filesystem>
#include <string>
#include <vector>

namespace valtz {

Result<std::vector<std::string>> finder_tags(const std::filesystem::path&);
Status set_finder_tags(const std::filesystem::path&,
                       const std::vector<std::string>& tags);

// Not there or unreadable: false.
bool finder_locked(const std::filesystem::path&);
Status set_finder_locked(const std::filesystem::path&, bool locked);

}  // namespace valtz
