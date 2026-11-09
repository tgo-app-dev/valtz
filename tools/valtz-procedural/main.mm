// valtz-procedural -- a PROGRAM drawn (DESIGN §4t): JavaScript -- a
// canvas's 2D or WebGL, SVG, HTML; three.js, Babylon.js -- run in a
// WKWebView nobody sees, stepped a frame at a time on a clock of its
// own, each frame taken as WebKit draws the page, and written: a
// picture (PNG, its transparency kept) or a movie (on black).
//
//   valtz-procedural <spec.json>
//
// The spec: {"program" (a .js file), "libs" (the folder holding
// three/ and babylonjs/), "width", "height", "frames" (0: a still),
// "fps_num", "fps_den", "seed", "epoch" (ms: what Date says at time 0),
// "out" (.png, .mov, .mp4), "codec" (a movie's: prores4444xq | prores4444
// | prores422hq | prores422 | hevc), "load_timeout", "frame_timeout"
// (s), "assets": [{"id", "name", "kind", "file", "type", "width",
// "height", "seconds", "inline" (bytes: smaller, also a data: URL)}] --
// the project's files the page may read, served by place, read-only;
// a byte range of one as a <video> asks), "rate" (the soundtrack's sample
// rate), "sound" (where a clip's soundtrack goes, a WAV), "previews" (a
// folder: a frame every quarter second, small, planar RGB)}.
// It says what happens as JSON lines on stdout: {"event": "log",
// "level", "text", "source", "line", "column"}, {"event": "loaded"},
// {"event": "frame", "frame", "of"}, {"event": "done", "frames",
// "width", "height", "empty", "why"}, {"event": "sound", "file",
// "seconds"}, {"event": "preview", "file", "width", "height", "frame"},
// {"event": "failed", "text"}. Exit 0 when the file is written.
//
// A PROCESS OF ITS OWN, which the core starts for each render and kills
// when it is stopped: a page that never returns (an endless loop) keeps
// its web process busy until the process that made the web view ends --
// letting the view go does not end it -- and WebKit is used from a main
// thread, which the core's jobs are not. Its watchdog ends it the same
// way when a frame runs past its time.
//
// SANDBOXED: the page gets only what this process serves it under its
// own scheme -- the harness, the program, Valtz's copies of three.js and
// Babylon.js -- and data the program makes (data:, blob:). Its CSP,
// WebKit's content rules and the navigation delegate refuse the rest; no
// file URL is ever loaded; the data store is in memory; what could
// reach a network, a device or the person's data is taken out of the
// page before it runs (harness.h); a dialog, a new window, a download, a
// camera are refused.

#import <AppKit/AppKit.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <AVFoundation/AVFoundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <VideoToolbox/VideoToolbox.h>
#import <WebKit/WebKit.h>

#include "harness.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;
using Json = nlohmann::json;
namespace hp = valtz::procedural;

namespace {

// ---- what is said ---------------------------------------------------------

std::mutex g_out_mu;

void
say(const Json& j)
{
  std::lock_guard lk(g_out_mu);
  const std::string line = j.dump(-1, ' ', false,
                                  Json::error_handler_t::replace);
  std::fwrite(line.data(), 1, line.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

[[noreturn]] void
fail(const std::string& text, int code = 2)
{
  say({{"event", "failed"}, {"text", text}});
  // _exit: the web process goes with this one, whatever it is doing.
  _exit(code);
}

std::string
replaced(std::string s, std::string_view key, std::string_view with)
{
  for (std::size_t at = s.find(key); at != std::string::npos;
       at = s.find(key, at + with.size())) {
    s.replace(at, key.size(), with);
  }
  return s;
}

NSString*
ns(std::string_view s)
{
  return [[NSString alloc] initWithBytes:s.data()
                                  length:s.size()
                                encoding:NSUTF8StringEncoding];
}

std::string
str(NSString* s)
{
  return s ? std::string(s.UTF8String ?: "") : std::string();
}

double
seconds_since(std::chrono::steady_clock::time_point t)
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t)
      .count();
}

// ---- the spec ---------------------------------------------------------------

// A file of the project's the page may read (media/procedural.h).
struct Asset {
  std::string id;
  std::string name;
  std::string kind;
  fs::path    file;
  std::string type;
  int         width = 0;
  int         height = 0;
  double      seconds = 0;
  std::uint64_t inline_bytes = 0;
};

struct Spec {
  fs::path    program;
  fs::path    libs;
  int         width = 0;
  int         height = 0;
  int         frames = 0;  // 0: a still
  int         fps_num = 24;
  int         fps_den = 1;
  std::uint64_t seed = 0;
  double      epoch = 0;
  fs::path    out;
  std::string codec = "prores422hq";
  double      load_timeout = 60;
  double      frame_timeout = 30;
  std::vector<Asset> assets;
  int         rate = 48000;
  fs::path    sound;
  fs::path    previews;

  double fps() const { return static_cast<double>(fps_num) / fps_den; }
};

Spec
read_spec(const char* path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    fail(std::string("cannot read the spec ") + path, 1);
  }
  Json j;
  try {
    j = Json::parse(f);
  } catch (const std::exception& e) {
    fail(std::string("the spec is not JSON: ") + e.what(), 1);
  }
  Spec s;
  s.program = j.value("program", std::string());
  s.libs = j.value("libs", std::string());
  s.width = j.value("width", 0);
  s.height = j.value("height", 0);
  s.frames = j.value("frames", 0);
  s.fps_num = j.value("fps_num", 24);
  s.fps_den = j.value("fps_den", 1);
  s.seed = j.value("seed", std::uint64_t{0});
  s.epoch = j.value("epoch", 0.0);
  s.out = j.value("out", std::string());
  s.codec = j.value("codec", std::string("prores422hq"));
  s.load_timeout = j.value("load_timeout", 60.0);
  s.frame_timeout = j.value("frame_timeout", 30.0);
  s.rate = j.value("rate", 48000);
  s.sound = j.value("sound", std::string());
  s.previews = j.value("previews", std::string());
  for (const auto& a : j.value("assets", Json::array())) {
    Asset x;
    x.id = a.value("id", std::string());
    x.name = a.value("name", std::string());
    x.kind = a.value("kind", std::string());
    x.file = a.value("file", std::string());
    x.type = a.value("type", std::string());
    x.width = a.value("width", 0);
    x.height = a.value("height", 0);
    x.seconds = a.value("seconds", 0.0);
    x.inline_bytes = a.value("inline", std::uint64_t{0});
    s.assets.push_back(std::move(x));
  }
  if (s.width < 16 || s.height < 16 || s.width > 8192 || s.height > 8192) {
    fail("the size is 16 to 8192 pixels a side", 1);
  }
  if (s.frames < 0 || s.fps_num <= 0 || s.fps_den <= 0) {
    fail("bad frames or rate", 1);
  }
  if (s.program.empty() || s.out.empty()) {
    fail("the spec names no program or no output", 1);
  }
  return s;
}

// ---- the watchdog -----------------------------------------------------------
//
// What the main thread waits for, and until when: past it, this process
// ends -- and its web process with it.

std::atomic<double> g_deadline{0};  // steady seconds; 0: none
std::atomic<int> g_waiting_frame{-1};
const auto g_start = std::chrono::steady_clock::now();

double
steady_now()
{
  return seconds_since(g_start);
}

void
expect_within(double seconds, int frame)
{
  g_waiting_frame = frame;
  g_deadline = steady_now() + seconds;
}

void
watch()
{
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      const double d = g_deadline.load();
      if (d > 0 && steady_now() > d) {
        const int f = g_waiting_frame.load();
        fail(f < 0 ? "the program did not finish loading in time -- an "
                     "endless loop, or waiting on something that never "
                     "comes"
                   : "frame " + std::to_string(f) +
                         " did not finish in time -- an endless loop, or "
                         "too much to draw in a frame",
             3);
      }
    }
  }).detach();
}

// The main run loop turned until `done` -- WebKit answers on it. A
// block, so a flag a completion handler sets can be read.
void
spin(bool (^done)(void))
{
  while (!done()) {
    @autoreleasepool {
      [[NSRunLoop mainRunLoop]
             runMode:NSDefaultRunLoopMode
          beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.005]];
    }
  }
}

}  // namespace

// ---- WebKit's delegates -----------------------------------------------------

// What the page may load, served from memory or Valtz's own files: the
// document, the harness, the program, the libraries, the project's
// assets it was given -- by place, read-only -- nothing else.
@interface VPScheme : NSObject <WKURLSchemeHandler> {
 @public
  std::vector<Asset> assets;
}
@property(nonatomic) NSData* page;
@property(nonatomic) NSData* loader;
@property(nonatomic) NSData* program;
@property(nonatomic) NSString* libs;
// Tasks the page stopped before their answer came (a clip's frame).
@property(nonatomic) NSMutableSet<id<WKURLSchemeTask>>* stopped;
@end

@implementation VPScheme

- (void)answer:(id<WKURLSchemeTask>)task
          data:(NSData*)body
          type:(NSString*)type
{
  NSDictionary* headers = @{
    @"Content-Type" : type,
    @"Content-Length" : [@(body.length) stringValue],
    @"Content-Security-Policy" : ns(hp::kContentSecurityPolicy),
    @"Cache-Control" : @"no-store",
  };
  [task didReceiveResponse:[[NSHTTPURLResponse alloc]
                                initWithURL:task.request.URL
                                 statusCode:200
                                HTTPVersion:@"HTTP/1.1"
                               headerFields:headers]];
  [task didReceiveData:body];
  [task didFinish];
}

// A file, whole -- or the byte range asked for ("Range: bytes=a-b", as a
// <video> or <audio> reads its file), 206 and its Content-Range.
- (void)file:(const fs::path&)file
        type:(NSString*)type
        task:(id<WKURLSchemeTask>)task
{
  NSFileHandle* fh =
      [NSFileHandle fileHandleForReadingAtPath:@(file.c_str())];
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  if (!fh || ec) {
    [self refuse:task why:"cannot read " + file.filename().string()];
    return;
  }
  NSString* range = [task.request valueForHTTPHeaderField:@"Range"];
  unsigned long long from = 0;
  unsigned long long to = size ? size - 1 : 0;
  bool partial = false;
  if ([range hasPrefix:@"bytes="] && size > 0) {
    NSArray<NSString*>* ab =
        [[range substringFromIndex:6] componentsSeparatedByString:@"-"];
    if (ab.count == 2) {
      if (ab[0].length == 0 && ab[1].length > 0) {  // the last n bytes
        const auto n = std::min<unsigned long long>(
            size, std::strtoull(ab[1].UTF8String, nullptr, 10));
        from = size - n;
      } else {
        from = std::strtoull(ab[0].UTF8String, nullptr, 10);
        if (ab[1].length > 0) {
          to = std::min<unsigned long long>(
              to, std::strtoull(ab[1].UTF8String, nullptr, 10));
        }
      }
      partial = from <= to && from < size;
    }
  }
  if (!partial) {
    from = 0;
    to = size ? size - 1 : 0;
  }
  [fh seekToFileOffset:from];
  NSData* body = size ? [fh readDataOfLength:to - from + 1] : [NSData data];
  [fh closeFile];
  NSMutableDictionary* headers = [@{
    @"Content-Type" : type,
    @"Content-Length" : [@(body.length) stringValue],
    @"Accept-Ranges" : @"bytes",
    @"Content-Security-Policy" : ns(hp::kContentSecurityPolicy),
    @"Cache-Control" : @"no-store",
  } mutableCopy];
  if (partial) {
    headers[@"Content-Range"] =
        [NSString stringWithFormat:@"bytes %llu-%llu/%llu", from,
                                   from + body.length - 1,
                                   static_cast<unsigned long long>(size)];
  }
  [task didReceiveResponse:[[NSHTTPURLResponse alloc]
                                initWithURL:task.request.URL
                                 statusCode:partial ? 206 : 200
                                HTTPVersion:@"HTTP/1.1"
                               headerFields:headers]];
  [task didReceiveData:body];
  [task didFinish];
}

- (void)refuse:(id<WKURLSchemeTask>)task why:(const std::string&)why
{
  say({{"event", "log"}, {"level", "error"}, {"text", why}});
  [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                             code:NSURLErrorFileDoesNotExist
                                         userInfo:nil]];
}

// /assets/<n>: the file; /assets/<n>/frame/<seconds>: a clip's frame
// there, a PNG -- decoded off the main thread, answered on it.
- (BOOL)asset:(NSString*)rel task:(id<WKURLSchemeTask>)task
{
  NSArray<NSString*>* parts = [rel componentsSeparatedByString:@"/"];
  const int n = parts.count > 0 ? parts[0].intValue : -1;
  if (parts.count == 0 || n < 0 ||
      n >= static_cast<int>(assets.size()) ||
      ![[@(n) stringValue] isEqualToString:parts[0]]) {
    return NO;
  }
  const Asset& a = assets[static_cast<std::size_t>(n)];
  if (parts.count == 1) {
    [self file:a.file type:ns(a.type) task:task];
    return YES;
  }
  if (parts.count != 3 || ![parts[1] isEqualToString:@"frame"] ||
      a.kind != "video") {
    return NO;
  }
  const double at = std::max(0.0, parts[2].doubleValue);
  AVURLAsset* movie =
      [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(a.file.c_str())]
                          options:nil];
  AVAssetImageGenerator* gen =
      [AVAssetImageGenerator assetImageGeneratorWithAsset:movie];
  gen.appliesPreferredTrackTransform = YES;
  gen.requestedTimeToleranceBefore = kCMTimeZero;
  gen.requestedTimeToleranceAfter = kCMTimeZero;
  const std::string name = a.name;
  [gen generateCGImageAsynchronouslyForTime:CMTimeMakeWithSeconds(at, 600)
                          completionHandler:^(CGImageRef img, CMTime,
                                              NSError* e) {
    (void)gen;  // kept until it has answered
    NSData* png = nil;
    if (img) {
      NSMutableData* buf = [NSMutableData data];
      CGImageDestinationRef dst = CGImageDestinationCreateWithData(
          (__bridge CFMutableDataRef)buf,
          (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
      CGImageDestinationAddImage(dst, img, nullptr);
      if (CGImageDestinationFinalize(dst)) {
        png = buf;
      }
      CFRelease(dst);
    }
    dispatch_async(dispatch_get_main_queue(), ^{
      if ([self.stopped containsObject:task]) {
        [self.stopped removeObject:task];
        return;
      }
      if (!png) {
        [self refuse:task
                 why:"cannot read \"" + name + "\" at " +
                     std::to_string(at) + " s: " +
                     str(e.localizedDescription)];
        return;
      }
      [self answer:task data:png type:@"image/png"];
    });
  }];
  return YES;
}

- (void)webView:(WKWebView*)webView
    startURLSchemeTask:(id<WKURLSchemeTask>)task
{
  NSURL* url = task.request.URL;
  NSString* path = url.path ?: @"";
  NSData* body = nil;
  NSString* type = @"text/javascript";
  if (![url.host isEqualToString:@"local"]) {
    body = nil;
  } else if ([path isEqualToString:@"/index.html"]) {
    body = self.page;
    type = @"text/html";
  } else if ([path isEqualToString:@"/valtz.js"]) {
    body = self.loader;
  } else if ([path isEqualToString:@"/program.js"]) {
    body = self.program;
  } else if ([path hasPrefix:@"/lib/"]) {
    body = [self library:[path substringFromIndex:5] type:&type];
  } else if ([path hasPrefix:@"/assets/"] &&
             [url.host isEqualToString:@"local"]) {
    if ([self asset:[path substringFromIndex:8] task:task]) {
      return;
    }
  }
  if (!body) {
    say({{"event", "log"}, {"level", "error"},
         {"text", "not found: " + str(url.absoluteString) +
                      " -- the page has the program, three.js (\"three\", "
                      "\"three/addons/...\"), Babylon.js (BABYLON) and "
                      "the assets it was given (valtz.assets), nothing "
                      "else"}});
    [task didFailWithError:[NSError errorWithDomain:NSURLErrorDomain
                                               code:NSURLErrorFileDoesNotExist
                                           userInfo:nil]];
    return;
  }
  NSDictionary* headers = @{
    @"Content-Type" : [type stringByAppendingString:@"; charset=utf-8"],
    @"Content-Security-Policy" : ns(hp::kContentSecurityPolicy),
    @"Cache-Control" : @"no-store",
  };
  [task didReceiveResponse:[[NSHTTPURLResponse alloc]
                                initWithURL:url
                                 statusCode:200
                                HTTPVersion:@"HTTP/1.1"
                               headerFields:headers]];
  [task didReceiveData:body];
  [task didFinish];
}

- (void)webView:(WKWebView*)webView
    stopURLSchemeTask:(id<WKURLSchemeTask>)task
{
  [self.stopped addObject:task];
}

// A library's file: three.js's build and its addons, Babylon.js -- by a
// path under Valtz's copies that goes nowhere else.
- (NSData*)library:(NSString*)rel type:(NSString**)type
{
  const std::string r = str(rel);
  fs::path file;
  if (r == "babylon.js") {
    file = fs::path(str(self.libs)) / "babylonjs" / "babylon.js";
  } else if (r == "three/three.module.js" || r == "three/three.core.js" ||
             r == "three/three.webgpu.js" || r == "three/three.tsl.js") {
    file = fs::path(str(self.libs)) / "three" / "build" /
           fs::path(r).filename();
  } else if (r.starts_with("three/addons/")) {
    const fs::path sub = fs::path(r.substr(13)).lexically_normal();
    if (sub.empty() || sub.is_absolute() ||
        (!sub.empty() && *sub.begin() == "..")) {
      return nil;
    }
    file = fs::path(str(self.libs)) / "three" / "addons" / sub;
  } else {
    return nil;
  }
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    return nil;
  }
  if (file.extension() == ".json") {
    *type = @"application/json";
  }
  return [NSData dataWithContentsOfFile:@(file.c_str())];
}

@end

// The page's messages: its log, its errors, "loaded".
@interface VPMessages : NSObject <WKScriptMessageHandler>
@property(nonatomic) BOOL loaded;
@property(nonatomic) BOOL loadedOk;
@property(nonatomic) int errors;
@end

@implementation VPMessages

- (void)userContentController:(WKUserContentController*)ucc
      didReceiveScriptMessage:(WKScriptMessage*)message
{
  if (![message.body isKindOfClass:[NSDictionary class]]) {
    return;
  }
  NSDictionary* m = message.body;
  NSString* event = m[@"event"];
  if ([event isEqualToString:@"loaded"]) {
    self.loaded = YES;
    self.loadedOk = [m[@"ok"] boolValue];
    return;
  }
  if (![event isEqualToString:@"log"]) {
    return;
  }
  Json j = {{"event", "log"}};
  NSString* level = [m[@"level"] isKindOfClass:[NSString class]]
                        ? m[@"level"] : @"log";
  j["level"] = str(level);
  if ([level isEqualToString:@"error"]) {
    ++self.errors;
  }
  if ([m[@"text"] isKindOfClass:[NSString class]]) {
    j["text"] = str(m[@"text"]);
  }
  if ([m[@"source"] isKindOfClass:[NSString class]]) {
    // The program's own file named as it is to whoever wrote it.
    std::string src = str(m[@"source"]);
    if (src.ends_with("/program.js")) {
      src = "program.js";
    }
    j["source"] = src;
  }
  for (NSString* k in @[ @"line", @"column" ]) {
    if ([m[k] isKindOfClass:[NSNumber class]] && [m[k] intValue] > 0) {
      j[str(k)] = [m[k] intValue];
    }
  }
  say(j);
}

@end

// Navigations, windows, dialogs, devices: the page's own document alone,
// the rest refused.
@interface VPGuard : NSObject <WKNavigationDelegate, WKUIDelegate>
@property(nonatomic) BOOL finished;
@property(nonatomic) NSString* failed;
@end

@implementation VPGuard

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationAction:(WKNavigationAction*)action
                    decisionHandler:
                        (void (^)(WKNavigationActionPolicy))handler
{
  NSURL* url = action.request.URL;
  const bool own = [url.scheme isEqualToString:ns(hp::kScheme)] &&
                   [url.host isEqualToString:@"local"] &&
                   [url.path isEqualToString:@"/index.html"] &&
                   action.targetFrame.isMainFrame && !self.finished;
  const bool blank = [url.absoluteString isEqualToString:@"about:blank"] ||
                     [url.absoluteString isEqualToString:@"about:srcdoc"];
  if (own || (blank && !action.targetFrame.isMainFrame)) {
    handler(WKNavigationActionPolicyAllow);
    return;
  }
  say({{"event", "log"}, {"level", "warn"},
       {"text", "refused to go to " + str(url.absoluteString) +
                    ": the page stays as it is"}});
  handler(WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView*)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse*)response
                      decisionHandler:
                          (void (^)(WKNavigationResponsePolicy))handler
{
  handler(response.canShowMIMEType ? WKNavigationResponsePolicyAllow
                                   : WKNavigationResponsePolicyCancel);
}

- (void)webView:(WKWebView*)webView
    didFinishNavigation:(WKNavigation*)navigation
{
  self.finished = YES;
}

- (void)webView:(WKWebView*)webView
    didFailProvisionalNavigation:(WKNavigation*)navigation
                       withError:(NSError*)error
{
  if (!self.finished) {
    self.failed = error.localizedDescription ?: @"the page did not load";
  }
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView*)webView
{
  fail("the page's process ended -- it crashed, or ran out of memory");
}

- (WKWebView*)webView:(WKWebView*)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration*)configuration
               forNavigationAction:(WKNavigationAction*)action
                    windowFeatures:(WKWindowFeatures*)features
{
  return nil;
}

- (void)webView:(WKWebView*)webView
    runJavaScriptAlertPanelWithMessage:(NSString*)message
                      initiatedByFrame:(WKFrameInfo*)frame
                     completionHandler:(void (^)(void))handler
{
  say({{"event", "log"}, {"level", "log"}, {"text", "alert: " + str(message)}});
  handler();
}

- (void)webView:(WKWebView*)webView
    runJavaScriptConfirmPanelWithMessage:(NSString*)message
                        initiatedByFrame:(WKFrameInfo*)frame
                       completionHandler:(void (^)(BOOL))handler
{
  handler(NO);
}

- (void)webView:(WKWebView*)webView
    runJavaScriptTextInputPanelWithPrompt:(NSString*)prompt
                              defaultText:(NSString*)text
                         initiatedByFrame:(WKFrameInfo*)frame
                        completionHandler:(void (^)(NSString*))handler
{
  handler(nil);
}

- (void)webView:(WKWebView*)webView
    runOpenPanelWithParameters:(WKOpenPanelParameters*)parameters
              initiatedByFrame:(WKFrameInfo*)frame
             completionHandler:(void (^)(NSArray<NSURL*>*))handler
{
  handler(nil);
}

- (void)webView:(WKWebView*)webView
    requestMediaCapturePermissionForOrigin:(WKSecurityOrigin*)origin
                          initiatedByFrame:(WKFrameInfo*)frame
                                      type:(WKMediaCaptureType)type
                           decisionHandler:
                               (void (^)(WKPermissionDecision))handler
{
  handler(WKPermissionDecisionDeny);
}

- (void)webView:(WKWebView*)webView
    requestDeviceOrientationAndMotionPermissionForOrigin:
        (WKSecurityOrigin*)origin
                                        initiatedByFrame:(WKFrameInfo*)frame
                                         decisionHandler:
                                             (void (^)(WKPermissionDecision))
                                                 handler
{
  handler(WKPermissionDecisionDeny);
}

@end

namespace {

// ---- the writers ------------------------------------------------------------

// A still: a PNG, its transparency kept.
void
write_png(CGImageRef img, const fs::path& out)
{
  NSURL* url = [NSURL fileURLWithPath:@(out.c_str())];
  CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
      (__bridge CFURLRef)url, (__bridge CFStringRef)UTTypePNG.identifier, 1,
      nullptr);
  if (!dst) {
    fail("cannot write " + out.string());
  }
  CGImageDestinationAddImage(dst, img, nullptr);
  const bool ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  if (!ok) {
    fail("cannot write " + out.string());
  }
}

// A movie: each frame on black, drawn into the 709 Core Video reads the
// file back in (the core's own clips' way: encode_movie_stack).
class MovieWriter {
public:
  MovieWriter(const Spec& s) : _s(s)
  {
    const bool hevc = s.codec == "hevc";
    AVVideoCodecType codec =
        hevc                         ? AVVideoCodecTypeHEVC
        : s.codec == "prores4444xq"  ? AVVideoCodecTypeAppleProRes4444XQ
        : s.codec == "prores4444"    ? AVVideoCodecTypeAppleProRes4444
        : s.codec == "prores422"     ? AVVideoCodecTypeAppleProRes422
                                     : AVVideoCodecTypeAppleProRes422HQ;
    std::error_code ec;
    fs::remove(s.out, ec);
    NSError* err = nil;
    _w = [AVAssetWriter
        assetWriterWithURL:[NSURL fileURLWithPath:@(s.out.c_str())]
                  fileType:hevc ? AVFileTypeMPEG4 : AVFileTypeQuickTimeMovie
                     error:&err];
    if (!_w) {
      fail("cannot write " + s.out.string());
    }
    NSMutableDictionary* settings = [@{
      AVVideoCodecKey : codec,
      AVVideoWidthKey : @(s.width),
      AVVideoHeightKey : @(s.height),
      AVVideoColorPropertiesKey : @{
        AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
        AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
        AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
      },
    } mutableCopy];
    if (hevc) {
      settings[AVVideoCompressionPropertiesKey] = @{
        AVVideoProfileLevelKey :
            (__bridge NSString*)kVTProfileLevel_HEVC_Main10_AutoLevel,
        AVVideoQualityKey : @0.8,
      };
    } else if ([codec isEqualToString:AVVideoCodecTypeAppleProRes4444] ||
               [codec isEqualToString:AVVideoCodecTypeAppleProRes4444XQ]) {
      // Opaque, on black: no alpha plane (vpipe's readers took one).
      settings[AVVideoCompressionPropertiesKey] = @{
        (__bridge NSString*)kVTCompressionPropertyKey_PreserveAlphaChannel :
            @NO,
      };
    }
    _in = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                             outputSettings:settings];
    _in.expectsMediaDataInRealTime = NO;
    _adaptor = [AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:_in
                                   sourcePixelBufferAttributes:@{
      (id)kCVPixelBufferPixelFormatTypeKey :
          @(kCVPixelFormatType_64RGBAHalf),
      (id)kCVPixelBufferWidthKey : @(s.width),
      (id)kCVPixelBufferHeightKey : @(s.height),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    }];
    [_w addInput:_in];
    if (![_w startWriting]) {
      fail("cannot write " + s.out.string() + ": " +
           str(_w.error.localizedDescription));
    }
    [_w startSessionAtSourceTime:kCMTimeZero];
    const void* keys[] = {kCVImageBufferColorPrimariesKey,
                          kCVImageBufferTransferFunctionKey,
                          kCVImageBufferYCbCrMatrixKey};
    const void* values[] = {kCVImageBufferColorPrimaries_ITU_R_709_2,
                            kCVImageBufferTransferFunction_ITU_R_709_2,
                            kCVImageBufferYCbCrMatrix_ITU_R_709_2};
    CFDictionaryRef tags = CFDictionaryCreate(
        nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    _space = CVImageBufferCreateColorSpaceFromAttachments(tags);
    CFRelease(tags);
    if (!_space) {
      _space = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
    }
    CGColorSpaceRef working =
        CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    _ctx = [CIContext contextWithOptions:@{
      kCIContextWorkingColorSpace : (__bridge id)working,
      kCIContextWorkingFormat : @(kCIFormatRGBAh),
    }];
    CGColorSpaceRelease(working);
  }

  ~MovieWriter()
  {
    if (_space) {
      CGColorSpaceRelease(_space);
    }
  }

  void add(CGImageRef img, int f)
  {
    const CGRect r = CGRectMake(0, 0, _s.width, _s.height);
    CIImage* pic = [CIImage imageWithCGImage:img];
    pic = [pic imageByCompositingOverImage:
                   [[CIImage imageWithColor:[CIColor blackColor]]
                       imageByCroppingToRect:r]];
    while (!_in.readyForMoreMediaData) {
      [NSThread sleepForTimeInterval:0.002];
    }
    CVPixelBufferRef pb = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(nullptr, _adaptor.pixelBufferPool,
                                           &pb) != kCVReturnSuccess ||
        !pb) {
      fail("no frame buffer to encode into");
    }
    [_ctx render:pic toCVPixelBuffer:pb bounds:r colorSpace:_space];
    const CMTime pts = CMTimeMake(static_cast<int64_t>(f) * _s.fps_den,
                                  _s.fps_num);
    const bool ok = [_adaptor appendPixelBuffer:pb withPresentationTime:pts];
    CVPixelBufferRelease(pb);
    if (!ok) {
      fail("cannot encode frame " + std::to_string(f) + ": " +
           str(_w.error.localizedDescription));
    }
  }

  void finish()
  {
    [_in markAsFinished];
    __block bool done = false;
    [_w finishWritingWithCompletionHandler:^{
      done = true;
    }];
    spin(^{ return done; });
    if (_w.status != AVAssetWriterStatusCompleted) {
      fail("cannot finish " + _s.out.string() + ": " +
           str(_w.error.localizedDescription));
    }
  }

private:
  const Spec&                          _s;
  AVAssetWriter*                       _w = nil;
  AVAssetWriterInput*                  _in = nil;
  AVAssetWriterInputPixelBufferAdaptor* _adaptor = nil;
  CIContext*                           _ctx = nil;
  CGColorSpaceRef                      _space = nullptr;
};

// The frame as WebKit draws the page: a pixel a CSS pixel, the size
// asked, sRGB, its transparency kept.
CGImageRef
snapshot(WKWebView* view, const Spec& s)
{
  WKSnapshotConfiguration* conf = [WKSnapshotConfiguration new];
  conf.afterScreenUpdates = YES;
  const CGFloat scale = view.window ? view.window.backingScaleFactor
                        : NSScreen.mainScreen
                            ? NSScreen.mainScreen.backingScaleFactor
                            : 1;
  conf.snapshotWidth = @(s.width / scale);
  __block NSImage* got = nil;
  __block bool done = false;
  __block NSString* why = nil;
  [view takeSnapshotWithConfiguration:conf
                    completionHandler:^(NSImage* img, NSError* e) {
                      got = img;
                      why = e.localizedDescription;
                      done = true;
                    }];
  spin(^{ return done; });
  if (!got) {
    fail("the page could not be drawn: " + str(why));
  }
  NSRect rect = NSMakeRect(0, 0, got.size.width, got.size.height);
  CGImageRef cg = [got CGImageForProposedRect:&rect context:nil hints:nil];
  if (!cg) {
    fail("the page could not be drawn");
  }
  if (static_cast<int>(CGImageGetWidth(cg)) == s.width &&
      static_cast<int>(CGImageGetHeight(cg)) == s.height) {
    return CGImageRetain(cg);
  }
  // Another scale than the screen's: drawn again at the size asked.
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef c = CGBitmapContextCreate(
      nullptr, s.width, s.height, 8, 0, srgb,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
          static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
  CGColorSpaceRelease(srgb);
  CGContextSetInterpolationQuality(c, kCGInterpolationHigh);
  CGContextDrawImage(c, CGRectMake(0, 0, s.width, s.height), cg);
  CGImageRef out = CGBitmapContextCreateImage(c);
  CGContextRelease(c);
  return out;
}

// Is every pixel clear: nothing drawn?
bool
clear(CGImageRef img)
{
  const std::size_t w = CGImageGetWidth(img);
  const std::size_t h = CGImageGetHeight(img);
  std::vector<std::uint8_t> px(w * h * 4);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef c = CGBitmapContextCreate(
      px.data(), w, h, 8, w * 4, srgb,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
          static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
  CGColorSpaceRelease(srgb);
  CGContextDrawImage(c, CGRectMake(0, 0, w, h), img);
  CGContextRelease(c);
  for (std::size_t i = 3; i < px.size(); i += 4) {
    if (px[i] != 0) {
      return false;
    }
  }
  return true;
}

// A frame shrunk for the stage's preview -- at most 480 on its longer
// side, on black -- written as planar RGB beside the others, its name
// said (the core reads it, then removes it).
void
preview(CGImageRef img, const Spec& s, int frame, int n)
{
  const double k = std::min(1.0, 480.0 / std::max(s.width, s.height));
  const int w = std::max(1, static_cast<int>(s.width * k));
  const int h = std::max(1, static_cast<int>(s.height * k));
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef c = CGBitmapContextCreate(
      rgba.data(), w, h, 8, static_cast<std::size_t>(w) * 4, srgb,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
          static_cast<std::uint32_t>(kCGBitmapByteOrder32Big));
  CGColorSpaceRelease(srgb);
  CGContextSetRGBFillColor(c, 0, 0, 0, 1);
  CGContextFillRect(c, CGRectMake(0, 0, w, h));
  CGContextSetInterpolationQuality(c, kCGInterpolationMedium);
  CGContextDrawImage(c, CGRectMake(0, 0, w, h), img);
  CGContextRelease(c);
  const std::size_t plane = static_cast<std::size_t>(w) * h;
  std::vector<std::uint8_t> planar(plane * 3);
  for (std::size_t i = 0; i < plane; ++i) {
    for (std::size_t ch = 0; ch < 3; ++ch) {
      planar[ch * plane + i] = rgba[i * 4 + ch];
    }
  }
  const fs::path file = s.previews / ("preview-" + std::to_string(n));
  const fs::path part = fs::path(file.string() + ".part");
  {
    std::ofstream f(part, std::ios::binary);
    f.write(reinterpret_cast<const char*>(planar.data()),
            static_cast<std::streamsize>(planar.size()));
  }
  std::error_code ec;
  fs::rename(part, file, ec);
  if (!ec) {
    say({{"event", "preview"}, {"file", file.string()}, {"width", w},
         {"height", h}, {"frame", frame}});
  }
}

// A clip's SOUNDTRACK, when its program made one (valtz.audio): rendered
// by the page, written as a 16-bit WAV, said. Silent, nothing.
void
soundtrack(WKWebView* view, const Spec& s)
{
  __block NSDictionary* got = nil;
  __block NSString* why = nil;
  __block bool done = false;
  [view callAsyncJavaScript:@"return await window.__valtz_sound();"
                  arguments:@{}
                    inFrame:nil
             inContentWorld:WKContentWorld.pageWorld
          completionHandler:^(id value, NSError* e) {
            if ([value isKindOfClass:[NSDictionary class]]) {
              got = value;
            }
            why = e ? (e.userInfo[@"WKJavaScriptExceptionMessage"]
                           ?: e.localizedDescription)
                    : nil;
            done = true;
          }];
  spin(^{ return done; });
  if (why) {
    say({{"event", "log"}, {"level", "error"},
         {"text", "the soundtrack: " + str(why)}});
    return;
  }
  NSString* b64 = got[@"data"];
  if (![b64 isKindOfClass:[NSString class]]) {
    return;  // none made, or silent
  }
  NSData* pcm = [[NSData alloc] initWithBase64EncodedString:b64 options:0];
  const int rate = [got[@"rate"] intValue];
  const int channels = [got[@"channels"] intValue];
  if (!pcm || rate <= 0 || channels <= 0) {
    return;
  }
  auto le = [](std::ofstream& f, std::uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
      f.put(static_cast<char>((v >> (8 * i)) & 0xff));
    }
  };
  std::ofstream f(s.sound, std::ios::binary);
  const auto size = static_cast<std::uint32_t>(pcm.length);
  f.write("RIFF", 4);
  le(f, 36 + size, 4);
  f.write("WAVEfmt ", 8);
  le(f, 16, 4);
  le(f, 1, 2);  // PCM
  le(f, static_cast<std::uint32_t>(channels), 2);
  le(f, static_cast<std::uint32_t>(rate), 4);
  le(f, static_cast<std::uint32_t>(rate * channels * 2), 4);
  le(f, static_cast<std::uint32_t>(channels * 2), 2);
  le(f, 16, 2);
  f.write("data", 4);
  le(f, size, 4);
  f.write(static_cast<const char*>(pcm.bytes),
          static_cast<std::streamsize>(pcm.length));
  if (!f) {
    fail("cannot write the soundtrack " + s.sound.string());
  }
  say({{"event", "sound"},
       {"file", s.sound.string()},
       {"seconds", static_cast<double>(pcm.length) / (2.0 * channels) /
                       rate}});
}

// A page left clear, said: what it held -- a canvas drawn on but never
// added to the page, nothing at all, nothing drawing a clip's frames.
std::string
clear_why(WKWebView* view, const Spec& s)
{
  __block NSDictionary* got = nil;
  __block bool done = false;
  [view callAsyncJavaScript:@"return window.__valtz_inspect();"
                  arguments:@{}
                    inFrame:nil
             inContentWorld:WKContentWorld.pageWorld
          completionHandler:^(id value, NSError*) {
            if ([value isKindOfClass:[NSDictionary class]]) {
              got = value;
            }
            done = true;
          }];
  spin(^{ return done; });
  if (!got) {
    return {};
  }
  const int canvases = [got[@"canvases"] intValue];
  const int contexts = [got[@"contexts"] intValue];
  const int elements = [got[@"elements"] intValue];
  const int drawers = [got[@"on_frame"] intValue] +
                      [got[@"waiting"] intValue];
  if (contexts > 0 && canvases == 0) {
    return "it drew on a canvas that is not on the page -- draw on "
           "valtz.canvas, or add the renderer's canvas to the page "
           "(document.body.appendChild(renderer.domElement))";
  }
  if (elements == 0) {
    return "the page holds nothing -- draw on valtz.canvas (2D or "
           "WebGL), or add SVG or HTML to document.body";
  }
  if (s.frames > 0 && drawers == 0 && contexts > 0) {
    return "nothing draws its frames -- draw each in valtz.onFrame((t, "
           "i) => ...), or with requestAnimationFrame / "
           "renderer.setAnimationLoop (each frame's callbacks run once, "
           "at its time)";
  }
  if (contexts > 0) {
    return "its canvas is on the page but clear -- nothing drawn on it, "
           "or drawn with no alpha: a three.js scene needs "
           "renderer.render(scene, camera) in each frame, a camera that "
           "sees it and light for lit materials";
  }
  return "the page's elements are clear: nothing visible was drawn";
}

// The content rules compiled, in a store of their own that goes with
// this process.
WKContentRuleList*
content_rules(const fs::path& dir)
{
  WKContentRuleListStore* store =
      [WKContentRuleListStore storeWithURL:[NSURL fileURLWithPath:
                                                      @(dir.c_str())]];
  __block WKContentRuleList* list = nil;
  __block NSString* why = nil;
  __block bool done = false;
  [store compileContentRuleListForIdentifier:@"valtz-procedural"
                      encodedContentRuleList:ns(hp::kContentRules)
                           completionHandler:^(WKContentRuleList* l,
                                               NSError* e) {
                             list = l;
                             why = e.localizedDescription;
                             done = true;
                           }];
  spin(^{ return done; });
  if (!list) {
    fail("cannot compile the page's content rules: " + str(why), 1);
  }
  return list;
}

int
render(const Spec& s)
{
  NSData* program = [NSData dataWithContentsOfFile:@(s.program.c_str())];
  if (!program) {
    fail("cannot read the program " + s.program.string(), 1);
  }
  const std::string code(static_cast<const char*>(program.bytes),
                         program.length);

  char tmpl[] = "/tmp/valtz-procedural-XXXXXX";
  const char* tmp = mkdtemp(tmpl);
  if (!tmp) {
    fail("cannot make a scratch folder", 1);
  }
  const fs::path scratch = tmp;
  struct Gone {
    fs::path p;
    ~Gone()
    {
      std::error_code ec;
      fs::remove_all(p, ec);
    }
  } gone{scratch};

  WKWebViewConfiguration* cfg = [WKWebViewConfiguration new];
  cfg.websiteDataStore = [WKWebsiteDataStore nonPersistentDataStore];
  cfg.preferences.javaScriptCanOpenWindowsAutomatically = NO;
  cfg.preferences.fraudulentWebsiteWarningEnabled = NO;
  cfg.preferences.elementFullscreenEnabled = NO;
  cfg.mediaTypesRequiringUserActionForPlayback =
      WKAudiovisualMediaTypeAll;
  cfg.defaultWebpagePreferences.allowsContentJavaScript = YES;
  // WebRTC off where this WebKit lets a host say so: the page's own copy
  // is taken away anyway (harness.h), in every frame.
  SEL peer = NSSelectorFromString(@"_setPeerConnectionEnabled:");
  if ([cfg.preferences respondsToSelector:peer]) {
    NSMethodSignature* sig =
        [cfg.preferences methodSignatureForSelector:peer];
    NSInvocation* inv = [NSInvocation invocationWithMethodSignature:sig];
    BOOL off = NO;
    inv.selector = peer;
    [inv setArgument:&off atIndex:2];
    [inv invokeWithTarget:cfg.preferences];
  }

  VPScheme* scheme = [VPScheme new];
  scheme.stopped = [NSMutableSet set];
  scheme->assets = s.assets;
  const bool babylon = code.find("BABYLON") != std::string::npos;
  scheme.page = [ns(replaced(replaced(std::string(hp::kPage), "@CSP@",
                                      hp::kContentSecurityPolicy),
                             "@BABYLON@",
                             babylon ? hp::kBabylonTag : ""))
      dataUsingEncoding:NSUTF8StringEncoding];
  scheme.loader = [ns(hp::kLoader) dataUsingEncoding:NSUTF8StringEncoding];
  scheme.program = program;
  scheme.libs = @(s.libs.c_str());
  [cfg setURLSchemeHandler:scheme forURLScheme:ns(hp::kScheme)];

  Json params = {{"width", s.width},   {"height", s.height},
                 {"fps", s.fps()},     {"frames", s.frames},
                 {"seed", s.seed},     {"epoch", s.epoch},
                 {"rate", s.rate},     {"assets", Json::array()}};
  // The assets, by place; a small one's bytes written in as well.
  for (std::size_t i = 0; i < s.assets.size(); ++i) {
    const Asset& a = s.assets[i];
    Json x = {{"name", a.name},       {"id", a.id},
              {"kind", a.kind},       {"type", a.type},
              {"width", a.width},     {"height", a.height},
              {"seconds", a.seconds},
              {"url", std::string(hp::kOrigin) + "/assets/" +
                          std::to_string(i)}};
    std::error_code ec;
    const auto size = fs::file_size(a.file, ec);
    if (!ec && size <= a.inline_bytes) {
      if (NSData* d = [NSData dataWithContentsOfFile:@(a.file.c_str())]) {
        x["data"] = "data:" + a.type + ";base64," +
                    str([d base64EncodedStringWithOptions:0]);
      }
    }
    params["assets"].push_back(std::move(x));
  }
  WKUserContentController* ucc = cfg.userContentController;
  const auto at_start = WKUserScriptInjectionTimeAtDocumentStart;
  [ucc addUserScript:[[WKUserScript alloc]
                         initWithSource:ns(replaced(
                                            std::string(hp::kBootstrap),
                                            "@PARAMS@", params.dump()))
                          injectionTime:at_start
                       forMainFrameOnly:NO]];
  VPMessages* messages = [VPMessages new];
  [ucc addScriptMessageHandler:messages name:@"valtz"];
  [ucc addContentRuleList:content_rules(scratch / "rules")];

  WKWebView* view =
      [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, s.width, s.height)
                         configuration:cfg];
  // The page's own background, or none: a still keeps its transparency.
  [view setValue:@NO forKey:@"drawsBackground"];
  // In a window nobody sees -- borderless, clear, far off any screen --
  // for WebKit loads no <video> or <audio> of a page in none (its
  // network state NO_SOURCE, nothing asked of the scheme).
  NSWindow* window = [[NSWindow alloc]
      initWithContentRect:NSMakeRect(-40000, -40000, s.width, s.height)
                styleMask:NSWindowStyleMaskBorderless
                  backing:NSBackingStoreBuffered
                    defer:NO];
  window.releasedWhenClosed = NO;
  window.alphaValue = 0;
  window.opaque = NO;
  window.backgroundColor = NSColor.clearColor;
  window.hasShadow = NO;
  window.ignoresMouseEvents = YES;
  window.collectionBehavior = NSWindowCollectionBehaviorTransient |
                              NSWindowCollectionBehaviorIgnoresCycle;
  window.contentView = view;
  [window orderFrontRegardless];
  VPGuard* guard = [VPGuard new];
  view.navigationDelegate = guard;
  view.UIDelegate = guard;

  // Loaded: the program run, what it asked to be waited for done.
  const auto t0 = std::chrono::steady_clock::now();
  expect_within(s.load_timeout, -1);
  [view loadRequest:[NSURLRequest
                        requestWithURL:[NSURL URLWithString:@"valtz-proc://"
                                                            @"local/"
                                                            @"index.html"]]];
  spin(^{
    return static_cast<bool>(messages.loaded) || guard.failed != nil;
  });
  if (guard.failed) {
    fail("the page did not load: " + str(guard.failed));
  }
  say({{"event", "loaded"},
       {"ok", static_cast<bool>(messages.loadedOk)},
       {"ms", static_cast<int>(seconds_since(t0) * 1000)}});

  const int n = s.frames > 0 ? s.frames : 1;
  std::optional<MovieWriter> movie;
  if (s.frames > 0) {
    movie.emplace(s);
  }
  bool first_clear = false;
  bool last_clear = false;
  double shown_at = 0;
  int previews = 0;
  for (int i = 0; i < n; ++i) {
    @autoreleasepool {
      expect_within(s.frame_timeout, i);
      __block bool stepped = false;
      __block NSString* why = nil;
      [view callAsyncJavaScript:@"return await window.__valtz_step(i);"
                      arguments:@{@"i" : @(i)}
                        inFrame:nil
                 inContentWorld:WKContentWorld.pageWorld
              completionHandler:^(id value, NSError* e) {
                why = e ? (e.userInfo[@"WKJavaScriptExceptionMessage"]
                               ?: e.localizedDescription)
                        : nil;
                stepped = true;
              }];
      spin(^{ return stepped; });
      if (why) {
        say({{"event", "log"}, {"level", "error"},
             {"text", "frame " + std::to_string(i) + ": " + str(why)}});
      }
      CGImageRef img = snapshot(view, s);
      if (i == 0) {
        first_clear = clear(img);
      }
      if (i == n - 1) {
        last_clear = i == 0 ? first_clear : clear(img);
      }
      if (movie) {
        movie->add(img, i);
      } else {
        write_png(img, s.out);
      }
      // The stage's preview: a frame a quarter second, small.
      if (movie && !s.previews.empty() &&
          (i == 0 || steady_now() - shown_at >= 0.25)) {
        shown_at = steady_now();
        preview(img, s, i, ++previews);
      }
      CGImageRelease(img);
      say({{"event", "frame"}, {"frame", i + 1}, {"of", n}});
    }
  }
  g_deadline = 0;
  // Nothing drawn: why, as the page holds it now.
  std::string why;
  if (first_clear && last_clear) {
    why = clear_why(view, s);
  }
  if (movie) {
    expect_within(120, n);
    movie->finish();
    g_deadline = 0;
  }
  // Its soundtrack, when it made one: rendered, written as a WAV.
  if (movie && !s.sound.empty()) {
    expect_within(120, n);
    soundtrack(view, s);
    g_deadline = 0;
  }
  say({{"event", "done"},
       {"why", why},
       {"frames", s.frames},
       {"width", s.width},
       {"height", s.height},
       {"errors", static_cast<int>(messages.errors)},
       // Nothing drawn: the page clear at its first and its last frame.
       {"empty", first_clear && last_clear},
       {"ms", static_cast<int>(seconds_since(t0) * 1000)}});
  [ucc removeAllScriptMessageHandlers];
  return 0;
}

}  // namespace

int
main(int argc, char** argv)
{
  @autoreleasepool {
    if (argc != 2) {
      std::fprintf(stderr, "usage: valtz-procedural <spec.json>\n");
      return 1;
    }
    const Spec s = read_spec(argv[1]);
    // No Dock icon, no menu: a process nobody sees.
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyProhibited];
    watch();
    return render(s);
  }
}
