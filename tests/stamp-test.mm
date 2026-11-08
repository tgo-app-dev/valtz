// A file handed out names its writer (media/stamp.h, DESIGN §7 Export):
// each format in its own place, its pictures' and sounds' bytes as they
// were, a generated picture's model kept, a second stamp replacing the
// first.
#include "testing.h"

#include "valtz/media/movie.h"
#include "valtz/media/pdf.h"
#include "valtz/media/stamp.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>

#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace valtz;
namespace fs = std::filesystem;

namespace {

constexpr std::string_view kSw = "Valtz 0.1.0 build 141 (Vpipe 0.1 abc)";

NSURL*
url_of(const fs::path& p)
{
  return [NSURL fileURLWithPath:@(p.c_str())];
}

// A picture of something in every channel, written by ImageIO as `uti`
// with `props` -- 16 bits a channel where `deep`.
bool
write_picture(const fs::path& p, CFStringRef uti, bool deep,
              NSDictionary* props)
{
  const int w = 48, h = 32;
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
  CGContextRef c = CGBitmapContextCreate(
      nullptr, w, h, deep ? 16 : 8, 0, cs,
      static_cast<std::uint32_t>(kCGImageAlphaNoneSkipLast) |
          (deep ? static_cast<std::uint32_t>(kCGBitmapByteOrder16Host)
                : 0u));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      CGContextSetRGBFillColor(c, x / double(w), y / double(h),
                               ((x * y) % 7) / 7.0, 1);
      CGContextFillRect(c, CGRectMake(x, y, 1, 1));
    }
  }
  CGImageRef img = CGBitmapContextCreateImage(c);
  CGImageDestinationRef d = CGImageDestinationCreateWithURL(
      (__bridge CFURLRef)url_of(p), uti, 1, nullptr);
  bool ok = d != nullptr;
  if (d) {
    CGImageDestinationAddImage(d, img, (__bridge CFDictionaryRef)props);
    ok = CGImageDestinationFinalize(d);
    CFRelease(d);
  }
  CGImageRelease(img);
  CGContextRelease(c);
  CGColorSpaceRelease(cs);
  return ok;
}

NSData*
pixels(const fs::path& p)
{
  CGImageSourceRef s =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url_of(p), nullptr);
  if (!s) {
    return nil;
  }
  CGImageRef i = CGImageSourceCreateImageAtIndex(s, 0, nullptr);
  NSData* d = i ? (__bridge_transfer NSData*)CGDataProviderCopyData(
                      CGImageGetDataProvider(i))
                : nil;
  if (i) {
    CGImageRelease(i);
  }
  CFRelease(s);
  return d;
}

// What ImageIO reads as its writer: TIFF's Software, PNG's, XMP's tool.
struct Read {
  std::string tiff, png, xmp;
};

Read
software_of(const fs::path& p)
{
  Read r;
  CGImageSourceRef s =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url_of(p), nullptr);
  if (!s) {
    return r;
  }
  NSDictionary* d = (__bridge_transfer NSDictionary*)
      CGImageSourceCopyPropertiesAtIndex(s, 0, nullptr);
  auto str = [](id v) {
    return [v isKindOfClass:[NSString class]]
               ? std::string([(NSString*)v UTF8String]) : std::string();
  };
  r.tiff = str(d[(id)kCGImagePropertyTIFFDictionary]
                [(id)kCGImagePropertyTIFFSoftware]);
  r.png = str(d[(id)kCGImagePropertyPNGDictionary]
               [(id)kCGImagePropertyPNGSoftware]);
  if (CGImageMetadataRef m = CGImageSourceCopyMetadataAtIndex(s, 0,
                                                              nullptr)) {
    if (CFStringRef t = CGImageMetadataCopyStringValueWithPath(
            m, nullptr, CFSTR("xmp:CreatorTool"))) {
      r.xmp = [(__bridge NSString*)t UTF8String];
      CFRelease(t);
    }
    CFRelease(m);
  }
  CFRelease(s);
  return r;
}

std::string
bytes_of(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in),
          std::istreambuf_iterator<char>()};
}

// A PNG's eXIf block's Software, read from its chunk.
std::string
exif_software(const fs::path& p)
{
  const std::string b = bytes_of(p);
  for (std::size_t i = 8; i + 12 <= b.size();) {
    const std::size_t n =
        (std::size_t(std::uint8_t(b[i])) << 24) |
        (std::size_t(std::uint8_t(b[i + 1])) << 16) |
        (std::size_t(std::uint8_t(b[i + 2])) << 8) | std::uint8_t(b[i + 3]);
    if (b.compare(i + 4, 4, "eXIf") == 0) {
      const std::vector<std::uint8_t> t(b.begin() + i + 8,
                                        b.begin() + i + 8 + n);
      return media::tiff_software(t).value_or("");
    }
    i += 12 + n;
  }
  return {};
}

// A JPEG's EXIF segments (APP1 "Exif\0\0") taken out, the rest as it was.
bool
strip_jpeg_exif(const fs::path& p)
{
  std::string b = bytes_of(p);
  if (b.size() < 4 || b.compare(0, 2, "\xFF\xD8") != 0) {
    return false;
  }
  std::size_t i = 2;
  while (i + 4 <= b.size() && std::uint8_t(b[i]) == 0xFF &&
         std::uint8_t(b[i + 1]) != 0xDA) {
    const std::size_t len = (std::size_t(std::uint8_t(b[i + 2])) << 8) |
                            std::uint8_t(b[i + 3]);
    if (std::uint8_t(b[i + 1]) == 0xE1 &&
        b.compare(i + 4, 6, std::string("Exif\0\0", 6)) == 0) {
      b.erase(i, 2 + len);
      continue;
    }
    i += 2 + len;
  }
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(b.data(), static_cast<std::streamsize>(b.size()));
  return out.good();
}

std::size_t
count(const std::string& hay, std::string_view needle)
{
  std::size_t n = 0;
  for (auto at = hay.find(needle); at != std::string::npos;
       at = hay.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

// A second of a 440 Hz tone, 16-bit stereo at 48 kHz.
void
write_tone(const fs::path& p)
{
  const std::uint32_t rate = 48000, n = 48000;
  std::ofstream f(p, std::ios::binary);
  auto u32 = [&](std::uint32_t v) { f.write((const char*)&v, 4); };
  auto u16 = [&](std::uint16_t v) { f.write((const char*)&v, 2); };
  f.write("RIFF", 4);
  u32(36 + n * 4);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(2);
  u32(rate);
  u32(rate * 4);
  u16(4);
  u16(16);
  f.write("data", 4);
  u32(n * 4);
  for (std::uint32_t i = 0; i < n; ++i) {
    const auto v = static_cast<std::int16_t>(
        8000 * std::sin(2 * M_PI * 440 * i / double(rate)));
    u16(static_cast<std::uint16_t>(v));
    u16(static_cast<std::uint16_t>(v));
  }
}

// A few frames of H.264 -- a movie as vpipe's writer leaves one: a
// picture track, no metadata.
bool
write_movie(const fs::path& p, AVFileType type)
{
  NSError* err = nil;
  [[NSFileManager defaultManager] removeItemAtURL:url_of(p) error:nil];
  AVAssetWriter* w = [AVAssetWriter assetWriterWithURL:url_of(p)
                                              fileType:type
                                                 error:&err];
  if (!w) {
    return false;
  }
  AVAssetWriterInput* in = [AVAssetWriterInput
      assetWriterInputWithMediaType:AVMediaTypeVideo
                     outputSettings:@{
                       AVVideoCodecKey : AVVideoCodecTypeH264,
                       AVVideoWidthKey : @64,
                       AVVideoHeightKey : @48,
                     }];
  in.expectsMediaDataInRealTime = NO;
  auto* a = [AVAssetWriterInputPixelBufferAdaptor
      assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
                                 sourcePixelBufferAttributes:nil];
  [w addInput:in];
  if (![w startWriting]) {
    return false;
  }
  [w startSessionAtSourceTime:kCMTimeZero];
  for (int i = 0; i < 6; ++i) {
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferCreate(nullptr, 64, 48, kCVPixelFormatType_32BGRA, nullptr,
                        &pb);
    CVPixelBufferLockBaseAddress(pb, 0);
    std::memset(CVPixelBufferGetBaseAddress(pb), 40 * i,
                CVPixelBufferGetDataSize(pb));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    while (!in.readyForMoreMediaData) {
      [NSThread sleepForTimeInterval:0.005];
    }
    [a appendPixelBuffer:pb withPresentationTime:CMTimeMake(i, 24)];
    CVPixelBufferRelease(pb);
  }
  [in markAsFinished];
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [w finishWritingWithCompletionHandler:^{
    dispatch_semaphore_signal(sem);
  }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  return w.status == AVAssetWriterStatusCompleted;
}

// The value of `ident` in `p`'s metadata, and its frames' count.
std::string
movie_software(const fs::path& p, AVMetadataIdentifier ident)
{
  AVURLAsset* a = [AVURLAsset URLAssetWithURL:url_of(p) options:nil];
  for (AVMetadataItem* m in a.metadata) {
    if ([m.identifier isEqualToString:ident] && m.stringValue) {
      return m.stringValue.UTF8String;
    }
  }
  return {};
}

std::int64_t
frames_of(const fs::path& p)
{
  AVURLAsset* a = [AVURLAsset URLAssetWithURL:url_of(p) options:nil];
  __block AVAssetTrack* t = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [a loadTracksWithMediaType:AVMediaTypeVideo
           completionHandler:^(NSArray<AVAssetTrack*>* ts, NSError*) {
             t = ts.firstObject;
             dispatch_semaphore_signal(sem);
           }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  if (!t) {
    return -1;
  }
  AVAssetReader* r = [AVAssetReader assetReaderWithAsset:a error:nil];
  AVAssetReaderTrackOutput* o =
      [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:t
                                                 outputSettings:nil];
  [r addOutput:o];
  [r startReading];
  std::int64_t n = 0;
  while (CMSampleBufferRef s = [o copyNextSampleBuffer]) {
    n += CMSampleBufferGetNumSamples(s);
    CFRelease(s);
  }
  return n;
}

}  // namespace

TEST(media, a_file_handed_out_names_its_writer)
{
  const auto dir = test::temp_dir("stamp");
  const std::string sw(kSw);

  // The software, a model kept: what vpipe's save-image writes for a
  // generated picture gives its "with" to the stamp's brackets.
  CHECK(media::stamped_software("Valtz 0.1.0 (Vpipe 0.1 d93*0 with Krea 2)",
                                sw) ==
        "Valtz 0.1.0 build 141 (Vpipe 0.1 abc with Krea 2)");
  CHECK(media::stamped_software("Canon EOS 5D Mark IV Firmware 1.3", sw) ==
        sw);
  CHECK(media::stamped_software("", sw) == sw);
  // The model alone, and named again in another writer's name.
  CHECK(media::stamped_model("Valtz 0.1.0 (Vpipe 0.1 d93*0 with Krea 2)") ==
        "Krea 2");
  CHECK(media::stamped_model("Vpipe 0.1 d93 with PrunaAI/P-H3-Image") ==
        "PrunaAI/P-H3-Image");
  CHECK(media::stamped_model("Canon EOS 5D Mark IV Firmware 1.3").empty());
  CHECK(media::stamped_model(sw).empty());
  CHECK(media::with_model(sw, "Krea 2") ==
        "Valtz 0.1.0 build 141 (Vpipe 0.1 abc with Krea 2)");
  CHECK(media::with_model("Valtz 0.1.0", "Krea 2") ==
        "Valtz 0.1.0 (with Krea 2)");
  CHECK(media::with_model(sw, "") == sw);

  // A TIFF block of either order: IFD 0's other entries kept, Software
  // replaced, read back.
  {
    const std::vector<std::uint8_t> be = {'M', 'M', 0, 42, 0, 0, 0, 8,
                                          0, 0, 0, 0, 0, 0};
    auto t = media::tiff_with_software(be, sw);
    REQUIRE(t);
    CHECK(media::tiff_software(*t) == sw);
    auto again = media::tiff_with_software(*t, "short");
    REQUIRE(again);
    CHECK(media::tiff_software(*again) == "short");
    CHECK(!media::tiff_with_software({'x', 'y'}, sw));
    // Software alone ImageIO would not read: Orientation 1 beside it,
    // once (IFD 0 of two entries, again and again).
    for (const auto* b : {&*t, &*again}) {
      const std::size_t at = (std::size_t((*b)[4]) << 24) |
                             (std::size_t((*b)[5]) << 16) |
                             (std::size_t((*b)[6]) << 8) | (*b)[7];
      CHECK(((std::size_t((*b)[at]) << 8) | (*b)[at + 1]) == 2);
      CHECK((*b)[at + 2] == 0x01 && (*b)[at + 3] == 0x12);  // first
      CHECK((*b)[at + 10] == 0 && (*b)[at + 11] == 1);       // normal
    }
  }

  // PICTURES. A PNG as vpipe writes a generated one -- its EXIF a
  // Software naming the model -- and one with none; a camera's JPEG and
  // a 16-bit TIFF; an OpenEXR.
  const NSDictionary* made = @{
    (id)kCGImagePropertyTIFFDictionary : @{
      (id)kCGImagePropertyTIFFSoftware :
          @"Valtz 0.1.0 (Vpipe 0.1 d93*0 with Krea 2)",
    },
  };
  const NSDictionary* camera = @{
    (id)kCGImagePropertyTIFFDictionary : @{
      (id)kCGImagePropertyTIFFMake : @"Canon",
      (id)kCGImagePropertyTIFFSoftware : @"Firmware 1.3",
    },
    (id)kCGImagePropertyExifDictionary : @{
      (id)kCGImagePropertyExifUserComment : @"kept",
    },
  };
  struct Pic {
    const char* name;
    CFStringRef uti;
    bool deep;
    const NSDictionary* props;
    std::string want;
  };
  const std::string with = "Valtz 0.1.0 build 141 (Vpipe 0.1 abc with Krea 2)";
  const Pic pics[] = {
      {"made.png", CFSTR("public.png"), true, made, with},
      {"plain.png", CFSTR("public.png"), false, nil, sw},
      {"camera.jpg", CFSTR("public.jpeg"), false, camera, sw},
      // A JPEG with no EXIF -- as vpipe writes a generated picture's:
      // ImageIO's copy would give it XMP alone -- and one with ImageIO's
      // own (an Exif IFD, no TIFF field): its Software would go unread.
      {"plain.jpg", CFSTR("public.jpeg"), false, nil, sw},
      {"bare.jpg", CFSTR("public.jpeg"), false, nil, sw},
      {"deep.tif", CFSTR("public.tiff"), true, camera, sw},
  };
  for (const auto& p : pics) {
    const fs::path f = dir / p.name;
    REQUIRE(write_picture(f, p.uti, p.deep, (NSDictionary*)p.props));
    if (std::string_view(p.name) == "plain.jpg") {
      // ImageIO writes an EXIF block of its own (its Exif IFD alone):
      // taken out, as vpipe's JPEG has none.
      REQUIRE(strip_jpeg_exif(f));
      REQUIRE(count(bytes_of(f), std::string("Exif\0\0", 6)) == 0);
    }
    NSData* before = pixels(f);
    REQUIRE(before != nil);
    REQUIRE_OK(media::stamp_software(f, sw));
    CHECK([pixels(f) isEqualToData:before]);
    const Read r = software_of(f);
    const bool png = fs::path(p.name).extension() == ".png";
    // ImageIO reads it as TIFF's Software -- Preview's TIFF tab,
    // Spotlight's Creator -- a PNG's EXIF block too (two entries at
    // least), and from the block itself, as other readers do.
    CHECK(r.tiff == p.want);
    if (png) {
      CHECK(exif_software(f) == p.want);
    }
    CHECK(r.xmp == p.want);
    if (png) {
      CHECK(r.png == p.want);
    }
    // Stamped again: one Software, the new one.
    REQUIRE_OK(media::stamp_software(f, "Valtz 0.2.0 build 200"));
    const std::string again = p.want == with
                                  ? "Valtz 0.2.0 build 200 (with Krea 2)"
                                  : "Valtz 0.2.0 build 200";
    CHECK(software_of(f).tiff == again);
    if (png) {
      CHECK(exif_software(f) == again);
    }
    CHECK([pixels(f) isEqualToData:before]);
    if (png) {
      // One text chunk says it, the old one gone.
      CHECK(count(bytes_of(f), std::string("tEXtSoftware\0", 13)) == 1);
      CHECK(software_of(f).png == again);
    }
  }
  // A JPEG given EXIF holds one segment of it, stamped again or not.
  CHECK(count(bytes_of(dir / "plain.jpg"), std::string("Exif\0\0", 6)) ==
        1);
  // What the file names not kept: an export's model is its own to say
  // (Controller::made_with) -- here none, the "with Krea 2" gone.
  for (const char* name : {"model.png", "model.jpg"}) {
    const fs::path f = dir / name;
    const bool png = fs::path(name).extension() == ".png";
    REQUIRE(write_picture(f, png ? CFSTR("public.png")
                                 : CFSTR("public.jpeg"),
                          false, (NSDictionary*)made));
    REQUIRE_OK(media::stamp_software(f, sw, false));
    CHECK(software_of(f).tiff == sw);
    CHECK(software_of(f).xmp == sw);
    REQUIRE_OK(media::stamp_software(
        f, media::with_model(sw, "FLUX.3"), false));
    CHECK(software_of(f).tiff ==
          "Valtz 0.1.0 build 141 (Vpipe 0.1 abc with FLUX.3)");
  }
  // The camera's own fields stay.
  {
    CGImageSourceRef s = CGImageSourceCreateWithURL(
        (__bridge CFURLRef)url_of(dir / "camera.jpg"), nullptr);
    NSDictionary* d = (__bridge_transfer NSDictionary*)
        CGImageSourceCopyPropertiesAtIndex(s, 0, nullptr);
    CHECK([d[(id)kCGImagePropertyTIFFDictionary]
              [(id)kCGImagePropertyTIFFMake] isEqual:@"Canon"]);
    CFRelease(s);
  }
  // OpenEXR: a "software" attribute in its header, the pixels as they
  // were -- every chunk where its moved offset says.
  {
    const fs::path f = dir / "linear.exr";
    REQUIRE(write_picture(f, (__bridge CFStringRef)@"com.ilm.openexr-image",
                          true, nil));
    NSData* before = pixels(f);
    REQUIRE(before != nil);
    REQUIRE_OK(media::stamp_software(f, sw));
    CHECK([pixels(f) isEqualToData:before]);
    std::string b = bytes_of(f);
    CHECK(b.find(std::string("software\0string\0", 16)) != std::string::npos);
    CHECK(b.find(sw) != std::string::npos);
    REQUIRE_OK(media::stamp_software(f, "Valtz 0.2.0 build 200"));
    b = bytes_of(f);
    CHECK(count(b, std::string("software\0string\0", 16)) == 1);
    CHECK(b.find(sw) == std::string::npos);
    CHECK([pixels(f) isEqualToData:before]);
  }

  // SOUNDS. A WAV gets an INFO list's ISFT; an .m4a its encoding tool --
  // written so by write_sound, as an export writes them.
  const fs::path tone = dir / "tone.wav";
  write_tone(tone);
  {
    const fs::path f = dir / "out.wav";
    REQUIRE_OK(media::write_sound(tone, f, "wav", 0, 0, sw));
    std::string b = bytes_of(f);
    CHECK(b.find("ISFT") != std::string::npos);
    CHECK(b.find(sw) != std::string::npos);
    // RIFF's size is the file's less its first eight bytes.
    std::uint32_t riff = 0;
    std::memcpy(&riff, b.data() + 4, 4);
    CHECK(riff == b.size() - 8);
    AVURLAsset* a = [AVURLAsset URLAssetWithURL:url_of(f) options:nil];
    CHECK(std::abs(CMTimeGetSeconds(a.duration) - 1.0) < 1e-3);
    // Again: the list replaced, one ISFT.
    REQUIRE_OK(media::stamp_software(f, "Valtz 0.2.0 build 200"));
    b = bytes_of(f);
    CHECK(count(b, "ISFT") == 1);
    CHECK(b.find(sw) == std::string::npos);
    std::memcpy(&riff, b.data() + 4, 4);
    CHECK(riff == b.size() - 8);
  }
  {
    const fs::path f = dir / "out.m4a";
    REQUIRE_OK(media::write_sound(tone, f, "m4a", 0, 0, sw));
    CHECK(movie_software(f, AVMetadataIdentifieriTunesMetadataEncodingTool)
          == sw);
  }

  // MOVIES as vpipe's writer leaves them: every frame passed through, the
  // writer named -- QuickTime's software key in a .mov, iTunes' tool in
  // an .mp4.
  for (const auto& [name, type, ident] :
       {std::tuple{"clip.mov", AVFileTypeQuickTimeMovie,
                   AVMetadataIdentifierQuickTimeMetadataSoftware},
        std::tuple{"clip.mp4", AVFileTypeMPEG4,
                   AVMetadataIdentifieriTunesMetadataEncodingTool}}) {
    const fs::path f = dir / name;
    REQUIRE(write_movie(f, type));
    REQUIRE(frames_of(f) == 6);
    REQUIRE_OK(media::stamp_software(f, sw));
    CHECK(movie_software(f, ident) == sw);
    CHECK(frames_of(f) == 6);
  }

  // What it does not stamp, it leaves.
  const fs::path txt = dir / "notes.txt";
  std::ofstream(txt) << "words";
  CHECK(media::stamp_software(txt, sw).code() == Code::Unsupported);
  CHECK(bytes_of(txt) == "words");
}

namespace {

// What made a picture as ImageIO reads it -- Preview's Inspector: the
// TIFF tab's Image Description, the Exif tab's User Comment, the IPTC
// tab's caption, the PNG tab's Description and Comment.
struct Described {
  std::string tiff, exif, iptc, png, png_comment;
};

Described
described_of(const fs::path& p)
{
  Described r;
  CGImageSourceRef s =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url_of(p), nullptr);
  if (!s) {
    return r;
  }
  NSDictionary* d = (__bridge_transfer NSDictionary*)
      CGImageSourceCopyPropertiesAtIndex(s, 0, nullptr);
  CFRelease(s);
  auto str = [](id v) {
    return [v isKindOfClass:[NSString class]]
               ? std::string([(NSString*)v UTF8String]) : std::string();
  };
  r.tiff = str(d[(id)kCGImagePropertyTIFFDictionary]
                [(id)kCGImagePropertyTIFFImageDescription]);
  r.exif = str(d[(id)kCGImagePropertyExifDictionary]
                [(id)kCGImagePropertyExifUserComment]);
  r.iptc = str(d[(id)kCGImagePropertyIPTCDictionary]
                [(id)kCGImagePropertyIPTCCaptionAbstract]);
  r.png = str(d[(id)kCGImagePropertyPNGDictionary]
               [(id)kCGImagePropertyPNGDescription]);
  r.png_comment = str(d[(id)kCGImagePropertyPNGDictionary]
                       [(id)kCGImagePropertyPNGComment]);
  return r;
}

}  // namespace

// An export's Prompt and Seed (media/stamp.h, DESIGN §7 Export): the one
// generation's words in each format's description, its seed in its
// comment -- where Preview's Inspector (ImageIO) and QuickTime's readers
// find them -- the pixels as they were, a second stamp replacing them, one
// without them keeping them.
TEST(media, a_file_handed_out_says_what_made_it)
{
  const auto dir = test::temp_dir("stamp-made");
  const std::string sw(kSw);
  media::MadeFrom made;
  made.prompt = "A red fox trotting through snow at dawn, 狐狸, <Picture 1>";
  made.seed = 1234567;
  const std::string seed = "Seed: 1234567";
  CHECK(made.comment() == seed);
  CHECK(media::MadeFrom{}.empty());
  const media::MadeFrom zero{.prompt = "", .seed = 0};
  CHECK(zero.comment() == "Seed: 0");

  // A TIFF block of either order, read back: ImageDescription in IFD 0, a
  // UserComment in an Exif IFD made for it; one that has an Exif IFD
  // keeps its other entries.
  for (const auto& empty : {std::vector<std::uint8_t>{'M', 'M', 0, 42, 0, 0,
                                                      0, 8, 0, 0, 0, 0, 0,
                                                      0},
                            std::vector<std::uint8_t>{'I', 'I', 42, 0, 8, 0,
                                                      0, 0, 0, 0, 0, 0, 0,
                                                      0}}) {
    auto t = media::tiff_with_software(empty, sw, made);
    REQUIRE(t);
    CHECK(media::tiff_software(*t) == sw);
    const media::MadeFrom back = media::tiff_made_from(*t);
    CHECK(back.prompt == made.prompt);
    CHECK(back.seed == made.seed);
    // Again with others, and with none: replaced, then kept.
    media::MadeFrom other{.prompt = "a boat", .seed = 7};
    auto again = media::tiff_with_software(*t, "short", other);
    REQUIRE(again);
    CHECK(media::tiff_made_from(*again).prompt == "a boat");
    CHECK(media::tiff_made_from(*again).seed == 7);
    auto kept = media::tiff_with_software(*again, sw);
    REQUIRE(kept);
    CHECK(media::tiff_software(*kept) == sw);
    CHECK(media::tiff_made_from(*kept).prompt == "a boat");
    CHECK(media::tiff_made_from(*kept).seed == 7);
  }

  // PICTURES: a PNG as vpipe writes a generated one, and one ImageIO
  // wrote with XMP; a JPEG with no EXIF (vpipe's), a camera's with its
  // own UserComment; a 16-bit TIFF.
  const NSDictionary* generated = @{
    (id)kCGImagePropertyTIFFDictionary : @{
      (id)kCGImagePropertyTIFFSoftware :
          @"Valtz 0.1.0 (Vpipe 0.1 d93*0 with Krea 2)",
    },
  };
  const NSDictionary* camera = @{
    (id)kCGImagePropertyTIFFDictionary : @{
      (id)kCGImagePropertyTIFFMake : @"Canon",
      (id)kCGImagePropertyTIFFSoftware : @"Firmware 1.3",
    },
    (id)kCGImagePropertyExifDictionary : @{
      (id)kCGImagePropertyExifUserComment : @"kept",
      (id)kCGImagePropertyExifFNumber : @2.8,
    },
  };
  struct Pic {
    const char* name;
    CFStringRef uti;
    bool deep;
    const NSDictionary* props;
  };
  const Pic pics[] = {
      {"made.png", CFSTR("public.png"), true, generated},
      {"plain.png", CFSTR("public.png"), false, nil},
      {"plain.jpg", CFSTR("public.jpeg"), false, nil},
      {"camera.jpg", CFSTR("public.jpeg"), false, camera},
      {"deep.tif", CFSTR("public.tiff"), true, camera},
  };
  for (const auto& p : pics) {
    const fs::path f = dir / p.name;
    REQUIRE(write_picture(f, p.uti, p.deep, (NSDictionary*)p.props));
    if (std::string_view(p.name) == "plain.jpg") {
      REQUIRE(strip_jpeg_exif(f));
    }
    NSData* before = pixels(f);
    REQUIRE(before != nil);
    REQUIRE_OK(media::stamp_software(f, sw, false, made));
    CHECK([pixels(f) isEqualToData:before]);
    CHECK(software_of(f).tiff == sw);
    const bool png = fs::path(p.name).extension() == ".png";
    Described d = described_of(f);
    if (png) {
      // The PNG tab: Description and Comment.
      CHECK(d.png == made.prompt);
      CHECK(d.png_comment == seed);
    } else {
      // The TIFF tab's Image Description, the Exif tab's User Comment --
      // a camera's own replaced -- the IPTC tab's caption (XMP's).
      CHECK(d.tiff == made.prompt);
      CHECK(d.exif == seed);
      CHECK(d.iptc == made.prompt);
    }
    // Stamped again with another generation's: replaced, said once.
    const media::MadeFrom other{.prompt = "a small boat", .seed = 42};
    REQUIRE_OK(media::stamp_software(f, sw, false, other));
    d = described_of(f);
    CHECK((png ? d.png : d.tiff) == "a small boat");
    CHECK((png ? d.png_comment : d.exif) == "Seed: 42");
    if (png) {
      const std::string b = bytes_of(f);
      CHECK(count(b, std::string("Description\0", 12)) == 1);
      CHECK(count(b, std::string("Comment\0", 8)) == 1);
    } else {
      CHECK(d.iptc == "a small boat");
    }
    // Stamped with no words: they stay.
    REQUIRE_OK(media::stamp_software(f, "Valtz 0.2.0 build 200"));
    d = described_of(f);
    CHECK((png ? d.png : d.tiff) == "a small boat");
    CHECK((png ? d.png_comment : d.exif) == "Seed: 42");
    CHECK([pixels(f) isEqualToData:before]);
  }
  // The camera's other fields stay.
  {
    CGImageSourceRef s = CGImageSourceCreateWithURL(
        (__bridge CFURLRef)url_of(dir / "camera.jpg"), nullptr);
    NSDictionary* d = (__bridge_transfer NSDictionary*)
        CGImageSourceCopyPropertiesAtIndex(s, 0, nullptr);
    CHECK([d[(id)kCGImagePropertyTIFFDictionary]
              [(id)kCGImagePropertyTIFFMake] isEqual:@"Canon"]);
    CHECK([d[(id)kCGImagePropertyExifDictionary]
              [(id)kCGImagePropertyExifFNumber] doubleValue] == 2.8);
    CFRelease(s);
  }
  // OpenEXR: "comments" and an int "seed" in its header, the pixels as
  // they were.
  {
    const fs::path f = dir / "linear.exr";
    REQUIRE(write_picture(f, (__bridge CFStringRef)@"com.ilm.openexr-image",
                          true, nil));
    NSData* before = pixels(f);
    REQUIRE_OK(media::stamp_software(f, sw, false, made));
    CHECK([pixels(f) isEqualToData:before]);
    const std::string b = bytes_of(f);
    const std::string comments = std::string("comments\0string\0", 16);
    const auto at = b.find(comments);
    REQUIRE(at != std::string::npos);
    CHECK(b.compare(at + comments.size() + 4, made.prompt.size(),
                    made.prompt) == 0);
    const std::string seed_attr("seed\0int\0\x04\0\0\0", 13);
    const auto s = b.find(seed_attr);
    REQUIRE(s != std::string::npos);
    std::int32_t v = 0;
    std::memcpy(&v, b.data() + s + seed_attr.size(), 4);
    CHECK(v == 1234567);
    REQUIRE_OK(media::stamp_software(f, sw, false, made));
    CHECK(count(bytes_of(f), comments) == 1);
    CHECK([pixels(f) isEqualToData:before]);
  }

  // MOVIES: the description and comment QuickTime keeps -- both kinds in
  // a .mov -- iTunes' in an .mp4; every frame passed through. A clip
  // joined with its sound says them too.
  const fs::path tone = dir / "tone.wav";
  write_tone(tone);
  struct Mov {
    const char* name;
    AVFileType type;
    std::vector<AVMetadataIdentifier> desc, comment;
  };
  const Mov movs[] = {
      // (QuickTime's user data holds Mac Roman: words past ASCII are its
      // metadata's alone.)
      {"clip.mov", AVFileTypeQuickTimeMovie,
       {AVMetadataIdentifierQuickTimeMetadataDescription},
       {AVMetadataIdentifierQuickTimeMetadataComment,
        AVMetadataIdentifierQuickTimeUserDataComment}},
      // iTunes' ©des, and the "desc" ffprobe reads as the description.
      {"clip.mp4", AVFileTypeMPEG4,
       {AVMetadataIdentifieriTunesMetadataDescription,
        [AVMetadataItem identifierForKey:@"desc"
                                keySpace:AVMetadataKeySpaceiTunes]},
       {AVMetadataIdentifieriTunesMetadataUserComment}},
  };
  for (const auto& m : movs) {
    const fs::path f = dir / m.name;
    REQUIRE(write_movie(f, m.type));
    REQUIRE_OK(media::stamp_software(f, sw, false, made));
    CHECK(frames_of(f) == 6);
    for (auto ident : m.desc) {
      CHECK(movie_software(f, ident) == made.prompt);
    }
    for (auto ident : m.comment) {
      CHECK(movie_software(f, ident) == seed);
    }
    // Again, another's: replaced; with none: kept.
    const media::MadeFrom boat{.prompt = "a boat", .seed = 9};
    REQUIRE_OK(media::stamp_software(f, sw, false, boat));
    CHECK(movie_software(f, m.desc[0]) == "a boat");
    if (m.type == AVFileTypeQuickTimeMovie) {
      CHECK(movie_software(f, AVMetadataIdentifierQuickTimeUserDataDescription)
            == "a boat");
    }
    REQUIRE_OK(media::stamp_software(f, "Valtz 0.2.0 build 200"));
    CHECK(movie_software(f, m.desc[0]) == "a boat");
    CHECK(movie_software(f, m.comment[0]) == "Seed: 9");
    CHECK(frames_of(f) == 6);

    const fs::path joined = dir / (std::string("joined-") + m.name);
    REQUIRE_OK(media::add_soundtrack(f, tone, joined, 0, 0,
                                     media::SoundCodec::Aac, sw, made));
    CHECK(movie_software(joined, m.desc[0]) == made.prompt);
    CHECK(movie_software(joined, m.comment[0]) == seed);
  }

  // A PDF: its Subject and Keywords, as Preview's Inspector shows them.
  {
    const fs::path png = dir / "page.png";
    REQUIRE(write_picture(png, CFSTR("public.png"), false, nil));
    media::PdfDocument doc;
    media::PdfLayer page{.kind = media::PdfLayer::Kind::Stack};
    page.layers.push_back({.kind = media::PdfLayer::Kind::Picture,
                           .file = png});
    doc.pages.push_back(std::move(page));
    const fs::path f = dir / "page.pdf";
    REQUIRE_OK(media::write_pdf(doc, f, {}, sw, made));
    CGPDFDocumentRef pdf =
        CGPDFDocumentCreateWithURL((__bridge CFURLRef)url_of(f));
    REQUIRE(pdf);
    CGPDFDictionaryRef info = CGPDFDocumentGetInfo(pdf);
    auto text = [&](const char* key) {
      CGPDFStringRef v = nullptr;
      std::string out;
      if (CGPDFDictionaryGetString(info, key, &v)) {
        if (CFStringRef t = CGPDFStringCopyTextString(v)) {
          out = [(__bridge NSString*)t UTF8String];
          CFRelease(t);
        }
      }
      return out;
    };
    CHECK(text("Subject") == made.prompt);
    CHECK(text("Keywords") == seed);
    CHECK(text("Creator") == sw);
    CGPDFDocumentRelease(pdf);
  }
}
