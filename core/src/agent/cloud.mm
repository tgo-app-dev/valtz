// The agent's CLOUD backend: Anthropic's Messages API, OpenAI's Chat
// Completions, or a server speaking the latter (LM Studio, Ollama, vLLM,
// a gateway) -- streamed, through NSURLSession. The key comes from the
// login Keychain and goes in the request's header, nowhere else.

#include "valtz/agent/agent.h"

#include "valtz/base/log.h"
#include "valtz/base/message.h"
#include "valtz/base/text.h"

#import <Foundation/Foundation.h>
#import <Security/Security.h>

#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <thread>

// ---- the streaming reader -----------------------------------------------
//
// One request's bytes, as they come, into the SSE splitter and the
// provider's stream reader; the whole body kept when it is not a stream
// (an error, or a server that answered whole).

@interface ValtzAgentStream : NSObject <NSURLSessionDataDelegate>
@property(nonatomic) NSInteger status;
@property(nonatomic) BOOL      eventStream;
@end

@implementation ValtzAgentStream {
@public
  valtz::agent::SseSplitter                    _sse;
  valtz::agent::StreamReader*                  _reader;
  const std::function<void(std::string_view)>* _onText;
  std::string                                  _body;
  std::string                                  _error;
  std::mutex                                   _mu;
  std::condition_variable                      _cv;
  bool                                         _done;
}

- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:(void (^)(NSURLSessionResponseDisposition))handler
{
  (void)session;
  (void)task;
  if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
    auto* http = (NSHTTPURLResponse*)response;
    self.status = http.statusCode;
    NSString* type = http.allHeaderFields[@"Content-Type"];
    self.eventStream =
        type != nil && [type containsString:@"text/event-stream"];
  }
  handler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data
{
  (void)session;
  (void)task;
  std::string_view bytes(static_cast<const char*>(data.bytes),
                         data.length);
  if (!self.eventStream || self.status >= 400) {
    _body.append(bytes);
    return;
  }
  for (auto& ev : _sse.feed(bytes)) {
    if (ev.data.empty() || ev.data == "[DONE]") {
      continue;
    }
    valtz::Json d = valtz::Json::parse(ev.data, nullptr, false);
    if (!d.is_discarded()) {
      _reader->event(ev.type, d, *_onText);
    }
  }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error
{
  (void)session;
  (void)task;
  std::lock_guard lk(_mu);
  if (error != nil) {
    _error = error.localizedDescription.UTF8String ?: "the request failed";
  }
  _done = true;
  _cv.notify_all();
}

@end

namespace valtz::agent {

namespace {

constexpr const char* kKeychainService = "com.tgous.valtz.agent";

NSMutableDictionary*
keychain_query(const std::string& provider)
{
  return [@{
    (__bridge id)kSecClass : (__bridge id)kSecClassGenericPassword,
    (__bridge id)kSecAttrService : @(kKeychainService),
    (__bridge id)kSecAttrAccount : @(provider.c_str()),
  } mutableCopy];
}

std::string
load_api_key(const std::string& provider)
{
  NSMutableDictionary* q = keychain_query(provider);
  q[(__bridge id)kSecReturnData] = @YES;
  q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
  CFTypeRef out = nullptr;
  if (SecItemCopyMatching((__bridge CFDictionaryRef)q, &out) !=
          errSecSuccess ||
      out == nullptr) {
    return {};
  }
  NSData* data = (__bridge_transfer NSData*)out;
  return std::string(static_cast<const char*>(data.bytes), data.length);
}

// Where a provider's requests go.
std::string
endpoint(const CloudConfig& c)
{
  std::string base = c.base_url;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  if (c.provider == "anthropic") {
    return (base.empty() ? "https://api.anthropic.com/v1" : base) +
           "/messages";
  }
  return (base.empty() ? "https://api.openai.com/v1" : base) +
         "/chat/completions";
}

class CloudBackend final : public Backend {
public:
  CloudBackend(CloudConfig cfg, std::string key)
    : _cfg(std::move(cfg)), _key(std::move(key))
  {
    if (_cfg.model.empty()) {
      _cfg.model = default_cloud_model(_cfg.provider);
    }
  }

  Json
  describe() const override
  {
    return {{"kind", "cloud"}, {"model", _cfg.model},
            {"where", _cfg.provider}};
  }

  bool sees() const override { return true; }
  std::int64_t
  context_tokens() const override
  {
    return cloud_context_tokens(_cfg);
  }

  Result<Reply>
  turn(const std::vector<Message>& messages,
       const std::vector<ToolSpec>& tools,
       const std::function<void(std::string_view)>& on_text,
       const std::atomic<bool>& cancel) override
  {
    const bool anthropic = _cfg.provider == "anthropic";
    std::string key = _key.empty() ? load_api_key(_cfg.provider) : _key;
    if (key.empty() && _cfg.provider != "openai-compatible") {
      return make_error(Code::Unsupported, msg::kAgentNoKey,
                        {{"provider", _cfg.provider}});
    }
    if (_cfg.model.empty()) {
      return make_error(Code::InvalidArgument, msg::kAgentNoCloudModel);
    }
    const Json body = anthropic
        ? anthropic_request(_cfg, messages, tools)
        : openai_request(_cfg, messages, tools);
    const std::string text = to_text(body);
    const std::string url = endpoint(_cfg);

    @autoreleasepool {
      NSURL* u = [NSURL URLWithString:@(url.c_str())];
      if (u == nil) {
        return make_error(Code::InvalidArgument,
                          std::format("not a URL: {}", url));
      }
      NSMutableURLRequest* req = [NSMutableURLRequest requestWithURL:u];
      req.HTTPMethod = @"POST";
      req.timeoutInterval = 600;
      [req setValue:@"application/json" forHTTPHeaderField:@"Content-Type"];
      [req setValue:@"text/event-stream" forHTTPHeaderField:@"Accept"];
      if (anthropic) {
        [req setValue:@(key.c_str()) forHTTPHeaderField:@"x-api-key"];
        [req setValue:@"2023-06-01"
            forHTTPHeaderField:@"anthropic-version"];
      } else if (!key.empty()) {
        [req setValue:[@"Bearer " stringByAppendingString:@(key.c_str())]
            forHTTPHeaderField:@"Authorization"];
      }
      req.HTTPBody = [NSData dataWithBytes:text.data() length:text.size()];

      auto reader = anthropic ? anthropic_stream() : openai_stream();
      ValtzAgentStream* stream = [[ValtzAgentStream alloc] init];
      stream->_reader = reader.get();
      stream->_onText = &on_text;
      stream->_done = false;
      NSURLSessionConfiguration* conf =
          [NSURLSessionConfiguration ephemeralSessionConfiguration];
      conf.timeoutIntervalForRequest = 600;
      NSURLSession* session =
          [NSURLSession sessionWithConfiguration:conf
                                        delegate:stream
                                   delegateQueue:nil];
      NSURLSessionDataTask* task = [session dataTaskWithRequest:req];
      [task resume];
      {
        std::unique_lock lk(stream->_mu);
        while (!stream->_done) {
          stream->_cv.wait_for(lk, std::chrono::milliseconds(100));
          if (cancel.load() && !stream->_done) {
            lk.unlock();
            [task cancel];
            lk.lock();
          }
        }
      }
      [session finishTasksAndInvalidate];
      if (cancel.load()) {
        return make_error(Code::Cancelled, "stopped");
      }
      const NSInteger status = stream.status;
      if (!stream->_error.empty() && status == 0) {
        return make_error(Code::Io, msg::kAgentCloudUnreachable,
                          {{"url", url}, {"why", stream->_error}});
      }
      if (status >= 400) {
        const Json e = Json::parse(stream->_body, nullptr, false);
        const std::string head(utf8_prefix(stream->_body, 300));
        std::string why = e.is_discarded()
            ? head
            : jget<std::string>(jget(e, "error", Json::object()), "message",
                                head);
        return make_error(Code::Engine, msg::kAgentCloudRefused,
                          {{"provider", _cfg.provider},
                           {"status", std::to_string(status)},
                           {"why", why}});
      }
      if (!stream.eventStream) {
        // Answered whole: a server that does not stream.
        const Json whole = Json::parse(stream->_body, nullptr, false);
        if (whole.is_discarded()) {
          return make_error(Code::Engine, "the API's answer is not JSON");
        }
        auto r = anthropic ? anthropic_response(whole)
                           : openai_response(whole);
        if (r.ok() && on_text && !r->content.empty()) {
          on_text(r->content);
        }
        return r;
      }
      if (const std::string e = reader->error(); !e.empty()) {
        return make_error(Code::Engine, msg::kAgentCloudError,
                          {{"provider", _cfg.provider}, {"why", e}});
      }
      return reader->reply();
    }
  }

private:
  CloudConfig _cfg;
  std::string _key;
};

}  // namespace

Status
store_api_key(const std::string& provider, const std::string& key)
{
  if (provider.empty()) {
    return make_error(Code::InvalidArgument, "no provider named");
  }
  NSMutableDictionary* q = keychain_query(provider);
  SecItemDelete((__bridge CFDictionaryRef)q);
  if (key.empty()) {
    return ok_status();
  }
  q[(__bridge id)kSecValueData] = [NSData dataWithBytes:key.data()
                                                 length:key.size()];
  q[(__bridge id)kSecAttrLabel] = @"Valtz agent API key";
  const OSStatus st = SecItemAdd((__bridge CFDictionaryRef)q, nullptr);
  if (st != errSecSuccess) {
    return make_error(Code::Io, msg::kAgentKeychain,
                      {{"status", std::to_string(st)}});
  }
  return ok_status();
}

bool
has_api_key(const std::string& provider)
{
  NSMutableDictionary* q = keychain_query(provider);
  q[(__bridge id)kSecMatchLimit] = (__bridge id)kSecMatchLimitOne;
  return SecItemCopyMatching((__bridge CFDictionaryRef)q, nullptr) ==
         errSecSuccess;
}

std::unique_ptr<Backend>
make_cloud_backend(CloudConfig cfg, std::string key)
{
  return std::make_unique<CloudBackend>(std::move(cfg), std::move(key));
}

}  // namespace valtz::agent
