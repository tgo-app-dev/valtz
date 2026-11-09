#include "valtz/base/url-fetch.h"

#import <Foundation/Foundation.h>

#include <spawn.h>
#include <sys/wait.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <format>
#include <mutex>
#include <regex>

extern char** environ;

namespace fs = std::filesystem;

// The session's delegate: each piece written to the file as it comes,
// counted; the end signalled.
@interface ValtzFetch : NSObject <NSURLSessionDataDelegate>
@property(nonatomic) FILE* file;
@property(nonatomic) std::uint64_t done;
@property(nonatomic) std::uint64_t total;
@property(nonatomic, copy) NSString* type;
@property(nonatomic) NSInteger status;
@property(nonatomic, strong) NSError* error;
@property(nonatomic) BOOL stopped;
@property(nonatomic, strong) dispatch_semaphore_t ended;
@property(nonatomic) const valtz::FetchProgress* progress;
@end

@implementation ValtzFetch
- (void)URLSession:(NSURLSession*)session
              dataTask:(NSURLSessionDataTask*)task
    didReceiveResponse:(NSURLResponse*)response
     completionHandler:
         (void (^)(NSURLSessionResponseDisposition))handler
{
  self.total = response.expectedContentLength > 0
                   ? static_cast<std::uint64_t>(
                         response.expectedContentLength)
                   : 0;
  self.type = response.MIMEType ?: @"";
  if ([response isKindOfClass:[NSHTTPURLResponse class]]) {
    self.status = static_cast<NSHTTPURLResponse*>(response).statusCode;
  }
  handler(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)session
          dataTask:(NSURLSessionDataTask*)task
    didReceiveData:(NSData*)data
{
  [data enumerateByteRangesUsingBlock:^(const void* bytes, NSRange range,
                                        BOOL*) {
    if (self.file) {
      std::fwrite(bytes, 1, range.length, self.file);
    }
  }];
  self.done += data.length;
  if (self.progress && *self.progress &&
      !(*self.progress)(self.done, self.total)) {
    self.stopped = YES;
    [task cancel];
  }
}

- (void)URLSession:(NSURLSession*)session
                    task:(NSURLSessionTask*)task
    didCompleteWithError:(NSError*)error
{
  self.error = error;
  dispatch_semaphore_signal(self.ended);
}
@end

namespace valtz {

Result<Fetched>
fetch_url(const std::string& url, const fs::path& out,
          const FetchProgress& progress)
{
  @autoreleasepool {
    NSURL* u = [NSURL URLWithString:@(url.c_str())];
    if (!u) {
      return make_error(Code::InvalidArgument,
                        std::format("not a URL: {}", url));
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
      return make_error(Code::Io, std::format("cannot write {}",
                                              out.string()));
    }
    ValtzFetch* d = [ValtzFetch new];
    d.file = f;
    d.ended = dispatch_semaphore_create(0);
    d.progress = &progress;
    NSURLSessionConfiguration* cfg =
        [NSURLSessionConfiguration ephemeralSessionConfiguration];
    cfg.timeoutIntervalForRequest = 60;
    NSOperationQueue* q = [NSOperationQueue new];
    q.maxConcurrentOperationCount = 1;
    NSURLSession* s = [NSURLSession sessionWithConfiguration:cfg
                                                    delegate:d
                                               delegateQueue:q];
    NSURLSessionDataTask* task = [s dataTaskWithURL:u];
    [task resume];
    dispatch_semaphore_wait(d.ended, DISPATCH_TIME_FOREVER);
    [s finishTasksAndInvalidate];
    std::fclose(f);
    d.file = nullptr;
    if (d.stopped) {
      fs::remove(out, ec);
      return make_error(Code::Cancelled, "stopped");
    }
    if (d.error) {
      fs::remove(out, ec);
      return make_error(Code::Io, std::format(
          "{} could not be fetched: {}", url,
          d.error.localizedDescription.UTF8String));
    }
    if (d.status >= 400) {
      fs::remove(out, ec);
      return make_error(Code::Io, std::format(
          "{} could not be fetched: HTTP {}", url,
          static_cast<long>(d.status)));
    }
    Fetched r;
    r.bytes = d.done;
    r.content_type = d.type.lowercaseString.UTF8String;
    return r;
  }
}

std::string
drive_confirm_url(std::string_view html)
{
  const std::string page(html);
  static const std::regex form(
      R"re(<form[^>]*id="download-form"[^>]*action="([^"]+)")re");
  std::smatch m;
  if (!std::regex_search(page, m, form)) {
    // The action may come before the id.
    static const std::regex any(
        std::string(R"re(<form[^>]*action="()re") +
        R"re(https://drive\.usercontent\.google\.com/download[^"]*)")re");
    if (!std::regex_search(page, m, any)) {
      return {};
    }
  }
  std::string url = m[1].str();
  static const std::regex field(
      R"re(<input type="hidden" name="([a-z_]+)" value="([^"]*)")re");
  std::string query;
  for (auto it = std::sregex_iterator(page.begin(), page.end(), field);
       it != std::sregex_iterator(); ++it) {
    query += (query.empty() ? "" : "&") + (*it)[1].str() + "=" +
             (*it)[2].str();
  }
  if (query.empty()) {
    return {};
  }
  return url + (url.find('?') == std::string::npos ? "?" : "&") + query;
}

Status
take_from_zip(const fs::path& zip, const std::vector<std::string>& take,
              const fs::path& dir)
{
  std::error_code ec;
  const fs::path unpacked =
      dir.parent_path() / (".unzip-" + dir.filename().string());
  fs::remove_all(unpacked, ec);
  fs::create_directories(unpacked, ec);
  const std::string exe = "/usr/bin/ditto";
  const std::string src = zip.string(), dst = unpacked.string();
  char* argv[] = {const_cast<char*>(exe.c_str()), const_cast<char*>("-x"),
                  const_cast<char*>("-k"), const_cast<char*>(src.c_str()),
                  const_cast<char*>(dst.c_str()), nullptr};
  pid_t pid = 0;
  if (posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv, environ) !=
      0) {
    return make_error(Code::Io, "cannot start ditto to open the zip");
  }
  int status = 0;
  waitpid(pid, &status, 0);
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fs::remove_all(unpacked, ec);
    return make_error(Code::Corrupt, std::format(
        "{} could not be opened as a zip", zip.filename().string()));
  }
  fs::create_directories(dir, ec);
  std::vector<std::string> missing;
  for (const auto& name : take) {
    fs::path found;
    for (auto it = fs::recursive_directory_iterator(unpacked, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      if (it->is_directory() && it->path().filename() == "__MACOSX") {
        it.disable_recursion_pending();
        continue;
      }
      if (it->is_regular_file() && it->path().filename() == name) {
        found = it->path();
        break;
      }
    }
    if (found.empty()) {
      missing.push_back(name);
      continue;
    }
    fs::copy_file(found, dir / name, fs::copy_options::overwrite_existing,
                  ec);
    if (ec) {
      fs::remove_all(unpacked, ec);
      return make_error(Code::Io, std::format("cannot write {}",
                                              (dir / name).string()));
    }
  }
  fs::remove_all(unpacked, ec);
  if (!missing.empty()) {
    std::string list;
    for (const auto& n : missing) {
      list += (list.empty() ? "" : ", ") + n;
    }
    return make_error(Code::Corrupt, std::format(
        "{} holds no {}", zip.filename().string(), list));
  }
  return ok_status();
}

}  // namespace valtz
