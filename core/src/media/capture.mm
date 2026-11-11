#include "valtz/media/capture.h"

#include "valtz/base/log.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>
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

// A PCM format as a person reads it: "48000 Hz, 1 ch, 32-bit float".
std::string
described(const AudioStreamBasicDescription& f)
{
  const bool flt = f.mFormatFlags & kAudioFormatFlagIsFloat;
  return std::format("{:.0f} Hz, {} ch, {}-bit {}{}{}", f.mSampleRate,
                     f.mChannelsPerFrame, f.mBitsPerChannel,
                     flt ? "float" : "integer",
                     f.mFormatFlags & kAudioFormatFlagIsNonInterleaved
                         ? ", non-interleaved" : "",
                     f.mBytesPerFrame * 8 / std::max<UInt32>(
                         1, f.mFormatFlags &
                                    kAudioFormatFlagIsNonInterleaved
                                ? 1 : f.mChannelsPerFrame) !=
                             f.mBitsPerChannel
                         ? std::format(" in {} bytes", f.mBytesPerFrame)
                         : "");
}

// Two formats the same, as a converter reads them.
bool
same_format(const AudioStreamBasicDescription& a,
            const AudioStreamBasicDescription& b)
{
  return a.mSampleRate == b.mSampleRate && a.mFormatID == b.mFormatID &&
         a.mFormatFlags == b.mFormatFlags &&
         a.mBytesPerPacket == b.mBytesPerPacket &&
         a.mFramesPerPacket == b.mFramesPerPacket &&
         a.mBytesPerFrame == b.mBytesPerFrame &&
         a.mChannelsPerFrame == b.mChannelsPerFrame &&
         a.mBitsPerChannel == b.mBitsPerChannel;
}

// A buffer's samples as float, interleaved (`channels` a frame), from ITS
// OWN PCM format: float (32 or 64 bits) or integer (signed or not, 8 to
// 32 bits in slots of 1 to 4 bytes -- packed, or padded high or low),
// interleaved or a buffer a channel, either byte order. False for one
// it cannot read.
bool
to_float(const AudioBufferList* abl, const AudioStreamBasicDescription& f,
         UInt32 frames, UInt32 channels, std::vector<float>& out)
{
  const UInt32 flags = f.mFormatFlags;
  const bool flt = flags & kAudioFormatFlagIsFloat;
  const bool split = flags & kAudioFormatFlagIsNonInterleaved;
  const bool big = flags & kAudioFormatFlagIsBigEndian;
  const bool high = flags & kAudioFormatFlagIsAlignedHigh;
  const bool sign = flags & kAudioFormatFlagIsSignedInteger;
  const UInt32 bits = f.mBitsPerChannel;
  // A sample's slot: a frame's bytes, shared by its channels unless each
  // has a buffer of its own.
  const UInt32 slot = split ? f.mBytesPerFrame
                            : f.mBytesPerFrame / std::max<UInt32>(1,
                                                                  channels);
  if (slot == 0 || slot > 8 || bits == 0 || bits > slot * 8 ||
      (flt && bits != 32 && bits != 64) ||
      (split && abl->mNumberBuffers < channels)) {
    return false;
  }
  out.assign(static_cast<size_t>(frames) * channels, 0.0f);
  const UInt32 width = slot * 8;
  for (UInt32 c = 0; c < channels; ++c) {
    const AudioBuffer& b = abl->mBuffers[split ? c : 0];
    const auto* base = static_cast<const std::uint8_t*>(b.mData);
    const size_t step = split ? slot : f.mBytesPerFrame;
    const size_t off = split ? 0 : static_cast<size_t>(c) * slot;
    for (UInt32 i = 0; i < frames; ++i) {
      const size_t at = static_cast<size_t>(i) * step + off;
      if (!base || at + slot > b.mDataByteSize) {
        break;
      }
      std::uint64_t u = 0;
      for (UInt32 k = 0; k < slot; ++k) {
        const std::uint64_t byte = base[at + (big ? slot - 1 - k : k)];
        u |= byte << (8 * k);
      }
      double x = 0;
      if (flt) {
        if (bits == 32) {
          const auto w = static_cast<std::uint32_t>(u);
          float v;
          std::memcpy(&v, &w, 4);
          x = v;
        } else {
          double v;
          std::memcpy(&v, &u, 8);
          x = v;
        }
      } else {
        // The sample's own bits, from where they lie in the slot.
        std::uint64_t v = high ? u >> (width - bits) : u;
        v &= bits == 64 ? ~0ull : ((1ull << bits) - 1);
        const double full = std::ldexp(1.0, static_cast<int>(bits) - 1);
        if (sign) {
          const auto s = static_cast<std::int64_t>(v << (64 - bits)) >>
                         (64 - bits);
          x = static_cast<double>(s) / full;
        } else {
          x = (static_cast<double>(v) - full) / full;
        }
      }
      out[static_cast<size_t>(i) * channels + c] = static_cast<float>(x);
    }
  }
  return true;
}

// The WAV written as the sound comes: made at the first buffer's rate and
// channels (24-bit PCM). Each buffer is read from ITS OWN format into
// float (to_float) and written so: the file's converter is told one
// format, which it takes once -- an external microphone handed its first
// buffer in 16-bit and the rest in float, and the file read them all as
// its first (each float's halves two loud samples, 2026-11-10).
struct Writer {
  fs::path                  path;
  std::mutex                mu;
  ExtAudioFileRef           file = nullptr;
  double                    rate = 0;
  UInt32                    channels = 0;
  std::int64_t              frames = 0;
  std::atomic<double>       seconds{0};
  // The peak of the last tenth of a second, decayed to it.
  std::atomic<double>       level{0};
  std::atomic<double>       peak_at{0};
  // The first sample's host time (SoundCapture::started_at), and the
  // clock its source stamps buffers by (null: the host's own -- a
  // ScreenCaptureKit stream's).
  std::atomic<double>       first_host{std::nan("")};
  CMClockRef                clock = nullptr;
  std::string               error;
  // The format the source last handed its sound in (said in the log as
  // it changes), and a buffer of another rate or count of channels than
  // the file's -- left out -- said once.
  AudioStreamBasicDescription last{};
  bool                      told_odd = false;
  std::vector<float>        samples;

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
      // What is written: float, interleaved, the file's rate and
      // channels -- whatever each buffer came as.
      AudioStreamBasicDescription f32 = out;
      f32.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
      f32.mBitsPerChannel = 32;
      f32.mBytesPerFrame = 4 * out.mChannelsPerFrame;
      f32.mBytesPerPacket = f32.mBytesPerFrame;
      NSURL* url = [NSURL fileURLWithPath:
          [NSString stringWithUTF8String:path.c_str()]];
      OSStatus st = ExtAudioFileCreateWithURL(
          (__bridge CFURLRef)url, kAudioFileWAVEType, &out, nullptr,
          kAudioFileFlags_EraseFile, &file);
      if (st == noErr) {
        st = ExtAudioFileSetProperty(file,
                                     kExtAudioFileProperty_ClientDataFormat,
                                     sizeof(f32), &f32);
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
      channels = out.mChannelsPerFrame;
      VALTZ_LOG_INFO("capture", "its sound comes as {}", described(*in));
      last = *in;
    } else if (!same_format(*in, last)) {
      // Said where the system's log keeps it: what a report is read by.
      VALTZ_LOG_WARN("capture", "its sound now comes as {} (was {})",
                     described(*in), described(last));
      last = *in;
    }
    if (in->mSampleRate != rate ||
        std::max<UInt32>(1, in->mChannelsPerFrame) != channels) {
      if (!told_odd) {
        told_odd = true;
        VALTZ_LOG_WARN("capture", "its sound came as {}: the file is {:.0f} "
                       "Hz, {} ch, so those buffers are left out",
                       described(*in), rate, channels);
      }
      return;
    }
    // The buffer's samples, as its format lays them.
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
    const bool read = to_float(abl, *in, n, channels, samples);
    if (block) {
      CFRelease(block);
    }
    if (!read) {
      if (!told_odd) {
        told_odd = true;
        VALTZ_LOG_WARN("capture", "its sound came as {}, which Valtz "
                       "cannot read: those buffers are left out",
                       described(*in));
      }
      return;
    }
    if (frames == 0) {
      const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
      if (CMTIME_IS_NUMERIC(pts)) {
        const CMTime host =
            clock ? CMSyncConvertTime(pts, clock, CMClockGetHostTimeClock())
                  : pts;
        first_host = CMTimeGetSeconds(host);
      }
    }
    AudioBufferList out;
    out.mNumberBuffers = 1;
    out.mBuffers[0].mNumberChannels = channels;
    out.mBuffers[0].mDataByteSize =
        static_cast<UInt32>(samples.size() * sizeof(float));
    out.mBuffers[0].mData = samples.data();
    ExtAudioFileWrite(file, n, &out);
    // The meter: the buffer's peak.
    double peak = 0;
    for (const float x : samples) {
      peak = std::max(peak, static_cast<double>(std::fabs(x)));
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

namespace {

// The CoreAudio device a microphone's source names (AVFoundation's
// unique id is its UID); unknown for another.
AudioObjectID
device_of(const std::string& source)
{
  if (source.rfind(kMicPrefix, 0) != 0) {
    return kAudioObjectUnknown;
  }
  CFStringRef uid = CFStringCreateWithCString(
      nullptr, source.c_str() + std::strlen(kMicPrefix),
      kCFStringEncodingUTF8);
  if (!uid) {
    return kAudioObjectUnknown;
  }
  AudioObjectPropertyAddress a{kAudioHardwarePropertyTranslateUIDToDevice,
                               kAudioObjectPropertyScopeGlobal,
                               kAudioObjectPropertyElementMain};
  AudioObjectID dev = kAudioObjectUnknown;
  UInt32 size = sizeof(dev);
  const OSStatus st = AudioObjectGetPropertyData(
      kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &dev);
  CFRelease(uid);
  return st == noErr ? dev : kAudioObjectUnknown;
}

// Where its INPUT volume is set: the device's main element, else each
// of its input channels' -- those that have one to set.
std::vector<UInt32>
volume_elements(AudioObjectID dev)
{
  std::vector<UInt32> out;
  if (dev == kAudioObjectUnknown) {
    return out;
  }
  auto settable = [&](UInt32 element) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyVolumeScalar,
                                 kAudioDevicePropertyScopeInput, element};
    Boolean can = false;
    return AudioObjectHasProperty(dev, &a) &&
           AudioObjectIsPropertySettable(dev, &a, &can) == noErr && can;
  };
  if (settable(kAudioObjectPropertyElementMain)) {
    return {kAudioObjectPropertyElementMain};
  }
  AudioObjectPropertyAddress cfg{kAudioDevicePropertyStreamConfiguration,
                                 kAudioDevicePropertyScopeInput,
                                 kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(dev, &cfg, 0, nullptr, &size) !=
          noErr || size == 0) {
    return out;
  }
  std::vector<std::byte> mem(size);
  auto* abl = reinterpret_cast<AudioBufferList*>(mem.data());
  if (AudioObjectGetPropertyData(dev, &cfg, 0, nullptr, &size, abl) !=
      noErr) {
    return out;
  }
  UInt32 n = 0;
  for (UInt32 b = 0; b < abl->mNumberBuffers; ++b) {
    n += abl->mBuffers[b].mNumberChannels;
  }
  for (UInt32 ch = 1; ch <= n; ++ch) {
    if (settable(ch)) {
      out.push_back(ch);
    }
  }
  return out;
}

std::optional<double>
input_volume_of(AudioObjectID dev)
{
  const auto els = volume_elements(dev);
  if (els.empty()) {
    return std::nullopt;
  }
  double sum = 0;
  for (const UInt32 e : els) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyVolumeScalar,
                                 kAudioDevicePropertyScopeInput, e};
    Float32 v = 0;
    UInt32 size = sizeof(v);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &v) !=
        noErr) {
      return std::nullopt;
    }
    sum += v;
  }
  return sum / static_cast<double>(els.size());
}

}  // namespace

Status
set_input_volume(const std::string& source, double volume)
{
  if (source.rfind(kMicPrefix, 0) != 0) {
    return make_error(Code::InvalidArgument, std::format(
        "{} has no input volume: only a microphone has one", source));
  }
  const AudioObjectID dev = device_of(source);
  const auto els = volume_elements(dev);
  if (els.empty()) {
    return make_error(Code::Unsupported, std::format(
        "{} has no input volume to set: its own knob sets its level",
        source));
  }
  const Float32 v = static_cast<Float32>(std::clamp(volume, 0.0, 1.0));
  for (const UInt32 e : els) {
    AudioObjectPropertyAddress a{kAudioDevicePropertyVolumeScalar,
                                 kAudioDevicePropertyScopeInput, e};
    const OSStatus st =
        AudioObjectSetPropertyData(dev, &a, 0, nullptr, sizeof(v), &v);
    if (st != noErr) {
      return make_error(Code::Io, std::format(
          "the input volume of {} was not set ({})", source,
          static_cast<int>(st)));
    }
  }
  return ok_status();
}

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
      s.input_volume = input_volume_of(device_of(s.id));
      out.push_back(std::move(s));
    }
  }
  std::stable_partition(out.begin(), out.end(),
                        [](const CaptureSource& s) { return s.preferred; });
  out.push_back({kSystem, "System Audio", "system", false, std::nullopt});
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
SoundCapture::started_at() const
{
  return _impl->writer.first_host;
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
      // One format throughout, AVFoundation's to convert to: 32-bit float
      // at the device's own rate and channels. Left to the device, an
      // external microphone gave 16-bit, then float.
      const AudioStreamBasicDescription* native =
          dev.activeFormat
              ? CMAudioFormatDescriptionGetStreamBasicDescription(
                    dev.activeFormat.formatDescription)
              : nullptr;
      NSMutableDictionary* want = [@{
        AVFormatIDKey: @(kAudioFormatLinearPCM),
        AVLinearPCMBitDepthKey: @32,
        AVLinearPCMIsFloatKey: @YES,
        AVLinearPCMIsNonInterleaved: @NO,
        AVLinearPCMIsBigEndianKey: @NO,
      } mutableCopy];
      if (native && native->mSampleRate > 0) {
        want[AVSampleRateKey] = @(native->mSampleRate);
      }
      if (native && native->mChannelsPerFrame > 0) {
        want[AVNumberOfChannelsKey] = @(native->mChannelsPerFrame);
      }
      @try {
        o.audioSettings = want;
      } @catch (NSException* e) {
        VALTZ_LOG_WARN("capture", "the microphone's own format kept: {}",
                       e.reason.UTF8String ?: "");
      }
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
      {
        // Its buffers are stamped by the session's clock.
        std::lock_guard lk(m.writer.mu);
        m.writer.clock = m.session.synchronizationClock;
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
write_capture_pieces(const std::vector<PcmPiece>& pieces,
                     const fs::path& out)
{
  Writer w;
  w.path = out;
  double at = 0;
  for (const auto& p : pieces) {
    const int ch = std::max(1, p.channels);
    const UInt32 bytes = static_cast<UInt32>(p.bits / 8);
    AudioStreamBasicDescription f{};
    f.mSampleRate = p.rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = (p.is_float ? kAudioFormatFlagIsFloat
                                 : kAudioFormatFlagIsSignedInteger) |
                     kAudioFormatFlagIsPacked;
    f.mBitsPerChannel = static_cast<UInt32>(p.bits);
    f.mChannelsPerFrame = static_cast<UInt32>(ch);
    f.mBytesPerFrame = bytes * f.mChannelsPerFrame;
    f.mFramesPerPacket = 1;
    f.mBytesPerPacket = f.mBytesPerFrame;
    std::vector<std::byte> data(p.samples.size() * bytes);
    for (size_t i = 0; i < p.samples.size(); ++i) {
      const double x = std::clamp(p.samples[i], -1.0, 1.0);
      std::byte* d = data.data() + i * bytes;
      if (p.is_float && p.bits == 32) {
        const float v = static_cast<float>(x);
        std::memcpy(d, &v, 4);
      } else {
        const double full = std::ldexp(1.0, p.bits - 1);
        const auto v = static_cast<std::int32_t>(
            std::lround(std::clamp(x * full, -full, full - 1)));
        std::memcpy(d, &v, bytes);  // little-endian: its low bytes
      }
    }
    const auto frames = static_cast<CMItemCount>(p.samples.size() /
                                                 static_cast<size_t>(ch));
    CMAudioFormatDescriptionRef fd = nullptr;
    CMBlockBufferRef bb = nullptr;
    CMSampleBufferRef sb = nullptr;
    if (CMAudioFormatDescriptionCreate(kCFAllocatorDefault, &f, 0, nullptr,
                                       0, nullptr, nullptr, &fd) != noErr ||
        CMBlockBufferCreateWithMemoryBlock(
            kCFAllocatorDefault, nullptr, data.size(), kCFAllocatorDefault,
            nullptr, 0, data.size(), kCMBlockBufferAssureMemoryNowFlag,
            &bb) != noErr ||
        CMBlockBufferReplaceDataBytes(data.data(), bb, 0, data.size()) !=
            noErr ||
        CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            kCFAllocatorDefault, bb, fd, frames,
            CMTimeMakeWithSeconds(at, static_cast<int32_t>(p.rate)),
            nullptr, &sb) != noErr) {
      if (bb) CFRelease(bb);
      if (fd) CFRelease(fd);
      return make_error(Code::Io, "a piece could not be made");
    }
    w.take(sb);
    CFRelease(sb);
    CFRelease(bb);
    CFRelease(fd);
    at += static_cast<double>(frames) / p.rate;
  }
  return w.close();
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
