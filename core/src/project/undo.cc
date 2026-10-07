#include "valtz/project/undo.h"

#include "valtz/base/log.h"
#include "valtz/project/project.h"
#include "valtz/project/records.h"
#include "project/keys.h"

#include <algorithm>
#include <array>
#include <format>

namespace valtz::project {

namespace {

// The scopes open on this thread: which log, which command. A scope opened
// inside another of the same log joins it.
thread_local std::vector<std::pair<const UndoLog*, std::uint64_t>> t_scopes;

std::uint64_t
open_scope(const UndoLog* log)
{
  for (auto it = t_scopes.rbegin(); it != t_scopes.rend(); ++it) {
    if (it->first == log) {
      return it->second;
    }
  }
  return 0;
}

bool
coalesces(std::string_view kind)
{
  return kind == "adjust" || kind == "crop" || kind == "keys" ||
         kind == "markup.objects";
}

std::array<std::uint8_t, 8>
seq_key(std::uint64_t seq)
{
  std::array<std::uint8_t, 8> k{};
  for (int i = 0; i < 8; ++i) {
    k[i] = static_cast<std::uint8_t>(seq >> (56 - 8 * i));
  }
  return k;
}

std::uint64_t
seq_of(db::Bytes key)
{
  std::uint64_t s = 0;
  for (const auto b : key) {
    s = (s << 8) | b;
  }
  return s;
}

constexpr std::string_view kSavedKey = "saved";

Json
bin(const std::optional<std::vector<std::uint8_t>>& b)
{
  return b ? Json::binary(*b) : Json();
}

std::optional<std::vector<std::uint8_t>>
unbin(const Json& j)
{
  if (j.is_binary()) {
    return std::vector<std::uint8_t>(j.get_binary().begin(),
                                     j.get_binary().end());
  }
  return std::nullopt;
}

}

void
blobs_named_in(const Json& j, std::set<std::string>& out)
{
  if (j.is_object()) {
    if (j.contains("hash") && j.contains("ext") && j.contains("size")) {
      if (const auto h = jget<std::string>(j, "hash", ""); h.size() == 64) {
        out.insert(h);
      }
    }
    for (const auto& [_, v] : j.items()) {
      blobs_named_in(v, out);
    }
  } else if (j.is_array()) {
    for (const auto& v : j) {
      blobs_named_in(v, out);
    }
  }
}

// ---- the log ------------------------------------------------------------

Result<std::unique_ptr<UndoLog>>
UndoLog::attach(Project& p)
{
  std::unique_ptr<UndoLog> log(new UndoLog(p));
  // The history as it was left.
  VALTZ_TRY(p.env().read([&](db::Txn& txn) -> Status {
    return db::for_each_prefix(txn, p.undo_table(), {},
        [&](const db::Cursor::Entry& e) {
          auto j = from_cbor(e.value);
          if (!j.ok()) {
            return true;
          }
          if (db::as_view(e.key) == kSavedKey) {
            log->_saved_seq = jget<std::uint64_t>(*j, "seq", 0);
            log->_untracked = jget(*j, "untracked", false);
            return true;
          }
          if (e.key.size() != 8) {
            return true;
          }
          UndoCommand c;
          c.seq = seq_of(e.key);
          c.kind = jget<std::string>(*j, "kind", "");
          c.args = jget(*j, "args", Json::object());
          c.asset = jget<std::string>(*j, "asset", "");
          c.layer = jget<std::string>(*j, "layer", "");
          c.at_ms = jget<std::int64_t>(*j, "at", 0);
          c.last_ms = jget<std::int64_t>(*j, "last", c.at_ms);
          c.done = jget(*j, "done", true);
          for (const auto& ch : jget(*j, "changes", Json::array())) {
            if (!ch.is_array() || ch.size() < 5) {
              continue;
            }
            const auto table = p.table(ch[0].get<std::string>());
            const auto key = unbin(ch[1]);
            if (!table || !key) {
              continue;  // a table this Valtz does not have
            }
            db::Change dc;
            dc.dbi = table->handle;
            dc.key = *key;
            dc.before = unbin(ch[2]);
            dc.after = unbin(ch[3]);
            dc.dup = ch[4].is_boolean() && ch[4].get<bool>();
            c.changes.push_back(std::move(dc));
          }
          log->_next_seq = std::max(log->_next_seq, c.seq + 1);
          log->_cmds.push_back(std::move(c));
          return true;
        });
  }));
  std::ranges::sort(log->_cmds, {}, &UndoCommand::seq);
  UndoLog* raw = log.get();
  p.env().set_journal(
      [raw](std::vector<db::Change>&& changes) {
        raw->journal_(std::move(changes));
      },
      p.record_tables());
  return log;
}

UndoLog::~UndoLog()
{
  _p.env().set_journal(nullptr, {});
}

void
UndoLog::journal_(std::vector<db::Change>&& changes)
{
  const std::uint64_t seq = open_scope(this);
  {
    std::lock_guard lk(_mu);
    if (auto it = _open.find(seq); seq != 0 && it != _open.end()) {
      auto& to = it->second.cmd.changes;
      to.insert(to.end(), std::make_move_iterator(changes.begin()),
                std::make_move_iterator(changes.end()));
      return;
    }
    // Written in no command: not undoable, but not as saved either.
    if (_untracked) {
      return;
    }
    _untracked = true;
  }
  (void)store_saved_();
  if (_changed) {
    _changed();
  }
}

UndoLog::Scope::Scope(Scope&& o) noexcept
    : _log(o._log), _seq(o._seq), _outer(o._outer)
{
  o._log = nullptr;
  o._outer = false;
}

UndoLog::Scope::~Scope()
{
  if (_log && _outer) {
    _log->end_(*this);
  }
}

UndoLog::Scope
UndoLog::command(std::string kind, Json args, std::string asset,
                 std::string layer)
{
  Scope s;
  s._log = this;
  if (const auto open = open_scope(this)) {
    s._seq = open;  // joins the action this is part of
    return s;
  }
  UndoCommand c;
  c.kind = std::move(kind);
  c.args = std::move(args);
  c.asset = std::move(asset);
  c.layer = std::move(layer);
  c.at_ms = c.last_ms = now_ms();
  {
    std::lock_guard lk(_mu);
    c.seq = _next_seq++;
    s._seq = c.seq;
    _open.emplace(c.seq, Open{std::move(c), 0, false});
  }
  s._outer = true;
  t_scopes.emplace_back(this, s._seq);
  return s;
}

UndoLog::Scope
UndoLog::join(std::uint64_t seq)
{
  Scope s;
  s._log = this;
  if (const auto open = open_scope(this)) {
    s._seq = open;
    return s;
  }
  UndoCommand c;
  bool rejoined = false;
  {
    std::lock_guard lk(_mu);
    UndoCommand* last = last_done_();
    if (last && last->seq == seq) {
      // Still the last: taken up again, its changes go on.
      c = std::move(*last);
      _cmds.erase(std::ranges::find(_cmds, seq, &UndoCommand::seq));
      rejoined = true;
    } else {
      // Something came after it: a command of its own, of its kind.
      auto was = std::ranges::find(_cmds, seq, &UndoCommand::seq);
      c.kind = was != _cmds.end() ? was->kind : "generate";
      c.args = was != _cmds.end() ? was->args : Json::object();
      c.asset = was != _cmds.end() ? was->asset : "";
      c.seq = _next_seq++;
      c.at_ms = c.last_ms = now_ms();
    }
    s._seq = c.seq;
    const std::size_t had = c.changes.size();
    _open.emplace(c.seq, Open{std::move(c), had, rejoined});
  }
  s._outer = true;
  t_scopes.emplace_back(this, s._seq);
  return s;
}

std::uint64_t
UndoLog::recording() const
{
  return open_scope(this);
}

void
UndoLog::end_(Scope& s)
{
  std::erase_if(t_scopes, [&](const auto& e) {
    return e.first == this && e.second == s._seq;
  });
  std::vector<std::uint64_t> dropped;
  std::optional<UndoCommand> stored;
  {
    std::lock_guard lk(_mu);
    auto it = _open.find(s._seq);
    if (it == _open.end()) {
      return;
    }
    UndoCommand c = std::move(it->second.cmd);
    const std::size_t had = it->second.had;
    const bool rejoined = it->second.rejoined;
    _open.erase(it);
    if (c.changes.size() == had) {
      // Nothing changed: a command taken up again goes back as it was;
      // a new one is no command.
      if (rejoined) {
        _cmds.push_back(std::move(c));
      }
      return;
    }
    // A new action: what was undone can no longer be redone.
    for (const auto& u : _cmds) {
      if (!u.done) {
        dropped.push_back(u.seq);
      }
    }
    std::erase_if(_cmds, [](const UndoCommand& u) { return !u.done; });
    UndoCommand* last = last_done_();
    const std::int64_t now = now_ms();
    if (last && coalesces(c.kind) && last->kind == c.kind &&
        last->asset == c.asset && last->layer == c.layer &&
        c.at_ms - last->last_ms < kCoalesceMs) {
      // A slider's next step: one command, the first before-values.
      for (auto& ch : c.changes) {
        auto same = std::ranges::find_if(last->changes,
            [&](const db::Change& x) {
              return !x.dup && !ch.dup && x.dbi == ch.dbi && x.key == ch.key;
            });
        if (same != last->changes.end()) {
          same->after = std::move(ch.after);
        } else {
          last->changes.push_back(std::move(ch));
        }
      }
      last->last_ms = now;
      stored = *last;
    } else {
      c.last_ms = now;
      c.done = true;
      _cmds.push_back(std::move(c));
      stored = _cmds.back();
    }
    while (_cmds.size() > kMaxCommands) {
      dropped.push_back(_cmds.front().seq);
      _cmds.erase(_cmds.begin());
    }
  }
  for (const auto seq : dropped) {
    (void)erase_(seq);
  }
  if (stored) {
    if (auto st = store_(*stored); !st.ok()) {
      VALTZ_LOG_WARN("undo", "cannot keep the history: {}",
                     st.error().message);
    }
  }
  if (_changed) {
    _changed();
  }
}

Status
UndoLog::store_(const UndoCommand& c)
{
  Json changes = Json::array();
  for (const auto& ch : c.changes) {
    changes.push_back(Json::array({_p.table_name(ch.dbi),
                                   Json::binary(ch.key), bin(ch.before),
                                   bin(ch.after), ch.dup}));
  }
  const Json doc = {{"kind", c.kind},   {"args", c.args},
                    {"asset", c.asset}, {"layer", c.layer},
                    {"at", c.at_ms},    {"last", c.last_ms},
                    {"done", c.done},   {"changes", std::move(changes)}};
  const auto k = seq_key(c.seq);
  return _p.env().write([&](db::Txn& txn) -> Status {
    return put_cbor(txn, _p.undo_table(), {k.data(), k.size()}, doc);
  });
}

Status
UndoLog::erase_(std::uint64_t seq)
{
  const auto k = seq_key(seq);
  return _p.env().write([&](db::Txn& txn) -> Status {
    return txn.del(_p.undo_table(), {k.data(), k.size()});
  });
}

Status
UndoLog::store_saved_()
{
  Json doc;
  {
    std::lock_guard lk(_mu);
    doc = {{"seq", _saved_seq}, {"untracked", _untracked}};
  }
  return _p.env().write([&](db::Txn& txn) -> Status {
    return put_cbor(txn, _p.undo_table(), db::as_bytes(kSavedKey), doc);
  });
}

UndoCommand*
UndoLog::last_done_()
{
  for (auto it = _cmds.rbegin(); it != _cmds.rend(); ++it) {
    if (it->done) {
      return &*it;
    }
  }
  return nullptr;
}

const UndoCommand*
UndoLog::last_done_() const
{
  for (auto it = _cmds.rbegin(); it != _cmds.rend(); ++it) {
    if (it->done) {
      return &*it;
    }
  }
  return nullptr;
}

UndoCommand*
UndoLog::first_undone_()
{
  for (auto& c : _cmds) {
    if (!c.done) {
      return &c;
    }
  }
  return nullptr;
}

std::optional<UndoCommand>
UndoLog::next_undo() const
{
  std::lock_guard lk(_mu);
  const UndoCommand* c = last_done_();
  if (!c) {
    return std::nullopt;
  }
  UndoCommand out = *c;
  out.changes.clear();
  return out;
}

std::vector<UndoCommand>
UndoLog::list() const
{
  std::lock_guard lk(_mu);
  std::vector<UndoCommand> out;
  for (const auto& c : _cmds) {
    UndoCommand x = c;
    x.changes.clear();
    out.push_back(std::move(x));
  }
  return out;
}

std::optional<UndoCommand>
UndoLog::next_redo() const
{
  std::lock_guard lk(_mu);
  for (const auto& c : _cmds) {
    if (!c.done) {
      UndoCommand out = c;
      out.changes.clear();
      return out;
    }
  }
  return std::nullopt;
}

Status
UndoLog::apply_(const UndoCommand& c, bool forward)
{
  // Written back (or forward) as it was: no command of its own.
  db::Env::Unjournaled quiet;
  return _p.env().write([&](db::Txn& txn) -> Status {
    auto step = [&](const db::Change& ch) -> Status {
      const db::Dbi dbi{ch.dbi, ch.dup};
      const db::Bytes key(ch.key);
      const auto& to = forward ? ch.after : ch.before;
      const auto& from = forward ? ch.before : ch.after;
      if (ch.dup) {
        // A pair that came (or went): it goes (or comes) again.
        if (to) {
          return txn.put(dbi, key, db::Bytes(*to));
        }
        return from ? txn.del(dbi, key, db::Bytes(*from)) : ok_status();
      }
      return to ? txn.put(dbi, key, db::Bytes(*to)) : txn.del(dbi, key);
    };
    if (forward) {
      for (const auto& ch : c.changes) {
        VALTZ_TRY(step(ch));
      }
    } else {
      for (auto it = c.changes.rbegin(); it != c.changes.rend(); ++it) {
        VALTZ_TRY(step(*it));
      }
    }
    return ok_status();
  });
}

Result<UndoCommand>
UndoLog::undo()
{
  UndoCommand c;
  {
    std::lock_guard lk(_mu);
    UndoCommand* last = last_done_();
    if (!last) {
      return make_error(Code::NotFound, "nothing to undo");
    }
    c = *last;
  }
  VALTZ_TRY(apply_(c, false));
  {
    std::lock_guard lk(_mu);
    if (auto it = std::ranges::find(_cmds, c.seq, &UndoCommand::seq);
        it != _cmds.end()) {
      it->done = false;
      c.done = false;
    }
  }
  VALTZ_TRY(store_(c));
  if (_changed) {
    _changed();
  }
  return c;
}

Result<UndoCommand>
UndoLog::redo()
{
  UndoCommand c;
  {
    std::lock_guard lk(_mu);
    UndoCommand* next = first_undone_();
    if (!next) {
      return make_error(Code::NotFound, "nothing to redo");
    }
    c = *next;
  }
  VALTZ_TRY(apply_(c, true));
  {
    std::lock_guard lk(_mu);
    if (auto it = std::ranges::find(_cmds, c.seq, &UndoCommand::seq);
        it != _cmds.end()) {
      it->done = true;
      c.done = true;
    }
  }
  VALTZ_TRY(store_(c));
  if (_changed) {
    _changed();
  }
  return c;
}

Status
UndoLog::clear()
{
  {
    std::lock_guard lk(_mu);
    _cmds.clear();
    _saved_seq = 0;
    _untracked = false;
  }
  VALTZ_TRY(_p.env().write([&](db::Txn& txn) -> Status {
    return txn.clear(_p.undo_table());
  }));
  VALTZ_TRY(store_saved_());
  if (_changed) {
    _changed();
  }
  return ok_status();
}

Status
UndoLog::mark_saved()
{
  {
    std::lock_guard lk(_mu);
    const UndoCommand* last = last_done_();
    _saved_seq = last ? last->seq : 0;
    _untracked = false;
  }
  VALTZ_TRY(store_saved_());
  if (_changed) {
    _changed();
  }
  return ok_status();
}

Status
UndoLog::mark_untracked()
{
  {
    std::lock_guard lk(_mu);
    _untracked = true;
  }
  VALTZ_TRY(store_saved_());
  if (_changed) {
    _changed();
  }
  return ok_status();
}

bool
UndoLog::at_saved() const
{
  std::lock_guard lk(_mu);
  const UndoCommand* last = last_done_();
  return !_untracked && (last ? last->seq : 0) == _saved_seq;
}

std::set<std::string>
UndoLog::held_blobs() const
{
  std::set<std::string> held;
  std::lock_guard lk(_mu);
  for (const auto& c : _cmds) {
    for (const auto& ch : c.changes) {
      if (ch.dup) {
        continue;
      }
      for (const auto* v : {&ch.before, &ch.after}) {
        if (*v) {
          if (auto j = from_cbor(db::Bytes(**v)); j.ok()) {
            blobs_named_in(*j, held);
          }
        }
      }
    }
  }
  return held;
}

bool
UndoLog::drop_oldest()
{
  std::uint64_t seq = 0;
  {
    std::lock_guard lk(_mu);
    if (_cmds.empty()) {
      return false;
    }
    seq = _cmds.front().seq;
    _cmds.erase(_cmds.begin());
  }
  (void)erase_(seq);
  return true;
}

}
