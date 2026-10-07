#include "valtz/media/movie.h"

#import <AVFoundation/AVFoundation.h>

#include <algorithm>
#include <cmath>
#include <format>
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

}

Status
add_soundtrack(const fs::path& video, const fs::path& audio,
               const fs::path& out, double start, double duration)
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
    const bool aac_in = !span && asbd &&
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
    auto vfmt = (__bridge CMFormatDescriptionRef)vt.formatDescriptions
                    .firstObject;
    AVAssetWriterInput* vin =
        [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo
                                           outputSettings:nil
                                         sourceFormatHint:vfmt];
    vin.transform = vt.preferredTransform;
    vin.expectsMediaDataInRealTime = NO;

    // AAC at the sound's own rate and channel count: nothing resampled.
    const double rate = asbd ? asbd->mSampleRate : 48000;
    const int channels = asbd ? static_cast<int>(asbd->mChannelsPerFrame) : 2;
    if (channels < 1 || channels > 2) {
      return make_error(Code::Unsupported, std::format(
          "a soundtrack of {} channels is not supported", channels));
    }
    NSDictionary* aac = aac_in ? nil : @{
      AVFormatIDKey : @(kAudioFormatMPEG4AAC),
      AVSampleRateKey : @(rate),
      AVNumberOfChannelsKey : @(channels),
      AVEncoderBitRateKey : @(96000 * channels),
    };
    // The hint describes what is APPENDED: the stored AAC when it is
    // passed through; for the encoder, the reader's float PCM speaks for
    // itself -- the file's own format (16-bit, say) would be misread.
    AVAssetWriterInput* ain =
        [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio
                                           outputSettings:aac
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
            std::string_view format, double start, double duration)
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
    } : @{
      AVFormatIDKey : @(kAudioFormatMPEG4AAC),
      // AAC's rates stop at 48 kHz.
      AVSampleRateKey : @(std::min(rate, 48000.0)),
      AVNumberOfChannelsKey : @(channels),
      AVEncoderBitRateKey : @(128000 * channels),
    };
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
  return ok_status();
}

}
