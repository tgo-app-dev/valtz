#include "valtz/media/capture.h"

#include "valtz/base/log.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>

namespace fs = std::filesystem;

namespace valtz::media {

namespace {

constexpr const char* kSystem = "system";
constexpr const char* kMicPrefix = "mic:";

double
now_s()
{
  return std::chrono::duration<double>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The WAV written as the sound comes: made at the first buffer's rate and
// channels (24-bit PCM), each buffer converted from its own format.
struct Writer {
  fs::path                  path;
  std::mutex                mu;
  ExtAudioFileRef           file = nullptr;
  double                    rate = 0;
  std::int64_t              frames = 0;
  std::atomic<double>       seconds{0};
  // The peak of the last tenth of a second, decayed to it.
  std::atomic<double>       level{0};
  std::atomic<double>       peak_at{0};
  std::string               error;

  void
  take(CMSampleBufferRef sb)
  {
    if (!sb || !CMSampleBufferIsValid(sb) ||
        CMSampleBufferGetNumSamples(sb) <= 0) {
      return;
    }
    CMAudioFormatDescriptionRef fd =
        CMSampleBufferGetFormatDescription(sb);
    const AudioStreamBasicDescription* in =
        fd ? CMAudioFormatDescriptionGetStreamBasicDescription(fd) : nullptr;
    if (!in || in->mFormatID != kAudioFormatLinearPCM) {
      return;
    }
    std::lock_guard lk(mu);
    if (!error.empty()) {
      return;
    }
    if (!file) {
      AudioStreamBasicDescription out{};
      out.mSampleRate = in->mSampleRate;
      out.mFormatID = kAudioFormatLinearPCM;
      out.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger |
                         kLinearPCMFormatFlagIsPacked;
      out.mChannelsPerFrame = std::max<UInt32>(1, in->mChannelsPerFrame);
      out.mBitsPerChannel = 24;
      out.mBytesPerFrame = 3 * out.mChannelsPerFrame;
      out.mFramesPerPacket = 1;
      out.mBytesPerPacket = out.mBytesPerFrame;
      NSURL* url = [NSURL fileURLWithPath:
          [NSString stringWithUTF8String:path.c_str()]];
      OSStatus st = ExtAudioFileCreateWithURL(
          (__bridge CFURLRef)url, kAudioFileWAVEType, &out, nullptr,
          kAudioFileFlags_EraseFile, &file);
      if (st == noErr) {
        st = ExtAudioFileSetProperty(file,
                                     kExtAudioFileProperty_ClientDataFormat,
                                     sizeof(*in), in);
      }
      if (st != noErr) {
        error = std::format("cannot write {} ({})", path.string(),
                            static_cast<int>(st));
        if (file) {
          ExtAudioFileDispose(file);
          file = nullptr;
        }
        return;
      }
      rate = in->mSampleRate;
    }
    // The buffer's samples, as the list ExtAudioFile writes.
    size_t need = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sb, &need, nullptr, 0, nullptr, nullptr, 0, nullptr);
    std::vector<std::byte> mem(std::max<size_t>(need, sizeof(AudioBufferList)));
    auto* abl = reinterpret_cast<AudioBufferList*>(mem.data());
    CMBlockBufferRef block = nullptr;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sb, nullptr, abl, mem.size(), nullptr, nullptr,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &block) != noErr) {
      return;
    }
    const auto n = static_cast<UInt32>(CMSampleBufferGetNumSamples(sb));
    ExtAudioFileWrite(file, n, abl);
    // The meter: float samples' peak (integer ones scaled).
    double peak = 0;
    const bool flt = in->mFormatFlags & kAudioFormatFlagIsFloat;
    for (UInt32 b = 0; b < abl->mNumberBuffers; ++b) {
      const AudioBuffer& buf = abl->mBuffers[b];
      if (flt && in->mBitsPerChannel == 32) {
        const auto* s = static_cast<const float*>(buf.mData);
        for (size_t i = 0; i < buf.mDataByteSize / 4; ++i) {
          peak = std::max(peak, static_cast<double>(std::fabs(s[i])));
        }
      } else if (!flt && in->mBitsPerChannel == 16) {
        const auto* s = static_cast<const std::int16_t*>(buf.mData);
        for (size_t i = 0; i < buf.mDataByteSize / 2; ++i) {
          peak = std::max(peak, std::fabs(s[i] / 32768.0));
        }
      }
    }
    if (block) {
      CFRelease(block);
    }
    frames += n;
    seconds = rate > 0 ? static_cast<double>(frames) / rate : 0.0;
    const double t = now_s();
    if (peak >= level || t - peak_at > 0.1) {
      level = std::min(1.0, peak);
      peak_at = t;
    }
  }

  Status
  close()
  {
    std::lock_guard lk(mu);
    if (file) {
      ExtAudioFileDispose(file);
      file = nullptr;
    }
    if (!error.empty()) {
      return make_error(Code::Io, error);
    }
    if (frames == 0) {
      return make_error(Code::NotFound, "nothing was heard: no sound came "
                                        "from the source");
    }
    return ok_status();
  }
};

// Blocks on an asynchronous completion, up to `seconds`.
bool
wait_for(dispatch_semaphore_t s, double seconds)
{
  return dispatch_semaphore_wait(
             s, dispatch_time(DISPATCH_TIME_NOW,
                              static_cast<int64_t>(seconds * 1e9))) == 0;
}

}  // namespace

}  // namespace valtz::media

// What both sources hand their sound to: the writer.
@interface ValtzCaptureSink
    : NSObject <AVCaptureAudioDataOutputSampleBufferDelegate,
                SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) valtz::media::Writer* writer;
@end

@implementation ValtzCaptureSink
- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
  (void)output;
  (void)connection;
  if (_writer) {
    _writer->take(sampleBuffer);
  }
}

- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type
{
  (void)stream;
  // The picture a stream must carry is let go: only its sound is kept.
  if (type == SCStreamOutputTypeAudio && _writer) {
    _writer->take(sampleBuffer);
  }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error
{
  (void)stream;
  VALTZ_LOG_WARN("capture", "the system's audio stopped: {}",
                 error.localizedDescription.UTF8String ?: "");
}
@end

namespace valtz::media {

std::vector<CaptureSource>
capture_sources()
{
  std::vector<CaptureSource> out;
  @autoreleasepool {
    AVCaptureDevice* def =
        [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio];
    AVCaptureDeviceDiscoverySession* ds = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeMicrophone,
                                           AVCaptureDeviceTypeExternal ]
                              mediaType:AVMediaTypeAudio
                               position:AVCaptureDevicePositionUnspecified];
    for (AVCaptureDevice* d in ds.devices) {
      CaptureSource s;
      s.id = std::string(kMicPrefix) + d.uniqueID.UTF8String;
      s.name = d.localizedName.UTF8String ?: "";
      s.kind = "microphone";
      s.preferred = def && [def.uniqueID isEqualToString:d.uniqueID];
      out.push_back(std::move(s));
    }
  }
  std::stable_partition(out.begin(), out.end(),
                        [](const CaptureSource& s) { return s.preferred; });
  out.push_back({kSystem, "System Audio", "system", false});
  return out;
}

std::string
microphone_permission()
{
  switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
  case AVAuthorizationStatusAuthorized: return "granted";
  case AVAuthorizationStatusNotDetermined: return "undetermined";
  default: return "denied";
  }
}

struct SoundCapture::Impl {
  std::string                source;
  Writer                     writer;
  ValtzCaptureSink*          sink = nil;
  AVCaptureSession*          session = nil;
  SCStream*                  stream = nil;
  dispatch_queue_t           queue = nullptr;
  bool                       stopped = false;
};

SoundCapture::SoundCapture() : _impl(std::make_unique<Impl>()) {}

SoundCapture::~SoundCapture()
{
  if (_impl && !_impl->stopped) {
    (void)stop();
  }
}

double
SoundCapture::seconds() const
{
  return _impl->writer.seconds;
}

double
SoundCapture::level() const
{
  // Fallen silent: the meter falls with it.
  Writer& w = _impl->writer;
  return now_s() - w.peak_at > 0.3 ? 0.0 : w.level.load();
}

const std::string&
SoundCapture::source() const
{
  return _impl->source;
}

const fs::path&
SoundCapture::file() const
{
  return _impl->writer.path;
}

Result<std::unique_ptr<SoundCapture>>
SoundCapture::start(const std::string& source, const fs::path& out)
{
  std::unique_ptr<SoundCapture> c(new SoundCapture());
  Impl& m = *c->_impl;
  m.source = source;
  m.writer.path = out;
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  fs::remove(out, ec);
  @autoreleasepool {
    m.queue = dispatch_queue_create("valtz.capture", DISPATCH_QUEUE_SERIAL);
    m.sink = [ValtzCaptureSink new];
    m.sink.writer = &m.writer;
    if (source.rfind(kMicPrefix, 0) == 0) {
      if (microphone_permission() != "granted") {
        return make_error(Code::Unsupported,
                          "Valtz may not use the microphone: allow it in "
                          "System Settings › Privacy & Security › "
                          "Microphone");
      }
      NSString* uid = [NSString stringWithUTF8String:
          source.substr(std::strlen(kMicPrefix)).c_str()];
      AVCaptureDevice* dev = [AVCaptureDevice deviceWithUniqueID:uid];
      if (!dev) {
        return make_error(Code::NotFound, std::format(
            "no microphone {} is there now", source));
      }
      NSError* err = nil;
      AVCaptureDeviceInput* in =
          [AVCaptureDeviceInput deviceInputWithDevice:dev error:&err];
      if (!in) {
        return make_error(Code::Io, std::format(
            "the microphone would not open: {}",
            err.localizedDescription.UTF8String ?: ""));
      }
      m.session = [AVCaptureSession new];
      AVCaptureAudioDataOutput* o = [AVCaptureAudioDataOutput new];
      [o setSampleBufferDelegate:m.sink queue:m.queue];
      if (![m.session canAddInput:in] || ![m.session canAddOutput:o]) {
        return make_error(Code::Io, "the microphone cannot be recorded");
      }
      [m.session addInput:in];
      [m.session addOutput:o];
      [m.session startRunning];
      if (!m.session.running) {
        return make_error(Code::Io, "the microphone did not start");
      }
      VALTZ_LOG_INFO("capture", "recording {} into {}",
                     dev.localizedName.UTF8String ?: "", out.string());
      return c;
    }
    if (source != kSystem) {
      return make_error(Code::NotFound, std::format(
          "'{}' is not a capture source", source));
    }
    // The system's audio: every app's but Valtz's own, through a stream
    // of the main display (its picture let go).
    __block SCShareableContent* content = nil;
    __block NSError* failed = nil;
    dispatch_semaphore_t got = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:YES
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent* sc,
                                                     NSError* e) {
      content = sc;
      failed = e;
      dispatch_semaphore_signal(got);
    }];
    if (!wait_for(got, 10) || !content || content.displays.count == 0) {
      return make_error(Code::Unsupported, std::format(
          "the system's audio is not open to Valtz: allow it in System "
          "Settings › Privacy & Security › Screen & System Audio "
          "Recording{}",
          failed ? std::format(" ({})",
                               failed.localizedDescription.UTF8String ?: "")
                 : std::string()));
    }
    NSMutableArray<SCRunningApplication*>* mine = [NSMutableArray array];
    for (SCRunningApplication* a in content.applications) {
      if (a.processID == getpid()) {
        [mine addObject:a];
      }
    }
    SCContentFilter* filter =
        [[SCContentFilter alloc] initWithDisplay:content.displays.firstObject
                           excludingApplications:mine
                                exceptingWindows:@[]];
    SCStreamConfiguration* cfg = [SCStreamConfiguration new];
    cfg.capturesAudio = YES;
    cfg.excludesCurrentProcessAudio = YES;
    cfg.sampleRate = 48000;
    cfg.channelCount = 2;
    cfg.width = 2;
    cfg.height = 2;
    cfg.minimumFrameInterval = CMTimeMake(1, 1);
    m.stream = [[SCStream alloc] initWithFilter:filter
                                  configuration:cfg
                                       delegate:m.sink];
    NSError* err = nil;
    if (![m.stream addStreamOutput:m.sink
                              type:SCStreamOutputTypeAudio
                sampleHandlerQueue:m.queue
                             error:&err] ||
        ![m.stream addStreamOutput:m.sink
                              type:SCStreamOutputTypeScreen
                sampleHandlerQueue:m.queue
                             error:&err]) {
      return make_error(Code::Io, std::format(
          "the system's audio cannot be recorded: {}",
          err.localizedDescription.UTF8String ?: ""));
    }
    __block NSError* start_err = nil;
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    [m.stream startCaptureWithCompletionHandler:^(NSError* e) {
      start_err = e;
      dispatch_semaphore_signal(started);
    }];
    if (!wait_for(started, 10) || start_err) {
      m.stream = nil;
      return make_error(Code::Unsupported, std::format(
          "the system's audio did not start: {}",
          start_err ? start_err.localizedDescription.UTF8String ?: ""
                    : "no answer"));
    }
    VALTZ_LOG_INFO("capture", "recording the system's audio into {}",
                   out.string());
  }
  return c;
}

Status
SoundCapture::stop()
{
  Impl& m = *_impl;
  if (m.stopped) {
    return make_error(Code::InvalidArgument, "the capture has stopped");
  }
  m.stopped = true;
  @autoreleasepool {
    if (m.session) {
      [m.session stopRunning];
      m.session = nil;
    }
    if (m.stream) {
      dispatch_semaphore_t done = dispatch_semaphore_create(0);
      [m.stream stopCaptureWithCompletionHandler:^(NSError*) {
        dispatch_semaphore_signal(done);
      }];
      wait_for(done, 5);
      m.stream = nil;
    }
    // The last buffers in flight, written before the file closes.
    if (m.queue) {
      dispatch_sync(m.queue, ^{});
    }
    m.sink.writer = nullptr;
  }
  return m.writer.close();
}

}
