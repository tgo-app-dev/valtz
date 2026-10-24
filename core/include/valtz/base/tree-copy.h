// A folder's files moved elsewhere -- the models', the cache's, when the
// person puts them on another drive or in another folder (Settings ›
// General › Storage). Folders MERGE: what is there already stays, a file
// of the same name and size taken as the one being moved.
//
// Across volumes it is a COPY, file by file, counted by the byte (a
// model's shard is tens of GB), into a hidden name renamed into place
// when whole -- a copy stopped or cut short leaves no file that passes
// for one -- the source left for the caller to delete once nothing reads
// it. On one volume it is a RENAME of each entry, at once.

#ifndef VALTZ_BASE_TREE_COPY_H
#define VALTZ_BASE_TREE_COPY_H

#include "valtz/base/result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <vector>

namespace valtz {

struct TreeSize {
  std::uint64_t bytes = 0;
  std::uint64_t files = 0;
};

// What is under `p` (nothing: 0); symbolic links not followed.
TreeSize tree_size(const std::filesystem::path& p);

// Every file under `from` copied to its place under `to`. `progress` is
// told the bytes copied so far, a few times a second at most; false
// stops the copy (Code::Cancelled). `made` gets each file written (to be
// taken back if the move does not go on).
Status copy_tree(const std::filesystem::path& from,
                 const std::filesystem::path& to,
                 const std::function<bool(std::uint64_t)>& progress,
                 std::vector<std::filesystem::path>* made);

// Every entry of `from` renamed into `to` (one volume), folders merged;
// `from` left empty.
Status move_tree(const std::filesystem::path& from,
                 const std::filesystem::path& to);

// `p`'s empty folders taken away from the bottom up -- `p` too when it
// is left empty.
void remove_empty_dirs(const std::filesystem::path& p);

}

#endif
