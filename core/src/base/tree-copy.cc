#include "valtz/base/tree-copy.h"

#include <copyfile.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <format>

namespace valtz {

namespace fs = std::filesystem;

TreeSize
tree_size(const fs::path& p)
{
  TreeSize t;
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(p, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec) && !it->is_symlink(ec)) {
      t.bytes += it->file_size(ec);
      ++t.files;
    }
  }
  return t;
}

namespace {

using Clock = std::chrono::steady_clock;

struct CopyCtx {
  const std::function<bool(std::uint64_t)>* progress = nullptr;
  std::uint64_t before = 0;  // the bytes of the files done
  Clock::time_point told{};
  bool stopped = false;
};

// copyfile's status callback: its bytes so far, told at most ten times a
// second.
int
on_copy_status(int what, int stage, copyfile_state_t s, const char*,
               const char*, void* ctx)
{
  auto* c = static_cast<CopyCtx*>(ctx);
  if (what != COPYFILE_COPY_DATA || stage != COPYFILE_PROGRESS) {
    return COPYFILE_CONTINUE;
  }
  const auto now = Clock::now();
  if (now - c->told < std::chrono::milliseconds(100)) {
    return COPYFILE_CONTINUE;
  }
  c->told = now;
  off_t copied = 0;
  copyfile_state_get(s, COPYFILE_STATE_COPIED, &copied);
  if (c->progress && *c->progress &&
      !(*c->progress)(c->before + static_cast<std::uint64_t>(copied))) {
    c->stopped = true;
    return COPYFILE_QUIT;
  }
  return COPYFILE_CONTINUE;
}

}

namespace {

struct CopyState {
  copyfile_state_t s;
  CopyCtx ctx;
  explicit CopyState(const std::function<bool(std::uint64_t)>& progress)
      : s(copyfile_state_alloc())
  {
    ctx.progress = &progress;
    copyfile_state_set(s, COPYFILE_STATE_STATUS_CB,
                       reinterpret_cast<const void*>(&on_copy_status));
    copyfile_state_set(s, COPYFILE_STATE_STATUS_CTX, &ctx);
  }
  ~CopyState() { copyfile_state_free(s); }
  CopyState(const CopyState&) = delete;
  CopyState& operator=(const CopyState&) = delete;
};

// One regular file into its place, through a hidden name renamed into
// place when whole.
Status
copy_one(const fs::path& src, const fs::path& dst, std::uint64_t bytes,
         CopyState& cs, const std::function<bool(std::uint64_t)>& progress,
         std::vector<fs::path>* made)
{
  std::error_code ec, ec2;
  // There already, whole: taken as it.
  if (fs::is_regular_file(dst, ec2) && fs::file_size(dst, ec2) == bytes) {
    cs.ctx.before += bytes;
    return {};
  }
  fs::create_directories(dst.parent_path(), ec);
  const fs::path tmp = dst.parent_path() /
                       ("." + dst.filename().string() + ".valtz-moving");
  if (copyfile(src.c_str(), tmp.c_str(), cs.s,
               COPYFILE_DATA | COPYFILE_STAT) != 0) {
    const int err = errno;
    fs::remove(tmp, ec2);
    if (cs.ctx.stopped || err == ECANCELED) {
      return make_error(Code::Cancelled, "stopped");
    }
    return make_error(Code::Io, std::format("cannot copy {}: {}",
                                            src.string(),
                                            std::strerror(err)));
  }
  fs::rename(tmp, dst, ec);
  if (ec) {
    fs::remove(tmp, ec2);
    return make_error(Code::Io, std::format("cannot write {}: {}",
                                            dst.string(), ec.message()));
  }
  if (made) {
    made->push_back(dst);
  }
  cs.ctx.before += bytes;
  if (progress && !progress(cs.ctx.before)) {
    return make_error(Code::Cancelled, "stopped");
  }
  return {};
}

}

Status
copy_tree(const fs::path& from, const fs::path& to,
          const std::function<bool(std::uint64_t)>& progress,
          std::vector<fs::path>* made)
{
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot make {}: {}",
                                            to.string(), ec.message()));
  }
  CopyState cs(progress);
  for (auto it = fs::recursive_directory_iterator(from, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      return make_error(Code::Io, std::format("cannot read {}: {}",
                                              from.string(), ec.message()));
    }
    const fs::path src = it->path();
    const fs::path dst = to / src.lexically_relative(from);
    if (it->is_symlink(ec)) {
      if (!fs::exists(fs::symlink_status(dst, ec))) {
        fs::copy_symlink(src, dst, ec);
        if (!ec && made) {
          made->push_back(dst);
        }
      }
      continue;
    }
    if (it->is_directory(ec)) {
      fs::create_directories(dst, ec);
      if (ec) {
        return make_error(Code::Io, std::format("cannot make {}: {}",
                                                dst.string(),
                                                ec.message()));
      }
      continue;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    VALTZ_TRY(copy_one(src, dst, it->file_size(ec), cs, progress, made));
  }
  return {};
}

Status
copy_files(const fs::path& from, const fs::path& to,
           const std::vector<fs::path>& rels,
           const std::function<bool(std::uint64_t)>& progress,
           std::vector<fs::path>* made)
{
  CopyState cs(progress);
  std::error_code ec;
  for (const auto& rel : rels) {
    const fs::path src = from / rel;
    if (!fs::is_regular_file(src, ec)) {
      return make_error(Code::NotFound, std::format("{} is not there",
                                                    src.string()));
    }
    VALTZ_TRY(copy_one(src, to / rel, fs::file_size(src, ec), cs,
                       progress, made));
  }
  return {};
}

Status
move_tree(const fs::path& from, const fs::path& to)
{
  std::error_code ec;
  fs::create_directories(to, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot make {}: {}",
                                            to.string(), ec.message()));
  }
  std::vector<fs::path> entries;
  for (const auto& e : fs::directory_iterator(from, ec)) {
    entries.push_back(e.path());
  }
  if (ec) {
    return make_error(Code::Io, std::format("cannot read {}: {}",
                                            from.string(), ec.message()));
  }
  for (const auto& src : entries) {
    const fs::path dst = to / src.filename();
    const auto st = fs::symlink_status(src, ec);
    const auto dt = fs::symlink_status(dst, ec);
    if (fs::is_directory(st) && fs::is_directory(dt)) {
      VALTZ_TRY(move_tree(src, dst));
      fs::remove(src, ec);
      continue;
    }
    // The same file there already: the one moved goes.
    if (fs::is_regular_file(st) && fs::is_regular_file(dt) &&
        fs::file_size(src, ec) == fs::file_size(dst, ec)) {
      fs::remove(src, ec);
      continue;
    }
    if (fs::is_directory(dt)) {
      return make_error(Code::AlreadyExists,
                        std::format("{} is in the way", dst.string()));
    }
    fs::rename(src, dst, ec);
    if (ec) {
      return make_error(Code::Io, std::format("cannot move {}: {}",
                                              src.string(), ec.message()));
    }
  }
  return {};
}

void
remove_empty_dirs(const fs::path& p)
{
  std::error_code ec;
  if (!fs::is_directory(fs::symlink_status(p, ec))) {
    return;
  }
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(p, ec)) {
    if (fs::is_directory(fs::symlink_status(e.path(), ec))) {
      dirs.push_back(e.path());
    }
  }
  for (const auto& d : dirs) {
    remove_empty_dirs(d);
  }
  // A Finder's .DS_Store alone does not keep a folder.
  int left = 0;
  bool ds = false;
  for (const auto& e : fs::directory_iterator(p, ec)) {
    ++left;
    ds = ds || e.path().filename() == ".DS_Store";
  }
  if (left == 1 && ds) {
    fs::remove(p / ".DS_Store", ec);
  }
  if (fs::is_empty(p, ec)) {
    fs::remove(p, ec);
  }
}

}
