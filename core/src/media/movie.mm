#include "valtz/media/movie.h"

#include "valtz/media/stamp.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <vector>

namespace valtz::media {

namespace fs = std::filesystem;

namespace {

NSURL*
file_url(const fs::path& p)
{
  return [NSURL fileURLWithPath:[NSString stringWithUTF8String:p.c_str()]];
}

AVAssetTrack*
first_track(AVAsset* asset, AVMediaType type)
{
  __block AVAssetTrack* out = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [asset loadTracksWithMediaType:type
               completionHandler:^(NSArray<AVAssetTrack*>* t, NSError*) {
                 out = t.firstObject;
                 dispatch_semaphore_signal(sem);
               }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  return out;
}

void
load_keys(id<AVAsynchronousKeyValueLoading> obj, NSArray<NSString*>* keys)
{
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [obj loadValuesAsynchronouslyForKeys:keys
                     completionHandler:^{
                       dispatch_semaphore_signal(sem);
                     }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
}

// `sb` moved `shift` earlier: a span cut from the middle starts at 0.
CMSampleBufferRef
retimed(CMSampleBufferRef sb, CMTime shift)
{
  CMItemCount n = 0;
  CMSampleBufferGetSampleTimingInfoArray(sb, 0, nullptr, &n);
  std::vector<CMSampleTimingInfo> t(static_cast<std::size_t>(n));
  CMSampleBufferGetSampleTimingInfoArray(sb, n, t.data(), &n);
  for (auto& x : t) {
    if (CMTIME_IS_VALID(x.presentationTimeStamp)) {
      x.presentationTimeStamp = CMTimeSubtract(x.presentationTimeStamp,
                                               shift);
    }
    if (CMTIME_IS_VALID(x.decodeTimeStamp)) {
      x.decodeTimeStamp = CMTimeSubtract(x.decodeTimeStamp, shift);
    }
  }
  CMSampleBufferRef out = nullptr;
  CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault, sb, n,
                                        t.data(), &out);
  return out;
}

// Copy `from` into `to` on `queue` until `from` runs dry (or the writer
// refuses), then mark `to` finished; `group` is left when it is. Each
// buffer is moved `shift` earlier (zero: as it is).
void
pump(AVAssetReaderOutput* from, AVAssetWriterInput* to,
     dispatch_queue_t queue, dispatch_group_t group,
     CMTime shift = kCMTimeZero)
{
  dispatch_group_enter(group);
  __block bool done = false;
  const bool move = CMTimeCompare(shift, kCMTimeZero) != 0;
  [to requestMediaDataWhenReadyOnQueue:queue
                            usingBlock:^{
                              while (!done && to.readyForMoreMediaData) {
                                CMSampleBufferRef sb =
                                    [from copyNextSampleBuffer];
                                if (sb && move) {
                                  CMSampleBufferRef m = retimed(sb, shift);
                                  CFRelease(sb);
                                  sb = m;
                                }
                                const bool ok =
                                    sb && [to appendSampleBuffer:sb];
                                if (sb) {
                                  CFRelease(sb);
                                }
                                if (!ok) {
                                  done = true;
                                  [to markAsFinished];
                                  dispatch_group_leave(group);
                                }
                              }
                            }];
}

std::string
why(NSError* e)
{
  return e ? std::string(e.localizedDescription.UTF8String ?: "")
           : std::string("unknown error");
}

// Who wrote a movie or a sound, as its container keeps it (stamp.h):
// QuickTime's software keys in a .mov, iTunes' encoding tool (ffprobe's
// "encoder") in an .mp4, .m4v or .m4a -- the two an AVAssetWriter writes
// for each -- and, given, what made it: the prompt as the description,
// the seed as the comment (QuickTime's metadata and user data in a .mov,
// iTunes' "desc" and "©cmt" in the rest). `kept` -- the source's own
// items -- go with them but the ones these replace.
NSArray<AVMetadataItem*>*
software_items(AVFileType type, std::string_view software,
               NSArray<AVMetadataItem*>* kept = nil,
               const MadeFrom& made = {})
{
  NSMutableArray<AVMetadataItem*>* out = [NSMutableArray array];
  if (software.empty()) {
    return kept ?: out;
  }
  const bool qt = [type isEqualToString:AVFileTypeQuickTimeMovie];
  auto text = [](std::string_view s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size()
                                  encoding:NSUTF8StringEncoding];
  };
  NSMutableArray<NSArray*>* items = [NSMutableArray array];  // id, value
  auto add = [&](NSArray<AVMetadataIdentifier>* ids, NSString* value) {
    for (AVMetadataIdentifier ident in ids) {
      [items addObject:@[ ident, value ]];
    }
  };
  add(qt ? @[ AVMetadataIdentifierQuickTimeMetadataSoftware,
              AVMetadataIdentifierQuickTimeUserDataSoftware ]
         : @[ AVMetadataIdentifieriTunesMetadataEncodingTool ],
      text(software) ?: @"Valtz");
  if (NSString* p = made.prompt.empty() ? nil : text(made.prompt)) {
    // QuickTime's user data holds a Mac Roman string: words past ASCII
    // are cut there, so only its metadata says them. An .mp4's "desc"
    // beside iTunes' ©des is what ffprobe reads as its description.
    const bool ascii = std::ranges::all_of(made.prompt, [](char c) {
      return static_cast<unsigned char>(c) < 0x80;
    });
    add(qt ? (ascii ? @[ AVMetadataIdentifierQuickTimeMetadataDescription,
                         AVMetadataIdentifierQuickTimeUserDataDescription ]
                    : @[ AVMetadataIdentifierQuickTimeMetadataDescription ])
           : @[ AVMetadataIdentifieriTunesMetadataDescription,
                [AVMetadataItem identifierForKey:@"desc"
                                        keySpace:AVMetadataKeySpaceiTunes] ],
        p);
  }
  if (const std::string c = made.comment(); !c.empty()) {
    add(qt ? @[ AVMetadataIdentifierQuickTimeMetadataComment,
                AVMetadataIdentifierQuickTimeUserDataComment ]
           : @[ AVMetadataIdentifieriTunesMetadataUserComment ],
        text(c));
  }
  NSMutableSet<AVMetadataIdentifier>* written = [NSMutableSet set];
  for (NSArray* i in items) {
    [written addObject:i[0]];
  }
  for (AVMetadataItem* m in kept) {
    if (![written containsObject:m.identifier]) {
      [out addObject:m];
    }
  }
  for (NSArray* i in items) {
    AVMutableMetadataItem* m = [AVMutableMetadataItem metadataItem];
    m.identifier = i[0];
    m.value = i[1];
    m.dataType = (__bridge NSString*)kCMMetadataBaseDataType_UTF8;
    [out addObject:m];
  }
  return out;
}

AVFileType
movie_type(const fs::path& out)
{
  const std::string ext = out.extension().string();
  return ext == ".mov"   ? AVFileTypeQuickTimeMovie
         : ext == ".m4v" ? AVFileTypeAppleM4V
         : ext == ".m4a" ? AVFileTypeAppleM4A
         : ext == ".wav" ? AVFileTypeWAVE
                         : AVFileTypeMPEG4;
}

// AAC as Apple's encoder takes it for `channels` of a sound at `rate` Hz:
// at the sound's own rate where it encodes that one, else the nearest
// above it that it does (48 kHz at most; the writer resamples), and at
// the bit rate nearest `per_channel` a channel it allows there, never
// above it. Asked for more than AAC carries -- 96 kb/s of an 8 kHz mono
// sound, where it holds about 6 bits a sample -- the writer refused the
// whole movie ("Cannot Encode Media"): a camcorder's AVI upscaled.
NSDictionary*
aac_settings(double rate, int channels, double per_channel)
{
  AudioFormatID aac = kAudioFormatMPEG4AAC;
  double to = std::min(rate, 48000.0);
  UInt32 size = 0;
  if (AudioFormatGetPropertyInfo(
          kAudioFormatProperty_AvailableEncodeSampleRates, sizeof(aac),
          &aac, &size) == noErr && size >= sizeof(AudioValueRange)) {
    // (Asked first, the size is the most it could be: the list is cut to
    // what came back, which ends sooner -- zero ranges after.)
    std::vector<AudioValueRange> rates(size / sizeof(AudioValueRange));
    if (AudioFormatGetProperty(
            kAudioFormatProperty_AvailableEncodeSampleRates, sizeof(aac),
            &aac, &size, rates.data()) == noErr) {
      rates.resize(size / sizeof(AudioValueRange));
      std::erase_if(rates, [](const auto& r) { return r.mMaximum <= 0; });
      const bool takes = std::ranges::any_of(rates, [&](const auto& r) {
        return to >= r.mMinimum && to <= r.mMaximum;
      });
      if (!takes) {
        double above = 0, below = 0;
        for (const auto& r : rates) {
          if (r.mMinimum > to && r.mMinimum <= 48000 &&
              (above == 0 || r.mMinimum < above)) {
            above = r.mMinimum;
          }
          if (r.mMaximum < to) {
            below = std::max(below, r.mMaximum);
          }
        }
        to = above > 0 ? above : below > 0 ? below : 48000;
      }
    }
  }
  // Its bit rates for that rate and channel count: a converter set up
  // for them says.
  const double want = per_channel * channels;
  double bitrate = want;
  AudioStreamBasicDescription pcm = {};
  pcm.mSampleRate = to;
  pcm.mFormatID = kAudioFormatLinearPCM;
  pcm.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  pcm.mBitsPerChannel = 32;
  pcm.mChannelsPerFrame = static_cast<UInt32>(channels);
  pcm.mFramesPerPacket = 1;
  pcm.mBytesPerFrame = pcm.mBytesPerPacket = 4 * pcm.mChannelsPerFrame;
  AudioStreamBasicDescription enc = {};
  enc.mSampleRate = to;
  enc.mFormatID = aac;
  enc.mChannelsPerFrame = static_cast<UInt32>(channels);
  enc.mFramesPerPacket = 1024;
  AudioConverterRef conv = nullptr;
  if (AudioConverterNew(&pcm, &enc, &conv) == noErr) {
    UInt32 n = 0;
    if (AudioConverterGetPropertyInfo(
            conv, kAudioConverterApplicableEncodeBitRates, &n, nullptr) ==
            noErr && n >= sizeof(AudioValueRange)) {
      std::vector<AudioValueRange> rates(n / sizeof(AudioValueRange));
      if (AudioConverterGetProperty(
              conv, kAudioConverterApplicableEncodeBitRates, &n,
              rates.data()) == noErr) {
        rates.resize(n / sizeof(AudioValueRange));
        std::erase_if(rates, [](const auto& r) { return r.mMaximum <= 0; });
        double below = 0;
        double lowest = std::numeric_limits<double>::max();
        for (const auto& r : rates) {
          lowest = std::min(lowest, r.mMinimum);
          if (r.mMinimum <= want) {
            below = std::max(below, std::min(r.mMaximum, want));
          }
        }
        if (below > 0) {
          bitrate = below;
        } else if (!rates.empty()) {
          bitrate = lowest;
        }
      }
    }
    AudioConverterDispose(conv);
  }
  return @{
    AVFormatIDKey : @(kAudioFormatMPEG4AAC),
    AVSampleRateKey : @(to),
    AVNumberOfChannelsKey : @(channels),
    AVEncoderBitRateKey : @(std::lround(bitrate)),
  };
}

// Linear PCM, 24-bit integer, interleaved: a ProRes movie's sound.
NSDictionary*
pcm24_settings(double rate, int channels)
{
  return @{
    AVFormatIDKey : @(kAudioFormatLinearPCM),
    AVSampleRateKey : @(rate),
    AVNumberOfChannelsKey : @(channels),
    AVLinearPCMBitDepthKey : @24,
    AVLinearPCMIsFloatKey : @NO,
    AVLinearPCMIsBigEndianKey : @NO,
    AVLinearPCMIsNonInterleaved : @NO,
  };
}

}

std::span<const std::string_view>
intermediate_formats()
{
  static constexpr std::array<std::string_view, 5> kFormats = {
    "prores4444xq", "prores4444", "prores422hq", "prores422", "hevc",
  };
  return kFormats;
}

bool
is_intermediate_format(std::string_view f)
{
  return std::ranges::find(intermediate_formats(), f) !=
         intermediate_formats().end();
}

bool
is_prores(std::string_view f)
{
  return f.starts_with("prores");
}

Status
add_soundtrack(const fs::path& video, const fs::path& audio,
               const fs::path& out, double start, double duration,
               SoundCodec sound, std::string_view software,
               const MadeFrom& made)
{
  @autoreleasepool {
    AVURLAsset* va = [AVURLAsset URLAssetWithURL:file_url(video) options:nil];
    AVURLAsset* aa = [AVURLAsset URLAssetWithURL:file_url(audio) options:nil];
    AVAssetTrack* vt = first_track(va, AVMediaTypeVideo);
    AVAssetTrack* at = first_track(aa, AVMediaTypeAudio);
    if (!vt) {
      return make_error(Code::Corrupt, std::format(
          "{} has no video track", video.filename().string()));
    }
    if (!at) {
      return make_error(Code::Corrupt, std::format(
          "{} has no audio track", audio.filename().string()));
    }
    load_keys(vt, @[ @"formatDescriptions", @"preferredTransform" ]);
    load_keys(at, @[ @"formatDescriptions" ]);

    NSError* err = nil;
    AVAssetReader* vr = [AVAssetReader assetReaderWithAsset:va error:&err];
    AVAssetReader* ar =
        vr ? [AVAssetReader assetReaderWithAsset:aa error:&err] : nil;
    if (!vr || !ar) {
      return make_error(Code::Io, "cannot read the movie: " + why(err));
    }
    // The video as it is stored (nil settings: no decode).
    AVAssetReaderTrackOutput* vout =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:vt
                                                   outputSettings:nil];
    vout.alwaysCopiesSampleData = NO;
    // AAC already (an export of a clip) is copied as it is; anything else
    // is read as interleaved 32-bit float, for the AAC encoder.
    auto afmt = (__bridge CMAudioFormatDescriptionRef)at.formatDescriptions
                    .firstObject;
    const AudioStreamBasicDescription* asbd =
        afmt ? CMAudioFormatDescriptionGetStreamBasicDescription(afmt)
             : nullptr;
    // A span of it is cut decoded, to the sample: AAC packets would cut
    // to the packet, and carry their priming into the middle.
    const bool span = duration > 0;
    const bool aac_in = sound == SoundCodec::Aac && !span && asbd &&
                        asbd->mFormatID == kAudioFormatMPEG4AAC;
    NSDictionary* pcm = aac_in ? nil : @{
      AVFormatIDKey : @(kAudioFormatLinearPCM),
      AVLinearPCMBitDepthKey : @32,
      AVLinearPCMIsFloatKey : @YES,
      AVLinearPCMIsBigEndianKey : @NO,
      AVLinearPCMIsNonInterleaved : @NO,
    };
    AVAssetReaderTrackOutput* aout =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:at
                                                   outputSettings:pcm];
    [vr addOutput:vout];
    [ar addOutput:aout];
    const CMTime from = CMTimeMakeWithSeconds(std::max(0.0, start), 48000);
    if (span) {
      ar.timeRange = CMTimeRangeMake(
          from, CMTimeMakeWithSeconds(duration, 48000));
    }

    std::error_code ec;
    fs::remove(out, ec);
    const std::string ext = out.extension().string();
    AVFileType type = ext == ".mov" ? AVFileTypeQuickTimeMovie
                      : ext == ".m4v" ? AVFileTypeAppleM4V
                                      : AVFileTypeMPEG4;
    AVAssetWriter* w = [AVAssetWriter assetWriterWithURL:file_url(out)
                                                fileType:type
                                                   error:&err];
    if (!w) {
      return make_error(Code::Io, "cannot write the movie: " + why(err));
    }
    w.metadata = software_items(type, software, nil, made);
    auto vfmt = (__bridge CMFormatDescriptionRef)vt.formatDescriptions
                    .firstObject;
    AVAssetWriterInput* vin =
        [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                           outputSettings:nil
                                         sourceFormatHint:vfmt];
    vin.transform = vt.preferredTransform;
    vin.expectsMediaDataInRealTime = NO;

    // AAC at the sound's own rate and channel count -- nothing resampled
    // -- where AAC encodes that rate (aac_settings); or PCM, any rate.
    const double rate = asbd ? asbd->mSampleRate : 48000;
    const int channels = asbd ? static_cast<int>(asbd->mChannelsPerFrame) : 2;
    if (channels < 1 || channels > 2) {
      return make_error(Code::Unsupported, std::format(
          "a soundtrack of {} channels is not supported", channels));
    }
    NSDictionary* enc = aac_in ? nil
        : sound == SoundCodec::Pcm ? pcm24_settings(rate, channels)
                                   : aac_settings(rate, channels, 96000);
    // The hint describes what is APPENDED: the stored AAC when it is
    // passed through; for the encoder, the reader's float PCM speaks for
    // itself -- the file's own format (16-bit, say) would be misread.
    AVAssetWriterInput* ain =
        [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                           outputSettings:enc
                                         sourceFormatHint:aac_in ? afmt
                                                                 : nullptr];
    ain.expectsMediaDataInRealTime = NO;
    if (![w canAddInput:vin] || ![w canAddInput:ain]) {
      return make_error(Code::Unsupported, std::format(
          "{} cannot hold this video and sound", ext));
    }
    [w addInput:vin];
    [w addInput:ain];

    if (![vr startReading] || ![ar startReading]) {
      return make_error(Code::Io, "cannot read the movie: " +
                        why(vr.error ?: ar.error));
    }
    if (![w startWriting]) {
      [vr cancelReading];
      [ar cancelReading];
      return make_error(Code::Io, "cannot write the movie: " +
                        why(w.error));
    }
    [w startSessionAtSourceTime:kCMTimeZero];
    dispatch_group_t group = dispatch_group_create();
    pump(vout, vin,
         dispatch_queue_create("valtz.movie.video", DISPATCH_QUEUE_SERIAL),
         group);
    pump(aout, ain,
         dispatch_queue_create("valtz.movie.audio", DISPATCH_QUEUE_SERIAL),
         group, span ? from : kCMTimeZero);
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);

    if (vr.status == AVAssetReaderStatusFailed ||
        ar.status == AVAssetReaderStatusFailed ||
        w.status == AVAssetWriterStatusFailed) {
      NSError* e = w.status == AVAssetWriterStatusFailed ? w.error
                   : vr.status == AVAssetReaderStatusFailed ? vr.error
                                                             : ar.error;
      [w cancelWriting];
      fs::remove(out, ec);
      return make_error(Code::Io, "joining video and sound failed: " +
                        why(e));
    }
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [w finishWritingWithCompletionHandler:^{
      dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (w.status != AVAssetWriterStatusCompleted) {
      fs::remove(out, ec);
      return make_error(Code::Io, "joining video and sound failed: " +
                        why(w.error));
    }
  }
  return ok_status();
}

Status
write_sound(const fs::path& src, const fs::path& out,
            std::string_view format, double start, double duration,
            std::string_view software)
{
  if (format != "wav" && format != "m4a") {
    return make_error(Code::InvalidArgument, std::format(
        "'{}' is not a sound format (wav, m4a)", format));
  }
  @autoreleasepool {
    AVURLAsset* a = [AVURLAsset URLAssetWithURL:file_url(src) options:nil];
    AVAssetTrack* t = first_track(a, AVMediaTypeAudio);
    if (!t) {
      return make_error(Code::Corrupt, std::format(
          "{} has no audio track", src.filename().string()));
    }
    load_keys(t, @[ @"formatDescriptions" ]);
    auto fmt = (__bridge CMAudioFormatDescriptionRef)t.formatDescriptions
                   .firstObject;
    const AudioStreamBasicDescription* asbd =
        fmt ? CMAudioFormatDescriptionGetStreamBasicDescription(fmt)
            : nullptr;
    if (!asbd || asbd->mSampleRate <= 0 || asbd->mChannelsPerFrame < 1) {
      return make_error(Code::Corrupt, std::format(
          "{} has a sound that cannot be read", src.filename().string()));
    }
    const double rate = asbd->mSampleRate;
    const int channels = static_cast<int>(asbd->mChannelsPerFrame);
    if (channels > 2) {
      return make_error(Code::Unsupported, std::format(
          "a sound of {} channels is not supported", channels));
    }

    NSError* err = nil;
    AVAssetReader* r = [AVAssetReader assetReaderWithAsset:a error:&err];
    if (!r) {
      return make_error(Code::Io, "cannot read the sound: " + why(err));
    }
    // Decoded, interleaved 32-bit float: the span is cut to the sample,
    // and the writer encodes from it.
    NSDictionary* pcm = @{
      AVFormatIDKey : @(kAudioFormatLinearPCM),
      AVLinearPCMBitDepthKey : @32,
      AVLinearPCMIsFloatKey : @YES,
      AVLinearPCMIsBigEndianKey : @NO,
      AVLinearPCMIsNonInterleaved : @NO,
    };
    AVAssetReaderTrackOutput* o =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t
                                                   outputSettings:pcm];
    [r addOutput:o];
    // The span on the sound's own sample clock.
    const auto scale = static_cast<CMTimeScale>(std::lround(rate));
    const CMTime from = CMTimeMake(
        static_cast<std::int64_t>(std::llround(std::max(0.0, start) * rate)),
        scale);
    if (duration > 0) {
      r.timeRange = CMTimeRangeMake(
          from, CMTimeMake(static_cast<std::int64_t>(
                               std::llround(duration * rate)),
                           scale));
    } else if (start > 0) {
      r.timeRange = CMTimeRangeMake(from, kCMTimePositiveInfinity);
    }

    std::error_code ec;
    fs::remove(out, ec);
    const bool wav = format == "wav";
    AVAssetWriter* w =
        [AVAssetWriter assetWriterWithURL:file_url(out)
                                 fileType:wav ? AVFileTypeWAVE
                                              : AVFileTypeAppleM4A
                                    error:&err];
    if (!w) {
      return make_error(Code::Io, "cannot write the sound: " + why(err));
    }
    // An .m4a names who wrote it in its metadata; a WAV's INFO chunk,
    // which AVAssetWriter does not write, is added once it is done.
    if (!wav) {
      w.metadata = software_items(AVFileTypeAppleM4A, software);
    }
    // PCM as deep as the sound is (16 bits for a model's WAV); a float
    // or compressed one has no depth of its own to keep: 24 bits.
    const bool int_pcm = asbd->mFormatID == kAudioFormatLinearPCM &&
                         !(asbd->mFormatFlags & kAudioFormatFlagIsFloat);
    const int bits = int_pcm && (asbd->mBitsPerChannel == 16 ||
                                 asbd->mBitsPerChannel == 24 ||
                                 asbd->mBitsPerChannel == 32)
                         ? static_cast<int>(asbd->mBitsPerChannel)
                         : 24;
    NSDictionary* settings = wav ? @{
      AVFormatIDKey : @(kAudioFormatLinearPCM),
      AVSampleRateKey : @(rate),
      AVNumberOfChannelsKey : @(channels),
      AVLinearPCMBitDepthKey : @(bits),
      AVLinearPCMIsFloatKey : @NO,
      AVLinearPCMIsBigEndianKey : @NO,
      AVLinearPCMIsNonInterleaved : @NO,
    } : aac_settings(rate, channels, 128000);
    AVAssetWriterInput* in =
        [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                           outputSettings:settings];
    in.expectsMediaDataInRealTime = NO;
    if (![w canAddInput:in]) {
      return make_error(Code::Unsupported, std::format(
          "{} cannot hold this sound", out.extension().string()));
    }
    [w addInput:in];
    if (![r startReading]) {
      return make_error(Code::Io, "cannot read the sound: " + why(r.error));
    }
    if (![w startWriting]) {
      [r cancelReading];
      return make_error(Code::Io, "cannot write the sound: " +
                        why(w.error));
    }
    [w startSessionAtSourceTime:kCMTimeZero];
    dispatch_group_t group = dispatch_group_create();
    pump(o, in,
         dispatch_queue_create("valtz.sound", DISPATCH_QUEUE_SERIAL), group,
         CMTimeCompare(from, kCMTimeZero) > 0 ? from : kCMTimeZero);
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    if (r.status == AVAssetReaderStatusFailed ||
        w.status == AVAssetWriterStatusFailed) {
      NSError* e = w.status == AVAssetWriterStatusFailed ? w.error : r.error;
      [w cancelWriting];
      fs::remove(out, ec);
      return make_error(Code::Io, "writing the sound failed: " + why(e));
    }
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [w finishWritingWithCompletionHandler:^{
      dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (w.status != AVAssetWriterStatusCompleted) {
      fs::remove(out, ec);
      return make_error(Code::Io, "writing the sound failed: " +
                        why(w.error));
    }
  }
  if (format == "wav" && !software.empty()) {
    // The sound is written: its INFO is not worth failing it over.
    (void)stamp_software(out, software);
  }
  return ok_status();
}

Status
remux_with_software(const fs::path& in, const fs::path& out,
                    std::string_view software, const MadeFrom& made)
{
  @autoreleasepool {
    AVURLAsset* a = [AVURLAsset URLAssetWithURL:file_url(in) options:nil];
    load_keys(a, @[ @"tracks", @"metadata" ]);
    NSError* err = nil;
    AVAssetReader* r = [AVAssetReader assetReaderWithAsset:a error:&err];
    if (!r) {
      return make_error(Code::Io, "cannot read the movie: " + why(err));
    }
    std::error_code ec;
    fs::remove(out, ec);
    const AVFileType type = movie_type(out);
    AVAssetWriter* w = [AVAssetWriter assetWriterWithURL:file_url(out)
                                                fileType:type
                                                   error:&err];
    if (!w) {
      return make_error(Code::Io, "cannot write the movie: " + why(err));
    }
    w.metadata = software_items(type, software, a.metadata, made);
    // Every picture and sound as it is stored (nil settings: no decode).
    std::vector<std::pair<AVAssetReaderTrackOutput*, AVAssetWriterInput*>>
        pairs;
    for (AVAssetTrack* t in a.tracks) {
      if (![t.mediaType isEqualToString:AVMediaTypeVideo] &&
          ![t.mediaType isEqualToString:AVMediaTypeAudio]) {
        continue;
      }
      load_keys(t, @[ @"formatDescriptions", @"preferredTransform" ]);
      AVAssetReaderTrackOutput* o =
          [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t
                                                     outputSettings:nil];
      o.alwaysCopiesSampleData = NO;
      auto hint = (__bridge CMFormatDescriptionRef)t.formatDescriptions
                      .firstObject;
      AVAssetWriterInput* i =
          [AVAssetWriterInput assetWriterInputWithMediaType:t.mediaType
                                             outputSettings:nil
                                           sourceFormatHint:hint];
      i.transform = t.preferredTransform;
      i.expectsMediaDataInRealTime = NO;
      if (![r canAddOutput:o] || ![w canAddInput:i]) {
        return make_error(Code::Unsupported, std::format(
            "{} cannot hold this movie's tracks",
            out.extension().string()));
      }
      [r addOutput:o];
      [w addInput:i];
      pairs.emplace_back(o, i);
    }
    if (pairs.empty()) {
      return make_error(Code::Corrupt, std::format(
          "{} has no picture or sound", in.filename().string()));
    }
    if (![r startReading]) {
      return make_error(Code::Io, "cannot read the movie: " + why(r.error));
    }
    if (![w startWriting]) {
      [r cancelReading];
      return make_error(Code::Io, "cannot write the movie: " +
                        why(w.error));
    }
    [w startSessionAtSourceTime:kCMTimeZero];
    // A queue a track: interleaved, one waits for another.
    dispatch_group_t group = dispatch_group_create();
    for (auto& [o, i] : pairs) {
      pump(o, i,
           dispatch_queue_create("valtz.remux", DISPATCH_QUEUE_SERIAL),
           group);
    }
    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    if (r.status == AVAssetReaderStatusFailed ||
        w.status == AVAssetWriterStatusFailed) {
      NSError* e = w.status == AVAssetWriterStatusFailed ? w.error : r.error;
      [w cancelWriting];
      fs::remove(out, ec);
      return make_error(Code::Io, "rewriting the movie failed: " + why(e));
    }
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [w finishWritingWithCompletionHandler:^{
      dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (w.status != AVAssetWriterStatusCompleted) {
      fs::remove(out, ec);
      return make_error(Code::Io, "rewriting the movie failed: " +
                        why(w.error));
    }
  }
  return ok_status();
}

}
