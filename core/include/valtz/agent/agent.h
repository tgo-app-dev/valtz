// THE AGENT (DESIGN §9b): a language model given Valtz's editor as TOOLS
// -- make a composition, add its pages, write on them, generate a
// picture into a box, place an asset, look at a page -- which it calls to
// work on the document, step by step, the person reviewing each step or
// letting it run.
//
// Its model thinks in one of three places, behind one interface
// (Backend): this Mac's helper, through vpipe's text-chat (the engine's
// kOpAgentTurn: the conversation's K/V kept with the helper between
// turns, saved to disk when a generation needs the memory); a fleet
// member's helper, sent the turns (a box with the memory for the model,
// beside one with the GPU for the pictures); or a cloud API -- Anthropic's
// Messages, OpenAI's Chat Completions, or any server speaking the
// latter. Each is handed the same conversation (Message) and the same
// tools (ToolSpec), and answers with a Reply: its words and the calls it
// makes.
//
// Here: those shapes, the cloud backend, and the wire formats as pure
// functions (tests run them with no network). The session that runs the
// loop -- the tools, the review, pause and resume, the landing of what it
// generates -- is the controller's (Controller::agent_start).

#ifndef VALTZ_AGENT_AGENT_H
#define VALTZ_AGENT_AGENT_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::agent {

// A call the model made to one of the tools.
struct ToolCall {
  std::string id;      // the backend's; the local helper gives none: made
  std::string name;
  Json        arguments = Json::object();
};

// A message of the conversation, as every backend is handed it (each
// writes it in its own wire format).
struct Message {
  std::string           role;     // system | user | assistant | tool
  std::string           content;  // its words
  // An assistant's reply as the local helper wrote it, tool calls and
  // all: handed back to it VERBATIM, which is how it knows the context it
  // kept holds this message (vpipe's text-chat matches by digest).
  std::string           raw;
  std::vector<ToolCall> calls;    // an assistant's
  std::string           call_id;  // a tool message: the call it answers
  std::string           name;     // a tool message: its tool
  // Pictures shown with it (a user message: a page as it is drawn).
  std::vector<std::filesystem::path> images;
  // An assistant's: what its turn cost and how fast it went (turn_stats)
  // -- shown to the person debugging it, never sent to a model.
  Json                  stats = Json::object();
};

Json to_json(const Message&);
Message message_from_json(const Json&);

struct ToolSpec {
  std::string name;
  std::string description;
  Json        parameters = Json::object();  // JSON Schema
};

struct Reply {
  std::string           raw;      // as given: Message::raw
  std::string           content;  // its words
  std::vector<ToolCall> calls;
  // stop | tool_calls | length | interrupted | stopped | error
  std::string           finish;
  // What it cost, as the backend says: the helper's {"prefilled",
  // "reused", "restored", "ctx_bytes", "ctx_pos", "prefill_ms",
  // "decode_ms", "decoded"; its drafter's "drafter", "rounds",
  // "drafted", "accepted"}; a cloud's {"input_tokens", "output_tokens",
  // "cache_read_tokens"}.
  Json                  usage = Json::object();
};

// A turn's cost and speed, for the person debugging it (Settings ›
// Agentic Helper › Show Speed; valtzctl agent --speed, enhance --speed):
// the backend's counts (Reply::usage; an enhance's chat, the same keys)
// and Valtz's clock -- `first_ms` until its first words came (< 0: none
// came), `turn_ms` the whole turn, any queue and load included.
// {"prefill_tokens", "prefill_ms", "prefill_tps", "decode_tokens",
// "decode_ms", "decode_tps"} -- the helper's by vpipe's own clock; a
// cloud's prefill timed to its first words (the network in it), its
// decode from there to the end; a side's _ms and _tps only where it has
// a time, its _tokens only where it is counted -- and, where the backend
// says, "cached_tokens", "reused", "restored", "ctx_pos"; a drafter's
// "drafter", "rounds", "per_round" (decoded tokens a verify forward),
// "accepted_pct" (of its drafts); "first_ms", "turn_ms".
Json turn_stats(const Json& usage, double first_ms, double turn_ms);

// Where the agent's model thinks.
class Backend {
public:
  virtual ~Backend() = default;
  // {"kind": "local" | "fleet" | "cloud", "model", "where"}: what the app
  // and the log say runs it.
  virtual Json describe() const = 0;
  // One turn: the whole conversation so far and the tools; the reply's
  // words to `on_text` as they are written. Blocking; `cancel` stops it.
  virtual Result<Reply> turn(const std::vector<Message>& messages,
                             const std::vector<ToolSpec>& tools,
                             const std::function<void(std::string_view)>&
                                 on_text,
                             const std::atomic<bool>& cancel) = 0;
  // Does it see pictures (a page shown to it)? Else it is told in words.
  virtual bool sees() const { return false; }
  // The tokens its model's context holds -- the helper's as this Mac (or
  // the member that ran the last turn) gives it, a cloud model's window:
  // a conversation nearing it is COMPACTED. 0: not known (compacted only
  // once a turn does not fit).
  virtual std::int64_t context_tokens() const { return 0; }
  // The conversation is over: what is kept for it let go.
  virtual void close() {}
};

// ---- compaction (DESIGN §9b) ------------------------------------------
//
// A conversation outgrowing what its model holds -- the helper's context
// as this Mac gives it (Controller::helper_context_: Qwen3.8 27B's
// 262,144 on tgo-m5-3), a cloud model's window -- goes on COMPACTED, as
// notes stand in for a long meeting. Before a turn that would not fit
// (with room for a reply and for the notes), the model is asked, the
// conversation as it is -- its kept K/V all reused, only the question
// prefilled -- to write what it needs to carry on: the request and what
// the person said since, what is made and where (ids), what is under
// way, what is left. The conversation then goes on from its HEAD (the
// system message, the person's first request: the head every turn is
// matched by), ONE message of notes -- the model's, the person's words
// since the request as they were, Valtz's own record of the steps run
// -- and the latest whole turns as they were. A turn that did not fit
// (the context full already: the model cannot be asked) is compacted on
// Valtz's notes alone, and tried again. The document itself is never
// compacted: the model reads it again with its tools.

// About how many tokens a model reads for these: a token every 3 bytes
// of words (English runs 4 a token, CJK 3 bytes a character -- over,
// rather than under), 1,600 a picture, 8 a message. From `from` on.
std::int64_t estimate_tokens(const Message&);
std::int64_t estimate_tokens(const std::vector<Message>&,
                             std::size_t from = 0);
std::int64_t estimate_tokens(const std::vector<ToolSpec>&);

// The tokens a turn's conversation took, its reply included, as its
// backend counted them: the helper's `ctx_pos`, a cloud's read (cached
// or not) and written. 0: not counted.
std::int64_t context_used(const Json& usage);

// Is `e` a conversation longer than its model's context -- the helper's
// own (kAgentContextFull), a cloud's refusal ("prompt is too long",
// "context_length_exceeded", "maximum context length")?
bool context_overflow(const Error& e);

// The question asking the model for its notes (a user message, put
// after the conversation as it is): `used` and `limit` in tokens.
Message compaction_request(std::int64_t used, std::int64_t limit);

// The HEAD kept through every compaction: the system message(s) and the
// person's first request -- the index after them.
std::size_t compaction_head(const std::vector<Message>&);

// Where the latest turns kept as they were begin: the earliest message
// after the head from which the rest is within `budget` tokens -- an
// assistant's reply or a user's message, never a tool's result apart
// from the call it answers. messages.size(): none fits.
std::size_t compaction_tail(const std::vector<Message>&,
                            std::int64_t budget);

struct CompactionNotes {
  std::string              summary;  // the model's own; "": not asked
  std::vector<std::string> said;     // the person's words since
  // Their bytes at most (each cut at 4,000): a share of the context.
  std::size_t              said_bytes = 16000;
  std::string              steps;    // Valtz's record of the steps run
  int                      steps_run = 0;
  std::int64_t             used = 0;
  std::int64_t             limit = 0;
};
// The conversation compacted: its head, the notes (a user message named
// "compaction"), its messages from `tail` on.
std::vector<Message> compact(const std::vector<Message>&, std::size_t tail,
                             const CompactionNotes&);
// Is `m` a compaction's notes?
bool is_compaction(const Message& m);

// ---- the local helper's wire format (vpipe text-chat's) ---------------
//
// The conversation as kOpAgentTurn's "conversation" param: messages in
// vpipe's shape -- an assistant's `raw` as its content, a tool's result as
// its content -- `images` the indexes of each message's pictures among
// the job's image inputs (in order). Its tools advertised.
Json conversation_beat(const std::vector<Message>& messages,
                       const std::vector<ToolSpec>& tools,
                       const std::string& key,
                       const std::filesystem::path& dir,
                       int max_new_tokens);
// The turn the helper made (kOpAgentTurn's Output data) as a Reply; its
// calls given ids `<turn>-<n>`, their arguments parsed (an argument that
// is not JSON is kept as {"_raw": text}).
Reply reply_from_turn(const Json& turn, int turn_number);

// ---- the cloud -----------------------------------------------------------

// Which API, which model. Kept in Settings (<support>/agent.json), the
// KEY never with it: the Keychain holds that.
struct CloudConfig {
  std::string provider;   // anthropic | openai | openai-compatible
  std::string model;      // "claude-sonnet-5-5", ...
  // An OpenAI-compatible server's root ("http://localhost:1234/v1");
  // "" the provider's own.
  std::string base_url;
  int         max_tokens = 4096;
  // The model's context window, tokens: compacted as a conversation
  // nears it. 0: the provider's own (cloud_context_tokens).
  std::int64_t context_tokens = 0;
};

Json to_json(const CloudConfig&);
CloudConfig cloud_config_from_json(const Json&);
// The model a provider's field starts from; "" none.
std::string default_cloud_model(std::string_view provider);
// The window a conversation is kept within: the configured one, else
// the provider's models' (Anthropic's 200,000; OpenAI's 128,000); 0 for
// a compatible server's, which only it knows -- compacted when it
// refuses a turn as too long.
std::int64_t cloud_context_tokens(const CloudConfig&);

// The provider's API key, in the login Keychain (service
// "com.tgous.valtz.agent", account the provider) -- never in a file, a
// log, a project or a fleet job. "" removes it.
Status store_api_key(const std::string& provider, const std::string& key);
bool has_api_key(const std::string& provider);

// A backend speaking to the provider's API -- its key from the Keychain,
// or `key` (tests, a mock server). Streams the reply as it comes.
std::unique_ptr<Backend> make_cloud_backend(CloudConfig cfg,
                                            std::string key = {});

// The request bodies, and the streams read back -- pure, for the tests.
// A picture rides with its message as base64 PNG.
Json anthropic_request(const CloudConfig&, const std::vector<Message>&,
                       const std::vector<ToolSpec>&, bool stream = true);
Json openai_request(const CloudConfig&, const std::vector<Message>&,
                    const std::vector<ToolSpec>&, bool stream = true);

// A server-sent-event stream read one event's data (JSON) at a time.
class StreamReader {
public:
  virtual ~StreamReader() = default;
  // One event; its words, if any, to `on_text`.
  virtual void event(std::string_view type, const Json& data,
                     const std::function<void(std::string_view)>& on_text)
      = 0;
  // An error the stream itself carried; "" none.
  virtual std::string error() const = 0;
  virtual Reply reply() const = 0;
};
std::unique_ptr<StreamReader> anthropic_stream();
std::unique_ptr<StreamReader> openai_stream();
// A whole (non-streamed) response read.
Result<Reply> anthropic_response(const Json&);
Result<Reply> openai_response(const Json&);

// Lines of an SSE body split into events ("event:" / "data:" fields, a
// blank line ending each); `feed` returns the events completed.
struct SseEvent {
  std::string type;
  std::string data;
};
class SseSplitter {
public:
  std::vector<SseEvent> feed(std::string_view bytes);

private:
  std::string _buf;
  std::string _type;
  std::string _data;
};

}  // namespace valtz::agent

#endif
