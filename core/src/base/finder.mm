#include "valtz/base/finder.h"

#import <Foundation/Foundation.h>

#include <format>

namespace valtz {

namespace {

NSURL*
url_of(const std::filesystem::path& p)
{
  return [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.c_str()]];
}

Error
failed(const char* what, const std::filesystem::path& p, NSError* e)
{
  return make_error(Code::Io,
                    std::format("{} {}: {}", what, p.string(),
                                e ? e.localizedDescription.UTF8String
                                  : "unknown error"));
}

}  // namespace

Result<std::vector<std::string>>
finder_tags(const std::filesystem::path& p)
{
  @autoreleasepool {
    NSURL* u = url_of(p);
    NSArray* tags = nil;
    NSError* e = nil;
    // (a fresh read: NSURL caches its resource values)
    [u removeAllCachedResourceValues];
    if (![u getResourceValue:&tags forKey:NSURLTagNamesKey error:&e]) {
      return failed("read the tags of", p, e);
    }
    std::vector<std::string> out;
    for (NSString* t in tags) {
      out.emplace_back(t.UTF8String);
    }
    return out;
  }
}

Status
set_finder_tags(const std::filesystem::path& p,
                const std::vector<std::string>& tags)
{
  @autoreleasepool {
    NSMutableArray* a = [NSMutableArray array];
    for (const auto& t : tags) {
      if (!t.empty()) {
        [a addObject:[NSString stringWithUTF8String:t.c_str()]];
      }
    }
    NSError* e = nil;
    if (![url_of(p) setResourceValue:a forKey:NSURLTagNamesKey error:&e]) {
      return failed("tag", p, e);
    }
    return ok_status();
  }
}

bool
finder_locked(const std::filesystem::path& p)
{
  @autoreleasepool {
    NSURL* u = url_of(p);
    [u removeAllCachedResourceValues];
    NSNumber* v = nil;
    return [u getResourceValue:&v forKey:NSURLIsUserImmutableKey error:nil] &&
           v.boolValue;
  }
}

Status
set_finder_locked(const std::filesystem::path& p, bool locked)
{
  @autoreleasepool {
    NSError* e = nil;
    if (![url_of(p) setResourceValue:@(locked)
                              forKey:NSURLIsUserImmutableKey
                               error:&e]) {
      return failed(locked ? "lock" : "unlock", p, e);
    }
    return ok_status();
  }
}

}  // namespace valtz
