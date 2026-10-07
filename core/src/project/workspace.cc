#include "valtz/project/workspace.h"

#include "valtz/base/log.h"
#include "valtz/project/project-file.h"

#include <sys/clonefile.h>

#include <format>
#include <fstream>
#include <iterator>

namespace valtz::project {

namespace fs = std::filesystem;

namespace {

constexpr const char* kOrigin = "origin.json";

fs::path
canonical_of(const fs::path& p)
{
  std::error_code ec;
  fs::path c = fs::weakly_canonical(fs::absolute(p, ec), ec);
  return ec ? p : c;
}

// What a package's save is, by content: its project.cbor -- or, a
// format-1 package, its database.
Result<std::string>
saved_hash(const fs::path& package, bool* legacy)
{
  std::error_code ec;
  if (fs::is_regular_file(package / kProjectFile, ec)) {
    *legacy = false;
    return project_file_hash(package / kProjectFile);
  }
  if (fs::is_regular_file(package / kLegacyDatabase, ec)) {
    *legacy = true;
    return project_file_hash(package / kLegacyDatabase);
  }
  return make_error(Code::NotFound, std::format("{} is not a Valtz project",
                                                package.string()));
}

Json
read_json(const fs::path& file)
{
  std::ifstream in(file);
  if (!in) {
    return Json::object();
  }
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  Json j = Json::parse(text, nullptr, /*allow_exceptions=*/false);
  return j.is_discarded() ? Json::object() : j;
}

// Every blob file under a blobs root: its hash (the file's stem) and path.
template <class Fn>
void
for_each_blob(const fs::path& root, Fn&& fn)
{
  std::error_code ec;
  if (!fs::is_directory(root, ec)) {
    return;
  }
  for (auto it = fs::recursive_directory_iterator(root, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (it->is_regular_file(ec)) {
      fn(it->path().stem().string(), it->path());
    }
  }
}

// `src` to `dst`: renamed on one volume, else copied (cloned where it
// can be) and the source removed.
Status
move_file(const fs::path& src, const fs::path& dst)
{
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  fs::rename(src, dst, ec);
  if (!ec) {
    return ok_status();
  }
  ec.clear();
  if (clonefile(src.c_str(), dst.c_str(), CLONE_NOFOLLOW) != 0) {
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      return make_error(Code::Io, std::format("cannot move {}: {}",
                                              src.string(), ec.message()));
    }
  }
  fs::remove(src, ec);
  return ok_status();
}

Status
clone_into(const fs::path& src, const fs::path& dst)
{
  std::error_code ec;
  fs::create_directories(dst.parent_path(), ec);
  if (fs::exists(dst, ec)) {
    return ok_status();
  }
  if (clonefile(src.c_str(), dst.c_str(), CLONE_NOFOLLOW) == 0) {
    return ok_status();
  }
  fs::copy_file(src, dst, ec);
  return ec ? make_error(Code::Io, std::format("cannot copy {}: {}",
                                               src.string(), ec.message()))
            : ok_status();
}

// The records of a format-1 package, as the save format holds them.
Result<Json>
legacy_records(const fs::path& package)
{
  VALTZ_ASSIGN(auto old, Project::open(package));
  return old->dump();
}

}

Workspace::~Workspace()
{
  _undo.reset();
  _project.reset();
}

Status
Workspace::attach_(bool fresh)
{
  _project->keep_blobs(true);
  if (_package) {
    _project->blobs().add_read_root(*_package / "blobs");
  }
  VALTZ_ASSIGN(_undo, UndoLog::attach(*_project));
  if (fresh) {
    VALTZ_TRY(_undo->clear());
    VALTZ_TRY(_undo->mark_saved());
  }
  return ok_status();
}

Status
Workspace::write_origin_()
{
  if (!_package) {
    return ok_status();
  }
  bool legacy = false;
  VALTZ_ASSIGN(const std::string hash, saved_hash(*_package, &legacy));
  const Json origin = {{"package", _package->string()}, {"saved", hash}};
  std::ofstream out(_dir / kOrigin, std::ios::trunc);
  out << to_text(origin);
  return out ? ok_status()
             : make_error(Code::Io, "cannot write the working copy's origin");
}

Result<std::unique_ptr<Workspace>>
Workspace::open(const fs::path& package_in, const fs::path& root,
                WorkspaceReport* report)
{
  const fs::path package = canonical_of(package_in);
  bool legacy = false;
  VALTZ_ASSIGN(const std::string hash, saved_hash(package, &legacy));
  WorkspaceReport r;
  r.legacy = legacy;
  std::error_code ec;
  fs::create_directories(root, ec);

  std::unique_ptr<Workspace> w(new Workspace());
  w->_package = package;
  // A working copy of this package left before: resumed when it is still
  // of this save; else it is of another, and goes.
  std::vector<fs::path> dirs;
  for (const auto& e : fs::directory_iterator(root, ec)) {
    dirs.push_back(e.path());
  }
  for (const auto& d : dirs) {
    const Json o = read_json(d / kOrigin);
    if (jget<std::string>(o, "package", "") != package.string()) {
      continue;
    }
    if (jget<std::string>(o, "saved", "") == hash &&
        fs::is_regular_file(d / kLegacyDatabase, ec) && w->_dir.empty()) {
      w->_dir = d;
      continue;
    }
    VALTZ_LOG_INFO("workspace", "dropping a stale working copy of {}",
                   package.string());
    fs::remove_all(d, ec);
  }
  if (!w->_dir.empty()) {
    auto p = Project::open(w->_dir);
    if (p.ok()) {
      w->_project = std::move(*p);
      // A working copy left from before the document model: its records
      // brought over (migrate.h).
      if (auto sch = w->_project->schema(); sch.ok() &&
          *sch < kSchemaVersion) {
        VALTZ_ASSIGN(Json doc, w->_project->dump());
        VALTZ_TRY(w->_project->load(doc));
      }
      VALTZ_TRY(w->attach_(false));
      if (w->_project->migrated()) {
        VALTZ_TRY(w->_undo->mark_untracked());
        r.migrated = true;
      }
      r.recovered = w->dirty();
      if (!legacy) {
        if (auto doc = read_project_file(package / kProjectFile); doc.ok()) {
          r.extensions = jget(*doc, "extensions", Json::array());
        }
      }
      if (report) {
        *report = r;
      }
      return w;
    }
    // Unreadable: made again.
    fs::remove_all(w->_dir, ec);
  }

  // Made from the package: its records, read in.
  w->_dir = root / Uuid::v7().str();
  VALTZ_ASSIGN(w->_project,
               Project::create(w->_dir, package.stem().string()));
  if (legacy) {
    VALTZ_ASSIGN(Json doc, legacy_records(package));
    VALTZ_TRY(w->_project->load(doc));
  } else {
    VALTZ_ASSIGN(Json doc, read_project_file(package / kProjectFile));
    VALTZ_TRY(read_project_document(*w->_project, doc));
    r.extensions = jget(doc, "extensions", Json::array());
  }
  VALTZ_TRY(w->attach_(true));
  VALTZ_TRY(w->write_origin_());
  if (w->_project->migrated()) {
    VALTZ_TRY(w->_undo->mark_untracked());
    r.migrated = true;
  }
  if (report) {
    *report = r;
  }
  return w;
}

Result<std::unique_ptr<Workspace>>
Workspace::create(const fs::path& package_in, std::string name,
                  const fs::path& root)
{
  const fs::path package = canonical_of(package_in);
  VALTZ_TRY(check_local_volume(package));
  std::error_code ec;
  if (fs::exists(package / kProjectFile, ec) ||
      fs::exists(package / kLegacyDatabase, ec)) {
    return make_error(Code::AlreadyExists,
                      std::format("{} already exists", package.string()));
  }
  fs::create_directories(package / "blobs", ec);
  if (ec) {
    return make_error(Code::Io, std::format("create {}: {}",
                                            package.string(), ec.message()));
  }
  fs::create_directories(root, ec);
  std::unique_ptr<Workspace> w(new Workspace());
  w->_dir = root / Uuid::v7().str();
  w->_package = package;
  VALTZ_ASSIGN(w->_project, Project::create(w->_dir, std::move(name)));
  VALTZ_TRY(w->attach_(true));
  VALTZ_TRY(w->save());
  return w;
}

Result<std::unique_ptr<Workspace>>
Workspace::create_untitled(const fs::path& dir, std::string name)
{
  std::unique_ptr<Workspace> w(new Workspace());
  w->_dir = dir;
  VALTZ_ASSIGN(w->_project, Project::create(dir, std::move(name)));
  VALTZ_TRY(w->attach_(true));
  return w;
}

bool
Workspace::dirty() const
{
  return !_undo->at_saved();
}

Status
Workspace::move_held_into_package_()
{
  VALTZ_ASSIGN(const auto held, _project->held_blobs());
  const fs::path to = *_package / "blobs";
  Status st = ok_status();
  for_each_blob(_project->blobs().root(),
                [&](const std::string& hash, const fs::path& file) {
    if (!st.ok() || !held.contains(hash)) {
      return;
    }
    const fs::path dst = to / file.parent_path().filename() /
                         file.filename();
    std::error_code ec;
    if (fs::exists(dst, ec)) {
      fs::remove(file, ec);
      return;
    }
    st = move_file(file, dst);
  });
  return st;
}

Status
Workspace::save(const Json& extensions)
{
  if (!_package) {
    return make_error(Code::InvalidArgument,
                      "an untitled project is saved with Save As");
  }
  std::error_code ec;
  fs::create_directories(*_package / "blobs", ec);
  // The media first: a crash then leaves the old save, or the new one.
  VALTZ_TRY(move_held_into_package_());
  VALTZ_ASSIGN(Json doc, project_document(*_project, extensions));
  VALTZ_TRY(write_project_file(*_package / kProjectFile, doc));
  // A format-1 package is format 2 from now on.
  fs::remove(*_package / kLegacyDatabase, ec);
  fs::remove(fs::path(*_package / kLegacyDatabase).string() + "-lock", ec);
  // The package's garbage: what no saved record holds goes -- into the
  // working copy while the history holds it.
  VALTZ_ASSIGN(const auto held, _project->held_blobs());
  const auto history = _undo->held_blobs();
  for_each_blob(*_package / "blobs",
                [&](const std::string& hash, const fs::path& file) {
    if (held.contains(hash)) {
      return;
    }
    std::error_code e2;
    if (history.contains(hash)) {
      (void)move_file(file, _project->blobs().root() /
                                file.parent_path().filename() /
                                file.filename());
    } else {
      fs::remove(file, e2);
    }
  });
  VALTZ_TRY(write_origin_());
  return _undo->mark_saved();
}

Status
Workspace::save_as(const fs::path& package_in, const Json& extensions)
{
  const fs::path package = canonical_of(package_in);
  if (_package && package == *_package) {
    return save(extensions);
  }
  VALTZ_TRY(check_local_volume(package));
  std::error_code ec;
  fs::remove_all(package, ec);
  fs::create_directories(package / "blobs", ec);
  if (ec) {
    return make_error(Code::Io, std::format("create {}: {}",
                                            package.string(), ec.message()));
  }
  // Every medium the records hold, from wherever it is now.
  VALTZ_ASSIGN(const auto held, _project->held_blobs());
  std::vector<fs::path> roots = _project->blobs().read_roots();
  roots.insert(roots.begin(), _project->blobs().root());
  Status st = ok_status();
  for (const auto& root : roots) {
    for_each_blob(root, [&](const std::string& hash, const fs::path& file) {
      if (st.ok() && held.contains(hash)) {
        st = clone_into(file, package / "blobs" /
                                 file.parent_path().filename() /
                                 file.filename());
      }
    });
  }
  VALTZ_TRY(st);
  // Named after its package; the name is not a change anyone undoes.
  {
    db::Env::Unjournaled quiet;
    VALTZ_TRY(_project->set_name(package.stem().string()));
  }
  VALTZ_ASSIGN(Json doc, project_document(*_project, extensions));
  VALTZ_TRY(write_project_file(package / kProjectFile, doc));
  // From now on, that package: its blobs read where they are.
  _package = package;
  _project->blobs().clear_read_roots();
  _project->blobs().add_read_root(package / "blobs");
  for_each_blob(_project->blobs().root(),
                [&](const std::string& hash, const fs::path& file) {
    if (held.contains(hash)) {
      std::error_code e2;
      fs::remove(file, e2);
    }
  });
  VALTZ_TRY(write_origin_());
  return _undo->mark_saved();
}

Status
Workspace::revert()
{
  if (!_package) {
    return make_error(Code::InvalidArgument,
                      "an untitled project has no save to go back to");
  }
  bool legacy = false;
  VALTZ_ASSIGN(const std::string hash, saved_hash(*_package, &legacy));
  (void)hash;
  if (legacy) {
    VALTZ_ASSIGN(Json doc, legacy_records(*_package));
    VALTZ_TRY(_project->load(doc));
  } else {
    VALTZ_ASSIGN(Json doc, read_project_file(*_package / kProjectFile));
    VALTZ_TRY(read_project_document(*_project, doc));
  }
  VALTZ_TRY(_undo->clear());
  // The media made since the save: nothing holds them now.
  std::error_code ec;
  fs::remove_all(_project->blobs().root(), ec);
  fs::create_directories(_project->blobs().root(), ec);
  VALTZ_TRY(write_origin_());
  VALTZ_TRY(_undo->mark_saved());
  if (_project->migrated()) {
    VALTZ_TRY(_undo->mark_untracked());
  }
  return ok_status();
}

Status
Workspace::close(bool discard)
{
  const bool keep = dirty() && !discard;
  _undo.reset();
  _project.reset();
  if (keep) {
    return ok_status();
  }
  std::error_code ec;
  fs::remove_all(_dir, ec);
  return ok_status();
}

Status
Workspace::collect_garbage(std::uint64_t budget)
{
  VALTZ_ASSIGN(const auto held, _project->held_blobs());
  for (;;) {
    const auto history = _undo->held_blobs();
    std::uint64_t history_only = 0;
    for_each_blob(_project->blobs().root(),
                  [&](const std::string& hash, const fs::path& file) {
      if (!held.contains(hash) && history.contains(hash)) {
        std::error_code ec;
        history_only += fs::file_size(file, ec);
      }
    });
    if (history_only > budget && _undo->drop_oldest()) {
      continue;
    }
    for_each_blob(_project->blobs().root(),
                  [&](const std::string& hash, const fs::path& file) {
      if (!held.contains(hash) && !history.contains(hash)) {
        std::error_code ec;
        fs::remove(file, ec);
      }
    });
    return ok_status();
  }
}

}
