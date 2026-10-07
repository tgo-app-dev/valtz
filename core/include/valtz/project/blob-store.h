// Content-addressed file store inside a project package:
//
//   <project>.valtz/blobs/<h0h1>/<sha256-hex>.<ext>
//
// A blob is named by the SHA-256 of its bytes, so importing the same
// file twice, or two builds producing identical output, costs one copy;
// a peer asked to build something is sent only the input blobs it lacks.
//
// Media never goes INTO LMDB: a multi-GB ProRes file would bloat the
// map, be copied on every compaction, and serialize writers. LMDB holds
// the records; the blob store holds the bytes.
//
// Ingest is crash-safe: bytes land in tmp/ and are renamed into place
// only after they are hashed. On APFS, ingesting a file from the same
// volume is a clonefile(2) -- instant, and no extra space until either
// copy is modified.
//
// READ ROOTS (a working copy's, DESIGN §5b): blobs are immutable, so a
// working copy reads its package's blobs where they are and writes new
// ones into its own root; a blob already in a read root is not stored
// again. Removing touches the own root only.

#ifndef VALTZ_PROJECT_BLOB_STORE_H
#define VALTZ_PROJECT_BLOB_STORE_H

#include "valtz/base/result.h"
#include "valtz/project/records.h"

#include <filesystem>
#include <string_view>
#include <vector>

namespace valtz::project {

class BlobStore {
public:
  explicit BlobStore(std::filesystem::path root);

  const std::filesystem::path& root() const noexcept { return _root; }
  // Blobs found under `root` are read there (never written there).
  void add_read_root(std::filesystem::path root);
  void clear_read_roots() { _read.clear(); }
  const std::vector<std::filesystem::path>& read_roots() const noexcept
  {
    return _read;
  }

  // Where the blob is: the own root, else a read root that has it, else
  // where the own root would keep it.
  std::filesystem::path path_of(const ContentHash&,
                                std::string_view ext) const;
  std::filesystem::path path_of(const BlobRef& b) const
  {
    return path_of(b.hash, b.ext);
  }
  bool contains(const BlobRef&) const;

  // Hash `src` and store it (clone when possible, else copy).
  Result<BlobRef> ingest_file(const std::filesystem::path& src);
  // Take ownership of a file already written into tmp_dir() (a build
  // output): hash it and rename it into place.
  Result<BlobRef> adopt_file(const std::filesystem::path& tmp_file,
                             std::string_view ext);
  Result<BlobRef> put_bytes(std::string_view bytes, std::string_view ext);
  // The blob's file gone (nothing else may hold it).
  Status remove(const BlobRef&);

  // Where builders write outputs before adopt_file(). Same volume as the
  // blobs, so adoption is a rename.
  std::filesystem::path tmp_dir() const;
  std::filesystem::path make_tmp_path(std::string_view ext) const;

  // `hash`.`ext` under a blobs root, as stored.
  static std::filesystem::path under(const std::filesystem::path& root,
                                     const ContentHash&,
                                     std::string_view ext);

private:
  std::filesystem::path              _root;  // .../blobs
  std::vector<std::filesystem::path> _read;  // read roots, in order
};

}

#endif
