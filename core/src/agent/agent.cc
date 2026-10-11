#include "valtz/agent/agent.h"

#include "valtz/base/text.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <iterator>
#include <map>

namespace valtz::agent {

namespace fs = std::filesystem;

namespace {

// JSON text to a value, leniently: anything that does not parse is kept
// as its text.
Json
arguments_of(std::string_view text)
{
  if (text.empty()) {
    return Json::object();
  }
  Json j = Json::parse(text, nullptr, false);
  if (j.is_discarded()) {
    return Json{{"_raw", std::string(text)}};
  }
  return j;
}

std::string
call_id_for(int turn, std::size_t n)
{
  return std::format("t{}-{}", turn, n);
}

// A picture's media type, by its name: what the APIs take.
std::string
image_type(const fs::path& file)
{
  std::string ext = file.extension().string();
  for (auto& c : ext) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return ext == ".jpg" || ext == ".jpeg" ? "image/jpeg" : "image/png";
}

// A picture as base64, for a message to a cloud API; "" unreadable.
std::string
png_base64(const fs::path& file)
{
  std::ifstream in(file, std::ios::binary);
  if (!in) {
    return {};
  }
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
  return base64(bytes);
}

// What the local helper gave back for an assistant message made by
// another backend: its words, then its calls as Qwen's own blocks.
std::string
hermes_text(const Message& m)
{
  std::string s = m.content;
  for (const auto& c : m.calls) {
    if (!s.empty()) {
      s += "\n";
    }
    s += "<tool_call>\n" +
         to_text(Json{{"name", c.name}, {"arguments", c.arguments}}) +
         "\n</tool_call>";
  }
  return s;
}

}  // namespace

// ---- messages ----------------------------------------------------------

Json
to_json(const Message& m)
{
  Json j = {{"role", m.role}, {"content", m.content}};
  if (!m.raw.empty()) {
    j["raw"] = m.raw;
  }
  if (!m.calls.empty()) {
    Json calls = Json::array();
    for (const auto& c : m.calls) {
      calls.push_back({{"id", c.id}, {"name", c.name},
                       {"arguments", c.arguments}});
    }
    j["calls"] = std::move(calls);
  }
  if (!m.call_id.empty()) {
    j["call_id"] = m.call_id;
  }
  if (!m.name.empty()) {
    j["name"] = m.name;
  }
  if (!m.images.empty()) {
    Json imgs = Json::array();
    for (const auto& p : m.images) {
      imgs.push_back(p.string());
    }
    j["images"] = std::move(imgs);
  }
  if (!m.stats.empty()) {
    j["stats"] = m.stats;
  }
  return j;
}

Message
message_from_json(const Json& j)
{
  Message m;
  m.role = jget<std::string>(j, "role", "");
  m.content = jget<std::string>(j, "content", "");
  m.raw = jget<std::string>(j, "raw", "");
  m.call_id = jget<std::string>(j, "call_id", "");
  m.name = jget<std::string>(j, "name", "");
  for (const auto& c : jget(j, "calls", Json::array())) {
    m.calls.push_back({jget<std::string>(c, "id", ""),
                       jget<std::string>(c, "name", ""),
                       jget(c, "arguments", Json::object())});
  }
  for (const auto& p : jget(j, "images", Json::array())) {
    if (p.is_string()) {
      m.images.emplace_back(p.get<std::string>());
    }
  }
  m.stats = jget(j, "stats", Json::object());
  return m;
}

Json
turn_stats(const Json& usage, double first_ms, double turn_ms)
{
  auto rate = [](double n, double ms) {
    return ms > 0 ? std::round(n * 10000.0 / ms) / 10.0 : 0.0;
  };
  Json s = Json::object();
  // A side's tokens, and its time where there is one.
  auto side = [&](const char* name, double n, double ms) {
    s[std::format("{}_tokens", name)] = std::llround(n);
    if (ms > 0) {
      s[std::format("{}_ms", name)] = std::llround(ms);
      if (n > 0) {
        s[std::format("{}_tps", name)] = rate(n, ms);
      }
    }
  };
  if (usage.contains("prefill_ms")) {
    // The helper's -- here or on a member: vpipe's own clock. A vpipe
    // without "decoded" says how long it wrote, not how much.
    side("prefill", jget(usage, "prefilled", 0.0),
         jget(usage, "prefill_ms", 0.0));
    if (usage.contains("decoded")) {
      side("decode", jget(usage, "decoded", 0.0),
           jget(usage, "decode_ms", 0.0));
    } else {
      s["decode_ms"] = jget<std::int64_t>(usage, "decode_ms", 0);
    }
  } else if (usage.contains("input_tokens") ||
             usage.contains("output_tokens")) {
    // A cloud's counts on Valtz's clock: until its first words, what it
    // read (the network's time in it); from there, what it wrote.
    const double first = first_ms >= 0 ? first_ms : 0.0;
    side("prefill", jget(usage, "input_tokens", 0.0), first);
    side("decode", jget(usage, "output_tokens", 0.0),
         std::max(0.0, turn_ms - first));
    if (const auto c = jget<std::int64_t>(usage, "cache_read_tokens", 0);
        c > 0) {
      s["cached_tokens"] = c;
    }
  }
  for (const char* k : {"reused", "restored", "ctx_pos"}) {
    if (usage.contains(k)) {
      s[k] = usage[k];
    }
  }
  if (first_ms >= 0) {
    s["first_ms"] = std::llround(first_ms);
  }
  s["turn_ms"] = std::llround(turn_ms);
  return s;
}

// ---- the local helper --------------------------------------------------

Json
conversation_beat(const std::vector<Message>& messages,
                  const std::vector<ToolSpec>& tools, const std::string& key,
                  const fs::path& dir, int max_new_tokens)
{
  Json msgs = Json::array();
  int image = 0;
  for (const auto& m : messages) {
    Json j = {{"role", m.role}};
    if (m.role == "assistant") {
      j["content"] = m.raw.empty() ? hermes_text(m) : m.raw;
    } else {
      j["content"] = m.content;
    }
    if (m.role == "tool" && !m.name.empty()) {
      j["name"] = m.name;
    }
    if (!m.images.empty()) {
      Json idx = Json::array();
      for (std::size_t i = 0; i < m.images.size(); ++i) {
        idx.push_back(image++);
      }
      j["images"] = std::move(idx);
    }
    msgs.push_back(std::move(j));
  }
  Json t = Json::array();
  for (const auto& s : tools) {
    t.push_back({{"name", s.name}, {"description", s.description},
                 {"parameters", s.parameters}});
  }
  Json beat = {{"messages", std::move(msgs)}, {"tools", std::move(t)}};
  if (!key.empty()) {
    beat["context"] = key;
  }
  if (!dir.empty()) {
    beat["context_dir"] = dir.string();
  }
  if (max_new_tokens > 0) {
    beat["max_new_tokens"] = max_new_tokens;
  }
  return beat;
}

Reply
reply_from_turn(const Json& turn, int turn_number)
{
  Reply r;
  r.raw = jget<std::string>(turn, "text", "");
  r.content = jget<std::string>(turn, "content", "");
  r.finish = jget<std::string>(turn, "finish", "stop");
  std::size_t n = 0;
  for (const auto& c : jget(turn, "tool_calls", Json::array())) {
    ToolCall call;
    call.id = call_id_for(turn_number, n++);
    call.name = jget<std::string>(c, "name", "");
    call.arguments = arguments_of(jget<std::string>(c, "arguments", ""));
    r.calls.push_back(std::move(call));
  }
  for (const char* k : {"prefilled", "reused", "restored", "ctx_bytes",
                        "ctx_pos", "prefill_ms", "decode_ms", "decoded"}) {
    if (turn.contains(k)) {
      r.usage[k] = turn[k];
    }
  }
  return r;
}

// ---- the cloud ---------------------------------------------------------

Json
to_json(const CloudConfig& c)
{
  return {{"provider", c.provider}, {"model", c.model},
          {"base_url", c.base_url}, {"max_tokens", c.max_tokens}};
}

CloudConfig
cloud_config_from_json(const Json& j)
{
  CloudConfig c;
  c.provider = jget<std::string>(j, "provider", "");
  c.model = jget<std::string>(j, "model", "");
  c.base_url = jget<std::string>(j, "base_url", "");
  c.max_tokens = std::clamp(jget(j, "max_tokens", 4096), 256, 64000);
  return c;
}

std::string
default_cloud_model(std::string_view provider)
{
  if (provider == "anthropic") {
    return "claude-sonnet-5-5";
  }
  if (provider == "openai") {
    return "gpt-5";
  }
  return {};
}

Json
anthropic_request(const CloudConfig& cfg, const std::vector<Message>& msgs,
                  const std::vector<ToolSpec>& tools, bool stream)
{
  Json body = {{"model", cfg.model}, {"max_tokens", cfg.max_tokens}};
  if (stream) {
    body["stream"] = true;
  }
  // The system text and the tools are the long, unchanging head of every
  // turn: cached (a cache point on the system block covers both).
  Json system = Json::array();
  Json out = Json::array();
  auto text_block = [](const std::string& t) {
    return Json{{"type", "text"}, {"text", t}};
  };
  auto image_block = [](const fs::path& p) {
    return Json{{"type", "image"},
                {"source", {{"type", "base64"},
                            {"media_type", image_type(p)},
                            {"data", png_base64(p)}}}};
  };
  // Tool results and the user message after them make ONE user turn:
  // the API takes the roles alternating.
  auto user_blocks = [&]() -> Json& {
    if (out.empty() || jget<std::string>(out.back(), "role", "") != "user") {
      out.push_back({{"role", "user"}, {"content", Json::array()}});
    }
    return out.back()["content"];
  };
  for (const auto& m : msgs) {
    if (m.role == "system") {
      if (!m.content.empty()) {
        system.push_back(text_block(m.content));
      }
    } else if (m.role == "user") {
      Json& c = user_blocks();
      for (const auto& p : m.images) {
        c.push_back(image_block(p));
      }
      if (!m.content.empty()) {
        c.push_back(text_block(m.content));
      }
    } else if (m.role == "tool") {
      user_blocks().push_back({{"type", "tool_result"},
                               {"tool_use_id", m.call_id},
                               {"content", m.content}});
    } else if (m.role == "assistant") {
      Json c = Json::array();
      if (!m.content.empty()) {
        c.push_back(text_block(m.content));
      }
      for (const auto& call : m.calls) {
        c.push_back({{"type", "tool_use"}, {"id", call.id},
                     {"name", call.name},
                     {"input", call.arguments.is_object()
                                   ? call.arguments
                                   : Json::object()}});
      }
      if (c.empty()) {
        c.push_back(text_block("(nothing)"));
      }
      out.push_back({{"role", "assistant"}, {"content", std::move(c)}});
    }
  }
  if (!system.empty()) {
    system.back()["cache_control"] = {{"type", "ephemeral"}};
    body["system"] = std::move(system);
  }
  if (!tools.empty()) {
    Json t = Json::array();
    for (const auto& s : tools) {
      t.push_back({{"name", s.name}, {"description", s.description},
                   {"input_schema", s.parameters}});
    }
    body["tools"] = std::move(t);
  }
  body["messages"] = std::move(out);
  return body;
}

Json
openai_request(const CloudConfig& cfg, const std::vector<Message>& msgs,
               const std::vector<ToolSpec>& tools, bool stream)
{
  Json body = {{"model", cfg.model}};
  // OpenAI's own takes the newer name; a compatible server, the one they
  // all read.
  body[cfg.provider == "openai" ? "max_completion_tokens" : "max_tokens"] =
      cfg.max_tokens;
  if (stream) {
    body["stream"] = true;
    body["stream_options"] = {{"include_usage", true}};
  }
  Json out = Json::array();
  for (const auto& m : msgs) {
    if (m.role == "system" || (m.role == "user" && m.images.empty())) {
      out.push_back({{"role", m.role}, {"content", m.content}});
    } else if (m.role == "user") {
      Json parts = Json::array();
      if (!m.content.empty()) {
        parts.push_back({{"type", "text"}, {"text", m.content}});
      }
      for (const auto& p : m.images) {
        parts.push_back(
            {{"type", "image_url"},
             {"image_url",
              {{"url", "data:" + image_type(p) + ";base64," +
                           png_base64(p)}}}});
      }
      out.push_back({{"role", "user"}, {"content", std::move(parts)}});
    } else if (m.role == "tool") {
      out.push_back({{"role", "tool"}, {"tool_call_id", m.call_id},
                     {"content", m.content}});
    } else if (m.role == "assistant") {
      Json a = {{"role", "assistant"}};
      a["content"] = m.content.empty() && !m.calls.empty()
                         ? Json(nullptr)
                         : Json(m.content);
      if (!m.calls.empty()) {
        Json calls = Json::array();
        for (const auto& c : m.calls) {
          calls.push_back({{"id", c.id}, {"type", "function"},
                           {"function", {{"name", c.name},
                                         {"arguments",
                                          to_text(c.arguments)}}}});
        }
        a["tool_calls"] = std::move(calls);
      }
      out.push_back(std::move(a));
    }
  }
  body["messages"] = std::move(out);
  if (!tools.empty()) {
    Json t = Json::array();
    for (const auto& s : tools) {
      t.push_back({{"type", "function"},
                   {"function", {{"name", s.name},
                                 {"description", s.description},
                                 {"parameters", s.parameters}}}});
    }
    body["tools"] = std::move(t);
  }
  return body;
}

// ---- server-sent events ------------------------------------------------

std::vector<SseEvent>
SseSplitter::feed(std::string_view bytes)
{
  std::vector<SseEvent> done;
  _buf.append(bytes);
  std::size_t start = 0;
  for (;;) {
    const std::size_t nl = _buf.find('\n', start);
    if (nl == std::string::npos) {
      break;
    }
    std::string_view line(_buf.data() + start, nl - start);
    start = nl + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      if (!_data.empty() || !_type.empty()) {
        done.push_back({std::move(_type), std::move(_data)});
        _type.clear();
        _data.clear();
      }
      continue;
    }
    if (line.front() == ':') {
      continue;   // a comment: a keep-alive
    }
    const std::size_t colon = line.find(':');
    std::string_view field = line.substr(0, colon);
    std::string_view value = colon == std::string_view::npos
                                 ? std::string_view()
                                 : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ') {
      value.remove_prefix(1);
    }
    if (field == "event") {
      _type = std::string(value);
    } else if (field == "data") {
      if (!_data.empty()) {
        _data += "\n";
      }
      _data.append(value);
    }
  }
  _buf.erase(0, start);
  return done;
}

namespace {

std::string
anthropic_finish(std::string_view stop)
{
  if (stop == "tool_use") {
    return "tool_calls";
  }
  if (stop == "max_tokens") {
    return "length";
  }
  return "stop";
}

class AnthropicStream final : public StreamReader {
public:
  void
  event(std::string_view type, const Json& d,
        const std::function<void(std::string_view)>& on_text) override
  {
    const std::string own =
        type.empty() ? jget<std::string>(d, "type", "") : std::string(type);
    type = own;
    if (type == "message_start") {
      const Json u = jget(jget(d, "message", Json::object()), "usage",
                          Json::object());
      _r.usage["input_tokens"] = jget(u, "input_tokens", 0);
      _r.usage["cache_read_tokens"] = jget(u, "cache_read_input_tokens", 0);
    } else if (type == "content_block_start") {
      const Json b = jget(d, "content_block", Json::object());
      Block blk;
      blk.type = jget<std::string>(b, "type", "");
      blk.id = jget<std::string>(b, "id", "");
      blk.name = jget<std::string>(b, "name", "");
      _blocks[jget(d, "index", 0)] = std::move(blk);
    } else if (type == "content_block_delta") {
      Block& blk = _blocks[jget(d, "index", 0)];
      const Json delta = jget(d, "delta", Json::object());
      const std::string dt = jget<std::string>(delta, "type", "");
      if (dt == "text_delta") {
        const std::string t = jget<std::string>(delta, "text", "");
        blk.text += t;
        if (on_text && !t.empty()) {
          on_text(t);
        }
      } else if (dt == "input_json_delta") {
        blk.json += jget<std::string>(delta, "partial_json", "");
      }
    } else if (type == "message_delta") {
      _stop = jget<std::string>(jget(d, "delta", Json::object()),
                                "stop_reason", _stop);
      const Json u = jget(d, "usage", Json::object());
      if (u.contains("output_tokens")) {
        _r.usage["output_tokens"] = u["output_tokens"];
      }
    } else if (type == "error") {
      _error = jget<std::string>(jget(d, "error", Json::object()),
                                 "message", "the API returned an error");
    }
  }

  std::string error() const override { return _error; }

  Reply
  reply() const override
  {
    Reply r = _r;
    for (const auto& [i, b] : _blocks) {
      if (b.type == "text") {
        r.content += b.text;
      } else if (b.type == "tool_use") {
        r.calls.push_back({b.id, b.name, arguments_of(b.json)});
      }
    }
    r.raw = r.content;
    r.finish = anthropic_finish(_stop);
    if (!r.calls.empty()) {
      r.finish = "tool_calls";
    }
    return r;
  }

private:
  struct Block {
    std::string type;
    std::string id;
    std::string name;
    std::string text;
    std::string json;
  };
  std::map<int, Block> _blocks;
  std::string          _stop;
  std::string          _error;
  Reply                _r;
};

class OpenAiStream final : public StreamReader {
public:
  void
  event(std::string_view, const Json& d,
        const std::function<void(std::string_view)>& on_text) override
  {
    if (d.contains("error")) {
      _error = jget<std::string>(jget(d, "error", Json::object()),
                                 "message", "the API returned an error");
      return;
    }
    if (const Json u = jget(d, "usage", Json()); u.is_object()) {
      _r.usage["input_tokens"] = jget(u, "prompt_tokens", 0);
      _r.usage["output_tokens"] = jget(u, "completion_tokens", 0);
    }
    for (const auto& ch : jget(d, "choices", Json::array())) {
      const Json delta = jget(ch, "delta", Json::object());
      if (const auto t = jget<std::string>(delta, "content", "");
          !t.empty()) {
        _r.content += t;
        if (on_text) {
          on_text(t);
        }
      }
      for (const auto& tc : jget(delta, "tool_calls", Json::array())) {
        Call& c = _calls[jget(tc, "index", 0)];
        if (auto id = jget<std::string>(tc, "id", ""); !id.empty()) {
          c.id = id;
        }
        const Json fn = jget(tc, "function", Json::object());
        c.name += jget<std::string>(fn, "name", "");
        c.args += jget<std::string>(fn, "arguments", "");
      }
      if (const auto f = jget<std::string>(ch, "finish_reason", "");
          !f.empty()) {
        _finish = f;
      }
    }
  }

  std::string error() const override { return _error; }

  Reply
  reply() const override
  {
    Reply r = _r;
    std::size_t n = 0;
    for (const auto& [i, c] : _calls) {
      r.calls.push_back({c.id.empty() ? std::format("call_{}", n) : c.id,
                         c.name, arguments_of(c.args)});
      ++n;
    }
    r.raw = r.content;
    r.finish = _finish.empty() ? "stop" : _finish;
    if (!r.calls.empty()) {
      r.finish = "tool_calls";
    }
    return r;
  }

private:
  struct Call {
    std::string id;
    std::string name;
    std::string args;
  };
  std::map<int, Call> _calls;
  std::string         _finish;
  std::string         _error;
  Reply               _r;
};

}  // namespace

std::unique_ptr<StreamReader>
anthropic_stream()
{
  return std::make_unique<AnthropicStream>();
}

std::unique_ptr<StreamReader>
openai_stream()
{
  return std::make_unique<OpenAiStream>();
}

Result<Reply>
anthropic_response(const Json& j)
{
  if (j.contains("error")) {
    return make_error(Code::Engine,
                      jget<std::string>(jget(j, "error", Json::object()),
                                        "message", "the API refused"));
  }
  Reply r;
  for (const auto& b : jget(j, "content", Json::array())) {
    const std::string t = jget<std::string>(b, "type", "");
    if (t == "text") {
      r.content += jget<std::string>(b, "text", "");
    } else if (t == "tool_use") {
      r.calls.push_back({jget<std::string>(b, "id", ""),
                         jget<std::string>(b, "name", ""),
                         jget(b, "input", Json::object())});
    }
  }
  r.raw = r.content;
  r.finish = r.calls.empty()
                 ? anthropic_finish(jget<std::string>(j, "stop_reason", ""))
                 : "tool_calls";
  const Json u = jget(j, "usage", Json::object());
  r.usage = {{"input_tokens", jget(u, "input_tokens", 0)},
             {"output_tokens", jget(u, "output_tokens", 0)},
             {"cache_read_tokens", jget(u, "cache_read_input_tokens", 0)}};
  return r;
}

Result<Reply>
openai_response(const Json& j)
{
  if (j.contains("error")) {
    return make_error(Code::Engine,
                      jget<std::string>(jget(j, "error", Json::object()),
                                        "message", "the API refused"));
  }
  Reply r;
  const Json choices = jget(j, "choices", Json::array());
  if (!choices.empty()) {
    const Json msg = jget(choices[0], "message", Json::object());
    r.content = jget<std::string>(msg, "content", "");
    for (const auto& tc : jget(msg, "tool_calls", Json::array())) {
      const Json fn = jget(tc, "function", Json::object());
      r.calls.push_back({jget<std::string>(tc, "id", ""),
                         jget<std::string>(fn, "name", ""),
                         arguments_of(jget<std::string>(fn, "arguments",
                                                        ""))});
    }
    r.finish = jget<std::string>(choices[0], "finish_reason", "stop");
  }
  if (!r.calls.empty()) {
    r.finish = "tool_calls";
  }
  r.raw = r.content;
  const Json u = jget(j, "usage", Json::object());
  r.usage = {{"input_tokens", jget(u, "prompt_tokens", 0)},
             {"output_tokens", jget(u, "completion_tokens", 0)}};
  return r;
}

}  // namespace valtz::agent
