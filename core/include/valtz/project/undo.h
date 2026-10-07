// UNDO AND REDO, as COMMANDS of record changes (DESIGN §5b).
//
// The journal sits under the records (db::Env::set_journal): every write
// the project makes to its record tables is noted -- each key's value
// before and after -- when its transaction commits. A COMMAND groups the
// changes of one action: a kind ("layer.add", "adjust", "generate", ...)
// with arguments for its name, the asset and layer it is about, and the
// changes, in order. Undo writes the before-values back, last first, in
// one transaction; redo writes the after-values. Neither is journaled.
//
// A command is recorded in a SCOPE (UndoLog::command): this thread's
// writes go into it until the scope ends. A scope opened while one is
// open on the same thread joins it, so an action made of others is one
// command. A job's commit joins the command it was submitted in
// (UndoLog::join) as long as that is still the last; otherwise it is a
// command of its own. Writes made in no scope are not undoable; they
// only make the project dirty.
//
// COALESCING: commands of a coalescing kind (a slider's -- adjust, crop,
// keys, markup objects) on the same asset and layer within two seconds
// are one: the first's before-values, the last's after-values.
//
// The history is stored in the working copy's "undo" table -- it survives
// a quit with unsaved changes, and a valtzctl run -- and never in a saved
// file. It keeps at most kMaxCommands; a new command clears what could be
// redone. The media the history's records name stay in the working copy
// while it holds them (held_blobs: Workspace collects the rest).

#ifndef VALTZ_PROJECT_UNDO_H
#define VALTZ_PROJECT_UNDO_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/db/lmdb.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace valtz::project {

class Project;

struct UndoCommand {
  std::uint64_t           seq = 0;     // its place in the history
  std::string             kind;        // "layer.add", "adjust", ...
  Json                    args = Json::object();  // {"name": ...}
  std::string             asset;       // what it is about ("" none)
  std::string             layer;
  std::int64_t            at_ms = 0;   // begun
  std::int64_t            last_ms = 0; // last joined or coalesced
  bool                    done = true; // false: undone, can be redone
  std::vector<db::Change> changes;
};

class UndoLog {
public:
  static constexpr std::size_t kMaxCommands = 500;
  static constexpr std::int64_t kCoalesceMs = 2000;

  // The project's history, read from its undo table; its record tables
  // journaled from now on.
  static Result<std::unique_ptr<UndoLog>> attach(Project&);
  ~UndoLog();

  // What a command's scope records into.
  class Scope {
  public:
    Scope() = default;
    Scope(Scope&&) noexcept;
    Scope& operator=(Scope&&) = delete;
    ~Scope();
    // Its place in the history once it ends (0 while nothing changed --
    // or for a joining scope, the command it joined).
    std::uint64_t seq() const noexcept { return _seq; }

  private:
    friend class UndoLog;
    UndoLog*      _log = nullptr;
    std::uint64_t _seq = 0;
    bool          _outer = false;
  };

  // A new command, `kind` with `args`, about `asset` / `layer`.
  Scope command(std::string kind, Json args = Json::object(),
                std::string asset = "", std::string layer = "");
  // `seq` again -- a job's commit joining its submission -- while it is
  // the last command; else a new one of its kind.
  Scope join(std::uint64_t seq);
  // The command this thread is recording, 0 none.
  std::uint64_t recording() const;

  // What undo and redo would do next (changes left out).
  std::optional<UndoCommand> next_undo() const;
  std::optional<UndoCommand> next_redo() const;
  // The whole history, oldest first (changes left out; `done` false:
  // could be redone).
  std::vector<UndoCommand> list() const;
  // The next command's changes written back (or forward); what it was.
  Result<UndoCommand> undo();
  Result<UndoCommand> redo();
  // The history gone (a revert).
  Status clear();

  // Where the history is now is where it was saved.
  Status mark_saved();
  // Changed in no command since the save (a migration on open): dirty.
  Status mark_untracked();
  bool at_saved() const;

  // The blobs the history's records name (by hash): kept while it holds
  // them.
  std::set<std::string> held_blobs() const;
  // The oldest command dropped (a budget kept); false with none.
  bool drop_oldest();

  // Told after anything changes the history (a command ends, an undo).
  void on_change(std::function<void()> fn) { _changed = std::move(fn); }

private:
  explicit UndoLog(Project& p) : _p(p) {}
  void journal_(std::vector<db::Change>&& changes);
  void end_(Scope&);
  Status store_(const UndoCommand&);
  Status erase_(std::uint64_t seq);
  Status store_saved_();
  Status apply_(const UndoCommand&, bool forward);
  UndoCommand* last_done_();
  const UndoCommand* last_done_() const;
  UndoCommand* first_undone_();

  Project&                 _p;
  mutable std::mutex       _mu;
  // Oldest first: done ones, then undone ones.
  std::vector<UndoCommand> _cmds;
  std::uint64_t            _next_seq = 1;
  // The last done command's seq at the save (0: none), and whether
  // something not undoable changed since.
  std::uint64_t            _saved_seq = 0;
  bool                     _untracked = false;
  // Commands being recorded, by seq (a scope's): `had` changes before
  // this scope (a command taken up again by a join), `rejoined` if so.
  struct Open {
    UndoCommand cmd;
    std::size_t had = 0;
    bool        rejoined = false;
  };
  std::map<std::uint64_t, Open> _open;
  std::function<void()>    _changed;
};

// The blobs (by hash) named in records: any {"hash", "ext"} map in them --
// a version's blob, a markup raster, a history state's picture.
void blobs_named_in(const Json& record, std::set<std::string>& out);

}

#endif
