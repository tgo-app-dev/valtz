#include "project/bookmark.h"

#import <Foundation/Foundation.h>

#include <sys/stat.h>

namespace valtz::project {

std::string
make_bookmark(const std::filesystem::path& path)
{
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:
                            [NSString stringWithUTF8String:path.c_str()]];
    NSData* d = [url bookmarkDataWithOptions:0
              includingResourceValuesForKeys:nil
                               relativeToURL:nil
                                       error:nil];
    if (!d) {
      return {};
    }
    return std::string([[d base64EncodedStringWithOptions:0] UTF8String]);
  }
}

std::optional<std::filesystem::path>
resolve_bookmark(const std::string& b64)
{
  if (b64.empty()) {
    return std::nullopt;
  }
  @autoreleasepool {
    NSData* d = [[NSData alloc]
        initWithBase64EncodedString:[NSString stringWithUTF8String:b64.c_str()]
                            options:0];
    if (!d) {
      return std::nullopt;
    }
    BOOL stale = NO;
    NSURL* url = [NSURL
        URLByResolvingBookmarkData:d
                           options:NSURLBookmarkResolutionWithoutUI |
                                   NSURLBookmarkResolutionWithoutMounting
                     relativeToURL:nil
               bookmarkDataIsStale:&stale
                             error:nil];
    if (!url || !url.path) {
      return std::nullopt;
    }
    return std::filesystem::path(url.path.UTF8String);
  }
}

std::optional<FileStamp>
stamp_of(const std::filesystem::path& p)
{
  struct stat st;
  if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return std::nullopt;
  }
  return FileStamp{static_cast<std::uint64_t>(st.st_size),
                   std::int64_t{st.st_mtimespec.tv_sec} * 1'000'000'000 +
                       st.st_mtimespec.tv_nsec};
}

}
