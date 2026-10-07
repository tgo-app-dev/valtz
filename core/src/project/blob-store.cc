#include "valtz/project/blob-store.h"

#include <sys/clonefile.h>

#include <cstdio>
#include <format>
#include <fstream>

namespace valtz::project {

namespace fs = std::filesystem;

namespace {

std::string
lower_ext(const fs::path& p)
{
  std::string e = p.extension().string();
  if (!e.empty() && e[0] == '.') {
    e.erase(0, 1);
  }
  for (auto& c : e) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return e;
}

}

BlobStore::BlobStore(fs::path root) : _root(std::move(root)) {}

fs::path
BlobStore::under(const fs::path& root, const ContentHash& h,
                 std::string_view ext)
{
  std::string hex = h.hex();
  std::string name = ext.empty() ? hex : std::format("{}.{}", hex, ext);
  return root / hex.substr(0, 2) / name;
}

void
BlobStore::add_read_root(fs::path root)
{
  _read.push_back(std::move(root));
}

fs::path
BlobStore::path_of(const ContentHash& h, std::string_view ext) const
{
  fs::path own = under(_root, h, ext);
  if (_read.empty()) {
    return own;
  }
  std::error_code ec;
  if (fs::is_regular_file(own, ec)) {
    return own;
  }
  for (const auto& r : _read) {
    fs::path p = under(r, h, ext);
    if (fs::is_regular_file(p, ec)) {
      return p;
    }
  }
  return own;
}

bool
BlobStore::contains(const BlobRef& b) const
{
  std::error_code ec;
  return fs::is_regular_file(path_of(b), ec);
}

fs::path
BlobStore::tmp_dir() const
{
  return _root.parent_path() / "tmp";
}

fs::path
BlobStore::make_tmp_path(std::string_view ext) const
{
  auto name = Uuid::v7().str();
  if (!ext.empty()) {
    name += ".";
    name += ext;
  }
  return tmp_dir() / name;
}

Result<BlobRef>
BlobStore::adopt_file(const fs::path& tmp_file, std::string_view ext)
{
  VALTZ_ASSIGN(ContentHash h, hash_file(tmp_file));
  std::error_code ec;
  auto size = fs::file_size(tmp_file, ec);
  if (ec) {
    return make_error(Code::Io, ec.message());
  }
  BlobRef ref{h, size, std::string(ext)};
  if (contains(ref)) {
    // Identical content is already stored (here or in a read root);
    // drop the duplicate.
    fs::remove(tmp_file, ec);
    return ref;
  }
  fs::path dst = under(_root, ref.hash, ref.ext);
  fs::create_directories(dst.parent_path(), ec);
  fs::rename(tmp_file, dst, ec);
  if (ec) {
    return make_error(Code::Io, std::format("store blob: {}", ec.message()));
  }
  return ref;
}

Result<BlobRef>
BlobStore::ingest_file(const fs::path& src)
{
  std::error_code ec;
  fs::create_directories(tmp_dir(), ec);
  std::string ext = lower_ext(src);
  fs::path tmp = make_tmp_path(ext);
  // Clone first (APFS, same volume): O(1), and shares blocks until one
  // side is written. A cross-volume source falls back to a real copy.
  if (clonefile(src.c_str(), tmp.c_str(), CLONE_NOFOLLOW) != 0) {
    fs::copy_file(src, tmp, ec);
    if (ec) {
      return make_error(Code::Io, std::format("copy {}: {}",
                                              src.string(), ec.message()));
    }
  }
  return adopt_file(tmp, ext);
}

Status
BlobStore::remove(const BlobRef& b)
{
  // The own root's alone: a read root's blobs are another's to remove.
  const fs::path own = under(_root, b.hash, b.ext);
  std::error_code ec;
  fs::remove(own, ec);
  return ec ? make_error(Code::Io, std::format("cannot remove {}: {}",
                                               own.string(), ec.message()))
            : ok_status();
}

Result<BlobRef>
BlobStore::put_bytes(std::string_view bytes, std::string_view ext)
{
  std::error_code ec;
  fs::create_directories(tmp_dir(), ec);
  fs::path tmp = make_tmp_path(ext);
  {
    std::ofstream out(tmp, std::ios::binary);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      return make_error(Code::Io, "write blob");
    }
  }
  return adopt_file(tmp, ext);
}

}
