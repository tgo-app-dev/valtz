// A project's WORKING COPY -- the active project cache (DESIGN §5b). A
// project is never edited in its package: opening one reads its saved
// records into a working copy, every change goes there, Save writes it
// back, Revert to Saved reads the save again.
//
//   <root>/<uuid>/          (an untitled one's: wherever it is given)
//     project.lmdb          the records, edited in place; the undo history
//     blobs/                media made since the last save
//     tmp/
//     origin.json           {"package", "saved": hash of its project.cbor}
//
// Only the records are copied: blobs are immutable and content-addressed,
// so the working copy reads its package's blobs where they are (a read
// root) and writes new ones into its own. A multi-GB project opens in the
// time it takes to read its records.
//
// SAVE moves the new media the records hold into the package, writes
// project.cbor atomically, then collects the package's garbage: a blob no
// saved record holds is removed -- or, while the undo history holds it,
// moved into the working copy. REVERT reads project.cbor back over the
// records, drops the media made since and clears the history. DIRTY is
// "not where it was saved" (UndoLog::at_saved): undoing back to the save
// point makes it clean.
//
// RECOVERY: closed clean, the working copy goes. Left dirty (a quit
// without saving, a crash), it stays, and the next open of the package
// resumes it -- unsaved changes and history -- as long as the package is
// still the save it was made from; else it is made again from the
// package.

#ifndef VALTZ_PROJECT_WORKSPACE_H
#define VALTZ_PROJECT_WORKSPACE_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/project/project.h"
#include "valtz/project/undo.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace valtz::project {

struct WorkspaceReport {
  bool recovered = false;  // resumed, with changes not saved
  bool legacy = false;     // a format-1 package (project.lmdb)
  // From before the document model (DESIGN §6a): brought over, dirty
  // until saved.
  bool migrated = false;
  // The extensions the saved file says it holds data of.
  Json extensions = Json::array();
};

class Workspace {
public:
  // A saved project: its working copy under `root` -- resumed, or made
  // from the package.
  static Result<std::unique_ptr<Workspace>>
  open(const std::filesystem::path& package,
       const std::filesystem::path& root, WorkspaceReport* report = nullptr);
  // A new project, saved at `package` at once.
  static Result<std::unique_ptr<Workspace>>
  create(const std::filesystem::path& package, std::string name,
         const std::filesystem::path& root);
  // An UNTITLED project (the anonymous session): a working copy at `dir`
  // and no package; Save is Save As.
  static Result<std::unique_ptr<Workspace>>
  create_untitled(const std::filesystem::path& dir, std::string name);

  ~Workspace();
  Workspace(const Workspace&) = delete;
  Workspace& operator=(const Workspace&) = delete;

  Project& project() noexcept { return *_project; }
  UndoLog& undo() noexcept { return *_undo; }
  const std::optional<std::filesystem::path>& package() const noexcept
  {
    return _package;
  }
  const std::filesystem::path& dir() const noexcept { return _dir; }
  bool untitled() const noexcept { return !_package; }
  bool dirty() const;

  // Written to its package. `extensions`: those whose models or data the
  // records hold ({"id", "version", "name"}).
  Status save(const Json& extensions = Json::array());
  // Written at `package` (replacing what is there), named after it, and
  // from then on its package.
  Status save_as(const std::filesystem::path& package,
                 const Json& extensions = Json::array());
  // Back to the save; the history cleared. Refused when untitled.
  Status revert();
  // The working copy: kept when dirty, unless `discard`; else gone.
  Status close(bool discard);
  // The working copy's media that nothing holds -- no record, no command
  // of the history -- gone. Past `budget` bytes held by the history alone,
  // its oldest commands go first.
  Status collect_garbage(std::uint64_t budget);

private:
  Workspace() = default;
  Status attach_(bool fresh);
  Status write_origin_();
  Status move_held_into_package_();

  std::filesystem::path                _dir;
  std::optional<std::filesystem::path> _package;
  std::unique_ptr<Project>             _project;
  std::unique_ptr<UndoLog>             _undo;
};

}

#endif
