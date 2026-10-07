// macOS bookmark data for linked originals (bookmark.mm).

#ifndef VALTZ_PROJECT_BOOKMARK_H
#define VALTZ_PROJECT_BOOKMARK_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace valtz::project {

// Base64 bookmark for `path`; empty if one cannot be made.
std::string make_bookmark(const std::filesystem::path& path);
// Where a bookmark points now. Never mounts volumes or shows UI: an
// original on an unplugged drive is simply not found.
std::optional<std::filesystem::path> resolve_bookmark(const std::string&);

struct FileStamp {
  std::uint64_t size = 0;
  std::int64_t  mtime_ns = 0;
};
std::optional<FileStamp> stamp_of(const std::filesystem::path&);

}

#endif
