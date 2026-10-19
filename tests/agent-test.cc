// The agent's wire formats (agent/agent.h): the local helper's
// conversation, the cloud APIs' requests and their streams read back --
// and the cloud backend itself, against a server on the loopback that
// answers as Anthropic's and OpenAI's do. No network, no key.

#include "testing.h"

#include "valtz/agent/agent.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <format>
#include <fstream>
#include <mutex>
#include <thread>

using namespace valtz;
namespace fs = std::filesystem;

namespace {

std::vector<agent::Message>
conversation()
{
  std::vector<agent::Message> m;
  m.push_back({.role = "system", .content = "You edit decks."});
  m.push_back({.role = "user", .content = "Title it Water."});
  agent::Message a{.role = "assistant", .content = "Setting it.",
                   .raw = "Setting it.\n<tool_call>\n{...}\n</tool_call>"};
  a.calls.push_back({"t1-0", "set_title", Json{{"title", "Water"}}});
  m.push_back(std::move(a));
  m.push_back({.role = "tool", .content = "{\"ok\":true}",
               .call_id = "t1-0", .name = "set_title"});
  return m;
}

std::vector<agent::ToolSpec>
tool_list()
{
  return {{"set_title", "Sets the title.",
           Json{{"type", "object"},
                {"properties", {{"title", {{"type", "string"}}}}}}}};
}

// One HTTP exchange on the loopback: the request read whole, `reply`
// written back, the connection closed.
class OneShotServer {
public:
  explicit OneShotServer(std::string reply) : _reply(std::move(reply))
  {
    _fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    ::bind(_fd, reinterpret_cast<sockaddr*>(&a), sizeof a);
    ::listen(_fd, 1);
    socklen_t len = sizeof a;
    ::getsockname(_fd, reinterpret_cast<sockaddr*>(&a), &len);
    _port = ntohs(a.sin_port);
    _thread = std::thread([this] { serve(); });
  }
  ~OneShotServer()
  {
    ::shutdown(_fd, SHUT_RDWR);
    ::close(_fd);
    if (_thread.joinable()) {
      _thread.join();
    }
  }
  int port() const { return _port; }
  std::string
  request()
  {
    std::lock_guard lk(_mu);
    return _request;
  }

private:
  void
  serve()
  {
    const int c = ::accept(_fd, nullptr, nullptr);
    if (c < 0) {
      return;
    }
    std::string req;
    char buf[4096];
    std::size_t want = std::string::npos;
    while (true) {
      const ssize_t n = ::read(c, buf, sizeof buf);
      if (n <= 0) {
        break;
      }
      req.append(buf, static_cast<std::size_t>(n));
      const auto head = req.find("\r\n\r\n");
      if (head != std::string::npos && want == std::string::npos) {
        std::size_t len = 0;
        for (const char* key : {"Content-Length: ", "content-length: "}) {
          if (auto p = req.find(key); p != std::string::npos) {
            len = std::stoul(req.substr(p + std::strlen(key)));
          }
        }
        want = head + 4 + len;
      }
      if (want != std::string::npos && req.size() >= want) {
        break;
      }
    }
    {
      std::lock_guard lk(_mu);
      _request = req;
    }
    std::size_t off = 0;
    while (off < _reply.size()) {
      const ssize_t n = ::write(c, _reply.data() + off, _reply.size() - off);
      if (n <= 0) {
        break;
      }
      off += static_cast<std::size_t>(n);
    }
    ::close(c);
  }

  int         _fd = -1;
  int         _port = 0;
  std::string _reply;
  std::mutex  _mu;
  std::string _request;
  std::thread _thread;
};

std::string
sse_response(const std::string& body)
{
  return std::format("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
                     "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
                     body.size(), body);
}

const char* kAnthropicStream =
    "event: message_start\n"
    "data: {\"type\":\"message_start\",\"message\":{\"usage\":"
    "{\"input_tokens\":120,\"cache_read_input_tokens\":100}}}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,"
    "\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"text_delta\",\"text\":\"Adding the \"}}\n\n"
    "event: ping\ndata: {\"type\":\"ping\"}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,"
    "\"delta\":{\"type\":\"text_delta\",\"text\":\"title.\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "event: content_block_start\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,"
    "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_01\","
    "\"name\":\"set_title\",\"input\":{}}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,"
    "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":"
    "\"{\\\"title\\\": \\\"Wa\"}}\n\n"
    "event: content_block_delta\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,"
    "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":"
    "\"ter\\\"}\"}}\n\n"
    "event: content_block_stop\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "event: message_delta\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":"
    "\"tool_use\"},\"usage\":{\"output_tokens\":30}}\n\n"
    "event: message_stop\ndata: {\"type\":\"message_stop\"}\n\n";

const char* kOpenAiStream =
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\","
    "\"content\":\"Adding \"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"it.\"}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":"
    "0,\"id\":\"call_9\",\"type\":\"function\",\"function\":{\"name\":"
    "\"set_title\",\"arguments\":\"{\\\"tit\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":"
    "0,\"function\":{\"arguments\":\"le\\\":\\\"Water\\\"}\"}}]}}]}\n\n"
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":"
    "\"tool_calls\"}]}\n\n"
    "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":90,"
    "\"completion_tokens\":12}}\n\n"
    "data: [DONE]\n\n";

}  // namespace

// The local helper's turn: an assistant's RAW reply handed back as it
// was written; a tool's result its content; pictures indexed in order.
TEST(agent, the_helper_is_handed_the_conversation_as_written)
{
  auto msgs = conversation();
  msgs.push_back({.role = "user", .content = "Look.",
                  .images = {"/tmp/a.jpg", "/tmp/b.jpg"}});
  const Json beat = agent::conversation_beat(msgs, tool_list(), "k1",
                                             "/tmp/ctx", 512);
  const Json m = beat["messages"];
  REQUIRE(m.size() == 5);
  CHECK(m[2]["content"].get<std::string>().find("<tool_call>") !=
        std::string::npos);
  CHECK(m[3]["role"] == "tool");
  CHECK(m[3]["name"] == "set_title");
  CHECK(m[4]["images"] == Json::array({0, 1}));
  CHECK(beat["context"] == "k1");
  CHECK(beat["context_dir"] == "/tmp/ctx");
  CHECK(beat["max_new_tokens"] == 512);
  CHECK(beat["tools"][0]["name"] == "set_title");
  // A reply made by another backend: its calls written as the helper's.
  agent::Message other{.role = "assistant", .content = "Sure."};
  other.calls.push_back({"x", "set_title", Json{{"title", "Ice"}}});
  const Json b2 = agent::conversation_beat({other}, {}, "", "", 0);
  CHECK(b2["messages"][0]["content"].get<std::string>().find(
            "\"Ice\"") != std::string::npos);
  // Its turn back: calls with ids, arguments parsed.
  const Json turn = {{"text", "raw"}, {"content", "words"},
                     {"finish", "tool_calls"}, {"prefilled", 12},
                     {"restored", "memory"},
                     {"tool_calls", Json::array(
                         {{{"name", "set_title"},
                           {"arguments", "{\"title\":\"Water\"}"}},
                          {{"name", "bad"}, {"arguments", "not json"}}})}};
  const auto r = agent::reply_from_turn(turn, 3);
  REQUIRE(r.calls.size() == 2);
  CHECK(r.calls[0].id == "t3-0");
  CHECK(r.calls[0].arguments["title"] == "Water");
  CHECK(r.calls[1].arguments["_raw"] == "not json");
  CHECK(r.raw == "raw");
  CHECK(r.usage["restored"] == "memory");
}

// A turn's speed for the person debugging it: the helper's by vpipe's
// clock, a cloud's counts on Valtz's -- its first words the prefill --
// and an older vpipe's, which says how long it wrote but not how much.
// Kept with its message, never sent to a model.
TEST(agent, a_turns_speed_is_counted)
{
  const Json turn = {{"text", "x"}, {"finish", "stop"}, {"prefilled", 680},
                     {"reused", 7700}, {"restored", "memory"},
                     {"ctx_pos", 8501}, {"prefill_ms", 200},
                     {"decode_ms", 1500}, {"decoded", 69}};
  const auto r = agent::reply_from_turn(turn, 1);
  const Json h = agent::turn_stats(r.usage, 400.0, 2100.0);
  CHECK(h["prefill_tokens"] == 680);
  CHECK(h["prefill_ms"] == 200);
  CHECK(h["prefill_tps"] == 3400.0);
  CHECK(h["decode_tokens"] == 69);
  CHECK(h["decode_tps"] == 46.0);
  CHECK(h["reused"] == 7700);
  CHECK(h["restored"] == "memory");
  CHECK(h["ctx_pos"] == 8501);
  CHECK(h["first_ms"] == 400);
  CHECK(h["turn_ms"] == 2100);

  CHECK(!h.contains("per_round"));   // no drafter decoded

  Json old = r.usage;
  old.erase("decoded");
  const Json o = agent::turn_stats(old, -1.0, 2100.0);
  CHECK(!o.contains("decode_tokens"));
  CHECK(!o.contains("decode_tps"));
  CHECK(o["decode_ms"] == 1500);
  CHECK(!o.contains("first_ms"));
  // An older vpipe's plain chat: how long it read, not how much.
  old.erase("prefilled");
  const Json p = agent::turn_stats(old, -1.0, 2100.0);
  CHECK(!p.contains("prefill_tokens"));
  CHECK(p["prefill_ms"] == 200);

  // Its drafter's share: the tokens a verify forward bought, the drafts
  // kept.
  Json mtp = turn;
  mtp.update({{"drafter", "mtp"}, {"rounds", 30}, {"drafted", 90},
              {"accepted", 37}});
  const Json d = agent::turn_stats(agent::reply_from_turn(mtp, 1).usage,
                                   400.0, 2100.0);
  CHECK(d["drafter"] == "mtp");
  CHECK(d["rounds"] == 30);
  CHECK(d["per_round"] == 2.3);
  CHECK(d["accepted_pct"] == 41);

  const Json cloud = {{"input_tokens", 1200}, {"cache_read_tokens", 6000},
                      {"output_tokens", 90}};
  const Json c = agent::turn_stats(cloud, 1500.0, 3300.0);
  CHECK(c["prefill_tokens"] == 1200);
  CHECK(c["prefill_ms"] == 1500);
  CHECK(c["prefill_tps"] == 800.0);
  CHECK(c["decode_ms"] == 1800);
  CHECK(c["decode_tps"] == 50.0);
  CHECK(c["cached_tokens"] == 6000);
  // A reply of calls alone: no words came, its decode the whole turn.
  const Json q = agent::turn_stats(cloud, -1.0, 3000.0);
  CHECK(q["prefill_tokens"] == 1200);
  CHECK(!q.contains("prefill_ms"));
  CHECK(!q.contains("prefill_tps"));
  CHECK(q["decode_tps"] == 30.0);

  agent::Message m{.role = "assistant", .content = "Done.", .stats = h};
  const auto back = agent::message_from_json(agent::to_json(m));
  CHECK(back.stats == h);
  const Json beat = agent::conversation_beat({m}, {}, "", "", 0);
  CHECK(!beat["messages"][0].contains("stats"));
  agent::CloudConfig cfg{"anthropic", "claude-test", "", 1000};
  CHECK(agent::anthropic_request(cfg, {m}, {}).dump().find("prefill")
        == std::string::npos);
  CHECK(agent::openai_request(cfg, {m}, {}).dump().find("prefill")
        == std::string::npos);
}

// The cloud's requests: Anthropic's system cached, results in user turns,
// calls as tool_use; OpenAI's tool messages and function calls.
TEST(agent, the_cloud_requests_are_each_apis_own)
{
  agent::CloudConfig cfg{"anthropic", "claude-test", "", 1000};
  const Json a = agent::anthropic_request(cfg, conversation(), tool_list());
  CHECK(a["model"] == "claude-test");
  CHECK(a["stream"] == true);
  CHECK(a["system"][0]["cache_control"]["type"] == "ephemeral");
  CHECK(a["tools"][0]["input_schema"]["type"] == "object");
  REQUIRE(a["messages"].size() == 3);
  CHECK(a["messages"][1]["content"][1]["type"] == "tool_use");
  CHECK(a["messages"][1]["content"][1]["input"]["title"] == "Water");
  CHECK(a["messages"][2]["role"] == "user");
  CHECK(a["messages"][2]["content"][0]["type"] == "tool_result");
  CHECK(a["messages"][2]["content"][0]["tool_use_id"] == "t1-0");
  // A user message after the results joins their turn.
  auto msgs = conversation();
  msgs.push_back({.role = "user", .content = "And now?"});
  const Json a2 = agent::anthropic_request(cfg, msgs, tool_list());
  REQUIRE(a2["messages"].size() == 3);
  CHECK(a2["messages"][2]["content"].size() == 2);

  cfg.provider = "openai";
  const Json o = agent::openai_request(cfg, conversation(), tool_list());
  CHECK(o["max_completion_tokens"] == 1000);
  CHECK(o["messages"][0]["role"] == "system");
  CHECK(o["messages"][2]["tool_calls"][0]["function"]["arguments"] ==
        "{\"title\":\"Water\"}");
  CHECK(o["messages"][3]["role"] == "tool");
  CHECK(o["messages"][3]["tool_call_id"] == "t1-0");
  CHECK(o["tools"][0]["type"] == "function");
  cfg.provider = "openai-compatible";
  CHECK(agent::openai_request(cfg, conversation(), {})
            .contains("max_tokens"));
}

// The streams read back: words as they come, a call's arguments whole
// from their pieces, the finish and the usage.
TEST(agent, the_cloud_streams_are_read_back)
{
  agent::SseSplitter split;
  auto reader = agent::anthropic_stream();
  std::string words;
  auto on = [&](std::string_view t) { words += t; };
  // In awkward pieces, as a network delivers them.
  const std::string all = kAnthropicStream;
  for (std::size_t i = 0; i < all.size(); i += 37) {
    for (auto& ev : split.feed(std::string_view(all).substr(i, 37))) {
      reader->event(ev.type, Json::parse(ev.data), on);
    }
  }
  const auto r = reader->reply();
  CHECK(words == "Adding the title.");
  CHECK(r.content == "Adding the title.");
  REQUIRE(r.calls.size() == 1);
  CHECK(r.calls[0].id == "toolu_01");
  CHECK(r.calls[0].arguments["title"] == "Water");
  CHECK(r.finish == "tool_calls");
  CHECK(r.usage["cache_read_tokens"] == 100);
  CHECK(r.usage["output_tokens"] == 30);

  agent::SseSplitter split2;
  auto oai = agent::openai_stream();
  words.clear();
  for (auto& ev : split2.feed(kOpenAiStream)) {
    if (ev.data != "[DONE]") {
      oai->event(ev.type, Json::parse(ev.data), on);
    }
  }
  const auto r2 = oai->reply();
  CHECK(words == "Adding it.");
  REQUIRE(r2.calls.size() == 1);
  CHECK(r2.calls[0].id == "call_9");
  CHECK(r2.calls[0].arguments["title"] == "Water");
  CHECK(r2.finish == "tool_calls");
  CHECK(r2.usage["input_tokens"] == 90);
}

// The cloud backend end to end, against a server on the loopback: the
// request it sends (the key in its header, nowhere else), the stream it
// reads, a refusal said in words.
TEST(agent, the_cloud_backend_speaks_to_a_server)
{
  {
    OneShotServer server(sse_response(kAnthropicStream));
    agent::CloudConfig cfg{
        "anthropic", "claude-test",
        std::format("http://127.0.0.1:{}/v1", server.port()), 1000};
    auto backend = agent::make_cloud_backend(cfg, "sk-test-key");
    std::string words;
    std::atomic<bool> cancel{false};
    auto r = backend->turn(conversation(), tool_list(),
                           [&](std::string_view t) { words += t; }, cancel);
    REQUIRE_OK(r);
    CHECK(r->calls.size() == 1);
    CHECK(words == "Adding the title.");
    const std::string req = server.request();
    CHECK(req.find("POST /v1/messages") != std::string::npos);
    CHECK(req.find("sk-test-key") != std::string::npos);
    CHECK(req.find("anthropic-version: 2023-06-01") != std::string::npos);
    CHECK(req.find("\"model\":\"claude-test\"") != std::string::npos);
  }
  {
    OneShotServer server(sse_response(kOpenAiStream));
    agent::CloudConfig cfg{
        "openai-compatible", "local-model",
        std::format("http://127.0.0.1:{}/v1/", server.port()), 1000};
    auto backend = agent::make_cloud_backend(cfg, "");
    std::atomic<bool> cancel{false};
    auto r = backend->turn(conversation(), tool_list(), {}, cancel);
    REQUIRE_OK(r);
    CHECK(r->calls.size() == 1);
    CHECK(server.request().find("POST /v1/chat/completions") !=
          std::string::npos);
  }
  {
    const std::string body =
        "{\"type\":\"error\",\"error\":{\"type\":\"authentication_error\","
        "\"message\":\"invalid x-api-key\"}}";
    OneShotServer server(std::format(
        "HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\n"
        "Content-Length: {}\r\nConnection: close\r\n\r\n{}",
        body.size(), body));
    agent::CloudConfig cfg{
        "anthropic", "claude-test",
        std::format("http://127.0.0.1:{}/v1", server.port()), 1000};
    auto backend = agent::make_cloud_backend(cfg, "sk-wrong");
    std::atomic<bool> cancel{false};
    auto r = backend->turn(conversation(), tool_list(), {}, cancel);
    REQUIRE(!r.ok());
    CHECK(r.error().message.find("401") != std::string::npos);
    CHECK(r.error().message.find("invalid x-api-key") != std::string::npos);
  }
}
