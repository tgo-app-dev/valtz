#include "valtz/project/undo.h"

#include "valtz/base/log.h"
#include "valtz/project/project.h"
#include "valtz/project/records.h"
#include "project/keys.h"

#include <algorithm>
#include <array>
#include <cstring>
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

namespace {

// A delta as the undo table keeps it: {"nb", "na", "hb", "ha" (the
// values' sizes and SHA-256), "w": [[at before, at after, bytes before,
// bytes after], ...]}.
Json
delta_json(const ValueDelta& d)
{
  Json w = Json::array();
  for (const auto& x : d.windows) {
    w.push_back(Json::array({x.at_before, x.at_after, Json::binary(x.before),
                             Json::binary(x.after)}));
  }
  auto hash = [](const ContentHash& h) {
    return Json::binary(std::vector<std::uint8_t>(h.bytes.begin(),
                                                  h.bytes.end()));
  };
  return {{"nb", d.size_before}, {"na", d.size_after},
          {"hb", hash(d.sum_before)}, {"ha", hash(d.sum_after)},
          {"w", std::move(w)}};
}

std::shared_ptr<const ValueDelta>
delta_from_json(const Json& j)
{
  auto d = std::make_shared<ValueDelta>();
  d->size_before = jget<std::size_t>(j, "nb", 0);
  d->size_after = jget<std::size_t>(j, "na", 0);
  for (const auto* k : {"hb", "ha"}) {
    const auto h = unbin(jget(j, k, Json()));
    if (!h || h->size() != 32) {
      return nullptr;
    }
    auto& to = std::string_view(k) == "hb" ? d->sum_before : d->sum_after;
    std::copy(h->begin(), h->end(), to.bytes.begin());
  }
  for (const auto& w : jget(j, "w", Json::array())) {
    if (!w.is_array() || w.size() != 4 || !w[0].is_number_unsigned() ||
        !w[1].is_number_unsigned()) {
      return nullptr;
    }
    auto before = unbin(w[2]);
    auto after = unbin(w[3]);
    if (!before || !after) {
      return nullptr;
    }
    d->windows.push_back({w[0].get<std::size_t>(), w[1].get<std::size_t>(),
                          std::move(*before), std::move(*after)});
  }
  return d;
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

void
blobs_in_cbor(db::Bytes cbor, std::set<std::string>& out)
{
  // The key "hash" (a text of 4) and a text of 64 (0x78 0x40), as CBOR
  // writes a BlobRef's -- then its 64 hex characters.
  static constexpr std::uint8_t kKey[] = {0x64, 'h', 'a', 's', 'h',
                                          0x78, 0x40};
  const auto* p = cbor.data();
  const auto* end = p + cbor.size();
  while (end - p >= static_cast<std::ptrdiff_t>(sizeof kKey + 64)) {
    const void* at = memmem(p, static_cast<std::size_t>(end - p), kKey,
                            sizeof kKey);
    if (!at) {
      break;
    }
    const auto* h = static_cast<const std::uint8_t*>(at) + sizeof kKey;
    if (end - h < 64) {
      break;
    }
    const bool hex = std::all_of(h, h + 64, [](std::uint8_t c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
    if (hex) {
      out.emplace(reinterpret_cast<const char*>(h), 64);
    }
    p = h;
  }
}

namespace {

// What is worth a delta (ValueDelta::of): values this large, a window
// peeled off where the next run both share begins this many bytes on at
// most, and a run this long to count as shared.
constexpr std::size_t kCompactMin = 16 * 1024;
constexpr std::size_t kGap = 32;
constexpr std::size_t kRun = 16;
// ...and bytes only one side has -- an object moved up or down the list,
// taken out here, put in there -- this long at most, a longer run after.
constexpr std::size_t kLongGap = 8192;
constexpr std::size_t kLongRun = 64;

// How far `a` and `b` match from their starts (64 bytes at a time first).
std::size_t
same_from_start(const std::uint8_t* a, const std::uint8_t* b, std::size_t n)
{
  std::size_t i = 0;
  while (i + 64 <= n && std::memcmp(a + i, b + i, 64) == 0) {
    i += 64;
  }
  while (i < n && a[i] == b[i]) {
    ++i;
  }
  return i;
}

// How far they match back from their ends.
std::size_t
same_from_end(const std::uint8_t* a_end, const std::uint8_t* b_end,
              std::size_t n)
{
  std::size_t i = 0;
  while (i + 64 <= n && std::memcmp(a_end - i - 64, b_end - i - 64, 64) == 0) {
    i += 64;
  }
  while (i < n && a_end[-1 - static_cast<std::ptrdiff_t>(i)] ==
                      b_end[-1 - static_cast<std::ptrdiff_t>(i)]) {
    ++i;
  }
  return i;
}

std::vector<std::uint8_t>
copy_of(const std::uint8_t* p, std::size_t n)
{
  return {p, p + n};
}

// `from`, its windows' `old` bytes replaced by their `now` bytes: the
// value at the other end of the delta.
template <class Old, class New, class At>
std::vector<std::uint8_t>
rebuild(db::Bytes from, const std::vector<ValueDelta::Window>& windows,
        Old old_of, New now_of, At at_of)
{
  std::vector<std::uint8_t> out;
  out.reserve(from.size() + 64);
  std::size_t pos = 0;
  for (const auto& w : windows) {
    const std::size_t at = at_of(w);
    out.insert(out.end(), from.begin() + static_cast<std::ptrdiff_t>(pos),
               from.begin() + static_cast<std::ptrdiff_t>(at));
    const auto& now = now_of(w);
    out.insert(out.end(), now.begin(), now.end());
    pos = at + old_of(w).size();
  }
  out.insert(out.end(), from.begin() + static_cast<std::ptrdiff_t>(pos),
             from.end());
  return out;
}

}

std::optional<ValueDelta>
ValueDelta::of(db::Bytes before, db::Bytes after)
{
  const std::size_t nb = before.size(), na = after.size();
  if (nb < kCompactMin || na < kCompactMin) {
    return std::nullopt;
  }
  const std::uint8_t* b = before.data();
  const std::uint8_t* a = after.data();
  // The shared start and end; what differs lies between.
  std::size_t sb = same_from_start(b, a, std::min(nb, na)), sa = sb;
  const std::size_t tail =
      same_from_end(b + nb, a + na, std::min(nb, na) - sb);
  std::size_t eb = nb - tail, ea = na - tail;
  std::vector<Window> head, back;
  // A window peeled off the differing middle's start (`forward`) or end:
  // of the gaps that a shared run follows -- any two lengths up to kGap
  // (a number written wider), or bytes one side alone has up to kLongGap
  // (an object moved up or down the list) -- the one whose run is
  // LONGEST (looked at up to kSee): objects much alike line up wrongly a
  // few bytes at a time otherwise, thousands of windows for one move.
  constexpr std::size_t kSee = 4096;
  auto peel = [&](bool forward) -> bool {
    std::size_t best_b = 0, best_a = 0, best_run = 0;
    auto run_of = [&](std::size_t gb, std::size_t ga, std::size_t need) {
      if (forward) {
        if (sb + gb + need > eb || sa + ga + need > ea ||
            std::memcmp(b + sb + gb, a + sa + ga, need) != 0) {
          return std::size_t{0};
        }
        return same_from_start(b + sb + gb, a + sa + ga,
                               std::min({eb - sb - gb, ea - sa - ga, kSee}));
      }
      if (eb < sb + gb + need || ea < sa + ga + need ||
          std::memcmp(b + eb - gb - need, a + ea - ga - need, need) != 0) {
        return std::size_t{0};
      }
      return same_from_end(b + eb - gb, a + ea - ga,
                           std::min({eb - sb - gb, ea - sa - ga, kSee}));
    };
    auto consider = [&](std::size_t gb, std::size_t ga, std::size_t need) {
      const std::size_t r = run_of(gb, ga, need);
      if (r > best_run) {
        best_run = r;
        best_b = gb;
        best_a = ga;
      }
    };
    for (std::size_t sum = 1; sum <= 2 * kGap; ++sum) {
      for (std::size_t gb = sum > kGap ? sum - kGap : 0;
           gb <= std::min(sum, kGap); ++gb) {
        consider(gb, sum - gb, kRun);
      }
    }
    if (best_run < kSee) {
      for (std::size_t g = kGap + 1; g <= kLongGap; ++g) {
        consider(g, 0, kLongRun);
        consider(0, g, kLongRun);
      }
    }
    if (best_run == 0) {
      return false;
    }
    if (forward) {
      head.push_back({sb, sa, copy_of(b + sb, best_b),
                      copy_of(a + sa, best_a)});
      sb += best_b;
      sa += best_a;
      const std::size_t run =
          same_from_start(b + sb, a + sa, std::min(eb - sb, ea - sa));
      sb += run;
      sa += run;
    } else {
      back.push_back({eb - best_b, ea - best_a,
                      copy_of(b + eb - best_b, best_b),
                      copy_of(a + ea - best_a, best_a)});
      eb -= best_b;
      ea -= best_a;
      const std::size_t run =
          same_from_end(b + eb, a + ea, std::min(eb - sb, ea - sa));
      eb -= run;
      ea -= run;
    }
    return true;
  };
  // Off its start, then off its end (a time stamped after the change),
  // while there is a middle; a few hundred windows at most -- past that,
  // kept whole.
  while (sb < eb && sa < ea && head.size() < 256 && peel(true)) {
  }
  while (sb < eb && sa < ea && back.size() < 256 && peel(false)) {
  }
  ValueDelta d;
  d.size_before = nb;
  d.size_after = na;
  d.windows = std::move(head);
  if (sb < eb || sa < ea) {
    d.windows.push_back({sb, sa, copy_of(b + sb, eb - sb),
                         copy_of(a + sa, ea - sa)});
  }
  for (auto it = back.rbegin(); it != back.rend(); ++it) {
    d.windows.push_back(std::move(*it));
  }
  if (d.bytes() * 4 > nb + na) {
    return std::nullopt;
  }
  d.sum_before = ContentHash::of(b, nb);
  d.sum_after = ContentHash::of(a, na);
  return d;
}

std::size_t
ValueDelta::bytes() const
{
  std::size_t n = 0;
  for (const auto& w : windows) {
    n += w.before.size() + w.after.size();
  }
  return n;
}

std::optional<std::vector<std::uint8_t>>
ValueDelta::forward(db::Bytes before) const
{
  if (before.size() != size_before ||
      ContentHash::of(before.data(), before.size()) != sum_before) {
    return std::nullopt;
  }
  auto out = rebuild(
      before, windows, [](const Window& w) -> const auto& { return w.before; },
      [](const Window& w) -> const auto& { return w.after; },
      [](const Window& w) { return w.at_before; });
  if (out.size() != size_after ||
      ContentHash::of(out.data(), out.size()) != sum_after) {
    return std::nullopt;
  }
  return out;
}

std::optional<std::vector<std::uint8_t>>
ValueDelta::backward(db::Bytes after) const
{
  if (after.size() != size_after ||
      ContentHash::of(after.data(), after.size()) != sum_after) {
    return std::nullopt;
  }
  auto out = rebuild(
      after, windows, [](const Window& w) -> const auto& { return w.after; },
      [](const Window& w) -> const auto& { return w.before; },
      [](const Window& w) { return w.at_after; });
  if (out.size() != size_before ||
      ContentHash::of(out.data(), out.size()) != sum_before) {
    return std::nullopt;
  }
  return out;
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
            UndoChange dc;
            dc.dbi = table->handle;
            dc.key = *key;
            dc.before = unbin(ch[2]);
            dc.after = unbin(ch[3]);
            dc.dup = ch[4].is_boolean() && ch[4].get<bool>();
            if (ch.size() > 5 && ch[5].is_object()) {
              dc.delta = delta_from_json(ch[5]);
              if (!dc.delta) {
                continue;  // unreadable: the change is lost, not invented
              }
            }
            c.changes.push_back(std::move(dc));
          }
          if (const Json b = jget(*j, "blobs", Json()); b.is_array()) {
            auto names = std::make_shared<std::set<std::string>>();
            for (const auto& h : b) {
              if (h.is_string()) {
                names->insert(h.get<std::string>());
              }
            }
            c.blobs = std::move(names);
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
      for (auto& ch : changes) {
        to.emplace_back(std::move(ch));
      }
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
  std::optional<UndoCommand> compacted;
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
    c.blobs.reset();  // more changes: its blobs found again
    // A new action: what was undone can no longer be redone.
    for (const auto& u : _cmds) {
      if (!u.done) {
        dropped.push_back(u.seq);
      }
    }
    std::erase_if(_cmds, [](const UndoCommand& u) { return !u.done; });
    UndoCommand* last = last_done_();
    const std::int64_t now = now_ms();
    // Never into the command a save was made at: its seq would stay the
    // saved one, and the change after the save read as saved (lost).
    // Never into one made compact (the last before an undo): its values
    // are not whole to go on from.
    const bool whole = last && std::ranges::none_of(
        last->changes, [](const UndoChange& x) {
          return x.delta && !x.before && !x.after;
        });
    if (last && whole && coalesces(c.kind) && last->kind == c.kind &&
        last->asset == c.asset && last->layer == c.layer &&
        c.at_ms - last->last_ms < kCoalesceMs &&
        (_untracked || last->seq != _saved_seq)) {
      // A slider's next step: one command, the first before-values.
      for (auto& ch : c.changes) {
        auto same = std::ranges::find_if(last->changes,
            [&](const db::Change& x) {
              return !x.dup && !ch.dup && x.dbi == ch.dbi && x.key == ch.key;
            });
        if (same != last->changes.end()) {
          same->after = std::move(ch.after);
          same->delta.reset();  // made again for its new after-value
        } else {
          last->changes.push_back(std::move(ch));
        }
      }
      last->last_ms = now;
      last->blobs.reset();  // its values changed: found again
      stored = *last;
    } else {
      c.last_ms = now;
      c.done = true;
      // The one before is the last no more: kept compact from now on.
      if (last) {
        compact_(*last);
        compacted = *last;
      }
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
  if (compacted && std::ranges::find(dropped, compacted->seq) ==
                       dropped.end()) {
    if (auto st = store_(*compacted); !st.ok()) {
      VALTZ_LOG_WARN("undo", "cannot keep the history: {}",
                     st.error().message);
    }
  }
  if (stored) {
    // Stored compact even while it is the last (whole in memory, so a
    // slider's next step coalesces): two copies of a page of thousands
    // were written at every edit. Its delta and blobs kept with it.
    {
      std::lock_guard lk(_mu);
      if (auto it = std::ranges::find(_cmds, stored->seq,
                                      &UndoCommand::seq);
          it != _cmds.end()) {
        prepare_(*it);
        *stored = *it;
      }
    }
    for (auto& ch : stored->changes) {
      if (ch.delta) {
        ch.before.reset();
        ch.after.reset();
      }
    }
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
    Json one = Json::array({_p.table_name(ch.dbi), Json::binary(ch.key),
                            bin(ch.before), bin(ch.after), ch.dup});
    if (ch.delta) {
      one.push_back(delta_json(*ch.delta));
    }
    changes.push_back(std::move(one));
  }
  Json doc = {{"kind", c.kind},   {"args", c.args},
              {"asset", c.asset}, {"layer", c.layer},
              {"at", c.at_ms},    {"last", c.last_ms},
              {"done", c.done},   {"changes", std::move(changes)}};
  if (c.blobs) {
    doc["blobs"] = Json(std::vector<std::string>(c.blobs->begin(),
                                                 c.blobs->end()));
  }
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
    auto step = [&](const UndoChange& ch) -> Status {
      const db::Dbi dbi{ch.dbi, ch.dup};
      const db::Bytes key(ch.key);
      if (ch.delta && !ch.before && !ch.after) {
        // Made from the value there now -- the one the command left (or,
        // redone, found) -- else refused: changed since by no command.
        VALTZ_ASSIGN(auto now, txn.get(dbi, key));
        auto made = !now ? std::nullopt
                    : forward ? ch.delta->forward(*now)
                              : ch.delta->backward(*now);
        if (!made) {
          return make_error(Code::Corrupt,
                            "the history no longer matches the project "
                            "(it was changed outside any command)");
        }
        return txn.put(dbi, key, db::Bytes(*made));
      }
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
    const auto names = c.blobs ? c.blobs : blobs_of_(c);
    held.insert(names->begin(), names->end());
  }
  return held;
}

std::shared_ptr<const std::set<std::string>>
UndoLog::blobs_of_(const UndoCommand& c)
{
  auto names = std::make_shared<std::set<std::string>>();
  for (const auto& ch : c.changes) {
    if (ch.dup) {
      continue;
    }
    for (const auto* v : {&ch.before, &ch.after}) {
      if (*v) {
        blobs_in_cbor(db::Bytes(**v), *names);
      }
    }
  }
  return names;
}

void
UndoLog::prepare_(UndoCommand& c) const
{
  // Its blobs found, and its large values' deltas made, while its values
  // are still whole.
  if (!c.blobs) {
    c.blobs = blobs_of_(c);
  }
  for (auto& ch : c.changes) {
    if (ch.dup || ch.delta || !ch.before || !ch.after) {
      continue;
    }
    if (auto d = ValueDelta::of(db::Bytes(*ch.before), db::Bytes(*ch.after))) {
      ch.delta = std::make_shared<const ValueDelta>(std::move(*d));
    }
  }
}

void
UndoLog::compact_(UndoCommand& c) const
{
  prepare_(c);
  for (auto& ch : c.changes) {
    if (ch.delta) {
      ch.before.reset();
      ch.after.reset();
    }
  }
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
