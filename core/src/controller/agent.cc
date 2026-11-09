// THE AGENT's sessions (DESIGN §9b): the loop, the review, pause and
// resume, the backends, the turns served for a fleet member -- and the
// Controller's agent_* calls. The tools are agent-tools.cc's.

#include "controller/agent-host.h"

#include "valtz/base/log.h"
#include "valtz/base/message.h"
#include "valtz/base/text.h"
#include "valtz/media/thumbnail.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>

namespace valtz {

namespace fs = std::filesystem;

namespace {

// The most a turn of the helper writes. Generous: a reply runs to its
// end (at 2,048 a chat's code was cut mid-file, 2026-11-05, and the
// session ended "done" on it). vpipe's text-chat books its K/V by the
// context, not by this, and stops at the context's end itself (a
// context full, below); this cuts only a runaway reply -- said, not
// taken as its last word.
constexpr int kReplyTokens = 32768;
// A turn's reply as compaction reckons it (most are a call or a few
// lines): the room it keeps for the next turn's.
constexpr int kTurnTokens = 2048;
// Compacted when the next turn, its reply and the notes' question and
// answer (a turn each) would pass 90% of the model's context -- the
// rest the estimates' slack -- into half of it at most: the head, the
// notes, the latest turns.
constexpr std::int64_t kCompactReserve = 2 * kTurnTokens + 512;
// The helper's K/V page (the graph builder's kHelperPageTokens): a turn's
// ctx_pos within one of its context's end has filled it.
constexpr std::int64_t kPageTokens = 16;
constexpr std::int64_t kCompactAtPct = 90;
constexpr std::int64_t kCompactToPct = 50;
// The steps the notes list, the latest; each line cut so; all of them
// within a twentieth of the context (3 bytes a token), the person's
// words within a tenth -- the notes must leave the context room.
constexpr std::size_t kNotedSteps = 40;
constexpr std::size_t kStepBytes = 360;
constexpr std::int64_t kStepsShare = 20;
constexpr std::int64_t kSaidShare = 10;
// A session's turns before it stops to ask whether to go on: a model
// calling tools in a loop is stopped by this, not by the person's budget.
constexpr int kMaxTurns = 200;
constexpr double kDefaultLease = 600;
// The pictures a person's message comes with that are SHOWN to a model
// that sees, the first of them, each this long at most: 768 a side is
// about 600 tokens to Qwen3.5's tower, where a page looked at is 1024.
constexpr std::size_t kShownAttached = 4;
constexpr int kAttachedSide = 768;
// A transcript's step: what it was asked and what came back, each cut so.
constexpr std::size_t kTranscriptArgs = 400;
constexpr std::size_t kTranscriptResult = 300;
constexpr std::size_t kTranscriptNote = 300;
// What marks a session's transcript in Assets, and its folder.
constexpr const char* kTranscriptTag = "agent-transcript";
constexpr const char* kTranscriptFolderRole = "agent";
constexpr const char* kTranscriptFolderName = "Agent Sessions";
// How with_stage_ tells where the person is, after their words.
constexpr std::string_view kWhereOpen =
    "\n\n[Where the person is as they say this: ";
constexpr std::string_view kWhereClose = ". \"This page\"";
// A turn that wrote a tool call nothing could read is taken again, told
// so, at most this many times running: it is not the session's end. The
// form shown is the one the helper's own template teaches (Qwen3.5 /
// 3.8's, as vpipe writes its tools turn: DESIGN §15 0zs; DeepSeek V4.1's
// DSML, 0zzd).
constexpr int kUnreadCalls = 2;
constexpr const char* kUnreadCall =
    "[Valtz] Your tool call could not be read, so nothing ran. Write each "
    "call again in the format the system message gives:\n<tool_call>\n"
    "<function=TOOL>\n<parameter=NAME>\nvalue\n</parameter>\n</function>\n"
    "</tool_call>";
// DeepSeek V4.1's own form, DSML, as its system message teaches it
// (vpipe's template, DESIGN §15 0zzd).
#define VALTZ_DSML_ "\uff5cDSML\uff5c"
constexpr const char* kUnreadDsmlCall =
    "[Valtz] Your tool call could not be read, so nothing ran. Write each "
    "call again in the format the system message gives:\n\n<" VALTZ_DSML_
    " calls>\n<" VALTZ_DSML_ " invoke name=\"TOOL\">\n<" VALTZ_DSML_
    " parameter name=\"NAME\" string=\"true\">value</" VALTZ_DSML_
    " parameter>\n</" VALTZ_DSML_ " invoke>\n</" VALTZ_DSML_ " calls>";

// A PROGRAM written in a reply's words, a procedural session's (DESIGN
// §4t): its longest fenced block that reads as one -- a small helper
// (Qwen3.5 9B, 2026-11-08) wrote a whole three.js program into its
// answer and called nothing. "" when there is none.
std::string
program_in_reply(const std::string& content)
{
  std::string best;
  std::size_t at = 0;
  for (;;) {
    const std::size_t open = content.find("```", at);
    if (open == std::string::npos) {
      break;
    }
    const std::size_t body = content.find('\n', open);
    if (body == std::string::npos) {
      break;
    }
    const std::size_t close = content.find("```", body);
    if (close == std::string::npos) {
      break;
    }
    const std::string lang = content.substr(open + 3, body - open - 3);
    std::string code = content.substr(body + 1, close - body - 1);
    at = close + 3;
    const bool js = lang.find("js") != std::string::npos ||
                    lang.find("javascript") != std::string::npos ||
                    lang.find_first_not_of(" \t\r") == std::string::npos;
    const bool draws = code.find("valtz") != std::string::npos ||
                       code.find("getContext") != std::string::npos ||
                       code.find("THREE") != std::string::npos ||
                       code.find("BABYLON") != std::string::npos ||
                       code.find("document.") != std::string::npos;
    if (js && draws && code.size() >= 120 && code.size() > best.size()) {
      best = std::move(code);
    }
  }
  return best;
}

// The form a reply's unread call was written in -- what to ask again
// with -- or null when it wrote none.
const char*
unread_call_form(const std::string& raw)
{
  if (raw.find("<tool_call>") != std::string::npos) {
    return kUnreadCall;
  }
  if (raw.find(VALTZ_DSML_) != std::string::npos) {
    return kUnreadDsmlCall;
  }
  return nullptr;
}
#undef VALTZ_DSML_

Json
read_json(const fs::path& file)
{
  std::ifstream in(file);
  const Json j = in ? Json::parse(in, nullptr, false) : Json();
  return j.is_object() ? j : Json::object();
}

Status
write_json(const fs::path& file, const Json& j)
{
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << to_text(j, 2) << "\n";
    if (!out) {
      return make_error(Code::Io, std::format("cannot write {}",
                                              tmp.string()));
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot write {}",
                                            file.string()));
  }
  return ok_status();
}

// The person's own words in a user message: as typed (`said`), or --
// one saved before it was kept -- its content up to where they were.
std::string
person_words_of(const agent::Message& m)
{
  if (!m.said.empty()) {
    return m.said;
  }
  std::string t = m.content;
  for (std::string_view marker :
       {kWhereOpen, std::string_view("\n[Valtz]"),
        std::string_view("\n[The person attached:")}) {
    if (const auto at = t.find(marker); at != std::string::npos) {
      t.resize(at);
    }
  }
  return t;
}

// Where the person was as they said it -- the composition, its page or
// frame, the layer -- as with_stage_ told it; "" where it told nothing.
std::string
where_said(const std::string& content)
{
  const auto at = content.find(kWhereOpen);
  if (at == std::string::npos) {
    return {};
  }
  const auto from = at + kWhereOpen.size();
  auto to = content.find(kWhereClose, from);
  if (to == std::string::npos) {
    to = content.find(']', from);
  }
  return content.substr(from, to == std::string::npos ? std::string::npos
                                                      : to - from);
}

// Text cut to `bytes` on a character -- at a word's end where one is in
// its last third -- "..." where it was.
std::string
cut(std::string_view t, std::size_t bytes)
{
  std::string_view kept = utf8_prefix(t, bytes);
  if (kept.size() == t.size()) {
    return std::string(kept);
  }
  if (const auto sp = kept.rfind(' ');
      sp != std::string_view::npos && sp > kept.size() * 2 / 3) {
    kept = kept.substr(0, sp);
  }
  return std::string(kept) + " ...";
}

// Inside a transcript's `code`: one line, no backtick to end it early.
std::string
code_span(std::string_view t, std::size_t bytes)
{
  std::string s = cut(one_line(std::string(t)), bytes);
  std::ranges::replace(s, '`', '\'');
  return "`" + s + "`";
}

// A program's code as a fenced block of a transcript: whole (to 200,000
// bytes), its fence longer than any run of backticks inside it -- a
// template literal's -- and on lines of its own, the step's list ended
// (in a list item a block's lines must be indented as it is).
std::string
code_block(std::string_view code, std::string_view language)
{
  constexpr std::size_t kMost = 200000;
  std::string body(utf8_prefix(code, kMost));
  std::size_t run = 0;
  std::size_t longest = 0;
  for (const char c : body) {
    run = c == '`' ? run + 1 : 0;
    longest = std::max(longest, run);
  }
  const std::string fence(std::max<std::size_t>(3, longest + 1), '`');
  while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
    body.pop_back();
  }
  std::string t = "\n" + fence + std::string(language) + "\n" + body +
                  "\n" + fence + "\n";
  if (code.size() > kMost) {
    t += "*Cut at 200,000 bytes.*\n";
  }
  return t + "\n";
}

// A time a person reads, local: "2026-11-03 14:02".
std::string
local_time(std::int64_t ms)
{
  const std::time_t t = static_cast<std::time_t>(ms / 1000);
  std::tm tm{};
  localtime_r(&t, &tm);
  char b[32];
  std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
  return b;
}

std::int64_t
now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// A key's characters as a file name takes them.
std::string
clean(std::string_view s)
{
  std::string out;
  for (char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-' || c == '_';
    out += ok ? c : '_';
  }
  return out;
}

}  // namespace

// ---- the helper: this Mac's, or a fleet member's ------------------------
//
// Each turn goes where agent turns go (fleet_candidates_: the offload
// policy, a member idle and holding a helper) -- the member that ran the
// last one first, since it holds the conversation; this Mac otherwise.
// Its context is kept under the session's key: here with the helper (and
// saved to the session's folder when it must go), on a member as long as
// the lease.
class Controller::AgentHost::HelperBackend final : public agent::Backend {
public:
  HelperBackend(Controller& c, std::string key, fs::path dir, double lease)
    : _c(c), _key(std::move(key)), _dir(std::move(dir)), _lease(lease)
  {}

  Json
  describe() const override
  {
    const auto* m = _c.agent_model();
    std::lock_guard lk(_mu);
    return {{"kind", _where.empty() ? "local" : "fleet"},
            {"model", m ? m->name : std::string()},
            {"where", _where}};
  }

  bool
  sees() const override
  {
    const auto* m = _c.agent_model();
    return m != nullptr && (m->has(models::Capability::VideoSummary) ||
                            m->has(models::Capability::Caption));
  }

  // This Mac's helper's, as its memory gives it -- or the member's that
  // ran the last turn, as it said (its "assistant_context").
  std::int64_t
  context_tokens() const override
  {
    {
      std::lock_guard lk(_mu);
      if (_member_context > 0) {
        return _member_context;
      }
    }
    std::lock_guard lk(_mu);
    if (_local_context < 0) {
      const auto* m = _c.agent_model();
      _local_context = m ? _c.helper_context_(*m) : 0;
    }
    return _local_context;
  }

  Result<agent::Reply>
  turn(const std::vector<agent::Message>& messages,
       const std::vector<agent::ToolSpec>& tools,
       const std::function<void(std::string_view)>& on_text,
       const std::atomic<bool>& cancel) override
  {
    const auto* m = _c.agent_model();
    const bool here = m != nullptr && _c._engine &&
                      _c._engine->supports(engine::kOpAgentTurn);
    auto members = _c.fleet_candidates_(std::string(engine::kOpAgentTurn),
                                        {}, here);
    {
      // The member holding the conversation first.
      std::lock_guard lk(_mu);
      auto it = std::ranges::find_if(members, [&](const Json& x) {
        return jget<std::string>(x, "id", "") == _member;
      });
      if (it != members.end() && it != members.begin()) {
        std::rotate(members.begin(), it, it + 1);
      }
    }
    const bool fleet_only =
        _c.offload_policy_(engine::kOpAgentTurn, "") ==
        fleet::kOffloadAlways;
    if (fleet_only && members.empty()) {
      return make_error(Code::Unsupported, msg::kFleetOnlyNoMember);
    }
    if (members.empty() && m == nullptr) {
      return make_error(Code::Unsupported, msg::kNoAssistantModel);
    }
    if (members.empty() && !here) {
      return make_error(Code::Unsupported, msg::kEngineCannotRun,
                        {{"operation", std::string(engine::kOpAgentTurn)}});
    }

    engine::JobSpec spec;
    spec.id = JobId::make();
    spec.op = std::string(engine::kOpAgentTurn);
    spec.params = m ? _c.helper_params_(*m) : Json::object();
    spec.params["disable_thinking"] = true;
    spec.params["conversation"] = agent::conversation_beat(
        messages, tools, _key, _dir / "context", kReplyTokens);
    for (const auto& msg : messages) {
      for (const auto& p : msg.images) {
        engine::JobInput in;
        in.role = "image";
        in.path = p;
        spec.inputs.push_back(std::move(in));
      }
    }
    spec.output_dir = _dir / "out";
    if (m != nullptr && members.empty()) {
      VALTZ_ASSIGN(spec.model, _c.resolve_model_(*m, false));
    }

    struct Wait {
      std::mutex              mu;
      std::condition_variable cv;
      bool                    done = false;
      Json                    turn;
      std::string             error;
      Code                    code = Code::Ok;
    };
    auto w = std::make_shared<Wait>();
    engine::JobSink sink = [w, on_text](const engine::JobEvent& ev) {
      using K = engine::JobEventKind;
      if (ev.kind == K::Text) {
        if (on_text && !ev.text.empty()) {
          on_text(ev.text);
        }
        return;
      }
      std::lock_guard lk(w->mu);
      if (ev.kind == K::Output) {
        w->turn = ev.data;
      } else if (ev.kind == K::Failed) {
        w->error = ev.text.empty() ? std::string("the turn failed")
                                   : ev.text;
        w->code = ev.error == Code::Ok ? Code::Engine : ev.error;
      } else if (ev.kind == K::Cancelled) {
        w->code = Code::Cancelled;
        w->error = "stopped";
      }
      if (ev.terminal()) {
        w->done = true;
        w->cv.notify_all();
      }
    };
    // In the job table, so a stop reaches it where it runs; never a task.
    JobRecord rec;
    rec.id = spec.id;
    rec.op = spec.op;
    rec.purpose = "agent";
    rec.title = "Agent turn";
    rec.created_ms = now_ms();
    _c._jobs.add(rec);
    const JobId id = spec.id;
    if (members.empty()) {
      {
        std::lock_guard lk(_mu);
        _where.clear();
        _member_context = 0;
      }
      VALTZ_TRY(_c._engine->submit(std::move(spec), std::move(sink)));
    } else {
      fleet::Offer offer;
      offer.id = id;
      offer.op = spec.op;
      offer.title = "Agent turn";
      offer.params = {{"conversation", spec.params["conversation"]},
                      {"lease", _lease}};
      offer.inputs = spec.inputs;
      {
        std::lock_guard lk(_mu);
        _member = jget<std::string>(members.front(), "id", "");
        _where = jget<std::string>(members.front(), "name", "");
        // Its agent model's, which serves the turn; a member from before
        // says only its helper's.
        const Json self = jget(members.front(), "self", Json::object());
        _member_context = jget<std::int64_t>(
            self, "agent_context",
            jget<std::int64_t>(self, "assistant_context", 0));
      }
      std::optional<engine::JobSpec> local;
      if (here && !fleet_only) {
        VALTZ_ASSIGN(spec.model, _c.resolve_model_(*m, false));
        local = std::move(spec);
      }
      VALTZ_TRY(_c.run_remote_(std::move(members), std::move(offer),
                               _dir / "out", std::move(local),
                               std::move(sink)));
    }
    {
      std::unique_lock lk(w->mu);
      bool asked = false;
      while (!w->done) {
        w->cv.wait_for(lk, std::chrono::milliseconds(100));
        if (cancel.load() && !asked) {
          asked = true;
          lk.unlock();
          (void)_c.cancel(id);
          lk.lock();
        }
      }
    }
    if (w->code != Code::Ok) {
      // No turn at all near its context's end: its prefill found no
      // pages (vpipe's log says so) -- a context full too.
      const std::int64_t cap = context_tokens();
      if (w->error.find("returned no turn") != std::string::npos &&
          cap > 0 && agent::estimate_tokens(messages) +
                             agent::estimate_tokens(tools) >= cap / 2) {
        return make_error(Code::OutOfMemory, msg::kAgentContextFull,
                          {{"tokens", std::to_string(cap)}});
      }
      return make_error(w->code, w->error);
    }
    if (!w->turn.is_object() || w->turn.empty()) {
      return make_error(Code::Engine, "the helper returned no turn");
    }
    // text-chat's beat for a conversation it did not take -- no finish,
    // no words: its prefill failed, the K/V pool full (vpipe's log says
    // so) -- or no model, no conversation read. Near its context, the
    // first: compacted and tried again.
    if (!w->turn.contains("finish") &&
        jget<std::string>(w->turn, "text", "").empty()) {
      const std::int64_t cap = context_tokens();
      if (cap <= 0 || agent::estimate_tokens(messages) +
                              agent::estimate_tokens(tools) >= cap / 2) {
        return make_error(Code::OutOfMemory, msg::kAgentContextFull,
                          {{"tokens", std::to_string(cap)}});
      }
      return make_error(Code::Engine,
                        "the helper took no turn (the log says why)");
    }
    // Its reply cut off by the CONTEXT: text-chat decodes a reply only as
    // far as its K/V pages go -- "length" at the last page, as if its
    // reply budget were spent -- or its decode failed there ("error":
    // pages memory did not give). A context full: compacted and tried
    // again, or, compaction off, said. Taken as it came, the turn ended
    // "done" on the words so far -- no error, nothing compacted
    // (2026-11-04: Flash-Next on the M5 Ultra, reproduced with the 9B
    // at a 1,024-token context: 919 tokens decoded to ctx_pos 1024).
    {
      const std::string finish = jget<std::string>(w->turn, "finish", "");
      const std::int64_t pos = jget<std::int64_t>(w->turn, "ctx_pos", 0);
      const std::int64_t cap = context_tokens();
      if (finish == "error" ||
          (finish == "length" && cap > 0 && pos + kPageTokens >= cap)) {
        VALTZ_LOG_WARN("agent", "the helper's reply was cut off by its "
                       "context ({}, ctx_pos {} of {})", finish, pos, cap);
        return make_error(Code::OutOfMemory, msg::kAgentContextFull,
                          {{"tokens", std::to_string(cap)}});
      }
    }
    agent::Reply r = agent::reply_from_turn(w->turn, ++_turns);
    r.limit = kReplyTokens;
    return r;
  }

  void
  close() override
  {
    if (_c._engine) {
      _c._engine->drop_context(_key);
    }
    std::string member;
    {
      std::lock_guard lk(_mu);
      member = _member;
    }
    // The member that held it told to let go (its lease would, later).
    if (!member.empty() && _c._fleet) {
      fleet::Offer offer;
      offer.id = JobId::make();
      offer.op = "agent-close";
      offer.title = "Agent closed";
      offer.params = {{"context", _key}};
      (void)_c._fleet->submit(member, offer, _dir / "out",
                              [](const engine::JobEvent&) {});
    }
    std::error_code ec;
    fs::remove_all(_dir / "context", ec);
  }

private:
  Controller&        _c;
  const std::string  _key;
  const fs::path     _dir;
  const double       _lease;
  mutable std::mutex _mu;
  std::string        _member;   // the member holding the conversation
  std::string        _where;    // its name; "" this Mac
  // Its helper's context, as it said; 0: this Mac's (or it said none).
  std::int64_t       _member_context = 0;
  // This Mac's, read once (its config); -1 not yet.
  mutable std::int64_t _local_context = -1;
  int                _turns = 0;
};

// ---- the host -----------------------------------------------------------

Controller::AgentHost::AgentHost(Controller& c) : _c(c)
{
  _settings = read_json(_c._paths.support / "agent.json");
  _house = std::thread([this] { housekeeping_(); });
}

Controller::AgentHost::~AgentHost()
{
  shutdown();
}

void
Controller::AgentHost::shutdown()
{
  std::vector<std::shared_ptr<Session>> all;
  {
    std::lock_guard lk(_mu);
    if (_stop) {
      return;
    }
    _stop = true;
    for (auto& [id, s] : _sessions) {
      all.push_back(s);
    }
  }
  _cv.notify_all();
  for (auto& s : all) {
    (void)stop(s->id);
  }
  for (auto& s : all) {
    if (s->thread.joinable()) {
      s->thread.join();
    }
  }
  if (_house.joinable()) {
    _house.join();
  }
}

std::shared_ptr<Controller::AgentHost::Session>
Controller::AgentHost::find_(const std::string& id) const
{
  std::lock_guard lk(_mu);
  auto it = _sessions.find(id);
  return it == _sessions.end() ? nullptr : it->second;
}

std::string
Controller::AgentHost::model_choice() const
{
  std::lock_guard lk(_mu);
  return jget<std::string>(_settings, "model", "");
}

int
Controller::AgentHost::memory_gb() const
{
  std::lock_guard lk(_mu);
  return std::max(0, jget(_settings, "memory_gb", 0));
}

int
Controller::AgentHost::expert_cache_gb() const
{
  std::lock_guard lk(_mu);
  return std::max(0, jget(_settings, "expert_cache_gb", 0));
}

std::string
Controller::AgentHost::i8_prefill() const
{
  std::lock_guard lk(_mu);
  return jget<std::string>(_settings, "i8_prefill", "auto");
}

bool
Controller::AgentHost::ngram_resident() const
{
  std::lock_guard lk(_mu);
  return jget(_settings, "ngram_resident", false);
}

Json
Controller::AgentHost::settings() const
{
  // The model that runs asked first, and what it holds: agent_model and
  // the plan's sizes take this lock.
  const auto* using_ = _c.agent_model();
  Json plan = _c.agent_memory_plan_();
  // Whether that model reads its prompts in int8 by itself (catalog
  // engine.vpipe.i8_prefill: Flash-Next's) -- what Auto means for it --
  // and whether this GPU runs the int8 GEMMs at all (an M5's matrix
  // cores: vpipe ignores the flag without them).
  const bool own_i8 =
      using_ != nullptr &&
      jget(jget(using_->engine, "vpipe", Json::object()), "i8_prefill",
           false);
  const bool cores = _c._hw.gpu_matrix_cores;
  // The context a new session's model would have here (its backend's
  // context_tokens): the chat's ring before anything is said.
  const std::int64_t helper_context =
      using_ != nullptr ? _c.helper_context_(*using_) : 0;
  std::lock_guard lk(_mu);
  Json s = {{"backend", jget<std::string>(_settings, "backend", "helper")},
            {"model", jget<std::string>(_settings, "model", "")},
            {"using", using_ ? using_->id : std::string()},
            {"memory_gb", std::max(0, jget(_settings, "memory_gb", 0))},
            {"expert_cache_gb",
             std::max(0, jget(_settings, "expert_cache_gb", 0))},
            {"i8_prefill",
             jget<std::string>(_settings, "i8_prefill", "auto")},
            {"i8_prefill_model", own_i8},
            {"ngram_resident", jget(_settings, "ngram_resident", false)},
            {"matrix_cores", cores},
            {"plan", std::move(plan)},
            {"review", jget(_settings, "review", true)},
            {"lease", jget(_settings, "lease", kDefaultLease)},
            {"compact", jget(_settings, "compact", true)}};
  agent::CloudConfig cloud =
      agent::cloud_config_from_json(jget(_settings, "cloud", Json::object()));
  if (cloud.provider.empty()) {
    cloud.provider = "anthropic";
  }
  if (cloud.model.empty()) {
    cloud.model = agent::default_cloud_model(cloud.provider);
  }
  s["cloud"] = agent::to_json(cloud);
  s["context_tokens"] = jget<std::string>(s, "backend", "") == "cloud"
                            ? agent::cloud_context_tokens(cloud)
                            : helper_context;
  Json keys = Json::object();
  for (const char* p : {"anthropic", "openai", "openai-compatible"}) {
    keys[p] = agent::has_api_key(p);
  }
  s["keys"] = std::move(keys);
  return s;
}

Status
Controller::AgentHost::set_settings(const Json& j)
{
  // The model named: a helper of the catalog ("" the Agentic Helper's),
  // here or not -- downloaded later, it is taken then.
  if (j.contains("model")) {
    const std::string id = jget<std::string>(j, "model", "");
    const auto* m = id.empty() ? nullptr : _c._catalog.find(id);
    if (!id.empty() && (m == nullptr || m->role != "assistant")) {
      return make_error(Code::InvalidArgument,
                        std::format("no helper '{}' for the agent", id));
    }
  }
  if (j.contains("memory_gb")) {
    const int gb = jget(j, "memory_gb", -1);
    if (gb != 0 && (gb < 16 || gb > 4096)) {
      return make_error(Code::InvalidArgument, std::format(
          "the agent's memory is 0 (no limit) or 16 to 4096 GB, not {}",
          to_text(j["memory_gb"])));
    }
  }
  // Its expert cache: Auto, or a size -- under vpipe's floor (two layers'
  // experts, ~15 GB for DeepSeek) it runs at the floor.
  if (j.contains("expert_cache_gb")) {
    const int gb = jget(j, "expert_cache_gb", -1);
    if (gb != 0 && (gb < 8 || gb > 4096)) {
      return make_error(Code::InvalidArgument, std::format(
          "the agent's expert cache is 0 (Auto) or 8 to 4096 GB, not {}",
          to_text(j["expert_cache_gb"])));
    }
  }
  // Its prompts read in int8: the model's own way, always, or never.
  if (j.contains("i8_prefill")) {
    const std::string v = jget<std::string>(j, "i8_prefill", "");
    if (v != "auto" && v != "on" && v != "off") {
      return make_error(Code::InvalidArgument, std::format(
          "the agent's int8 prefill is auto, on or off, not {}",
          to_text(j["i8_prefill"])));
    }
  }
  // Its n-gram table held in memory (vpipe's ngram_resident): on or off.
  if (j.contains("ngram_resident") && !j["ngram_resident"].is_boolean()) {
    return make_error(Code::InvalidArgument, std::format(
        "the agent's n-gram table in memory is true or false, not {}",
        to_text(j["ngram_resident"])));
  }
  std::lock_guard lk(_mu);
  Json next = _settings;
  if (j.contains("model")) {
    next["model"] = jget<std::string>(j, "model", "");
  }
  if (j.contains("ngram_resident")) {
    next["ngram_resident"] = jget(j, "ngram_resident", false);
  }
  if (j.contains("i8_prefill")) {
    next["i8_prefill"] = jget<std::string>(j, "i8_prefill", "auto");
  }
  if (j.contains("memory_gb")) {
    next["memory_gb"] = jget(j, "memory_gb", 0);
  }
  if (j.contains("expert_cache_gb")) {
    next["expert_cache_gb"] = jget(j, "expert_cache_gb", 0);
  }
  if (j.contains("backend")) {
    const std::string b = jget<std::string>(j, "backend", "helper");
    if (b != "helper" && b != "cloud") {
      return make_error(Code::InvalidArgument,
                        std::format("no agent backend '{}'", b));
    }
    next["backend"] = b;
  }
  if (j.contains("review")) {
    next["review"] = jget(j, "review", true);
  }
  if (j.contains("compact")) {
    next["compact"] = jget(j, "compact", true);
  }
  if (j.contains("lease")) {
    next["lease"] = std::clamp(jget(j, "lease", kDefaultLease), 60.0,
                               3600.0);
  }
  if (j.contains("cloud")) {
    Json cur = jget(next, "cloud", Json::object());
    // Bound first: items() of the temporary dangles (C++20 range-for).
    const Json given = jget(j, "cloud", Json::object());
    for (const auto& [k, v] : given.items()) {
      cur[k] = v;
    }
    const auto c = agent::cloud_config_from_json(cur);
    if (!c.provider.empty() && c.provider != "anthropic" &&
        c.provider != "openai" && c.provider != "openai-compatible") {
      return make_error(Code::InvalidArgument,
                        std::format("no cloud provider '{}'", c.provider));
    }
    next["cloud"] = agent::to_json(c);
  }
  VALTZ_TRY(write_json(_c._paths.support / "agent.json", next));
  _settings = std::move(next);
  return ok_status();
}

std::unique_ptr<agent::Backend>
Controller::AgentHost::backend_(const std::string& kind,
                                const std::string& key, const fs::path& dir)
{
  if (_c._cfg.agent_backend) {
    return _c._cfg.agent_backend();
  }
  Json st;
  {
    std::lock_guard lk(_mu);
    st = _settings;
  }
  if (kind == "cloud") {
    auto cfg = agent::cloud_config_from_json(jget(st, "cloud",
                                                  Json::object()));
    if (cfg.provider.empty()) {
      cfg.provider = "anthropic";
    }
    return agent::make_cloud_backend(std::move(cfg));
  }
  return std::make_unique<HelperBackend>(
      _c, key, dir, jget(st, "lease", kDefaultLease));
}

std::string
Controller::AgentHost::system_text_(const Session& s)
{
  const std::string today = [] {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return std::string(b);
  }();
  // Without Valtz's tools: no guide -- a few words, the conversation's
  // room the person's (the guide and the tools take some 20,000 tokens).
  if (s.skills == "none") {
    return std::format(
        "You are a helpful assistant, chatting with the person in Valtz, "
        "an app for making pictures, clips and sound. You have no tools "
        "here: answer in words. The pictures the person attaches are "
        "shown to you when you can see pictures.\n\nToday is {}.\n",
        today);
  }
  std::string t = _c.skill_text(s.skills == "chat" ? "valtz-chat"
                                : s.skills == "procedural"
                                    ? "valtz-procedural"
                                    : "valtz-agent");
  t += "\n## This session\n\n";
  t += std::format("Today is {}.\n", today);
  // A procedural session draws at the person's size and length: the
  // card they set -- what they asked for in words alone overriding it --
  // or defaults.
  if (s.skills == "procedural") {
    if (const std::string card = card_text_(s.render); !card.empty()) {
      if (jget(s.render, "card", false)) {
        t += "The person's card says: draw " + card +
             ". render_program draws at it: a `width`, `height`, "
             "`seconds` or `fps` of yours is taken only where the "
             "person's own words name it (\"a 10 s loop\", \"1920 x "
             "1080\"), so leave them out unless they do.\n";
      } else {
        t += "Draw " + card + " -- render_program's defaults -- unless "
             "the person asks for another size or length.\n";
      }
    }
  }
  if (auto* p = _c.project(s.project)) {
    if (s.target) {
      if (auto a = p->asset(*s.target); a.ok()) {
        auto size = _c.own_frame(s.project, *s.target);
        const Rational rate = _c.rate_of_(*p, *a);
        auto n = _c.composition_length(s.project, *s.target);
        const double secs =
            n.ok() ? static_cast<double>(*n) / rate.to_double() : 0;
        if (a->cls == project::AssetClass::Still) {
          t += std::format(
              "The ACTIVE composition is \"{}\" (id {}), a still, {} x {} "
              "pixels, {} page{}.\n",
              a->name, s.target->str(), size.ok() ? size->width : 0,
              size.ok() ? size->height : 0, a->pages,
              a->pages == 1 ? "" : "s");
        } else if (a->kind == project::AssetKind::Audio) {
          t += std::format(
              "The ACTIVE composition is \"{}\" (id {}), a composition "
              "of sound, {:.1f} s long.\n",
              a->name, s.target->str(), secs);
        } else {
          t += std::format(
              "The ACTIVE composition is \"{}\" (id {}), a timeline, {} "
              "x {} pixels at {:.3g} fps, {:.1f} s long, {} layer{}.\n",
              a->name, s.target->str(), size.ok() ? size->width : 0,
              size.ok() ? size->height : 0, rate.to_double(), secs,
              a->layers.size(), a->layers.size() == 1 ? "" : "s");
        }
      }
    } else {
      t += "No composition is active.\n";
    }
    if (!s.row.empty()) {
      t += "The person's prompt row holds:\n";
      for (const auto& id : s.row) {
        auto a = p->asset(id);
        if (!a.ok()) {
          continue;
        }
        std::string what = project::to_str(a->kind);
        if (Controller::is_program(*a)) {
          what = "a program: read_program shows its code";
        } else if (s.skills == "procedural") {
          // What a program reads it as (DESIGN §4t).
          what += a->kind == project::AssetKind::Video
                      ? std::format(" -- await valtz.frame(\"{}\", t)",
                                    a->name)
                  : a->kind == project::AssetKind::Image
                      ? std::format(" -- await valtz.image(\"{}\")",
                                    a->name)
                  : a->kind == project::AssetKind::Text
                      ? std::format(" -- await valtz.text(\"{}\")",
                                    a->name)
                      : std::format(" -- await valtz.bytes(\"{}\")",
                                    a->name);
        }
        // A clip's or a sound's length, a clip's size and rate.
        if (auto v = p->version(id);
            v.ok() && (a->kind == project::AssetKind::Video ||
                       a->kind == project::AssetKind::Audio)) {
          what += std::format(", {:.1f} s", v->info.duration.seconds());
          if (a->kind == project::AssetKind::Video) {
            what += std::format(
                ", {} x {} at {:.3g} fps{}", v->info.frame.width,
                v->info.frame.height, v->info.frame_rate.to_double(),
                v->info.has_audio ? ", with sound" : ", silent");
          }
        }
        t += std::format("- \"{}\" (id {}), {}\n", a->name, id.str(),
                         what);
      }
    }
  }
  return t;
}

std::string
Controller::AgentHost::with_stage_(ProjectId pid, const std::string& words,
                                  const Json& stage)
{
  auto* p = _c.project(pid);
  const auto cid =
      AssetId::parse(jget<std::string>(stage, "composition", ""));
  if (!p || !cid) {
    return words;
  }
  auto a = p->asset(*cid);
  if (!a.ok() || (a->cls != project::AssetClass::Still &&
                   a->cls != project::AssetClass::Composition)) {
    return words;
  }
  // The composition, as the model's tools name it.
  std::string where = std::format("\"{}\" (composition {}", a->name,
                                  cid->str());
  const auto page = jget<std::int64_t>(stage, "page", 0);
  const auto frame = jget<std::int64_t>(stage, "frame", -1);
  if (a->cls == project::AssetClass::Still) {
    where += std::format(", a still of {} page{})", a->pages,
                         a->pages == 1 ? "" : "s");
    if (page > 0) {
      where += std::format(", on page {}", page);
    }
  } else {
    const double fps = rate_(pid, *cid).to_double();
    auto length = _c.composition_length(pid, *cid);
    where += std::format(", a timeline at {:.3g} fps)", fps);
    if (frame >= 0) {
      where += std::format(", at frame {} ({:.2f} s{})", frame,
                           fps > 0 ? static_cast<double>(frame) / fps : 0.0,
                           length.ok() && *length > 0
                               ? std::format(" of {:.2f}",
                                             static_cast<double>(*length) /
                                                 std::max(1e-9, fps))
                               : std::string());
    }
  }
  if (stage.contains("layer") && stage["layer"].is_string()) {
    // Its id, or "0" for layer 0's "" (as the tools take it).
    std::string l = stage["layer"].get<std::string>();
    if (l == "0") { l.clear(); }
    const auto it = std::ranges::find_if(
        a->layers, [&](const project::Layer& x) { return x.id == l; });
    if (it != a->layers.end()) {
      std::string named = it->name;
      if (named.empty() && it->source) {
        if (auto src = p->asset(*it->source); src.ok()) {
          named = src->name;
        }
      }
      // Layer 0's id is "", as the tools name it "0".
      where += std::format("; layer {}{} selected", l.empty() ? "0" : l,
                           named.empty() ? std::string()
                                         : " (\"" + named + "\")");
      std::string objs;
      for (const auto& o : jget(stage, "objects", Json::array())) {
        if (o.is_string()) {
          objs += (objs.empty() ? "" : ", ") + o.get<std::string>();
        }
      }
      if (!objs.empty()) {
        where += ", with its objects " + objs;
      }
    }
  }
  return words + "\n\n[Where the person is as they say this: " + where +
         ". \"This page\", \"here\", \"the current frame\" and \"the "
         "selected layer\" mean these.]";
}

agent::Message
Controller::AgentHost::person_message_(ProjectId pid,
                                       const std::string& words,
                                       const Json& stage,
                                       const std::vector<AssetId>& attached,
                                       bool list, bool tools)
{
  // Where they are, for the tools to look at; without tools, words the
  // model can do nothing with (a chat without them answered about the
  // composition it was told of).
  agent::Message m{.role = "user",
                   .content = tools ? with_stage_(pid, words, stage)
                                    : words};
  m.said = words;
  auto* p = _c.project(pid);
  std::string told;
  for (const auto& id : attached) {
    auto a = p ? p->asset(id) : Result<project::Asset>(make_error(
                                    Code::NotFound, msg::kProjectNotOpen));
    if (!a.ok() || std::ranges::find(m.attached, id.str()) !=
                       m.attached.end()) {
      continue;
    }
    m.attached.push_back(id.str());
    std::string what = project::to_str(a->kind);
    if (auto v = p->version(id); v.ok() && v->info.frame.width > 0) {
      what += std::format(", {} x {}", v->info.frame.width,
                          v->info.frame.height);
    }
    told += tools ? std::format("{}\"{}\" (id {}, {})",
                                told.empty() ? "" : "; ", a->name,
                                id.str(), what)
                  : std::format("{}\"{}\" ({})", told.empty() ? "" : "; ",
                                a->name, what);
  }
  if (list && !told.empty()) {
    m.content += "\n[The person attached: " + told +
                 (tools ? ". Your tools take them by these ids.]" : ".]");
  }
  return m;
}

void
Controller::AgentHost::show_attached_(Session& s, agent::Message& m)
{
  if (m.attached.empty() || !s.backend || !s.backend->sees()) {
    return;
  }
  auto* p = _c.project(s.project);
  if (p == nullptr) {
    return;
  }
  for (const auto& t : m.attached) {
    if (m.images.size() >= kShownAttached) {
      break;
    }
    const auto id = AssetId::parse(t);
    if (!id) {
      continue;
    }
    auto a = p->asset(*id);
    // Pictures and stills (a still's first page); a clip or a sound is
    // listed, the tools look at it.
    if (!a.ok() || a->head == 0 ||
        (a->kind != project::AssetKind::Image &&
         a->cls != project::AssetClass::Still)) {
      continue;
    }
    const std::string name = "attached-" + JobId::make().str();
    Result<fs::path> drawn = make_error(Code::NotFound, "not drawn");
    if (a->cls == project::AssetClass::Still) {
      const fs::path f = s.dir / (name + ".png");
      auto st = _c.flatten(s.project, *id, f, std::nullopt, std::nullopt,
                           {}, 0);
      drawn = st.ok() ? Result<fs::path>(f) : Result<fs::path>(st.error());
    } else {
      drawn = _c.rendered_frame_(s.project, *id, 0, 0, 0);
    }
    const fs::path small = s.dir / (name + ".jpg");
    if (!drawn.ok() ||
        !media::write_thumbnail(*drawn, media::MediaType::Image,
                                kAttachedSide, small).ok()) {
      VALTZ_LOG_WARN("agent", "session {}: \"{}\" attached could not be "
                     "shown", s.id, a->name);
      continue;
    }
    if (a->cls == project::AssetClass::Still) {
      std::error_code ec;
      fs::remove(*drawn, ec);
    }
    m.images.push_back(small);
  }
}

void
Controller::AgentHost::take_said_(Session& s)
{
  std::deque<agent::Message> said;
  {
    std::lock_guard lk(s.mu);
    said.swap(s.said);
  }
  if (said.empty()) {
    return;
  }
  for (auto& m : said) {
    show_attached_(s, m);
  }
  // Only this thread adds to the conversation: in the order said.
  std::lock_guard lk(s.mu);
  for (auto& m : said) {
    s.messages.push_back(std::move(m));
  }
}

std::int64_t
Controller::AgentHost::context_estimate_(const Session& s) const
{
  // (The session's lock held.) The guide and the tools are most of a
  // first turn: counted once.
  static const std::int64_t full = agent::estimate_tokens(tool_specs());
  static const std::int64_t chat =
      agent::estimate_tokens(tool_specs("chat"));
  static const std::int64_t procedural =
      agent::estimate_tokens(tool_specs("procedural"));
  const std::int64_t specs = s.skills == "full"         ? full
                             : s.skills == "chat"       ? chat
                             : s.skills == "procedural" ? procedural
                                                        : 0;
  if (s.ctx_used > 0 && s.ctx_counted <= s.messages.size()) {
    return s.ctx_used +
           agent::estimate_tokens(s.messages, s.ctx_counted) * 5 / 4;
  }
  return agent::estimate_tokens(s.messages) + specs;
}

// ---- the transcript (DESIGN §9b) -------------------------------------

std::string
Controller::AgentHost::transcript_text_(const Session& s) const
{
  const Json backend = s.backend ? s.backend->describe() : Json::object();
  const std::int64_t limit = s.backend ? s.backend->context_tokens() : 0;
  std::string works_on;
  if (s.target) {
    if (auto* p = _c.project(s.project)) {
      if (auto a = p->asset(*s.target); a.ok()) {
        works_on = a->name;
      }
    }
  }
  std::lock_guard lk(s.mu);
  auto names = [&](const std::vector<std::string>& ids) {
    std::string out;
    auto* p = _c.project(s.project);
    for (const auto& t : ids) {
      const auto id = AssetId::parse(t);
      auto a = p && id ? p->asset(*id)
                       : Result<project::Asset>(make_error(
                             Code::NotFound, "no such asset"));
      out += (out.empty() ? "" : ", ") +
             (a.ok() ? std::format("“{}” ({})", a->name,
                                   project::to_str(a->kind))
                     : std::format("an asset since removed ({})", t));
    }
    return out;
  };

  std::string t = "# Agent: " + cut(one_line(s.prompt), 80) + "\n\n";
  // Who thought, on what, how far it went.
  std::string model = jget<std::string>(backend, "model", "");
  const std::string where = jget<std::string>(backend, "where", "");
  if (jget<std::string>(backend, "kind", "") == "local") {
    model += model.empty() ? "this Mac's helper" : " on this Mac";
  } else if (!where.empty()) {
    model += (model.empty() ? "" : " on ") + where;
  }
  t += "**Began** " + local_time(s.created_ms);
  if (!model.empty()) {
    t += " · **Thinks on** " + model;
  }
  if (!works_on.empty() && s.skills == "full") {
    t += " · **Works on** “" + works_on + "”";
  }
  // A chat's tools, or none: the editor's go unsaid.
  if (s.skills == "chat") {
    t += " · **Tools** the chat's: it looks and adds";
  } else if (s.skills == "procedural") {
    t += " · **Tools** procedural: it writes programs and draws them";
  } else if (s.skills == "none") {
    t += " · **Tools** none";
  }
  t += "\n";
  std::map<std::string, int> by_state;
  for (const auto& st : s.steps) {
    ++by_state[st.state];
  }
  std::string counts;
  for (const auto& [state, n] : by_state) {
    counts += std::format("{}{} {}", counts.empty() ? "" : ", ", n, state);
  }
  t += std::format("**Steps** {}{} · **Turns** {}", s.steps.size(),
                   counts.empty() ? "" : " (" + counts + ")", s.turns);
  if (s.compactions > 0) {
    t += std::format(" · **Compacted** {} time{}", s.compactions,
                     s.compactions == 1 ? "" : "s");
  }
  if (limit > 0) {
    t += std::format(" · **Context** about {} of {} tokens",
                     context_estimate_(s), limit);
  }
  t += "\n**State** " + s.state + " · written " +
       local_time(now_ms()) + "\n";
  if (!s.error.empty()) {
    t += "**Error** " + cut(one_line(s.error), kTranscriptNote) + "\n";
  }

  // The conversation, as the app's chat shows it: the person's words,
  // the agent's, its steps under the turn that asked for them.
  std::size_t next = 0;
  std::string last;   // who spoke last: "you", "agent"
  auto heading = [&](const char* who, const char* title) {
    if (last != who) {
      t += std::string("\n## ") + title + "\n\n";
      last = who;
    }
  };
  auto step_text = [&](const Step& st) {
    std::string line = std::format("{}. **{}** ({}) — ", st.n,
                                   st.title, code_span(st.tool, 64));
    if (st.state == "failed") {
      line += "failed: " + cut(one_line(st.error), kTranscriptResult);
    } else if (st.state == "skipped") {
      line += "skipped";
      const std::string note = jget<std::string>(st.result, "note", "");
      if (!note.empty()) {
        line += ": " + cut(one_line(note), kTranscriptResult);
      }
    } else if (st.state == "proposed") {
      line += "waiting for your word";
    } else {
      line += st.state;
    }
    line += "\n";
    if (st.tool == "ask_user") {
      const std::string q = jget<std::string>(st.args, "question", "");
      const std::string a = jget<std::string>(st.result, "answer", "");
      line += "   It asked: " + cut(one_line(q), kTranscriptNote) + "\n";
      if (!a.empty()) {
        agent::Message said{.role = "user", .content = a};
        line += "   You answered: " +
                cut(one_line(person_words_of(said)), kTranscriptNote) +
                "\n";
      }
      return line;
    }
    // A program's code (render_program's): a fenced block of its own,
    // whole and as written, not cut in a line of JSON escapes.
    Json args = st.args;
    std::string code;
    if (args.is_object() && args.contains("code") &&
        args["code"].is_string()) {
      code = args["code"].get<std::string>();
      args.erase("code");
    }
    if (!args.empty()) {
      line += "   " + code_span(to_text(args), kTranscriptArgs) + "\n";
    }
    if (st.state == "done" || st.state == "waiting") {
      if (!st.result.is_null() && !st.result.empty()) {
        line += "   → " +
                code_span(to_text(st.result), kTranscriptResult) + "\n";
      }
    }
    if (!one_line(code).empty()) {
      line += code_block(code, "javascript");
    }
    return line;
  };
  auto show = [&](const agent::Message& m) {
    if (m.role == "system" || m.role == "tool") {
      return;
    }
    if (agent::is_compaction(m)) {
      t += std::format(
          "\n*Compacted here: the {} messages before it were summarized "
          "for the model (about {} of {} tokens); this transcript keeps "
          "them whole.*\n",
          jget(m.stats, "compacted", 0),
          jget<std::int64_t>(m.stats, "tokens", 0),
          jget<std::int64_t>(m.stats, "limit", 0));
      return;
    }
    if (m.role == "user") {
      if (m.said.empty() && m.content.starts_with("[Valtz]")) {
        std::string note = one_line(m.content.substr(8));
        if (!m.images.empty()) {
          note += std::format(" ({} picture{} shown)", m.images.size(),
                              m.images.size() == 1 ? "" : "s");
        }
        t += "\n*Valtz: " + cut(note, kTranscriptNote) + "*\n";
        return;
      }
      heading("you", "You");
      t += person_words_of(m);
      t += "\n";
      if (const std::string w = where_said(m.content); !w.empty()) {
        t += "\n*Where: " + one_line(w) + "*\n";
      }
      if (!m.attached.empty()) {
        t += "\n*Attached: " + names(m.attached) + "*\n";
      }
      return;
    }
    if (m.role == "assistant") {
      const std::string words = one_line(m.content).empty()
                                    ? std::string()
                                    : m.content;
      if (!words.empty() || !m.calls.empty()) {
        heading("agent", "Agent");
      }
      if (!words.empty()) {
        t += words;
        t += "\n";
      }
      if (const int n = jget(m.stats, "cut_off", -1); n >= 0) {
        t += n > 0 ? std::format("\n*Cut off here: the reply reached its "
                                 "limit of {} tokens.*\n", n)
                   : std::string("\n*Cut off here: the reply reached its "
                                 "limit.*\n");
      }
      if (!m.calls.empty()) {
        t += "\n";
        for (std::size_t i = 0; i < m.calls.size() &&
                                next < s.steps.size(); ++i) {
          t += step_text(s.steps[next++]);
        }
      }
    }
  };
  const std::size_t head = agent::compaction_head(s.messages);
  for (std::size_t i = 0; i < head; ++i) {
    show(s.messages[i]);
  }
  for (const auto& m : s.earlier) {
    show(m);
  }
  for (std::size_t i = head; i < s.messages.size(); ++i) {
    show(s.messages[i]);
  }
  // Steps of a turn not yet written back.
  if (next < s.steps.size()) {
    heading("agent", "Agent");
    t += "\n";
    while (next < s.steps.size()) {
      t += step_text(s.steps[next++]);
    }
  }
  return t;
}

void
Controller::AgentHost::write_transcript_(Session& s)
{
  {
    std::lock_guard lk(_mu);
    if (_stop) {
      return;
    }
  }
  auto* p = _c.project(s.project);
  if (p == nullptr) {
    return;
  }
  const std::string text = transcript_text_(s);
  // The session's own write: never refused by a hold (a session's
  // working again meanwhile).
  CoreWrites own;
  std::optional<AssetId> id;
  {
    std::lock_guard lk(s.mu);
    id = s.transcript;
  }
  if (id) {
    // Removed by the person: left so.
    if (!p->asset(*id).ok()) {
      return;
    }
    if (auto was = p->read_text(*id); was.ok() && *was == text) {
      return;
    }
    if (auto r = p->update_text(*id, text); !r.ok()) {
      VALTZ_LOG_WARN("agent", "session {}: its transcript was not "
                     "written: {}", s.id, r.error().message);
      return;
    }
    _c.post_("assets.changed", JobId{}, {{"project", s.project},
                                         {"asset", *id},
                                         {"reason", "edited"}});
    return;
  }
  auto made = p->add_text("Agent: " + cut(one_line(s.prompt), 60), text,
                          {kTranscriptTag});
  if (!made.ok()) {
    VALTZ_LOG_WARN("agent", "session {}: its transcript was not made: {}",
                   s.id, made.error().message);
    return;
  }
  // In the Agent Sessions folder -- found by its role, whatever the
  // person renamed it; made the first time.
  if (auto folders = p->folders(); folders.ok()) {
    std::string folder;
    for (const auto& f : *folders) {
      if (jget<std::string>(f, "role", "") == kTranscriptFolderRole) {
        folder = jget<std::string>(f, "id", "");
      }
    }
    if (folder.empty()) {
      folder = AssetId::make().str();
      folders->push_back({{"id", folder},
                          {"name", kTranscriptFolderName},
                          {"role", kTranscriptFolderRole}});
      if (!p->set_folders(*folders).ok()) {
        folder.clear();
      }
    }
    if (!folder.empty()) {
      (void)p->set_asset_folder(made->id, folder);
    }
  }
  std::string state;
  {
    std::lock_guard lk(s.mu);
    s.transcript = made->id;
    state = s.state;
  }
  VALTZ_LOG_INFO("agent", "session {}: its transcript is \"{}\"", s.id,
                 made->name);
  _c.post_("assets.changed", JobId{}, {{"project", s.project},
                                       {"asset", made->id},
                                       {"reason", "agent-transcript"}});
  // The app's card links to it.
  _c.post_("agent.changed", JobId{},
           {{"agent", s.id}, {"project", s.project.str()},
            {"state", state}});
}

Result<std::string>
Controller::AgentHost::start(AgentRequest req)
{
  if (one_line(req.prompt).empty()) {
    return make_error(Code::InvalidArgument, msg::kAgentEmptyRequest);
  }
  if (req.skills.empty()) {
    req.skills = "full";
  }
  if (req.skills != "full" && req.skills != "chat" &&
      req.skills != "procedural" && req.skills != "none") {
    return make_error(Code::InvalidArgument,
                      std::format("no agent skills '{}': full, chat, "
                                  "procedural or none", req.skills));
  }
  auto* ws = _c.workspace_(req.project);
  if (ws == nullptr) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // One agent at a time CHANGES a project: a second with the editor's
  // tools refused while one is there to be spoken to (stopped, it is
  // not). Chats beside it look and add. Starts one at a time, so two
  // asked at once cannot both pass; each session's lock taken apart from
  // the host's, never one inside the other.
  std::lock_guard starting(_start_mu);
  if (req.skills == "full") {
    std::vector<std::shared_ptr<Session>> mine;
    {
      std::lock_guard lk(_mu);
      for (const auto& [id, o] : _sessions) {
        if (o->project == req.project && o->skills == "full") {
          mine.push_back(o);
        }
      }
    }
    for (const auto& o : mine) {
      std::lock_guard olk(o->mu);
      if (!o->stop_asked && o->state != "stopped") {
        return make_error(Code::Busy, msg::kAgentOneEditor);
      }
    }
  }
  auto s = std::make_shared<Session>();
  s->id = JobId::make().str();
  s->project = req.project;
  s->target = req.composition;
  if (!s->target) {
    s->target = _c.project_composition(req.project);
  }
  s->prompt = req.prompt;
  s->row = req.row;
  s->skills = req.skills;
  s->render = req.render.is_object() ? req.render : Json::object();
  s->person_words.push_back(req.prompt);
  s->review = req.review;
  s->created_ms = now_ms();
  // In the project's working copy: an anonymous session's goes with it.
  s->dir = ws->dir() / "agent" / s->id;
  std::error_code ec;
  fs::create_directories(s->dir, ec);
  std::string kind = req.backend;
  if (kind.empty()) {
    std::lock_guard lk(_mu);
    kind = jget<std::string>(_settings, "backend", "helper");
  }
  s->backend = backend_(kind, "valtz-agent-" + s->id, s->dir);
  {
    std::lock_guard lk(_mu);
    s->compact = jget(_settings, "compact", true);
  }
  s->messages.push_back({.role = "system", .content = system_text_(*s)});
  // The request, the row's media its attachments: the system message
  // lists them already -- by id, for the tools; with none, here.
  const bool tools = s->skills != "none";
  s->messages.push_back(person_message_(req.project, req.prompt, req.stage,
                                        req.row, /*list=*/!tools, tools));
  {
    std::lock_guard lk(_mu);
    if (_stop) {
      return make_error(Code::Cancelled, "shutting down");
    }
    _sessions[s->id] = s;
  }
  VALTZ_LOG_INFO("agent", "session {} started ({}, skills {})", s->id,
                 to_text(s->backend->describe()), s->skills);
  // Held from the request: read-only to the person until it is done --
  // an editor's; a chat holds nothing.
  s->working = true;
  if (s->skills == "full") {
    _c.agent_hold_(s->project, s->id, true);
  }
  s->thread = std::thread([this, s] {
    // Its writes are the agent's own: never refused by its hold -- nor a
    // chat's, adding to the assets, by an editor's.
    CoreWrites own;
    run_(*s);
    // Over: the project the person's again.
    _c.agent_hold_(s->project, s->id, false);
  });
  return s->id;
}

void
Controller::AgentHost::set_state_(Session& s, std::string state)
{
  {
    std::lock_guard lk(s.mu);
    s.state = std::move(state);
  }
  post_(s);
}

void
Controller::AgentHost::post_(Session& s, std::optional<int> step)
{
  Json d;
  bool working = false;
  {
    std::lock_guard lk(s.mu);
    d = {{"agent", s.id}, {"project", s.project.str()}, {"state", s.state}};
    working = s.state != "done" && s.state != "failed" &&
              s.state != "stopped";
  }
  if (step) {
    d["step"] = *step;
  }
  // At work -- paused, asking, waiting for a word too -- it holds the
  // project: read-only to the person (Controller::agent_hold_). Its last
  // command noted as it stops working, for the next words said to it.
  bool stopped_working = false;
  {
    std::lock_guard lk(s.mu);
    stopped_working = s.working && !working;
    s.working = working;
  }
  if (stopped_working) {
    const std::uint64_t last = last_command_(s.project);
    std::lock_guard lk(s.mu);
    s.seen_seq = last;
  }
  _c.agent_hold_(s.project, s.id, working && s.skills == "full");
  _c.post_("agent.changed", JobId{}, std::move(d));
  // At rest -- done, failed, stopped -- its transcript written: the
  // project the person's again first, a session's own write meanwhile
  // never in their command (commands are a thread's).
  if (stopped_working) {
    write_transcript_(s);
  }
}

std::uint64_t
Controller::AgentHost::last_command_(ProjectId pid) const
{
  project::Workspace* w = _c.workspace_(pid);
  if (!w) {
    return 0;
  }
  const auto c = w->undo().next_undo();
  return c ? c->seq : 0;
}

Json
Controller::AgentHost::state_json_(const Session& s, bool messages) const
{
  // The backend asked before the session's lock: a turn holds the
  // backend's own while it streams, and its words take the session's.
  const Json backend = s.backend ? s.backend->describe() : Json::object();
  const std::int64_t limit = s.backend ? s.backend->context_tokens() : 0;
  std::lock_guard lk(s.mu);
  Json steps = Json::array();
  for (const auto& st : s.steps) {
    Json j = {{"n", st.n}, {"tool", st.tool}, {"title", st.title},
              {"args", st.args}, {"state", st.state}};
    if (!st.result.is_null()) {
      j["result"] = st.result;
    }
    if (!st.error.empty()) {
      j["error"] = st.error;
    }
    if (st.job) {
      j["job"] = st.job->str();
    }
    steps.push_back(std::move(j));
  }
  Json j = {{"agent", s.id},
            {"project", s.project.str()},
            {"state", s.state},
            {"review", s.review},
            {"skills", s.skills},
            {"backend", backend},
            {"prompt", s.prompt},
            {"composition", s.target ? s.target->str() : std::string()},
            {"error", s.error},
            {"error_fields", s.error.empty() ? Json::object()
                                             : s.error_fields},
            {"writing", s.writing},
            {"turns", s.turns},
            {"usage", s.usage},
            {"stats", s.stats},
            {"pending", static_cast<int>(s.pending.size())},
            {"created", s.created_ms},
            {"compactions", s.compactions},
            {"context", {{"used", context_estimate_(s)},
                         {"counted", s.ctx_used},
                         {"limit", limit},
                         {"compact_at", s.compact ? kCompactAtPct : 0}}},
            {"steps", std::move(steps)}};
  if (s.transcript) {
    j["transcript"] = s.transcript->str();
  }
  if (messages) {
    Json ms = Json::array();
    // Shown whole: what was compacted away in its place, and each
    // compaction a line of its own where it was made.
    auto show = [&ms](const agent::Message& m) {
      if (m.role == "system") {
        return;
      }
      if (agent::is_compaction(m)) {
        ms.push_back({{"role", "compaction"},
                      {"content", ""},
                      {"compacted", jget(m.stats, "compacted", 0)},
                      {"tokens", jget<std::int64_t>(m.stats, "tokens", 0)},
                      {"limit", jget<std::int64_t>(m.stats, "limit", 0)},
                      {"asked", jget(m.stats, "asked", false)}});
        return;
      }
      Json x = {{"role", m.role}, {"content", m.content}};
      if (!m.calls.empty()) {
        Json names = Json::array();
        for (const auto& c : m.calls) {
          names.push_back(c.name);
        }
        x["calls"] = std::move(names);
      }
      if (!m.name.empty()) {
        x["name"] = m.name;
      }
      if (!m.stats.empty()) {
        x["stats"] = m.stats;
      }
      // The person's own words and what they attached; the pictures
      // shown with it, counted.
      if (!m.said.empty()) {
        x["said"] = m.said;
      }
      if (!m.attached.empty()) {
        x["attached"] = m.attached;
      }
      if (!m.images.empty()) {
        x["shown"] = m.images.size();
      }
      ms.push_back(std::move(x));
    };
    const std::size_t head = agent::compaction_head(s.messages);
    for (std::size_t i = 0; i < head; ++i) {
      show(s.messages[i]);
    }
    for (const auto& m : s.earlier) {
      show(m);
    }
    for (std::size_t i = head; i < s.messages.size(); ++i) {
      show(s.messages[i]);
    }
    j["messages"] = std::move(ms);
  }
  return j;
}

void
Controller::AgentHost::save_(Session& s)
{
  Json j = state_json_(s, false);
  Json ms = Json::array();
  {
    std::lock_guard lk(s.mu);
    for (const auto& m : s.messages) {
      ms.push_back(agent::to_json(m));
    }
    // What was compacted away: on record, never sent again.
    if (!s.earlier.empty()) {
      Json e = Json::array();
      for (const auto& m : s.earlier) {
        e.push_back(agent::to_json(m));
      }
      j["earlier"] = std::move(e);
    }
  }
  j["messages"] = std::move(ms);
  (void)write_json(s.dir / "session.json", j);
}

bool
Controller::AgentHost::gate_(Session& s)
{
  std::unique_lock lk(s.mu);
  if (s.stop_asked) {
    return false;
  }
  if (!s.pause_asked) {
    return true;
  }
  const std::string was = s.state;
  s.state = "paused";
  lk.unlock();
  post_(s);
  lk.lock();
  s.cv.wait(lk, [&] { return !s.pause_asked || s.stop_asked; });
  if (s.stop_asked) {
    return false;
  }
  s.state = was;
  return true;
}

bool
Controller::AgentHost::wait_words_(Session& s)
{
  {
    std::unique_lock lk(s.mu);
    s.cv.wait(lk, [&] { return s.stop_asked || !s.said.empty(); });
    if (s.stop_asked) {
      return false;
    }
    s.error.clear();
  }
  take_said_(s);
  return true;
}

bool
Controller::AgentHost::wait_pending_(Session& s)
{
  std::unique_lock lk(s.mu);
  if (s.pending.empty() && s.held.empty()) {
    return true;
  }
  const std::string was = s.state;
  s.state = "waiting";
  lk.unlock();
  post_(s);
  lk.lock();
  // Shots held behind one being made count: each is asked for as the one
  // before it lands.
  s.cv.wait(lk, [&] {
    return (s.pending.empty() && s.held.empty()) || s.stop_asked;
  });
  s.state = was;
  return !s.stop_asked;
}

void
Controller::AgentHost::tell_landed_(Session& s)
{
  std::lock_guard lk(s.mu);
  if (s.landed.empty()) {
    return;
  }
  std::string t = "[Valtz] What you asked to be made has finished:\n";
  for (const auto& l : s.landed) {
    t += "- " + to_text(l) + "\n";
  }
  s.landed.clear();
  s.messages.push_back({.role = "user", .content = std::move(t)});
}

void
Controller::AgentHost::run_(Session& s)
{
  const auto& table = tools_for(s.skills);
  const auto specs = tool_specs(s.skills);
  bool told_after_done = false;
  // The last turn did not fit, and the conversation was compacted for it.
  bool overflowed = false;
  // Turns running that wrote a call nothing could read.
  int unread = 0;
  // Programs a procedural session wrote in its words and Valtz drew.
  int drawn_unasked = 0;
  // The pictures the request came with, shown with it.
  {
    agent::Message first;
    {
      std::lock_guard lk(s.mu);
      if (s.messages.size() > 1) {
        first = s.messages[1];
      }
    }
    show_attached_(s, first);
    if (!first.images.empty()) {
      std::lock_guard lk(s.mu);
      s.messages[1].images = std::move(first.images);
    }
  }
  for (;;) {
    if (!gate_(s)) {
      break;
    }
    tell_landed_(s);
    // Words the person said while it worked: told at this turn.
    take_said_(s);
    {
      std::lock_guard lk(s.mu);
      if (s.turns >= kMaxTurns) {
        s.error_fields = message_fields(
            msg::kAgentTooManyTurns,
            {{"turns", std::to_string(kMaxTurns)}});
        s.error = jget<std::string>(s.error_fields, "message", "");
        s.turns = 0;
      }
    }
    if (!s.error.empty()) {
      set_state_(s, "failed");
      save_(s);
      if (!wait_words_(s)) {
        break;
      }
      continue;
    }
    // 0. Compacted first, if the turn would outgrow its model's context.
    if (needs_compaction_(s, specs) && !compact_(s, specs, false)) {
      break;
    }
    // 1. A turn of the model.
    {
      std::lock_guard lk(s.mu);
      s.writing.clear();
    }
    set_state_(s, "thinking");
    std::vector<agent::Message> msgs;
    {
      std::lock_guard lk(s.mu);
      msgs = s.messages;
    }
    // Its speed by Valtz's clock (agent::turn_stats): until its first
    // words, and the whole turn. Shared: a late word may come after.
    const auto t_turn = std::chrono::steady_clock::now();
    auto first_ns = std::make_shared<std::atomic<std::int64_t>>(-1);
    auto on_text = [this, &s, t_turn, first_ns](std::string_view t) {
      std::int64_t none = -1;
      first_ns->compare_exchange_strong(
          none, std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - t_turn).count());
      {
        std::lock_guard lk(s.mu);
        s.writing.append(t);
      }
      _c.post_("agent.text", JobId{},
               {{"agent", s.id}, {"text", std::string(t)}});
    };
    auto reply = s.backend->turn(msgs, specs, on_text, s.cancel);
    {
      std::lock_guard lk(s.mu);
      if (s.stop_asked) {
        break;
      }
    }
    if (!reply.ok()) {
      // Too long for its model, whatever was estimated: compacted on
      // Valtz's notes (the model cannot be asked) and tried again, once.
      if (agent::context_overflow(reply.error()) && !overflowed &&
          s.compact) {
        overflowed = true;
        VALTZ_LOG_INFO("agent", "session {}: the turn did not fit: {}",
                       s.id, reply.error().message);
        if (!compact_(s, specs, true)) {
          break;
        }
        continue;
      }
      VALTZ_LOG_WARN("agent", "session {}: {}", s.id,
                     reply.error().message);
      {
        std::lock_guard lk(s.mu);
        s.error = reply.error().message;
        s.error_fields = message_fields(reply.error());
      }
      continue;   // failed: it waits for the person's word
    }
    std::vector<agent::ToolCall> calls = reply->calls;
    const std::int64_t first = first_ns->load();
    Json stats = agent::turn_stats(
        reply->usage, first < 0 ? -1.0 : first / 1e6,
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t_turn).count());
    overflowed = false;
    // Cut off at its budget (the context's end is a context full, taken
    // above): kept as written, and said wherever it is shown -- the
    // chat, the transcript, valtzctl -- the person asking for the rest.
    if (reply->finish == "length") {
      stats["cut_off"] = reply->limit;
      VALTZ_LOG_WARN("agent", "session {}: its reply was cut off at its "
                     "limit of {} tokens", s.id, reply->limit);
    }
    {
      std::lock_guard lk(s.mu);
      ++s.turns;
      s.usage = reply->usage;
      s.stats = stats;
      s.messages.push_back({.role = "assistant",
                            .content = reply->content,
                            .raw = reply->raw,
                            .calls = calls,
                            .stats = std::move(stats)});
      // What the conversation took, its reply in it: the next turn's
      // count starts from here.
      s.ctx_used = agent::context_used(reply->usage);
      s.ctx_counted = s.messages.size();
    }
    save_(s);
    // A call written that neither the helper's parser nor Valtz's
    // lenient reading could make out (the 27B's, 2026-10-20: the session
    // ended "done" on its first turn): said, and the turn taken again.
    if (const char* again = unread_call_form(reply->raw);
        calls.empty() && again != nullptr && unread < kUnreadCalls) {
      ++unread;
      VALTZ_LOG_WARN("agent", "session {}: a tool call it wrote could "
                     "not be read; asked again", s.id);
      std::lock_guard lk(s.mu);
      s.messages.push_back({.role = "user", .content = again});
      continue;
    }
    if (!calls.empty()) {
      unread = 0;
    }
    // A procedural session's program written in its words, no tool
    // called: drawn by Valtz as render_program would, a step of its own,
    // and the session told what it drew -- then asked to go on with the
    // tool. Twice at most.
    if (calls.empty() && s.skills == "procedural" && drawn_unasked < 2) {
      if (const std::string code = program_in_reply(reply->content);
          !code.empty()) {
        ++drawn_unasked;
        VALTZ_LOG_INFO("agent", "session {}: a program in its words, "
                       "drawn for it", s.id);
        Step step;
        {
          std::lock_guard lk(s.mu);
          step.n = static_cast<int>(s.steps.size()) + 1;
          step.tool = "render_program";
          step.title = "Draw a program";
          step.args = {{"code", code}};
          step.state = "running";
          s.steps.push_back(step);
        }
        set_state_(s, "acting");
        post_(s, step.n);
        std::vector<fs::path> shown;
        auto r = t_render_program(s, step, step.args, shown);
        Json result = r.ok() ? *r : Json{{"error", r.error().message}};
        {
          std::lock_guard lk(s.mu);
          Step& x = s.steps[static_cast<std::size_t>(step.n - 1)];
          x.state = r.ok() ? "done" : "failed";
          if (r.ok()) {
            x.result = result;
          } else {
            x.error = r.error().message;
          }
          agent::Message m{
              .role = "user",
              .content =
                  "[Valtz] Your reply held a program but called no tool, "
                  "so Valtz drew it with render_program: " +
                  to_text(result) +
                  "\nDraw programs with render_program, never in your "
                  "words. If what it drew is not right, call "
                  "render_program with it put right; if it is, say so in "
                  "a sentence."};
          m.images = shown;
          s.messages.push_back(std::move(m));
        }
        post_(s, step.n);
        save_(s);
        continue;
      }
    }
    if (calls.empty()) {
      // Its pictures still out: waited for, and it is told how they
      // landed -- once -- so it can look and finish.
      bool pending = false;
      {
        std::lock_guard lk(s.mu);
        pending = !s.pending.empty() || !s.landed.empty();
      }
      if (pending && !told_after_done) {
        told_after_done = true;
        if (!wait_pending_(s)) {
          break;
        }
        continue;
      }
      told_after_done = false;
      set_state_(s, "done");
      save_(s);
      if (!wait_words_(s)) {
        break;
      }
      continue;
    }
    // 2. Its calls, in order.
    std::vector<fs::path> shown;
    for (auto& call : calls) {
      if (!gate_(s)) {
        break;
      }
      const auto t = std::ranges::find_if(table, [&](const Tool& x) {
        return x.name == call.name;
      });
      Step step;
      {
        std::lock_guard lk(s.mu);
        step.n = static_cast<int>(s.steps.size()) + 1;
        step.call_id = call.id;
        step.tool = call.name;
        step.title = t == table.end() ? call.name : t->title;
        step.args = call.arguments;
        step.state = "proposed";
        s.steps.push_back(step);
      }
      auto update = [&](const std::function<void(Step&)>& f) {
        std::lock_guard lk(s.mu);
        f(s.steps[static_cast<std::size_t>(step.n - 1)]);
        step = s.steps[static_cast<std::size_t>(step.n - 1)];
      };
      Json result;
      if (t == table.end()) {
        // A tool it guessed (`update_composition`, before there was
        // one): those named alike said, for its next turn to call one.
        const auto words = [](const std::string& n) {
          std::vector<std::string> w;
          std::size_t from = 0;
          while (from <= n.size()) {
            const std::size_t to = std::min(n.find('_', from), n.size());
            if (to - from > 3) {
              w.push_back(n.substr(from, to - from));
            }
            from = to + 1;
          }
          return w;
        };
        std::string alike;
        int listed = 0;
        for (const auto& x : table) {
          const auto theirs = words(x.name);
          bool shares = false;
          for (const auto& w : words(call.name)) {
            shares = shares || std::ranges::find(theirs, w) != theirs.end();
          }
          if (shares && listed++ < 8) {
            alike += (alike.empty() ? "" : ", ") + x.name;
          }
        }
        result = {{"error",
                   alike.empty()
                       ? std::format("there is no tool '{}'", call.name)
                       : std::format("there is no tool '{}'; named alike: "
                                     "{}",
                                     call.name, alike)}};
        update([&](Step& x) {
          x.state = "failed";
          x.error = jget<std::string>(result, "error", "");
        });
      } else {
        bool skip = false;
        std::string note;
        bool review = false;
        {
          std::lock_guard lk(s.mu);
          review = s.review && t->kind != "read";
        }
        if (review) {
          {
            std::lock_guard lk(s.mu);
            s.state = "review";
            s.decision.reset();
          }
          post_(s, step.n);
          std::unique_lock lk(s.mu);
          s.cv.wait(lk, [&] {
            return s.stop_asked ||
                   (s.decision && s.decision->step == step.n);
          });
          if (s.stop_asked) {
            break;
          }
          const Decision d = *s.decision;
          s.decision.reset();
          if (d.what == "skip") {
            skip = true;
            note = d.note;
          } else {
            if (d.what == "approve_all") {
              s.review = false;
            }
            if (d.args.is_object()) {
              call.arguments = d.args;
              s.steps[static_cast<std::size_t>(step.n - 1)].args = d.args;
              step.args = d.args;
            }
          }
        }
        if (skip) {
          result = {{"skipped", true},
                    {"note", note.empty()
                                 ? std::string("The person declined this "
                                               "step.")
                                 : "The person declined this step: " +
                                       note}};
          update([](Step& x) { x.state = "skipped"; });
        } else {
          update([](Step& x) { x.state = "running"; });
          set_state_(s, "acting");
          post_(s, step.n);
          auto r = (this->*(t->run))(s, step, call.arguments, shown);
          if (r.ok()) {
            result = *r;
            update([&](Step& x) {
              // A picture that landed meanwhile has said how already.
              for (const auto& [k, v] : result.items()) {
                if (!x.result.is_object() || !x.result.contains(k)) {
                  x.result[k] = v;
                }
              }
              // (A held shot asked for meanwhile has its job already.)
              if (step.job) {
                x.job = step.job;
              }
              if (x.state == "running") {
                x.state = (step.job && s.pending.contains(*step.job)) ||
                                  s.held.contains(x.n)
                              ? "waiting"
                              : "done";
              }
            });
          } else {
            result = {{"error", r.error().message}};
            update([&](Step& x) {
              x.state = "failed";
              x.error = r.error().message;
            });
          }
        }
      }
      {
        std::lock_guard lk(s.mu);
        s.messages.push_back({.role = "tool",
                              .content = to_text(result),
                              .call_id = call.id,
                              .name = call.name});
      }
      post_(s, step.n);
    }
    {
      std::lock_guard lk(s.mu);
      if (s.stop_asked) {
        break;
      }
      if (!shown.empty()) {
        agent::Message look{.role = "user",
                            .content = "[Valtz] Here is what you asked to "
                                       "see, as it is drawn now."};
        look.images = shown;
        s.messages.push_back(std::move(look));
      }
      // A program that drew nothing, its guide unread: the guide now.
      if (s.program_guide_due) {
        s.program_guide_due = false;
        s.messages.push_back(
            {.role = "user",
             .content = "[Valtz] Your program drew nothing. How a program "
                        "is written here -- read it before the next "
                        "try:\n\n" +
                        _c.skill_text("valtz-procedural")});
      }
    }
    save_(s);
  }
  // Stopped: its turn's context let go, what it did kept on record.
  {
    std::lock_guard lk(s.mu);
    s.state = "stopped";
  }
  s.backend->close();
  save_(s);
  post_(s);
}

// ---- compaction (agent::compact, DESIGN §9b) -----------------------------

namespace {

// The tokens the conversation takes now: as its last turn was counted,
// and what came since -- estimated, a quarter over; all estimated when
// nothing was counted (the first turn, a compaction just made).
std::int64_t
conversation_tokens(const std::vector<agent::Message>& msgs,
                    std::int64_t used, std::size_t counted,
                    const std::vector<agent::ToolSpec>& specs)
{
  if (used > 0 && counted <= msgs.size()) {
    return used + agent::estimate_tokens(msgs, counted) * 5 / 4;
  }
  return agent::estimate_tokens(msgs) + agent::estimate_tokens(specs);
}

std::string
trimmed(std::string_view t)
{
  const auto* ws = " \t\r\n";
  const auto a = t.find_first_not_of(ws);
  if (a == std::string_view::npos) {
    return {};
  }
  return std::string(t.substr(a, t.find_last_not_of(ws) - a + 1));
}

// The person's own words among these: a user's message not Valtz's.
void
person_words(const std::vector<agent::Message>& msgs, std::size_t from,
             std::size_t to, std::vector<std::string>& out)
{
  for (std::size_t i = from; i < to && i < msgs.size(); ++i) {
    const auto& m = msgs[i];
    if (m.role == "user" && !agent::is_compaction(m) &&
        !m.content.starts_with("[Valtz]") && !one_line(m.content).empty()) {
      out.push_back(m.content);
    }
  }
}

}  // namespace

bool
Controller::AgentHost::needs_compaction_(
    Session& s, const std::vector<agent::ToolSpec>& specs) const
{
  const std::int64_t limit = s.backend->context_tokens();
  if (limit <= 0 || !s.compact) {
    return false;
  }
  std::lock_guard lk(s.mu);
  // Worth it only where what came since the last notes is a tenth of
  // the context: a context little bigger than the head (the guide and
  // the tools, ~15,000 tokens) would be compacted again every turn, the
  // notes into notes.
  std::size_t from = agent::compaction_head(s.messages);
  if (from < s.messages.size() && agent::is_compaction(s.messages[from])) {
    ++from;
  }
  if (agent::estimate_tokens(s.messages, from) < limit / 10) {
    return false;
  }
  const std::int64_t need =
      conversation_tokens(s.messages, s.ctx_used, s.ctx_counted, specs);
  return need + kCompactReserve > limit * kCompactAtPct / 100;
}

bool
Controller::AgentHost::compact_(Session& s,
                                const std::vector<agent::ToolSpec>& specs,
                                bool full)
{
  std::vector<agent::Message> msgs;
  std::vector<agent::Message> earlier;
  std::vector<Step> steps;
  std::int64_t used = 0;
  {
    std::lock_guard lk(s.mu);
    msgs = s.messages;
    earlier = s.earlier;
    steps = s.steps;
    used = conversation_tokens(msgs, s.ctx_used, s.ctx_counted, specs);
  }
  // A backend that does not say: as long as it is now.
  std::int64_t limit = s.backend->context_tokens();
  if (limit <= 0) {
    limit = used;
  }
  const std::size_t head = agent::compaction_head(msgs);

  // The notes: Valtz's record of the latest steps, the person's words
  // compacted away -- before and now -- and the model's.
  agent::CompactionNotes notes;
  notes.used = used;
  notes.limit = limit;
  notes.steps_run = static_cast<int>(steps.size());
  notes.said_bytes = static_cast<std::size_t>(std::clamp<std::int64_t>(
      limit * 3 / kSaidShare, 2000, 16000));
  const auto steps_bytes = static_cast<std::size_t>(
      std::max<std::int64_t>(limit * 3 / kStepsShare, 1000));
  std::size_t listed = 0;
  for (std::size_t i = steps.size(); i-- > 0 && listed < kNotedSteps;) {
    const Step& st = steps[i];
    const std::string outcome =
        !st.error.empty() ? "failed: " + st.error
        : st.result.is_null() ? st.state
                              : st.state + ": " + to_text(st.result);
    const std::string line = std::format(
        "[{}] {} ({}) {} -> {}", st.n, st.title, st.tool,
        to_text(st.args), outcome);
    const std::string row =
        "- " + std::string(utf8_prefix(line, kStepBytes)) +
        (line.size() > kStepBytes ? " ..." : "") + "\n";
    if (notes.steps.size() + row.size() > steps_bytes) {
      break;
    }
    notes.steps.insert(0, row);
    ++listed;
  }
  if (listed < steps.size()) {
    notes.steps.insert(0, std::format("- (the {} before these, left out)\n",
                                      steps.size() - listed));
  }
  // The latest turns kept within half the context, once the head and
  // the notes -- the model's at their longest -- are in.
  person_words(earlier, 0, earlier.size(), notes.said);
  person_words(msgs, head, msgs.size(), notes.said);
  const std::vector<agent::Message> head_msgs(
      msgs.begin(), msgs.begin() + static_cast<long>(head));
  const std::int64_t room =
      limit * kCompactToPct / 100 - agent::estimate_tokens(head_msgs) -
      agent::estimate_tokens(specs) - kTurnTokens -
      agent::estimate_tokens(agent::compact(head_msgs, head, notes)
                                 .back());
  const std::size_t tail =
      agent::compaction_tail(msgs, std::max<std::int64_t>(0, room));
  // Nothing to compact but notes made before: left as it is.
  if (tail <= head ||
      (tail == head + 1 && agent::is_compaction(msgs[head]))) {
    VALTZ_LOG_INFO("agent", "session {}: nothing to compact", s.id);
    return true;
  }

  set_state_(s, "compacting");
  if (!full) {
    std::vector<agent::Message> ask = msgs;
    ask.push_back(agent::compaction_request(used, limit));
    auto r = s.backend->turn(ask, specs, {}, s.cancel);
    {
      std::lock_guard lk(s.mu);
      if (s.stop_asked) {
        return false;
      }
    }
    if (r.ok()) {
      notes.summary = trimmed(r->content);
    } else {
      VALTZ_LOG_WARN("agent", "session {}: its notes were not written: {}",
                     s.id, r.error().message);
    }
  }
  notes.said.clear();
  person_words(earlier, 0, earlier.size(), notes.said);
  person_words(msgs, head, tail, notes.said);
  std::vector<agent::Message> next = agent::compact(msgs, tail, notes);
  next[head].stats = {{"compacted", static_cast<std::int64_t>(tail - head)},
                      {"tokens", used},
                      {"limit", limit},
                      {"asked", !notes.summary.empty()}};
  {
    std::lock_guard lk(s.mu);
    s.earlier.insert(s.earlier.end(),
                     msgs.begin() + static_cast<long>(head),
                     msgs.begin() + static_cast<long>(tail));
    // (Only this thread adds to the conversation; kept whatever came.)
    next.insert(next.end(),
                s.messages.begin() +
                    static_cast<long>(std::min(msgs.size(),
                                               s.messages.size())),
                s.messages.end());
    s.messages = std::move(next);
    s.ctx_used = 0;
    s.ctx_counted = 0;
    ++s.compactions;
  }
  VALTZ_LOG_INFO("agent",
                 "session {}: compacted {} messages (about {} of {} "
                 "tokens), {} kept as they were, notes {}",
                 s.id, tail - head, used, limit, msgs.size() - tail,
                 notes.summary.empty() ? "Valtz's alone" : "its own");
  save_(s);
  post_(s);
  return true;
}

// ---- the person's word ----------------------------------------------------

Status
Controller::AgentHost::say(const std::string& id, const std::string& words,
                           const Json& stage,
                           const std::vector<AssetId>& attached,
                           const Json& render)
{
  auto s = find_(id);
  if (!s) {
    return make_error(Code::NotFound, msg::kAgentNoSession);
  }
  if (one_line(words).empty()) {
    return make_error(Code::InvalidArgument, msg::kAgentEmptyRequest);
  }
  agent::Message m = person_message_(s->project, words, stage, attached,
                                     /*list=*/true, s->skills != "none");
  // Done and spoken to again: what the person changed since -- it held
  // nothing meanwhile -- makes what it last saw out of date.
  const std::uint64_t last = last_command_(s->project);
  {
    std::lock_guard lk(s->mu);
    if (!s->working && last != s->seen_seq) {
      m.content += "\n[Valtz] The person changed the project since you "
                   "finished: look again at what you work on before you "
                   "change it.";
    }
  }
  {
    std::lock_guard lk(s->mu);
    if (s->state == "stopped") {
      return make_error(Code::InvalidArgument, msg::kAgentStopped);
    }
    // The card changed since they last spoke (Procedural's Size, Length):
    // its programs drawn at the new one, and the words said before it
    // naming no size or length of it -- it was sent at the request alone,
    // and a session kept drawing clips after the card was set to Still.
    if (render.is_object() && s->skills == "procedural" &&
        card_text_(render) != card_text_(s->render)) {
      s->render = render;
      s->render_words = s->person_words.size();
      m.content += "\n[Valtz] The person's card now says: draw " +
                   card_text_(render) + ". render_program draws at it.";
    }
    s->person_words.push_back(words);
    // An answer to its question: the ask's result (what it attached
    // listed in it, for its tools).
    if (s->answer.has_value() || s->state == "asking") {
      s->answer = m.content;
    } else {
      s->said.push_back(std::move(m));
    }
    s->pause_asked = false;
    // The person spoke: its programs may be tried again.
    s->program_failures = 0;
  }
  s->cv.notify_all();
  return ok_status();
}

Status
Controller::AgentHost::review(const std::string& id, int step,
                              const std::string& what, const Json& args,
                              const std::string& note)
{
  if (what != "approve" && what != "skip" && what != "approve_all") {
    return make_error(Code::InvalidArgument,
                      std::format("no review decision '{}'", what));
  }
  auto s = find_(id);
  if (!s) {
    return make_error(Code::NotFound, msg::kAgentNoSession);
  }
  {
    std::lock_guard lk(s->mu);
    if (s->state != "review" || step < 1 ||
        step > static_cast<int>(s->steps.size()) ||
        s->steps[static_cast<std::size_t>(step - 1)].state != "proposed") {
      return make_error(Code::InvalidArgument, msg::kAgentStepNotWaiting);
    }
    s->decision = Decision{step, what, args, note};
  }
  s->cv.notify_all();
  return ok_status();
}

Status
Controller::AgentHost::set_review(const std::string& id, bool review)
{
  auto s = find_(id);
  if (!s) {
    return make_error(Code::NotFound, msg::kAgentNoSession);
  }
  int waiting = 0;
  {
    std::lock_guard lk(s->mu);
    s->review = review;
    // Auto with a step waiting: that one goes too.
    if (!review && s->state == "review") {
      for (const auto& st : s->steps) {
        if (st.state == "proposed") {
          waiting = st.n;
        }
      }
      if (waiting > 0) {
        s->decision = Decision{waiting, "approve", Json(), ""};
      }
    }
  }
  s->cv.notify_all();
  post_(*s);
  return ok_status();
}

Status
Controller::AgentHost::pause(const std::string& id)
{
  auto s = find_(id);
  if (!s) {
    return make_error(Code::NotFound, msg::kAgentNoSession);
  }
  {
    std::lock_guard lk(s->mu);
    s->pause_asked = true;
  }
  s->cv.notify_all();
  return ok_status();
}

Status
Controller::AgentHost::resume(const std::string& id)
{
  auto s = find_(id);
  if (!s) {
    return make_error(Code::NotFound, msg::kAgentNoSession);
  }
  {
    std::lock_guard lk(s->mu);
    s->pause_asked = false;
    // Failed: resumed, it tries the turn again.
    if (s->state == "failed" && s->said.empty()) {
      s->said.push_back({.role = "user", .content = "Go on."});
    }
  }
  s->cv.notify_all();
  return ok_status();
}

Status
Controller::AgentHost::stop(const std::string& id)
{
  auto s = find_(id);
  if (!s) {
    return make_error(Code::NotFound, msg::kAgentNoSession);
  }
  {
    std::lock_guard lk(s->mu);
    s->stop_asked = true;
  }
  s->cancel.store(true);
  s->cv.notify_all();
  return ok_status();
}

Json
Controller::AgentHost::state(const std::string& id) const
{
  auto s = find_(id);
  return s ? state_json_(*s, true) : Json();
}

Json
Controller::AgentHost::list(ProjectId pid) const
{
  std::vector<std::shared_ptr<Session>> all;
  {
    std::lock_guard lk(_mu);
    for (const auto& [id, s] : _sessions) {
      if (s->project == pid) {
        all.push_back(s);
      }
    }
  }
  std::ranges::sort(all, [](const auto& a, const auto& b) {
    return a->created_ms > b->created_ms;
  });
  Json out = Json::array();
  for (const auto& s : all) {
    out.push_back(state_json_(*s, false));
  }
  return out;
}

void
Controller::AgentHost::project_closing(ProjectId pid)
{
  std::vector<std::shared_ptr<Session>> mine;
  {
    std::lock_guard lk(_mu);
    for (const auto& [id, s] : _sessions) {
      if (s->project == pid) {
        mine.push_back(s);
      }
    }
  }
  for (auto& s : mine) {
    (void)stop(s->id);
  }
  for (auto& s : mine) {
    if (s->thread.joinable()) {
      s->thread.join();
    }
  }
  std::lock_guard lk(_mu);
  for (auto& s : mine) {
    _sessions.erase(s->id);
  }
}

// ---- pictures landing -----------------------------------------------------

void
Controller::AgentHost::on_event(const std::string& kind, JobId job,
                                const Json& data)
{
  if (kind == "assist.enhanced") {
    // Kept for enhance_prompt, which takes its own once the job ends; the
    // newest few only (the person's own Enhance goes to the app).
    std::lock_guard lk(_mu);
    _replies.emplace_back(job, data);
    while (_replies.size() > 32) {
      _replies.pop_front();
    }
    return;
  }
  if (kind != "job.finished" && kind != "job.failed" &&
      kind != "job.cancelled") {
    return;
  }
  {
    std::lock_guard lk(_mu);
    if (!_landings.contains(job)) {
      return;
    }
    _events.push_back({job, kind, data});
  }
  _cv.notify_all();
}

void
Controller::AgentHost::land_(JobId job, const std::string& kind,
                             const Json& data)
{
  Landing l;
  {
    std::lock_guard lk(_mu);
    auto it = _landings.find(job);
    if (it == _landings.end()) {
      return;
    }
    l = it->second;
    _landings.erase(it);
  }
  Json result = {{"job", job.str()}};
  if (!l.model.empty()) {
    result["model"] = l.model;
  }
  bool ok = kind == "job.finished";
  if (ok) {
    const auto asset = AssetId::parse(jget<std::string>(data, "asset", ""));
    if (!asset) {
      ok = false;
      result["error"] = "nothing came back";
    } else {
      result["asset"] = asset->str();
      if (l.composition) {
        // Into the generation's own command: undone with it.
        auto scope = _c.join_(l.project, job);
        // A shot goes over the topmost clip or picture -- the titles stay
        // over it -- at its start.
        std::optional<std::string> over;
        if (l.clip) {
          if (auto* p = _c.project(l.project)) {
            if (auto tl = p->asset(*l.composition); tl.ok()) {
              for (const auto& x : tl->layers) {
                const std::string k = layer_kind_(l.project, x);
                if (k == "clip" || k == "picture") {
                  over = x.id;
                }
              }
            }
          }
        }
        auto placed = _c.place_result(l.project, *asset, l.composition,
                                      over,
                                      l.sound || l.clip ? l.start : l.page,
                                      job);
        if (!placed.ok()) {
          result["error"] = placed.error().message;
        } else if (l.clip) {
          const auto& [comp, layer] = *placed;
          result["composition"] = comp.str();
          result["layer"] = layer.empty() ? "0" : layer;
          result.update(lay_shot_(l, comp, layer, *asset, !over));
        } else if (l.sound) {
          // A song, a voice-over: on the timeline from its start, cut,
          // at its volume.
          const auto& [comp, layer] = *placed;
          result["composition"] = comp.str();
          result["layer"] = layer.empty() ? "0" : layer;
          result.update(lay_sound_(l, comp, layer, *asset));
        } else {
          const auto& [comp, layer] = *placed;
          result["composition"] = comp.str();
          result["layer"] = layer.empty() ? "0" : layer;
          result["page"] = l.page + 1;
          if (l.composition) {
            (void)page_alone_(l.project, comp, layer, l.page);
          }
          if (l.box) {
            auto* p = _c.project(l.project);
            auto v = p ? p->version(*asset) : Result<project::AssetVersion>(
                             make_error(Code::NotFound, "gone"));
            auto frame = _c.own_frame(l.project, comp);
            if (v.ok() && frame.ok() && v->info.frame.width > 0) {
              const media::PixelSize content{v->info.frame.width,
                                             v->info.frame.height};
              (void)_c.set_crop(l.project, comp,
                                box_crop_(content, *frame, *l.box), layer);
              result["box"] = {(*l.box)[0], (*l.box)[1], (*l.box)[2],
                               (*l.box)[3]};
            }
          }
          if (l.bottom) {
            (void)to_back_(l.project, comp, layer);
          }
        }
      }
    }
  } else {
    result["error"] = jget<std::string>(data, "message",
                                        kind == "job.cancelled"
                                            ? "stopped"
                                            : "the generation failed");
  }
  auto s = find_(l.session);
  if (!s) {
    return;
  }
  const bool landed_ok = ok && !result.contains("error");
  {
    std::lock_guard lk(s->mu);
    s->pending.erase(job);
    Json told = result;
    told["step"] = l.step;
    s->landed.push_back(std::move(told));
    if (l.step >= 1 && l.step <= static_cast<int>(s->steps.size())) {
      Step& st = s->steps[static_cast<std::size_t>(l.step - 1)];
      st.state = ok && !result.contains("error") ? "done" : "failed";
      for (const auto& [k, v] : result.items()) {
        st.result[k] = v;
      }
      if (result.contains("error")) {
        st.error = jget<std::string>(result, "error", "");
      }
    }
  }
  s->cv.notify_all();
  post_(*s, l.step);
  // Shots held behind it: asked for now, carrying on from it -- or failed
  // with it.
  if (l.clip) {
    std::optional<AssetId> made;
    if (landed_ok) {
      made = AssetId::parse(jget<std::string>(result, "asset", ""));
    }
    release_held_(*s, l.step, made,
                  jget<std::string>(result, "error", ""));
  }
}

void
Controller::AgentHost::housekeeping_()
{
  // What lands here is the agent's own: never refused by its hold.
  CoreWrites own;
  std::unique_lock lk(_mu);
  while (!_stop) {
    _cv.wait_for(lk, std::chrono::seconds(15),
                 [&] { return _stop || !_events.empty(); });
    while (!_events.empty()) {
      Pending e = std::move(_events.front());
      _events.pop_front();
      lk.unlock();
      land_(e.job, e.kind, e.data);
      lk.lock();
    }
    // Members' sessions whose lease ran out: their contexts let go.
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, fs::path>> gone;
    for (auto it = _leases.begin(); it != _leases.end();) {
      if (it->second.until <= now) {
        gone.emplace_back(it->first, it->second.dir);
        it = _leases.erase(it);
      } else {
        ++it;
      }
    }
    if (!gone.empty()) {
      lk.unlock();
      for (const auto& [key, dir] : gone) {
        VALTZ_LOG_INFO("agent", "a member's session ({}) let go: its "
                                "lease ran out", key);
        if (_c._engine) {
          _c._engine->drop_context(key);
        }
        std::error_code ec;
        fs::remove(dir / (key + ".vctx"), ec);
      }
      lk.lock();
    }
  }
}

// ---- turns served for a member ---------------------------------------------

Result<engine::JobSpec>
Controller::AgentHost::served_turn(const fleet::Offer& o,
                                   const std::string& from,
                                   const fs::path& dir)
{
  // This Mac's agent model thinks for the member's agent too.
  const auto* m = _c.agent_model();
  if (m == nullptr) {
    return make_error(Code::Unsupported, msg::kNoAssistantModel);
  }
  Json conv = jget(o.params, "conversation", Json::object());
  const std::string key =
      "fleet-" + clean(from) + "-" +
      clean(jget<std::string>(conv, "context", o.id.str()));
  // Its saved context in this Mac's fleet folder, never a path of the
  // sender's.
  const fs::path ctx_dir = dir.parent_path().parent_path() / "agent";
  conv["context"] = key;
  conv["context_dir"] = ctx_dir.string();
  const double lease =
      std::clamp(jget(o.params, "lease", kDefaultLease), 60.0, 3600.0);
  engine::JobSpec spec;
  spec.id = o.id;
  spec.op = o.op;
  VALTZ_ASSIGN(spec.model, _c.resolve_model_(*m, false));
  spec.params = _c.helper_params_(*m);
  // Kept warm as long as the session's lease: the conversation stays in
  // memory between its turns.
  spec.params["keep_loaded"] =
      std::max(jget(spec.params, "keep_loaded", 0.0), lease);
  spec.params["disable_thinking"] = true;
  spec.params["conversation"] = std::move(conv);
  spec.inputs = o.inputs;
  {
    std::lock_guard lk(_mu);
    _leases[key] = {std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(
                            static_cast<std::int64_t>(lease * 1000)),
                    ctx_dir};
  }
  return spec;
}

void
Controller::AgentHost::close_served(const std::string& from,
                                    const std::string& context)
{
  const std::string key = "fleet-" + clean(from) + "-" + clean(context);
  fs::path dir;
  {
    std::lock_guard lk(_mu);
    auto it = _leases.find(key);
    if (it == _leases.end()) {
      return;
    }
    dir = it->second.dir;
    _leases.erase(it);
  }
  if (_c._engine) {
    _c._engine->drop_context(key);
  }
  std::error_code ec;
  fs::remove(dir / (key + ".vctx"), ec);
}

// ---- the Controller's calls -------------------------------------------------

Result<std::string>
Controller::agent_start(AgentRequest req)
{
  return _agent->start(std::move(req));
}

Status
Controller::agent_say(const std::string& agent, const std::string& text,
                      const Json& stage,
                      const std::vector<AssetId>& attached,
                      const Json& render)
{
  return _agent->say(agent, text, stage, attached, render);
}

Status
Controller::agent_review(const std::string& agent, int step,
                         const std::string& decision, const Json& args,
                         const std::string& note)
{
  return _agent->review(agent, step, decision, args, note);
}

Status
Controller::agent_set_review(const std::string& agent, bool review)
{
  return _agent->set_review(agent, review);
}

Status
Controller::agent_pause(const std::string& agent)
{
  return _agent->pause(agent);
}

Status
Controller::agent_resume(const std::string& agent)
{
  return _agent->resume(agent);
}

Status
Controller::agent_stop(const std::string& agent)
{
  return _agent->stop(agent);
}

Json
Controller::agent_state(const std::string& agent) const
{
  return _agent->state(agent);
}

Json
Controller::agents(ProjectId pid) const
{
  return _agent->list(pid);
}

Json
Controller::agent_settings() const
{
  return _agent->settings();
}

Status
Controller::set_agent_settings(const Json& j)
{
  return _agent->set_settings(j);
}

Status
Controller::set_agent_key(const std::string& provider, const std::string& key)
{
  if (provider != "anthropic" && provider != "openai" &&
      provider != "openai-compatible") {
    return make_error(Code::InvalidArgument,
                      std::format("no cloud provider '{}'", provider));
  }
  return agent::store_api_key(provider, key);
}

Json
Controller::agent_tools() const
{
  return _agent->tools_json();
}

}  // namespace valtz
