// A conversation COMPACTED when it outgrows its model's context
// (agent.h, DESIGN §9b): the estimates, the question, the head and tail,
// the notes -- pure, so the tests run them without a model.

#include "valtz/agent/agent.h"

#include "valtz/base/message.h"
#include "valtz/base/text.h"

#include <algorithm>
#include <cctype>
#include <format>

namespace valtz::agent {

namespace {

constexpr std::int64_t kBytesPerToken = 3;
constexpr std::int64_t kPictureTokens = 1600;
constexpr std::int64_t kMessageTokens = 8;
// The person's words carried in the notes: each cut so.
constexpr std::size_t kSaidBytes = 4000;
constexpr char kCompactionName[] = "compaction";

std::int64_t
text_tokens(std::size_t bytes)
{
  return static_cast<std::int64_t>(bytes) / kBytesPerToken + 1;
}

std::string
lower(std::string s)
{
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

}  // namespace

std::int64_t
estimate_tokens(const Message& m)
{
  // An assistant's as the helper gave it (its calls in it), or its
  // words and calls apart (a cloud's): the longer.
  std::size_t bytes = m.content.size();
  for (const auto& c : m.calls) {
    bytes += c.name.size() + to_text(c.arguments).size() + 32;
  }
  bytes = std::max(bytes, m.raw.size());
  return kMessageTokens + text_tokens(bytes) +
         kPictureTokens * static_cast<std::int64_t>(m.images.size());
}

std::int64_t
estimate_tokens(const std::vector<Message>& messages, std::size_t from)
{
  std::int64_t n = 0;
  for (std::size_t i = from; i < messages.size(); ++i) {
    n += estimate_tokens(messages[i]);
  }
  return n;
}

std::int64_t
estimate_tokens(const std::vector<ToolSpec>& tools)
{
  std::int64_t n = 0;
  for (const auto& t : tools) {
    n += kMessageTokens +
         text_tokens(t.name.size() + t.description.size() +
                     to_text(t.parameters).size());
  }
  return n;
}

std::int64_t
context_used(const Json& usage)
{
  if (const auto pos = jget<std::int64_t>(usage, "ctx_pos", 0); pos > 0) {
    return pos;
  }
  std::int64_t n = 0;
  for (const char* k : {"input_tokens", "cache_read_tokens",
                        "cache_write_tokens", "output_tokens"}) {
    n += std::max<std::int64_t>(0, jget<std::int64_t>(usage, k, 0));
  }
  return n;
}

bool
context_overflow(const Error& e)
{
  if (e.key == msg::kAgentContextFull.key) {
    return true;
  }
  std::string said = e.message;
  for (const auto& [k, v] : e.args) {
    said += " " + v;
  }
  said = lower(std::move(said));
  for (const char* w : {"prompt is too long", "context_length_exceeded",
                        "maximum context length", "context window",
                        "too many tokens"}) {
    if (said.find(w) != std::string::npos) {
      return true;
    }
  }
  return false;
}

Message
compaction_request(std::int64_t used, std::int64_t limit)
{
  return {.role = "user",
          .content = std::format(
              "[Valtz] Your context is nearly full (about {} of {} "
              "tokens). Everything after the person's request is about "
              "to be replaced by notes you write now, for yourself, to "
              "carry on from. Call no tool: write only the notes.\n"
              "1. The request, and every instruction, preference or "
              "correction the person gave since.\n"
              "2. What is done: each composition, page, layer, text, "
              "picture, clip, sound and prompt you made or changed, with "
              "its id, and where it is (page, box, start).\n"
              "3. What is under way: what you asked to be made that has "
              "not landed.\n"
              "4. What is left to do, in order, and what you learned not "
              "to repeat (a mistake, a tool's limit).\n"
              "At most about 700 words.",
              used, limit)};
}

std::size_t
compaction_head(const std::vector<Message>& messages)
{
  std::size_t i = 0;
  while (i < messages.size() && messages[i].role == "system") {
    ++i;
  }
  if (i < messages.size() && messages[i].role == "user" &&
      !is_compaction(messages[i])) {
    ++i;
  }
  return i;
}

std::size_t
compaction_tail(const std::vector<Message>& messages, std::int64_t budget)
{
  const std::size_t head = compaction_head(messages);
  std::size_t tail = messages.size();
  std::int64_t n = 0;
  for (std::size_t i = messages.size(); i-- > head;) {
    n += estimate_tokens(messages[i]);
    if (n > budget) {
      break;
    }
    // A boundary: an earlier compaction's notes are never kept as they
    // were (the new notes stand for them).
    if (messages[i].role != "tool" && !is_compaction(messages[i])) {
      tail = i;
    }
  }
  return tail;
}

std::vector<Message>
compact(const std::vector<Message>& messages, std::size_t tail,
        const CompactionNotes& notes)
{
  const std::size_t head = compaction_head(messages);
  tail = std::clamp(tail, head, messages.size());
  std::vector<Message> out(messages.begin(),
                           messages.begin() + static_cast<long>(head));
  std::string t = std::format(
      "[Valtz] This conversation grew too long for your context (about "
      "{} of {} tokens), so everything after the person's request was "
      "compacted into these notes; the latest turns follow as they "
      "were.\n\n",
      notes.used, notes.limit);
  if (!notes.summary.empty()) {
    t += "Your own notes on it:\n" + notes.summary + "\n\n";
  } else {
    t += "Your notes could not be asked for (the context was full): read "
         "the document again with get_project before you go on.\n\n";
  }
  if (!notes.said.empty()) {
    t += "What the person said since the request, in their words:\n";
    std::size_t all = 0;
    for (const auto& w : notes.said) {
      if (all >= notes.said_bytes) {
        t += "- (more, left out)\n";
        break;
      }
      const std::string_view kept = utf8_prefix(w, kSaidBytes);
      all += kept.size();
      t += "- " + std::string(kept) +
           (kept.size() < w.size() ? " (...)" : "") + "\n";
    }
    t += "\n";
  }
  if (!notes.steps.empty()) {
    t += std::format("The steps you ran, as Valtz recorded them ({} in "
                     "all):\n",
                     notes.steps_run) +
         notes.steps + "\n\n";
  }
  t += "The document is as these steps left it: get_project and the view "
       "tools show it now. Carry on from here.";
  out.push_back({.role = "user", .content = std::move(t),
                 .name = kCompactionName});
  out.insert(out.end(), messages.begin() + static_cast<long>(tail),
             messages.end());
  return out;
}

bool
is_compaction(const Message& m)
{
  return m.role == "user" && m.name == kCompactionName;
}

}  // namespace valtz::agent
