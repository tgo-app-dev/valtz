#include "valtz/media/thumbnail.h"

#import <AVFoundation/AVFoundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <format>

namespace valtz::media {

namespace {

CGImageRef
image_thumbnail(NSURL* url, int max_px)
{
  CGImageSourceRef src = CGImageSourceCreateWithURL(
      (__bridge CFURLRef)url, nullptr);
  if (!src) {
    return nullptr;
  }
  NSDictionary* opts = @{
    (id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
    (id)kCGImageSourceCreateThumbnailWithTransform : @YES,
    (id)kCGImageSourceThumbnailMaxPixelSize : @(max_px),
  };
  CGImageRef img = CGImageSourceCreateThumbnailAtIndex(
      src, 0, (__bridge CFDictionaryRef)opts);
  CFRelease(src);
  return img;
}

CGImageRef
video_thumbnail(NSURL* url, int max_px)
{
  AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
  AVAssetImageGenerator* gen =
      [[AVAssetImageGenerator alloc] initWithAsset:asset];
  gen.appliesPreferredTrackTransform = YES;
  gen.maximumSize = CGSizeMake(max_px, max_px);
  gen.requestedTimeToleranceBefore = CMTimeMake(1, 2);
  gen.requestedTimeToleranceAfter = CMTimeMake(1, 2);

  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  __block CMTime dur = kCMTimeInvalid;
  [asset loadValuesAsynchronouslyForKeys:@[ @"duration" ]
                       completionHandler:^{
                         dispatch_semaphore_signal(sem);
                       }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  dur = asset.duration;
  CMTime at = CMTIME_IS_NUMERIC(dur) && CMTimeGetSeconds(dur) > 0
                  ? CMTimeMultiplyByRatio(dur, 1, 10)
                  : kCMTimeZero;

  __block CGImageRef out = nullptr;
  [gen generateCGImageAsynchronouslyForTime:at
                          completionHandler:^(CGImageRef img, CMTime,
                                              NSError*) {
                            if (img) {
                              out = CGImageRetain(img);
                            }
                            dispatch_semaphore_signal(sem);
                          }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  return out;
}

}

Status
write_thumbnail(const std::filesystem::path& src, MediaType type,
                int max_px, const std::filesystem::path& dst)
{
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:
                            [NSString stringWithUTF8String:src.c_str()]];
    CGImageRef img = nullptr;
    switch (type) {
    case MediaType::Image:
      img = image_thumbnail(url, max_px);
      break;
    case MediaType::Video:
      img = video_thumbnail(url, max_px);
      break;
    default:
      return make_error(Code::Unsupported, "no thumbnail for this media");
    }
    if (!img) {
      return make_error(Code::Unsupported, std::format(
          "could not decode a thumbnail from {}", src.filename().string()));
    }
    NSURL* out = [NSURL fileURLWithPath:
                            [NSString stringWithUTF8String:dst.c_str()]];
    CGImageDestinationRef d = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)out, (__bridge CFStringRef)UTTypeJPEG.identifier,
        1, nullptr);
    if (!d) {
      CGImageRelease(img);
      return make_error(Code::Io, "cannot write thumbnail");
    }
    NSDictionary* props = @{
      (id)kCGImageDestinationLossyCompressionQuality : @0.85,
    };
    CGImageDestinationAddImage(d, img, (__bridge CFDictionaryRef)props);
    bool ok = CGImageDestinationFinalize(d);
    CFRelease(d);
    CGImageRelease(img);
    if (!ok) {
      return make_error(Code::Io, "thumbnail encode failed");
    }
    return ok_status();
  }
}

}
