// The Mac's camera (camera.h): an AVCaptureSession of the camera's video
// -- frames into IOSurface-backed pixel buffers, the newest kept for the
// preview and a still -- and a movie file output for a clip, with the
// default microphone's sound when Valtz may record it.

#include "valtz/media/camera.h"

#include "valtz/base/log.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>

namespace fs = std::filesystem;

// The newest frame, and the end of a recording.
@interface ValtzCameraSink : NSObject <
    AVCaptureVideoDataOutputSampleBufferDelegate,
    AVCaptureFileOutputRecordingDelegate>
@property(nonatomic, strong) dispatch_semaphore_t finished;
@property(nonatomic, strong) NSError* recordError;
- (CVPixelBufferRef)copyNewest:(std::uint64_t*)count CF_RETURNS_RETAINED;
@end

@implementation ValtzCameraSink {
  std::mutex _mu;
  CVPixelBufferRef _newest;
  std::uint64_t _count;
}

- (void)dealloc
{
  if (_newest) {
    CVPixelBufferRelease(_newest);
  }
}

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sample
           fromConnection:(AVCaptureConnection*)connection
{
  CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sample);
  if (!pb) {
    return;
  }
  CVPixelBufferRetain(pb);
  CVPixelBufferRef old = nullptr;
  {
    std::lock_guard lk(_mu);
    old = _newest;
    _newest = pb;
    ++_count;
  }
  if (old) {
    CVPixelBufferRelease(old);
  }
}

- (CVPixelBufferRef)copyNewest:(std::uint64_t*)count
{
  std::lock_guard lk(_mu);
  if (count) {
    *count = _count;
  }
  if (_newest) {
    CVPixelBufferRetain(_newest);
  }
  return _newest;
}

- (void)captureOutput:(AVCaptureFileOutput*)output
    didFinishRecordingToOutputFileAtURL:(NSURL*)url
                        fromConnections:(NSArray<AVCaptureConnection*>*)c
                                  error:(NSError*)error
{
  // A recording can end "with an error" that says it finished well.
  const bool fine =
      !error || [error.userInfo[AVErrorRecordingSuccessfullyFinishedKey]
                    boolValue];
  self.recordError = fine ? nil : error;
  if (self.finished) {
    dispatch_semaphore_signal(self.finished);
  }
}

@end

namespace valtz::media {

namespace {

AVCaptureDevice*
device_for(const std::string& id)
{
  if (id.empty()) {
    return [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
  }
  return [AVCaptureDevice
      deviceWithUniqueID:[NSString stringWithUTF8String:id.c_str()]];
}

NSArray<AVCaptureDevice*>*
cameras()
{
  AVCaptureDeviceDiscoverySession* ds = [AVCaptureDeviceDiscoverySession
      discoverySessionWithDeviceTypes:@[
        AVCaptureDeviceTypeBuiltInWideAngleCamera,
        AVCaptureDeviceTypeContinuityCamera,
        AVCaptureDeviceTypeExternal,
      ]
                            mediaType:AVMediaTypeVideo
                             position:AVCaptureDevicePositionUnspecified];
  return ds.devices;
}

std::string
str(NSString* s)
{
  return s ? std::string(s.UTF8String) : std::string();
}

}

std::vector<CameraSource>
camera_sources()
{
  std::vector<CameraSource> out;
  @autoreleasepool {
    AVCaptureDevice* def =
        [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    for (AVCaptureDevice* d in cameras()) {
      CameraSource s;
      s.id = str(d.uniqueID);
      s.name = str(d.localizedName);
      s.preferred = def && [d.uniqueID isEqualToString:def.uniqueID];
      out.push_back(std::move(s));
    }
  }
  std::ranges::stable_partition(out, [](const CameraSource& s) {
    return s.preferred;
  });
  return out;
}

std::string
camera_permission()
{
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
  case AVAuthorizationStatusAuthorized: return "granted";
  case AVAuthorizationStatusNotDetermined: return "undetermined";
  default: return "denied";
  }
}

bool
request_camera_access()
{
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  __block BOOL ok = NO;
  [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                           completionHandler:^(BOOL granted) {
    ok = granted;
    dispatch_semaphore_signal(done);
  }];
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  return ok;
}

struct CameraCapture::Impl {
  AVCaptureSession* session = nil;
  AVCaptureMovieFileOutput* movie = nil;
  ValtzCameraSink* sink = nil;
  dispatch_queue_t frames = nil;
  dispatch_queue_t control = nil;
  std::string name;
  fs::path recording;

  ~Impl()
  {
    if (movie.isRecording) {
      sink.finished = dispatch_semaphore_create(0);
      [movie stopRecording];
      dispatch_semaphore_wait(sink.finished,
                              dispatch_time(DISPATCH_TIME_NOW,
                                            5 * NSEC_PER_SEC));
      std::error_code ec;
      fs::remove(recording, ec);
    }
    if (session) {
      AVCaptureSession* s = session;
      dispatch_sync(control, ^{ [s stopRunning]; });
    }
  }
};

CameraCapture::CameraCapture() : _impl(std::make_unique<Impl>()) {}
CameraCapture::~CameraCapture() = default;

Result<std::unique_ptr<CameraCapture>>
CameraCapture::start(const std::string& camera, bool sound)
{
  if (camera_permission() != "granted") {
    return make_error(Code::Unsupported,
                      "Valtz may not use the camera: allow it in System "
                      "Settings › Privacy & Security › Camera");
  }
  std::unique_ptr<CameraCapture> c(new CameraCapture());
  Impl& m = *c->_impl;
  @autoreleasepool {
    AVCaptureDevice* dev = device_for(camera);
    if (!dev) {
      return make_error(Code::NotFound,
                        camera.empty() ? std::string("there is no camera")
                                       : std::format("no camera {} is there "
                                                     "now", camera));
    }
    m.name = str(dev.localizedName);
    NSError* err = nil;
    AVCaptureDeviceInput* in =
        [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
    if (!in) {
      return make_error(Code::Io, std::format(
          "{} cannot be used: {}", m.name,
          str(err.localizedDescription)));
    }
    m.session = [AVCaptureSession new];
    [m.session beginConfiguration];
    if ([m.session canSetSessionPreset:AVCaptureSessionPresetHigh]) {
      m.session.sessionPreset = AVCaptureSessionPresetHigh;
    }
    if (![m.session canAddInput:in]) {
      [m.session commitConfiguration];
      return make_error(Code::Io, std::format("{} cannot be used", m.name));
    }
    [m.session addInput:in];
    // A clip's sound: the default microphone, when Valtz may.
    if (sound && [AVCaptureDevice authorizationStatusForMediaType:
                      AVMediaTypeAudio] == AVAuthorizationStatusAuthorized) {
      AVCaptureDevice* mic =
          [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
      AVCaptureDeviceInput* ain =
          mic ? [AVCaptureDeviceInput deviceInputWithDevice:mic error:nil]
              : nil;
      if (ain && [m.session canAddInput:ain]) {
        [m.session addInput:ain];
      }
    }
    // Frames into IOSurfaces, BGRA: what a layer shows as it is.
    m.sink = [ValtzCameraSink new];
    m.frames = dispatch_queue_create("valtz.camera.frames",
                                     DISPATCH_QUEUE_SERIAL);
    AVCaptureVideoDataOutput* video = [AVCaptureVideoDataOutput new];
    video.videoSettings = @{
      (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
      (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    video.alwaysDiscardsLateVideoFrames = YES;
    [video setSampleBufferDelegate:m.sink queue:m.frames];
    if ([m.session canAddOutput:video]) {
      [m.session addOutput:video];
    }
    m.movie = [AVCaptureMovieFileOutput new];
    if ([m.session canAddOutput:m.movie]) {
      [m.session addOutput:m.movie];
    } else {
      m.movie = nil;
    }
    [m.session commitConfiguration];
    // startRunning blocks while the camera wakes: not on the caller's
    // thread (the app's main one).
    m.control = dispatch_queue_create("valtz.camera.control",
                                      DISPATCH_QUEUE_SERIAL);
    AVCaptureSession* s = m.session;
    dispatch_async(m.control, ^{ [s startRunning]; });
  }
  VALTZ_LOG_INFO("capture", "camera {} on", m.name);
  return c;
}

const std::string&
CameraCapture::name() const
{
  return _impl->name;
}

CameraCapture::Frame
CameraCapture::newest() const
{
  Frame f;
  CVPixelBufferRef pb = [_impl->sink copyNewest:&f.count];
  if (!pb) {
    return f;
  }
  f.width = static_cast<int>(CVPixelBufferGetWidth(pb));
  f.height = static_cast<int>(CVPixelBufferGetHeight(pb));
  if (IOSurfaceRef s = CVPixelBufferGetIOSurface(pb)) {
    CFRetain(s);
    f.surface = s;
  }
  CVPixelBufferRelease(pb);
  return f;
}

Status
CameraCapture::snap(const fs::path& out)
{
  CVPixelBufferRef pb = [_impl->sink copyNewest:nullptr];
  if (!pb) {
    return make_error(Code::NotFound,
                      "the camera has not shown a picture yet");
  }
  Status st = ok_status();
  @autoreleasepool {
    CIImage* img = [CIImage imageWithCVPixelBuffer:pb];
    CIContext* ctx = [CIContext contextWithOptions:nil];
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef cg = [ctx createCGImage:img fromRect:img.extent
                                format:kCIFormatRGBA8 colorSpace:srgb];
    CGColorSpaceRelease(srgb);
    std::string ext = out.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    UTType* type = ext == ".png"    ? UTTypePNG
                   : ext == ".heic" ? UTTypeHEIC
                                    : UTTypeJPEG;
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    NSURL* url = [NSURL fileURLWithPath:
        [NSString stringWithUTF8String:out.c_str()]];
    CGImageDestinationRef dst = cg ? CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)url, (__bridge CFStringRef)type.identifier, 1,
        nullptr) : nullptr;
    if (!dst) {
      st = make_error(Code::Io, std::format("{} could not be written",
                                            out.string()));
    } else {
      NSDictionary* props = @{
        (id)kCGImageDestinationLossyCompressionQuality: @0.92,
      };
      CGImageDestinationAddImage(dst, cg, (__bridge CFDictionaryRef)props);
      if (!CGImageDestinationFinalize(dst)) {
        st = make_error(Code::Io, std::format("{} could not be written",
                                              out.string()));
      }
      CFRelease(dst);
    }
    if (cg) {
      CGImageRelease(cg);
    }
  }
  CVPixelBufferRelease(pb);
  return st;
}

Status
CameraCapture::record(const fs::path& out)
{
  Impl& m = *_impl;
  if (!m.movie) {
    return make_error(Code::Unsupported, std::format(
        "{} cannot record a clip here", m.name));
  }
  if (m.movie.isRecording) {
    return make_error(Code::Busy, "a clip is being recorded already");
  }
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  fs::remove(out, ec);
  m.recording = out;
  m.sink.recordError = nil;
  m.sink.finished = dispatch_semaphore_create(0);
  NSURL* url = [NSURL fileURLWithPath:
      [NSString stringWithUTF8String:out.c_str()]];
  [m.movie startRecordingToOutputFileURL:url recordingDelegate:m.sink];
  return ok_status();
}

const fs::path&
CameraCapture::file() const
{
  return _impl->recording;
}

bool
CameraCapture::recording() const
{
  return _impl->movie && _impl->movie.isRecording;
}

double
CameraCapture::seconds() const
{
  if (!recording()) {
    return 0;
  }
  const CMTime t = _impl->movie.recordedDuration;
  return CMTIME_IS_NUMERIC(t) ? CMTimeGetSeconds(t) : 0;
}

Status
CameraCapture::stop_recording()
{
  Impl& m = *_impl;
  if (!m.movie || !m.sink.finished) {
    return make_error(Code::InvalidArgument, "no clip is being recorded");
  }
  if (m.movie.isRecording) {
    [m.movie stopRecording];
  }
  const long late = dispatch_semaphore_wait(
      m.sink.finished, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC));
  m.sink.finished = nil;
  if (late) {
    return make_error(Code::Io, "the clip did not finish writing");
  }
  if (NSError* e = m.sink.recordError) {
    return make_error(Code::Io, std::format(
        "the clip could not be recorded: {}",
        str(e.localizedDescription)));
  }
  std::error_code ec;
  if (!fs::exists(m.recording, ec) || fs::file_size(m.recording, ec) == 0) {
    return make_error(Code::Io, "the clip came out empty");
  }
  return ok_status();
}

}
