// A thin RAII layer over LMDB.
//
// One Env per project file. The env is opened with MDB_NOTLS so a read
// transaction is not bound to the thread that began it (Swift concurrency
// and the controller's worker pool hop threads), and with a map size that
// GROWS: LMDB needs the map reserved up front, and a MDB_MAP_FULL on a
// user's project must not be a hard failure. Env::write() retries a
// transaction once after doubling the map.
//
// LMDB allows one write transaction per env at a time; that is the
// serialization point for mutations, and readers never block on it.
//
// Keys and values are byte spans. Record encoding lives one level up
// (valtz/project/codec.h); this layer knows nothing about it.
//
// A JOURNAL (Env::set_journal) is told what each write transaction
// changed -- every key's value before and after, in the sub-databases it
// watches -- when the transaction commits; the attempts of a retried
// transaction are dropped. It is what undo is made of (project/undo.h):
// nothing above has to remember to record what it wrote.

#ifndef VALTZ_DB_LMDB_H
#define VALTZ_DB_LMDB_H

#include "valtz/base/result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct MDB_env;
struct MDB_txn;
struct MDB_cursor;

namespace valtz::db {

using Bytes = std::span<const std::uint8_t>;

inline Bytes
as_bytes(std::string_view s)
{
  return {reinterpret_cast<const std::uint8_t*>(s.data()), s.size()};
}

inline std::string_view
as_view(Bytes b)
{
  return {reinterpret_cast<const char*>(b.data()), b.size()};
}

// A named sub-database. The handle is an index into the env's table and
// is valid for the life of the Env once opened.
struct Dbi {
  unsigned handle = 0;
  bool     dupsort = false;
};

class Txn;

// One key's change in a write transaction. For a DUPSORT sub-database a
// change is one key/value PAIR coming (after) or going (before).
struct Change {
  unsigned                                 dbi = 0;
  std::vector<std::uint8_t>                key;
  std::optional<std::vector<std::uint8_t>> before;
  std::optional<std::vector<std::uint8_t>> after;
  bool                                     dup = false;
};

using Journal = std::function<void(std::vector<Change>&&)>;

class Env {
public:
  struct Options {
    std::size_t initial_map_bytes = std::size_t{256} << 20;
    std::size_t max_map_bytes = std::size_t{64} << 30;
    unsigned    max_dbs = 32;
    unsigned    max_readers = 126;
    bool        read_only = false;
  };

  // `path` is the database FILE (MDB_NOSUBDIR); LMDB creates
  // `<path>-lock` beside it.
  static Result<std::unique_ptr<Env>>
  open(const std::filesystem::path& path, const Options& opts);
  static Result<std::unique_ptr<Env>>
  open(const std::filesystem::path& path)
  {
    return open(path, Options{});
  }

  ~Env();
  Env(const Env&) = delete;
  Env& operator=(const Env&) = delete;

  // Open (creating when `create`) a named sub-database. Call during
  // setup, before concurrent use.
  Result<Dbi> open_dbi(const char* name, bool dupsort = false,
                       bool create = true);

  // Run `fn` in a write transaction and commit. If the map fills, the
  // transaction is aborted, the map is doubled (up to max_map_bytes) and
  // `fn` runs again from the start -- so `fn` must be re-runnable (build
  // its writes from its inputs, not from state it mutates).
  Status write(const std::function<Status(Txn&)>& fn);

  // Run `fn` in a read-only snapshot.
  Status read(const std::function<Status(Txn&)>& fn) const;

  std::size_t map_bytes() const noexcept { return _map_bytes; }
  const std::filesystem::path& path() const noexcept { return _path; }
  MDB_env* raw() const noexcept { return _env; }

  // What write transactions change in `watched`, told when each commits
  // (on the committing thread). Set during setup; none: no journal.
  void set_journal(Journal journal, std::vector<Dbi> watched);
  // While one lives, this thread's writes go unjournaled (an undo
  // writing back what was, a project read in whole).
  class Unjournaled {
  public:
    Unjournaled();
    ~Unjournaled();
    Unjournaled(const Unjournaled&) = delete;
    Unjournaled& operator=(const Unjournaled&) = delete;
  };

private:
  Env() = default;
  Status grow_map_();

  Journal               _journal;
  std::vector<bool>     _watched;  // by dbi handle

  MDB_env*              _env = nullptr;
  std::filesystem::path _path;
  Options               _opts;
  std::size_t           _map_bytes = 0;
  // Serializes map growth against new transactions: mdb_env_set_mapsize
  // requires that this process has no transaction open.
  mutable std::mutex    _grow_mu;
  mutable std::size_t   _active_txns = 0;
};

class Cursor;

class Txn {
public:
  ~Txn();
  Txn(const Txn&) = delete;
  Txn& operator=(const Txn&) = delete;

  bool read_only() const noexcept { return _read_only; }

  // The returned span points into the memory map and is valid until the
  // transaction ends. Copy out anything that must outlive it.
  Result<std::optional<Bytes>> get(Dbi, Bytes key) const;

  // No-overwrite put when `unique`; AlreadyExists if the key is present.
  Status put(Dbi, Bytes key, Bytes value, bool unique = false);
  // Delete a key (all duplicates for a dupsort dbi when `value` empty).
  Status del(Dbi, Bytes key, Bytes value = {});
  // Every key of `dbi` gone (the sub-database stays).
  Status clear(Dbi);

  Result<Cursor> cursor(Dbi) const;

  MDB_txn* raw() const noexcept { return _txn; }

private:
  friend class Env;
  Txn(MDB_txn* t, bool ro) : _txn(t), _read_only(ro) {}
  Status commit_();
  void abort_();
  // The journal's: is `dbi` watched, and note a change.
  bool journaled_(Dbi) const;
  void note_(Dbi, Bytes key, std::optional<Bytes> before,
             std::optional<Bytes> after, bool dup);
  Result<bool> has_pair_(Dbi, Bytes key, Bytes value) const;

  MDB_txn* _txn = nullptr;
  bool     _read_only = true;
  // Set by Env::write while a journal listens.
  const std::vector<bool>* _watched = nullptr;
  std::vector<Change>*     _changes = nullptr;
};

class Cursor {
public:
  Cursor(Cursor&&) noexcept;
  Cursor& operator=(Cursor&&) noexcept;
  ~Cursor();

  struct Entry {
    Bytes key;
    Bytes value;
  };

  // Position at the first key >= `key` (all keys when empty).
  std::optional<Entry> seek(Bytes key = {});
  std::optional<Entry> next();
  // For dupsort dbis: position at `key` and walk its duplicates.
  std::optional<Entry> seek_exact(Bytes key);
  std::optional<Entry> next_dup();

private:
  friend class Txn;
  explicit Cursor(MDB_cursor* c) : _c(c) {}
  std::optional<Entry> op_(int op, Bytes key);

  MDB_cursor* _c = nullptr;
};

// Iterate every key in `dbi` that starts with `prefix`.
Status for_each_prefix(const Txn&, Dbi, Bytes prefix,
                       const std::function<bool(const Cursor::Entry&)>& fn);

}

#endif
