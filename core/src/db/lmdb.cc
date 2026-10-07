#include "valtz/db/lmdb.h"

#include <lmdb.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <thread>

namespace valtz::db {

namespace {

Error
lmdb_error(int rc, std::string_view what)
{
  Code code = Code::Io;
  switch (rc) {
  case MDB_NOTFOUND:
    code = Code::NotFound;
    break;
  case MDB_KEYEXIST:
    code = Code::AlreadyExists;
    break;
  case MDB_CORRUPTED:
  case MDB_PAGE_NOTFOUND:
  case MDB_INVALID:
  case MDB_VERSION_MISMATCH:
    code = Code::Corrupt;
    break;
  case MDB_MAP_FULL:
  case MDB_READERS_FULL:
  case MDB_TXN_FULL:
    code = Code::Busy;
    break;
  default:
    break;
  }
  return make_error(code, std::format("lmdb: {}: {}", what,
                                      mdb_strerror(rc)));
}

MDB_val
to_val(Bytes b)
{
  MDB_val v;
  v.mv_size = b.size();
  v.mv_data = const_cast<std::uint8_t*>(b.data());
  return v;
}

Bytes
from_val(const MDB_val& v)
{
  return {static_cast<const std::uint8_t*>(v.mv_data), v.mv_size};
}

// Counts open transactions so map growth can wait for zero.
struct TxnGuard {
  std::mutex&  mu;
  std::size_t& n;
  TxnGuard(std::mutex& m, std::size_t& c) : mu(m), n(c)
  {
    std::lock_guard lk(mu);
    ++n;
  }
  ~TxnGuard()
  {
    std::lock_guard lk(mu);
    --n;
  }
};

}

Result<std::unique_ptr<Env>>
Env::open(const std::filesystem::path& path, const Options& opts)
{
  std::unique_ptr<Env> env(new Env());
  env->_path = path;
  env->_opts = opts;
  env->_map_bytes = opts.initial_map_bytes;

  if (int rc = mdb_env_create(&env->_env); rc != 0) {
    return lmdb_error(rc, "env_create");
  }
  mdb_env_set_maxdbs(env->_env, opts.max_dbs);
  mdb_env_set_maxreaders(env->_env, opts.max_readers);

  // An existing file larger than the initial map keeps its size: LMDB
  // adopts the larger of the requested map and the file's own.
  std::error_code ec;
  if (auto sz = std::filesystem::file_size(path, ec); !ec) {
    env->_map_bytes = std::max<std::size_t>(env->_map_bytes, sz * 2);
  }
  mdb_env_set_mapsize(env->_env, env->_map_bytes);

  unsigned flags = MDB_NOSUBDIR | MDB_NOTLS;
  if (opts.read_only) {
    flags |= MDB_RDONLY;
  }
  if (int rc = mdb_env_open(env->_env, path.c_str(), flags, 0644);
      rc != 0) {
    mdb_env_close(env->_env);
    env->_env = nullptr;
    return lmdb_error(rc, std::format("open {}", path.string()));
  }

  // Clear stale reader slots left by a crashed process.
  int dead = 0;
  mdb_reader_check(env->_env, &dead);

  MDB_envinfo info;
  if (mdb_env_info(env->_env, &info) == 0) {
    env->_map_bytes = info.me_mapsize;
  }
  return env;
}

Env::~Env()
{
  if (_env) {
    mdb_env_close(_env);
  }
}

Result<Dbi>
Env::open_dbi(const char* name, bool dupsort, bool create)
{
  MDB_txn* txn = nullptr;
  unsigned tflags = _opts.read_only ? MDB_RDONLY : 0;
  if (int rc = mdb_txn_begin(_env, nullptr, tflags, &txn); rc != 0) {
    return lmdb_error(rc, "txn_begin (open_dbi)");
  }
  unsigned flags = 0;
  if (dupsort) {
    flags |= MDB_DUPSORT;
  }
  if (create && !_opts.read_only) {
    flags |= MDB_CREATE;
  }
  MDB_dbi dbi = 0;
  if (int rc = mdb_dbi_open(txn, name, flags, &dbi); rc != 0) {
    mdb_txn_abort(txn);
    return lmdb_error(rc, std::format("dbi_open {}", name));
  }
  if (int rc = mdb_txn_commit(txn); rc != 0) {
    return lmdb_error(rc, "commit (open_dbi)");
  }
  return Dbi{dbi, dupsort};
}

Status
Env::grow_map_()
{
  std::size_t next = std::min(_map_bytes * 2, _opts.max_map_bytes);
  if (next <= _map_bytes) {
    return make_error(Code::Busy, std::format(
        "project database is full ({} MB map limit)", _map_bytes >> 20));
  }
  // mdb_env_set_mapsize needs no transaction open in this process. A
  // write is not open (we just aborted it); readers are short-lived, so
  // wait them out.
  for (;;) {
    std::unique_lock lk(_grow_mu);
    if (_active_txns == 0) {
      if (int rc = mdb_env_set_mapsize(_env, next); rc != 0) {
        return lmdb_error(rc, "set_mapsize");
      }
      _map_bytes = next;
      return ok_status();
    }
    lk.unlock();
    std::this_thread::yield();
  }
}

namespace {

// Unjournaled scopes open on this thread.
thread_local int t_unjournaled = 0;

std::vector<std::uint8_t>
copy_of(Bytes b)
{
  return {b.begin(), b.end()};
}

}

Env::Unjournaled::Unjournaled() { ++t_unjournaled; }
Env::Unjournaled::~Unjournaled() { --t_unjournaled; }

void
Env::set_journal(Journal journal, std::vector<Dbi> watched)
{
  _journal = std::move(journal);
  _watched.assign(_opts.max_dbs + 2, false);
  for (const Dbi d : watched) {
    if (d.handle < _watched.size()) {
      _watched[d.handle] = true;
    }
  }
}

Status
Env::write(const std::function<Status(Txn&)>& fn)
{
  if (_opts.read_only) {
    return make_error(Code::InvalidArgument, "database is read-only");
  }
  for (int attempt = 0; attempt < 8; ++attempt) {
    Status st;
    bool map_full = false;
    {
      TxnGuard g(_grow_mu, _active_txns);
      MDB_txn* raw = nullptr;
      if (int rc = mdb_txn_begin(_env, nullptr, 0, &raw); rc != 0) {
        return lmdb_error(rc, "txn_begin");
      }
      Txn txn(raw, false);
      std::vector<Change> changes;
      const bool journaled = _journal && t_unjournaled == 0;
      if (journaled) {
        txn._watched = &_watched;
        txn._changes = &changes;
      }
      st = fn(txn);
      if (st.ok()) {
        st = txn.commit_();
        if (st.ok() && journaled && !changes.empty()) {
          _journal(std::move(changes));
        }
      }
      map_full = !st.ok() && st.code() == Code::Busy &&
                 st.error().message.find("MDB_MAP_FULL") !=
                     std::string::npos;
      // ~Txn aborts if commit did not run or failed.
    }
    if (!map_full) {
      return st;
    }
    VALTZ_TRY(grow_map_());
  }
  return make_error(Code::Busy, "project database could not grow");
}

Status
Env::read(const std::function<Status(Txn&)>& fn) const
{
  TxnGuard g(_grow_mu, _active_txns);
  MDB_txn* raw = nullptr;
  if (int rc = mdb_txn_begin(_env, nullptr, MDB_RDONLY, &raw); rc != 0) {
    return lmdb_error(rc, "txn_begin (read)");
  }
  Txn txn(raw, true);
  return fn(txn);
}

Txn::~Txn()
{
  abort_();
}

void
Txn::abort_()
{
  if (_txn) {
    mdb_txn_abort(_txn);
    _txn = nullptr;
  }
}

Status
Txn::commit_()
{
  int rc = mdb_txn_commit(_txn);
  _txn = nullptr;  // committed or freed by LMDB either way
  if (rc != 0) {
    return lmdb_error(rc, "commit");
  }
  return ok_status();
}

Result<std::optional<Bytes>>
Txn::get(Dbi dbi, Bytes key) const
{
  MDB_val k = to_val(key);
  MDB_val v;
  int rc = mdb_get(_txn, dbi.handle, &k, &v);
  if (rc == MDB_NOTFOUND) {
    return std::optional<Bytes>{};
  }
  if (rc != 0) {
    return lmdb_error(rc, "get");
  }
  return std::optional<Bytes>{from_val(v)};
}

bool
Txn::journaled_(Dbi dbi) const
{
  return _changes && _watched && dbi.handle < _watched->size() &&
         (*_watched)[dbi.handle];
}

void
Txn::note_(Dbi dbi, Bytes key, std::optional<Bytes> before,
           std::optional<Bytes> after, bool dup)
{
  Change c;
  c.dbi = dbi.handle;
  c.key = copy_of(key);
  if (before) { c.before = copy_of(*before); }
  if (after) { c.after = copy_of(*after); }
  c.dup = dup;
  _changes->push_back(std::move(c));
}

Result<bool>
Txn::has_pair_(Dbi dbi, Bytes key, Bytes value) const
{
  MDB_cursor* c = nullptr;
  if (int rc = mdb_cursor_open(_txn, dbi.handle, &c); rc != 0) {
    return lmdb_error(rc, "cursor_open");
  }
  MDB_val k = to_val(key);
  MDB_val v = to_val(value);
  const int rc = mdb_cursor_get(c, &k, &v, MDB_GET_BOTH);
  mdb_cursor_close(c);
  if (rc != 0 && rc != MDB_NOTFOUND) {
    return lmdb_error(rc, "get_both");
  }
  return rc == 0;
}

Status
Txn::put(Dbi dbi, Bytes key, Bytes value, bool unique)
{
  // What was there, for the journal: copied before the write moves it.
  const bool journaled = journaled_(dbi);
  std::optional<std::vector<std::uint8_t>> before;
  bool fresh_pair = false;
  if (journaled) {
    if (dbi.dupsort) {
      VALTZ_ASSIGN(const bool had, has_pair_(dbi, key, value));
      fresh_pair = !had;
    } else {
      VALTZ_ASSIGN(auto b, get(dbi, key));
      if (b) {
        before = copy_of(*b);
      }
    }
  }
  MDB_val k = to_val(key);
  MDB_val v = to_val(value);
  unsigned flags = 0;
  if (unique) {
    flags |= dbi.dupsort ? MDB_NODUPDATA : MDB_NOOVERWRITE;
  }
  if (int rc = mdb_put(_txn, dbi.handle, &k, &v, flags); rc != 0) {
    return lmdb_error(rc, "put");
  }
  if (journaled) {
    if (dbi.dupsort) {
      if (fresh_pair) {
        note_(dbi, key, std::nullopt, value, true);
      }
    } else if (!before ||
               !std::equal(before->begin(), before->end(), value.begin(),
                           value.end())) {
      note_(dbi, key,
            before ? std::optional<Bytes>(Bytes(*before)) : std::nullopt,
            value, false);
    }
  }
  return ok_status();
}

Status
Txn::del(Dbi dbi, Bytes key, Bytes value)
{
  // What goes, for the journal: a record, a pair, or every pair of a key.
  std::vector<std::vector<std::uint8_t>> gone;
  const bool journaled = journaled_(dbi);
  if (journaled) {
    if (!dbi.dupsort) {
      VALTZ_ASSIGN(auto b, get(dbi, key));
      if (b) {
        gone.push_back(copy_of(*b));
      }
    } else if (!value.empty()) {
      VALTZ_ASSIGN(const bool had, has_pair_(dbi, key, value));
      if (had) {
        gone.push_back(copy_of(value));
      }
    } else {
      VALTZ_ASSIGN(Cursor c, cursor(dbi));
      for (auto e = c.seek_exact(key); e; e = c.next_dup()) {
        gone.push_back(copy_of(e->value));
      }
    }
  }
  MDB_val k = to_val(key);
  MDB_val v = to_val(value);
  int rc = mdb_del(_txn, dbi.handle, &k, value.empty() ? nullptr : &v);
  if (rc != 0 && rc != MDB_NOTFOUND) {
    return lmdb_error(rc, "del");
  }
  for (const auto& g : gone) {
    note_(dbi, key, Bytes(g), std::nullopt, dbi.dupsort);
  }
  return ok_status();
}

Status
Txn::clear(Dbi dbi)
{
  if (journaled_(dbi)) {
    // Each key noted as it goes.
    std::vector<std::vector<std::uint8_t>> keys;
    {
      VALTZ_ASSIGN(Cursor c, cursor(dbi));
      for (auto e = c.seek(); e; e = c.next()) {
        if (keys.empty() || !std::equal(keys.back().begin(),
                                        keys.back().end(), e->key.begin(),
                                        e->key.end())) {
          keys.push_back(copy_of(e->key));
        }
      }
    }
    for (const auto& k : keys) {
      VALTZ_TRY(del(dbi, Bytes(k)));
    }
    return ok_status();
  }
  if (int rc = mdb_drop(_txn, dbi.handle, 0); rc != 0) {
    return lmdb_error(rc, "drop");
  }
  return ok_status();
}

Result<Cursor>
Txn::cursor(Dbi dbi) const
{
  MDB_cursor* c = nullptr;
  if (int rc = mdb_cursor_open(_txn, dbi.handle, &c); rc != 0) {
    return lmdb_error(rc, "cursor_open");
  }
  return Cursor(c);
}

Cursor::Cursor(Cursor&& o) noexcept : _c(o._c)
{
  o._c = nullptr;
}

Cursor&
Cursor::operator=(Cursor&& o) noexcept
{
  if (this != &o) {
    if (_c) {
      mdb_cursor_close(_c);
    }
    _c = o._c;
    o._c = nullptr;
  }
  return *this;
}

Cursor::~Cursor()
{
  if (_c) {
    mdb_cursor_close(_c);
  }
}

std::optional<Cursor::Entry>
Cursor::op_(int op, Bytes key)
{
  MDB_val k = to_val(key);
  MDB_val v{};
  if (mdb_cursor_get(_c, &k, &v, static_cast<MDB_cursor_op>(op)) != 0) {
    return std::nullopt;
  }
  return Entry{from_val(k), from_val(v)};
}

std::optional<Cursor::Entry>
Cursor::seek(Bytes key)
{
  if (key.empty()) {
    return op_(MDB_FIRST, {});
  }
  return op_(MDB_SET_RANGE, key);
}

std::optional<Cursor::Entry>
Cursor::next()
{
  return op_(MDB_NEXT, {});
}

std::optional<Cursor::Entry>
Cursor::seek_exact(Bytes key)
{
  return op_(MDB_SET_KEY, key);
}

std::optional<Cursor::Entry>
Cursor::next_dup()
{
  return op_(MDB_NEXT_DUP, {});
}

Status
for_each_prefix(const Txn& txn, Dbi dbi, Bytes prefix,
                const std::function<bool(const Cursor::Entry&)>& fn)
{
  VALTZ_ASSIGN(Cursor c, txn.cursor(dbi));
  for (auto e = c.seek(prefix); e; e = c.next()) {
    if (e->key.size() < prefix.size() ||
        std::memcmp(e->key.data(), prefix.data(), prefix.size()) != 0) {
      break;
    }
    if (!fn(*e)) {
      break;
    }
  }
  return ok_status();
}

}
