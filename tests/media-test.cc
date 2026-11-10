#include "testing.h"

#include "valtz/assist/transcript.h"
#include "valtz/assist/video-summary.h"
#include "valtz/base/json.h"
#include "valtz/media/beats.h"
#include "valtz/media/bookmarks.h"
#include "valtz/media/color.h"
#include "valtz/media/crop.h"
#include "valtz/media/exif.h"
#include "valtz/media/keyframes.h"
#include "valtz/media/model-input.h"
#include "valtz/media/movie.h"
#include "valtz/media/probe.h"
#include "valtz/media/refmod.h"
#include "valtz/media/reverb.h"
#include "valtz/media/sound.h"
#include "valtz/media/space.h"
#include "valtz/project/records.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

using namespace valtz;

namespace {

// Write a small 16-bit RGBA PNG tagged Display P3.
bool
write_png16_p3(const std::filesystem::path& path)
{
  const size_t w = 8, h = 4;
  std::vector<std::uint16_t> px(w * h * 4, 0x8000);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceDisplayP3);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 16, w * 8, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
          static_cast<std::uint32_t>(kCGBitmapByteOrder16Little));
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(std::strlen(path.c_str())), false);
  CGImageDestinationRef dst =
      CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
  CGImageDestinationAddImage(dst, img, nullptr);
  bool ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  CFRelease(url);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return ok;
}

// Write an opaque 8-bit sRGB PNG of one grey, 64 x 36.
bool
write_png_grey(const std::filesystem::path& path, std::uint8_t v)
{
  const size_t w = 64, h = 36;
  std::vector<std::uint8_t> px(w * h * 4, v);
  for (size_t i = 3; i < px.size(); i += 4) { px[i] = 255; }
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 8, w * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(std::strlen(path.c_str())), false);
  CGImageDestinationRef dst =
      CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
  CGImageDestinationAddImage(dst, img, nullptr);
  bool ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  CFRelease(url);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return ok;
}

}

TEST(media, probe_16bit_p3_png_with_alpha)
{
  auto p = test::temp_dir("media") / "p3.png";
  REQUIRE(write_png16_p3(p));
  auto r = media::probe_file(p);
  REQUIRE_OK(r);
  CHECK(r->type == media::MediaType::Image);
  CHECK(r->frame.width == 8);
  CHECK(r->frame.height == 4);
  CHECK(r->frame.bits_per_component == 16);
  CHECK(r->codec_name == "PNG");  // the format's name, not its type
  CHECK(r->frame.alpha != media::AlphaMode::None);
  const auto& c = r->frame.color;
  CHECK(c.primaries == media::Primaries::SMPTE432 ||
        c.icc_name.find("P3") != std::string::npos);
}

TEST(media, probe_rejects_missing_and_garbage)
{
  CHECK(!media::probe_file("/nonexistent/x.png").ok());
  auto p = test::temp_dir("media") / "junk.png";
  FILE* f = std::fopen(p.c_str(), "wb");
  std::fputs("not a png", f);
  std::fclose(f);
  CHECK(!media::probe_file(p).ok());
}

TEST(media, probe_movie_when_available)
{
  const char* mov = std::getenv("VALTZ_TEST_MOVIE");
  if (!mov) {
    SKIP("set VALTZ_TEST_MOVIE to a video file");
  }
  auto r = media::probe_file(mov);
  REQUIRE_OK(r);
  CHECK(r->type == media::MediaType::Video);
  CHECK(r->frame.width > 0);
  CHECK(r->frame_rate.num > 0);
  CHECK(r->duration.valid());
}

namespace {

// `secs` of a 440 Hz tone, stereo, 16-bit, as a WAV.
void
write_tone(const std::filesystem::path& wav, double secs, int rate)
{
  const auto n = static_cast<std::uint32_t>(secs * rate);
  std::vector<std::int16_t> pcm(std::size_t{n} * 2);
  for (std::uint32_t i = 0; i < n; ++i) {
    const auto v = static_cast<std::int16_t>(
        8000 * std::sin(2 * M_PI * 440.0 * i / rate));
    pcm[2 * i] = pcm[2 * i + 1] = v;
  }
  std::ofstream f(wav, std::ios::binary);
  auto u32 = [&](std::uint32_t v) { f.write((const char*)&v, 4); };
  auto u16 = [&](std::uint16_t v) { f.write((const char*)&v, 2); };
  const std::uint32_t bytes = n * 4;
  f.write("RIFF", 4);
  u32(36 + bytes);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);  // PCM
  u16(2);
  u32(static_cast<std::uint32_t>(rate));
  u32(static_cast<std::uint32_t>(rate) * 4);
  u16(4);
  u16(16);
  f.write("data", 4);
  u32(bytes);
  f.write((const char*)pcm.data(), bytes);
}

}

// A file's SOUND heard as one channel (DESIGN §4h): a 5.1 sound, its six
// channels each at its own level, made one channel at 16 kHz -- each
// sample the six AVERAGED, none dropped or weighted.
TEST(media, a_sound_is_heard_as_one_channel)
{
  const auto dir = fs::temp_directory_path() / "valtz-mono-test";
  fs::create_directories(dir);
  const auto in = dir / "six.wav";
  {
    // WAVE_FORMAT_EXTENSIBLE, 5.1 (mask 0x3F), 48 kHz, a second: channel
    // k holds (k + 1) x 1000 throughout.
    const std::uint32_t rate = 48000, n = 48000, ch = 6;
    std::vector<std::int16_t> pcm(std::size_t{n} * ch);
    for (std::uint32_t i = 0; i < n; ++i) {
      for (std::uint32_t c = 0; c < ch; ++c) {
        pcm[i * ch + c] = static_cast<std::int16_t>((c + 1) * 1000);
      }
    }
    std::ofstream f(in, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write((const char*)&v, 4); };
    auto u16 = [&](std::uint16_t v) { f.write((const char*)&v, 2); };
    const std::uint32_t bytes = n * ch * 2;
    f.write("RIFF", 4);
    u32(60 + bytes);
    f.write("WAVEfmt ", 8);
    u32(40);
    u16(0xFFFE);
    u16(ch);
    u32(rate);
    u32(rate * ch * 2);
    u16(ch * 2);
    u16(16);
    u16(22);
    u16(16);
    u32(0x3F);
    // KSDATAFORMAT_SUBTYPE_PCM
    const unsigned char guid[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                    0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                    0x00, 0x38, 0x9B, 0x71};
    f.write((const char*)guid, 16);
    f.write("data", 4);
    u32(bytes);
    f.write((const char*)pcm.data(), bytes);
  }
  const auto out = dir / "mono.wav";
  auto secs = media::mono_sound(in, out, 16000);
  REQUIRE_OK(secs);
  CHECK(std::abs(*secs - 1.0) < 0.01);
  // Its samples: the data chunk's, 16-bit, one channel.
  std::ifstream f(out, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
  std::vector<std::int16_t> mono;
  for (std::size_t at = 12; at + 8 <= bytes.size();) {
    std::uint32_t len = 0;
    std::memcpy(&len, bytes.data() + at + 4, 4);
    if (std::memcmp(bytes.data() + at, "data", 4) == 0) {
      mono.resize(len / 2);
      std::memcpy(mono.data(), bytes.data() + at + 8, mono.size() * 2);
      break;
    }
    at += 8 + len + (len & 1);
  }
  REQUIRE(mono.size() > 8000);
  // The six levels' mean, 3500, away from the edges the resampler rings.
  for (std::size_t i = 2000; i < mono.size() - 2000; i += 1000) {
    CHECK(std::abs(mono[i] - 3500) <= 40);
  }
  fs::remove_all(dir);
}

// A separator's sound (DESIGN §4j): any sound as TWO channels at its
// rate -- a mono one on both sides -- written as a float WAV; and its
// sources written as they stream out, nothing clipped past full scale.
TEST(media, a_sound_is_heard_as_two_channels_and_written_unclipped)
{
  const auto dir = fs::temp_directory_path() / "valtz-stereo-test";
  fs::create_directories(dir);
  // A mono 48 kHz tone, a second.
  const auto in = dir / "mono.wav";
  {
    const std::uint32_t rate = 48000, n = 48000;
    std::ofstream f(in, std::ios::binary);
    auto u32 = [&](std::uint32_t v) { f.write((const char*)&v, 4); };
    auto u16 = [&](std::uint16_t v) { f.write((const char*)&v, 2); };
    f.write("RIFF", 4);
    u32(36 + n * 2);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * 2);
    u16(2);
    u16(16);
    f.write("data", 4);
    u32(n * 2);
    for (std::uint32_t i = 0; i < n; ++i) {
      const auto v = static_cast<std::int16_t>(
          8000 * std::sin(2 * M_PI * 440.0 * i / rate));
      f.write((const char*)&v, 2);
    }
  }
  // A float WAV's samples, interleaved, and its channel count.
  auto read_float = [](const fs::path& p, int* channels, int* rate) {
    std::ifstream f(p, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    std::vector<float> out;
    for (std::size_t at = 12; at + 8 <= b.size();) {
      std::uint32_t len = 0;
      std::memcpy(&len, b.data() + at + 4, 4);
      if (std::memcmp(b.data() + at, "fmt ", 4) == 0) {
        std::uint16_t tag = 0, ch = 0;
        std::memcpy(&tag, b.data() + at + 8, 2);
        std::memcpy(&ch, b.data() + at + 10, 2);
        std::memcpy(rate, b.data() + at + 12, 4);
        *channels = tag == 3 ? ch : -1;
      }
      if (std::memcmp(b.data() + at, "data", 4) == 0) {
        out.resize(len / 4);
        std::memcpy(out.data(), b.data() + at + 8, out.size() * 4);
        break;
      }
      at += 8 + len + (len & 1);
    }
    return out;
  };
  const auto two = dir / "two.wav";
  auto secs = media::stereo_sound(in, two, 44100);
  REQUIRE_OK(secs);
  CHECK(std::abs(*secs - 1.0) < 0.01);
  int ch = 0, rate = 0;
  auto s = read_float(two, &ch, &rate);
  CHECK(ch == 2);
  CHECK(rate == 44100);
  REQUIRE(s.size() > 2 * 40000);
  float peak = 0;
  for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
    CHECK(std::abs(s[i] - s[i + 1]) < 1e-4f);  // both sides alike
    peak = std::max(peak, std::abs(s[i]));
  }
  CHECK(peak > 0.15f && peak < 0.35f);
  auto info = media::probe_file(two);
  REQUIRE_OK(info);
  CHECK(info->type == media::MediaType::Audio);

  // Written as it comes, planar pieces: past full scale kept as it is.
  const auto out = dir / "loud.wav";
  {
    media::FloatWav w;
    REQUIRE_OK(w.open(out, 2, 44100));
    const std::vector<float> a = {1.5f, -2.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    REQUIRE_OK(w.append_planar(a.data(), 3));    // L 1.5 -2 .25, R .5 .75 1
    REQUIRE_OK(w.append_interleaved(a.data(), 3));
    CHECK(w.frames() == 6);
    REQUIRE_OK(w.close());
  }
  auto l = read_float(out, &ch, &rate);
  REQUIRE(l.size() == 12);
  CHECK(l[0] == 1.5f);
  CHECK(l[1] == 0.5f);
  CHECK(l[2] == -2.0f);
  CHECK(l[5] == 1.0f);
  CHECK(l[6] == 1.5f);
  CHECK(l[7] == -2.0f);
  auto loud = media::probe_file(out);
  REQUIRE_OK(loud);
  CHECK(loud->type == media::MediaType::Audio);
  fs::remove_all(dir);
}

// A clip's picture and sound, written apart, joined: the video passed
// through, a 16-bit WAV made AAC at its own rate -- and AS LONG AS IT
// IS. (Telling the encoder the WAV's 16-bit format while the reader hands
// it float once doubled every soundtrack.)
TEST(media, soundtrack_joins_at_its_own_length)
{
  const char* mov = std::getenv("VALTZ_TEST_MOVIE");
  if (!mov) {
    SKIP("set VALTZ_TEST_MOVIE to a video file");
  }
  auto in = media::probe_file(mov);
  REQUIRE_OK(in);
  const double secs = in->duration.seconds();
  REQUIRE(secs > 0.5);
  // A 440 Hz tone, 32 kHz stereo, 16-bit, as long as the movie.
  const int rate = 32000;
  const auto dir = test::temp_dir("soundtrack");
  const auto wav = dir / "sound.wav";
  write_tone(wav, secs, rate);
  // A .mov holds any picture the test movie has (ProRes too).
  const auto out = dir / "clip.mov";
  REQUIRE_OK(media::add_soundtrack(mov, wav, out));
  auto r = media::probe_file(out);
  REQUIRE_OK(r);
  CHECK(r->has_audio);
  CHECK(r->audio_sample_rate == rate);
  CHECK(r->audio_channels == 2);
  CHECK(r->frame.width == in->frame.width);
  CHECK(std::abs(r->duration.seconds() - secs) < 0.1);
}

// A movie's frames are its PICTURE's: a soundtrack that runs on past the
// last frame makes the movie longer, not its frames more. (Counted over
// the movie, a 73-frame clip offered frames after its last, drawn black.)
TEST(media, probe_counts_the_picture_not_the_sound)
{
  const char* mov = std::getenv("VALTZ_TEST_MOVIE");
  if (!mov) {
    SKIP("set VALTZ_TEST_MOVIE to a video file");
  }
  auto in = media::probe_file(mov);
  REQUIRE_OK(in);
  REQUIRE(in->frame_count > 0);
  const double secs = static_cast<double>(in->frame_count) /
                      in->frame_rate.to_double();
  const auto dir = test::temp_dir("long-sound");
  const auto wav = dir / "sound.wav";
  write_tone(wav, 2 * secs, 32000);
  const auto out = dir / "clip.mov";
  REQUIRE_OK(media::add_soundtrack(mov, wav, out));
  auto r = media::probe_file(out);
  REQUIRE_OK(r);
  CHECK(r->duration.seconds() > 1.5 * secs);  // the sound's
  CHECK(r->frame_count == in->frame_count);   // the picture's
}

TEST(media, pixel_format_table_roundtrips_fourcc)
{
  using media::PixelFormat;
  for (auto f : {PixelFormat::BGRA8, PixelFormat::RGBAHalf,
                 PixelFormat::YCbCr420_10, PixelFormat::AYCbCr4444_16}) {
    CHECK(media::from_cv_fourcc(media::info(f).cv_fourcc) == f);
  }
  CHECK(media::info(PixelFormat::AYCbCr4444_16).has_alpha);
}

namespace {

CFNumberRef
num(double v)
{
  return CFNumberCreate(nullptr, kCFNumberDoubleType, &v);
}

CFDictionaryRef
dict(std::initializer_list<std::pair<CFStringRef, CFTypeRef>> kv)
{
  std::vector<const void*> k, v;
  for (auto& [a, b] : kv) {
    k.push_back(a);
    v.push_back(b);
  }
  return CFDictionaryCreate(nullptr, k.data(), v.data(),
                            static_cast<CFIndex>(k.size()),
                            &kCFTypeDictionaryKeyCallBacks,
                            &kCFTypeDictionaryValueCallBacks);
}

// A small gray JPEG with what a camera writes: make, model, exposure,
// ISO, location, and a quarter turn (orientation 6).
bool
write_camera_jpeg(const std::filesystem::path& path)
{
  CGColorSpaceRef cs = CGColorSpaceCreateDeviceGray();
  CGContextRef ctx = CGBitmapContextCreate(nullptr, 8, 4, 8, 0, cs,
                                           kCGImageAlphaNone);
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(std::strlen(path.c_str())), false);
  CGImageDestinationRef dst =
      CGImageDestinationCreateWithURL(url, CFSTR("public.jpeg"), 1, nullptr);
  int iso_value = 200;  // a camera's SHORT
  CFNumberRef iso = CFNumberCreate(nullptr, kCFNumberIntType, &iso_value);
  CFArrayRef isos = CFArrayCreate(nullptr, (const void**)&iso, 1,
                                  &kCFTypeArrayCallBacks);
  CFNumberRef t = num(1.0 / 125), f = num(8), lat = num(37.8),
              lon = num(122.4), six = num(6);
  CFDictionaryRef tiff = dict({{kCGImagePropertyTIFFMake, CFSTR("Canon")},
                               {kCGImagePropertyTIFFModel,
                                CFSTR("Canon EOS R5")},
                               {kCGImagePropertyTIFFSoftware,
                                CFSTR("Lightroom")}});
  CFDictionaryRef exif = dict({{kCGImagePropertyExifExposureTime, t},
                               {kCGImagePropertyExifFNumber, f},
                               {kCGImagePropertyExifISOSpeedRatings, isos}});
  CFDictionaryRef gps = dict({{kCGImagePropertyGPSLatitude, lat},
                              {kCGImagePropertyGPSLatitudeRef, CFSTR("N")},
                              {kCGImagePropertyGPSLongitude, lon},
                              {kCGImagePropertyGPSLongitudeRef, CFSTR("W")}});
  CFDictionaryRef props = dict({{kCGImagePropertyTIFFDictionary, tiff},
                                {kCGImagePropertyExifDictionary, exif},
                                {kCGImagePropertyGPSDictionary, gps},
                                {kCGImagePropertyOrientation, six}});
  CGImageDestinationAddImage(dst, img, props);
  const bool ok = CGImageDestinationFinalize(dst);
  for (CFTypeRef r : {(CFTypeRef)props, (CFTypeRef)gps, (CFTypeRef)exif,
                      (CFTypeRef)tiff, (CFTypeRef)six, (CFTypeRef)lon,
                      (CFTypeRef)lat, (CFTypeRef)f, (CFTypeRef)t,
                      (CFTypeRef)isos, (CFTypeRef)iso, (CFTypeRef)dst,
                      (CFTypeRef)url, (CFTypeRef)img}) {
    CFRelease(r);
  }
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return ok;
}

std::vector<std::uint8_t>
read_bytes(const std::filesystem::path& p)
{
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

// A number in ImageIO's properties of `path`: top level, or in `sub`.
double
property(const std::filesystem::path& path, CFStringRef sub, CFStringRef key)
{
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(std::strlen(path.c_str())), false);
  CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(url);
  if (!src) {
    return NAN;
  }
  CFDictionaryRef p = CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr);
  CFRelease(src);
  double v = NAN;
  if (p) {
    CFDictionaryRef d = sub ? (CFDictionaryRef)CFDictionaryGetValue(p, sub)
                            : p;
    CFTypeRef n = d ? CFDictionaryGetValue(d, key) : nullptr;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) {
      CFNumberGetValue((CFNumberRef)n, kCFNumberDoubleType, &v);
    }
    CFRelease(p);
  }
  return v;
}

}

// The fields a person reads, as the probe keeps them: names as EXIF
// spells them, numbers as numbers, west and south negative.
TEST(media, probe_reads_exif_fields)
{
  auto p = test::temp_dir("media") / "camera.jpg";
  REQUIRE(write_camera_jpeg(p));
  auto r = media::probe_file(p);
  REQUIRE_OK(r);
  const Json& e = r->exif;
  CHECK(jget<std::string>(e, "Make", "") == "Canon");
  CHECK(jget<std::string>(e, "Model", "") == "Canon EOS R5");
  CHECK(jget<std::string>(e, "Software", "") == "Lightroom");
  CHECK(std::abs(jget(e, "ExposureTime", 0.0) - 0.008) < 1e-6);
  CHECK(jget(e, "FNumber", 0.0) == 8.0);
  CHECK(jget(e, "ISOSpeedRatings", 0) == 200);
  CHECK(std::abs(jget(e, "GPSLatitude", 0.0) - 37.8) < 1e-6);
  CHECK(std::abs(jget(e, "GPSLongitude", 0.0) + 122.4) < 1e-6);

  // A picture without any has none, and none is recorded.
  auto plain = test::temp_dir("media") / "plain.png";
  REQUIRE(write_png16_p3(plain));
  auto q = media::probe_file(plain);
  REQUIRE_OK(q);
  CHECK(q->exif.empty());
  Json j = *q;
  CHECK(!j.contains("exif"));
}

// Everything a file says of itself (the agent's get_exif): each
// dictionary under its name, values as recorded.
TEST(media, exif_all_reads_a_files_metadata_whole)
{
  auto p = test::temp_dir("media") / "camera.jpg";
  REQUIRE(write_camera_jpeg(p));
  const Json e = media::exif_all(p);
  CHECK(jget<std::string>(jget(e, "TIFF", Json()), "Make", "") == "Canon");
  CHECK(jget<std::string>(jget(e, "TIFF", Json()), "Model", "") ==
        "Canon EOS R5");
  CHECK(jget(jget(e, "Exif", Json()), "FNumber", 0.0) == 8.0);
  const Json iso = jget(jget(e, "Exif", Json()), "ISOSpeedRatings", Json());
  CHECK(iso.is_array() && iso.size() == 1 && iso[0].get<int>() == 200);
  const Json gps = jget(e, "GPS", Json());
  CHECK(std::abs(jget(gps, "Latitude", 0.0) - 37.8) < 1e-6);
  CHECK(jget<std::string>(gps, "LongitudeRef", "") == "W");
  CHECK(jget(e, "PixelWidth", 0) == 8);
  CHECK(jget<std::string>(e, "ProfileName", "") != "");
  // Nothing for a file that is no picture.
  auto txt = test::temp_dir("media") / "words.txt";
  {
    std::ofstream f(txt);
    f << "no picture";
  }
  CHECK(media::exif_all(txt).empty());
}

// The block carried to an edit's result: the camera's tags, upright, at
// the result's size. Spliced into a JPEG as its APP1 segment, ImageIO
// reads it back as such.
TEST(media, exif_block_is_upright_at_the_result_size)
{
  auto p = test::temp_dir("media") / "camera.jpg";
  REQUIRE(write_camera_jpeg(p));
  const auto block = media::exif_block_for(p, {300, 200});
  REQUIRE(block.size() > 8);
  CHECK((block[0] == 'I' && block[1] == 'I') ||
        (block[0] == 'M' && block[1] == 'M'));

  auto plain = test::temp_dir("media") / "carrier.jpg";
  REQUIRE(write_camera_jpeg(plain));
  // A JPEG with only our segment: SOI, APP1 "Exif", then the rest of a
  // JPEG with its own APP1 dropped.
  auto bytes = read_bytes(plain);
  REQUIRE(bytes.size() > 4);
  std::vector<std::uint8_t> out = {0xFF, 0xD8, 0xFF, 0xE1};
  const std::size_t len = block.size() + 8;
  out.push_back(static_cast<std::uint8_t>(len >> 8));
  out.push_back(static_cast<std::uint8_t>(len & 0xFF));
  for (char c : {'E', 'x', 'i', 'f', '\0', '\0'}) {
    out.push_back(static_cast<std::uint8_t>(c));
  }
  out.insert(out.end(), block.begin(), block.end());
  std::size_t i = 2;
  while (i + 4 <= bytes.size() && bytes[i] == 0xFF &&
         bytes[i + 1] != 0xDA) {
    const std::size_t l = (std::size_t(bytes[i + 2]) << 8) | bytes[i + 3];
    if (bytes[i + 1] != 0xE1) {
      out.insert(out.end(), bytes.begin() + i, bytes.begin() + i + 2 + l);
    }
    i += 2 + l;
  }
  out.insert(out.end(), bytes.begin() + i, bytes.end());
  auto spliced = test::temp_dir("media") / "spliced.jpg";
  std::ofstream(spliced, std::ios::binary)
      .write(reinterpret_cast<const char*>(out.data()),
             static_cast<std::streamsize>(out.size()));

  CHECK(property(spliced, nullptr, kCGImagePropertyOrientation) == 1);
  CHECK(property(spliced, kCGImagePropertyExifDictionary,
                 kCGImagePropertyExifPixelXDimension) == 300);
  CHECK(property(spliced, kCGImagePropertyExifDictionary,
                 kCGImagePropertyExifPixelYDimension) == 200);
  CHECK(property(spliced, kCGImagePropertyExifDictionary,
                 kCGImagePropertyExifFNumber) == 8);
  auto f = media::exif_fields(spliced);
  CHECK(jget<std::string>(f, "Make", "") == "Canon");
  CHECK(std::abs(jget(f, "GPSLongitude", 0.0) + 122.4) < 1e-6);

  // Nothing to carry: no block.
  auto none = test::temp_dir("media") / "none.png";
  REQUIRE(write_png16_p3(none));
  CHECK(media::exif_block_for(none, {8, 4}).empty());
}

// A crop is data: the identity writes nothing, values survive a round
// trip (held to sane ranges), and the placement puts the content where
// the definition says -- in Core Image's frame (y up).
TEST(media, a_negative_zoom_flips_the_picture)
{
  // Kept as written, held to its range either way; none at all is the
  // least, not nothing.
  CHECK(media::crop_from_json({{"scale_x", -1.0}}).scale_x == -1);
  CHECK(media::crop_from_json({{"scale_y", -1e9}}).scale_y == -100);
  CHECK(media::crop_from_json({{"scale_x", 0.0}}).scale_x == 0.01);
  CHECK(media::held_zoom(-0.001) == -0.01);
  // Flipped across: the content's left edge lands on the canvas's right,
  // its centre where it was.
  media::Crop c;
  c.content = {800, 600};
  c.scale_x = -1;
  auto p = media::crop_placement(c, 800, 600);
  auto at = [&](double x, double y) {
    const auto& m = p.transform;
    return std::pair{m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty};
  };
  CHECK(std::abs(at(0, 300).first - 800) < 1e-9);
  CHECK(std::abs(at(400, 300).first - 400) < 1e-9);
  CHECK(std::abs(at(400, 0).second - 0) < 1e-9);  // not upside down
  // Keyed from 1 to -1, it turns over -- held off nothing on its way.
  const auto k = media::keyed_crop_from_json(
      {{"keys", Json::array({{{"frame", 0}, {"scale_x", 1.0}},
                             {{"frame", 48}, {"scale_x", -1.0}}})},
       {"rate_num", 24}, {"rate_den", 1}});
  CHECK(std::abs(k.at(12).scale_x - 0.5) < 1e-9);
  CHECK(k.at(48).scale_x == -1);
  c.scale_x = k.at(24).scale_x;
  p = media::crop_placement(c, 800, 600);
  CHECK(std::abs(p.transform.a) >= 0.01 - 1e-12);
}

TEST(media, crop_is_data_and_places_the_content)
{
  media::Crop id;
  id.content = {800, 600};
  CHECK(id.identity());
  CHECK(media::to_json(id).empty());

  media::Crop c;
  c.content = {800, 600};
  c.scale_x = c.scale_y = 0.5;
  c.offset_x = 0.25;
  c.offset_y = 0.25;
  c.rotate = 90;
  c.pad = {1, 0, 0, 0.5};
  CHECK(!c.identity());
  const media::Crop back = media::crop_from_json(media::to_json(c));
  CHECK(back == c);
  CHECK(media::crop_from_json({{"scale", 1e9}}).scale_x <= 100);
  // One "scale" is both; each axis may differ.
  const media::Crop xy = media::crop_from_json({{"scale", 0.5},
                                                {"scale_y", 2.0}});
  CHECK(xy.scale_x == 0.5 && xy.scale_y == 2.0);
  // A whole turn either way, not folded into one: a clip keyed 0 and 360
  // turns all the way round.
  CHECK(media::crop_from_json({{"rotate", 360.0}}).rotate == 360);
  CHECK(media::crop_from_json({{"rotate", -370.0}}).rotate == -360);
  {
    const auto k = media::keyed_crop_from_json(
        {{"keys", Json::array({{{"frame", 0}}, {{"frame", 48}}})},
         {"rotate_keys", Json::array({{{"frame", 0}, {"rotate", 0}},
                                      {{"frame", 48}, {"rotate", 360}}})},
         {"rate_num", 24}, {"rate_den", 1}});
    CHECK(k.at(24).rotate == 180);
    CHECK(k.at(48).rotate == 360);
  }

  // At the content's own size the canvas is 800 x 600, and the content's
  // centre lands at the canvas centre plus a quarter of the canvas right
  // and a quarter DOWN: (400 + 200, 300 - 150) in Core Image's y-up
  // frame.
  auto p = media::crop_placement(c, 800, 600);
  CHECK(std::abs(p.canvas_w - 800) < 1e-9 && std::abs(p.canvas_h - 600) < 1e-9);
  auto at = [&](double x, double y) {
    const auto& m = p.transform;
    return std::pair{m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty};
  };
  auto [cx, cy] = at(400, 300);
  CHECK(std::abs(cx - 600) < 1e-9);
  CHECK(std::abs(cy - 150) < 1e-9);
  // Turned 90 degrees clockwise on screen, half size: the content's
  // right edge (y up: x = 800, mid-height) ends up BELOW its centre.
  auto [rx, ry] = at(800, 300);
  CHECK(std::abs(rx - 600) < 1e-9);
  CHECK(std::abs(ry - (150 - 200)) < 1e-9);
  // Decoded at half size, the canvas halves and the content lands alike.
  auto half = media::crop_placement(c, 400, 300);
  CHECK(std::abs(half.canvas_w - 400) < 1e-9);
  const auto& h = half.transform;
  CHECK(std::abs(h.a * 200 + h.c * 150 + h.tx - 300) < 1e-9);

  // Each axis on its own: half as wide, twice as tall, then turned.
  media::Crop xy2;
  xy2.content = {800, 600};
  xy2.scale_x = 0.5;
  xy2.scale_y = 2;
  auto q = media::crop_placement(xy2, 800, 600);
  auto put = [&](double x, double y) {
    const auto& m = q.transform;
    return std::pair{m.a * x + m.c * y + m.tx, m.b * x + m.d * y + m.ty};
  };
  CHECK(std::abs(put(800, 300).first - (400 + 200)) < 1e-9);
  CHECK(std::abs(put(400, 600).second - (300 + 600)) < 1e-9);
  xy2.rotate = 90;  // clockwise: the content's top now points right
  q = media::crop_placement(xy2, 800, 600);
  CHECK(std::abs(put(400, 600).first - (400 + 600)) < 1e-9);

  media::Trim t = media::trim_from_json({{"in", 40}, {"out", 12},
                                         {"rate_num", 24}, {"rate_den", 1}});
  CHECK(t.in == 12 && t.out == 40);  // put in order
  CHECK(media::trim_from_json(media::to_json(t)) == t);
  CHECK(media::to_json(media::Trim{}).empty());
}

// A clip's track: keys at frames, linear between, held before the first
// and after the last; a still's flat value reads as one key at frame 0;
// a track survives its JSON.
TEST(media, keyframes_interpolate_linearly)
{
  media::KeyedAdjustments k;
  media::Adjustments a, b;
  a.exposure = 0;
  b.exposure = 1;
  b.contrast = -0.5;
  k.keys = {{10, a}, {30, b}};
  k.rate = Rational{24, 1};
  CHECK(k.at(0).exposure == 0);
  CHECK(std::abs(k.at(20).exposure - 0.5) < 1e-12);
  CHECK(std::abs(k.at(25).contrast + 0.375) < 1e-12);
  CHECK(k.at(99).exposure == 1);
  CHECK(media::keyed_adjustments_from_json(media::to_json(k)) == k);

  const auto flat = media::keyed_adjustments_from_json({{"exposure", 0.4}});
  REQUIRE(flat.keys.size() == 1);
  CHECK(flat.keys[0].frame == 0 && flat.keys[0].value.exposure == 0.4);
  CHECK(media::to_json(media::KeyedAdjustments{}).empty());

  // A clip's crop: placement and turn keyed apart, one background.
  media::KeyedCrop c;
  media::Crop c0, c1;
  c0.content = c1.content = {800, 600};
  c1.scale_x = 0.5;
  c1.scale_y = 0.25;
  c.place.keys = {{0, c0}, {40, c1}};
  c.turn.keys = {{0, {0}}, {20, {20}}};
  c.pad = {0, 0, 1, 1};
  c.rate = c.place.rate = c.turn.rate = Rational{24, 1};
  const media::Crop mid = c.at(10);
  CHECK(std::abs(mid.rotate - 10) < 1e-12);       // the turn's own keys
  CHECK(std::abs(mid.scale_x - 0.875) < 1e-12);   // the placement's
  CHECK(std::abs(mid.scale_y - 0.8125) < 1e-12);
  CHECK(mid.pad[2] == 1);                         // the clip's background
  CHECK(c.at(30).rotate == 20);
  // Identity keys keep their places through JSON.
  const auto back = media::keyed_crop_from_json(media::to_json(c));
  CHECK(back == c);
  // A crop keyed whole (older): each key's rotation is the turn's.
  Json whole = {{"keys", Json::array({{{"frame", 0}, {"content_w", 800}},
                                      {{"frame", 40}, {"rotate", 30},
                                       {"content_w", 800}}})}};
  const auto split = media::keyed_crop_from_json(whole);
  CHECK(split.turn.keys.size() == 2 && split.turn.at(40).degrees == 30);
  CHECK(split.place.keys[1].value.rotate == 0);
}

// How a key goes on to the next: HELD (its value to the next key's frame,
// then the next's), linear, or along a SPLINE -- eased in and out between
// two keys, smooth through a key the values run on past, level where they
// turn, never past either key; kept through JSON on the look's tracks,
// linear on a speed's.
TEST(media, keyframes_hold_or_ease_on)
{
  auto adj = [](double e) {
    media::Adjustments a;
    a.exposure = e;
    return a;
  };
  using media::Interp;
  media::KeyedAdjustments k;
  k.rate = Rational{24, 1};
  k.keys = {{10, adj(0), Interp::Hold}, {30, adj(1)}};
  CHECK(k.at(5).exposure == 0 && k.at(29.9).exposure == 0);
  CHECK(k.at(30).exposure == 1);
  // Two keys alone: an ease in and out, through the middle.
  k.keys[0].interp = Interp::Spline;
  CHECK(std::abs(k.at(20).exposure - 0.5) < 1e-12);
  CHECK(std::abs(k.at(15).exposure - 0.15625) < 1e-12);  // 3t² - 2t³
  CHECK(std::abs(k.at(25).exposure - 0.84375) < 1e-12);
  // Through a key the values run on past: the same slope either side.
  k.keys = {{0, adj(0), Interp::Spline}, {10, adj(1), Interp::Spline},
            {20, adj(2)}};
  auto slope = [&](double f, double d) {
    return (k.at(f + d).exposure - k.at(f).exposure) / d;
  };
  CHECK(std::abs(k.at(10).exposure - 1) < 1e-12);
  CHECK(std::abs(slope(10 - 1e-6, 1e-6) - slope(10, 1e-6)) < 1e-4);
  CHECK(std::abs(k.at(5).exposure - 0.375) < 1e-12);
  // Where they turn: level, never above the peak; a near-level run never
  // past its keys.
  k.keys = {{0, adj(0), Interp::Spline}, {10, adj(1), Interp::Spline},
            {20, adj(0)}};
  for (double f = 0; f <= 20; f += 0.25) {
    CHECK(k.at(f).exposure <= 1 + 1e-12 && k.at(f).exposure >= -1e-12);
  }
  k.keys = {{0, adj(0), Interp::Spline}, {10, adj(1), Interp::Spline},
            {20, adj(1.05)}};
  for (double f = 10; f <= 20; f += 0.25) {
    CHECK(k.at(f).exposure >= 1 - 1e-12 && k.at(f).exposure <= 1.05 + 1e-12);
  }
  // Next to a held key the curve starts level: as two keys alone.
  k.keys = {{0, adj(0.8), Interp::Hold}, {10, adj(0), Interp::Spline},
            {20, adj(1)}};
  CHECK(std::abs(k.at(15).exposure - 0.5) < 1e-12);
  CHECK(std::abs(k.at(12.5).exposure - 0.15625) < 1e-12);
  // A key made inside a span takes its way on.
  CHECK(k.interp_at(5) == Interp::Hold && k.interp_at(10) == Interp::Spline);
  CHECK(k.interp_at(-1) == Interp::Linear);

  // JSON: written unless linear, read back.
  const Json j = media::to_json(k);
  CHECK(j["keys"][0]["interp"] == "hold");
  CHECK(j["keys"][1]["interp"] == "spline");
  CHECK(!j["keys"][2].contains("interp"));
  CHECK(media::keyed_adjustments_from_json(j) == k);
  // A crop's two tracks, each its own; one keyed whole: the turn the
  // placement's.
  media::KeyedCrop c;
  media::Crop c0, c1;
  c0.content = c1.content = {800, 600};
  c1.scale_x = 0.5;
  c.place.keys = {{0, c0, Interp::Spline}, {40, c1}};
  c.turn.keys = {{0, {0}, Interp::Hold}, {20, {20}}};
  c.rate = c.place.rate = c.turn.rate = Rational{24, 1};
  CHECK(media::keyed_crop_from_json(media::to_json(c)) == c);
  CHECK(c.at(10).rotate == 0 && c.at(20).rotate == 20);
  CHECK(std::abs(c.at(20).scale_x - 0.75) < 1e-12);
  CHECK(c.at(10).scale_x > 0.875);  // eased: slower out of the first key
  const Json whole = {
      {"keys", Json::array({{{"frame", 0}, {"content_w", 800},
                             {"interp", "hold"}},
                            {{"frame", 40}, {"rotate", 30},
                             {"content_w", 800}}})}};
  const auto split = media::keyed_crop_from_json(whole);
  CHECK(split.turn.keys[0].interp == Interp::Hold);
  CHECK(split.turn.at(39).degrees == 0);
  // A speed's (and a sound's) keys are linear whatever they say.
  const auto sp = media::keyed_speed_from_json(
      {{"keys", Json::array({{{"frame", 0}, {"rate", 1},
                              {"interp", "hold"}},
                             {{"frame", 10}, {"rate", 2}}})}});
  CHECK(sp.keys[0].interp == Interp::Linear);
  CHECK(std::abs(sp.at(5).rate - 1.5) < 1e-12);
}

// A clip's frames, each with its look at its own frame: as many frames
// as asked for, from where it is asked, at the size asked.
TEST(media, a_clip_decodes_with_its_look)
{
  const char* mov = std::getenv("VALTZ_TEST_MOVIE");
  if (!mov) {
    SKIP("set VALTZ_TEST_MOVIE to a video file");
  }
  auto info = media::probe_file(mov);
  REQUIRE_OK(info);
  const Rational rate = info->frame_rate;
  REQUIRE(rate.num > 0);
  media::KeyedAdjustments adj;
  media::Adjustments bright;
  bright.exposure = 1;
  adj.keys = {{0, {}}, {10, bright}};
  const media::PixelSize size{64, 48};
  std::vector<_Float16> buf(3 * 64 * 48);
  std::vector<std::int64_t> frames;
  std::vector<double> means;
  REQUIRE_OK(media::decode_movie(
      mov, adj, {}, rate, 2, 4, size,
      [&](std::int64_t) -> Result<media::FrameBuffer> {
        return media::FrameBuffer{buf.data(), buf.size() * 2, {}};
      },
      [&](std::int64_t f) -> Status {
        frames.push_back(f);
        double sum = 0;
        for (auto v : buf) { sum += static_cast<double>(v); }
        means.push_back(sum / static_cast<double>(buf.size()));
        return ok_status();
      }));
  CHECK((frames == std::vector<std::int64_t>{2, 3, 4, 5}));
  // The exposure ramps up from frame 0: later frames come out brighter.
  REQUIRE(means.size() == 4);
  CHECK(means[3] > means[0]);
}

// A layer faded by its opacity -- its alpha keyed 0 to 1: a fade in from
// black -- is read for an export on black: half way, between black and
// the picture. It was read as planar RGB off a clear frame, its colour
// un-premultiplied: whole at any opacity above 0, black only at 0.
TEST(media, a_faded_layer_is_read_on_black)
{
  auto dir = test::temp_dir("media-fade");
  REQUIRE(write_png_grey(dir / "grey.png", 200));
  media::MovieStack stack;
  stack.rate = {10, 1};
  stack.frames = 11;
  stack.canvas.frame_w = 64;
  stack.canvas.frame_h = 36;
  media::MovieLayer l;
  l.file = dir / "grey.png";
  l.timing.start = 0;
  l.timing.length = 1.1;
  l.timing.rate = {10, 1};
  l.adjust.rate = {10, 1};
  media::Adjustments clear;
  clear.alpha = 0;
  l.adjust.keys = {{0, clear}, {10, media::Adjustments{}}};
  stack.layers.push_back(l);
  const media::PixelSize size{64, 36};
  std::vector<_Float16> buf(3 * 64 * 36);
  std::vector<double> means;
  REQUIRE_OK(media::decode_movie_stack(
      stack, {10, 1}, 0, 11, size,
      [&](std::int64_t) -> Result<media::FrameBuffer> {
        return media::FrameBuffer{buf.data(), buf.size() * 2, {}};
      },
      [&](std::int64_t) -> Status {
        double sum = 0;
        for (auto v : buf) {
          sum += static_cast<double>(v);
        }
        means.push_back(sum / static_cast<double>(buf.size()));
        return ok_status();
      }));
  REQUIRE(means.size() == 11);
  CHECK(means[0] < 0.01);
  CHECK(means[10] > 0.4);
  CHECK(means[2] < means[5]);
  CHECK(means[5] < means[8]);
  CHECK(means[5] < 0.9 * means[10]);
  CHECK(means[5] > 0.2 * means[10]);
}

// A project exported in BT.709 writes a 709 clip as it was read: its
// frames drawn into the space Core Video reads 709 video in. The named
// BT.709 space's curve is another, and lifted every mid-grey of a
// project's export by about a tenth.
TEST(media, a_709_export_keeps_a_709_clips_colour)
{
  auto dir = test::temp_dir("media-709");
  const auto png = dir / "grey.png";
  REQUIRE(write_png_grey(png, 128));
  // A short 709 clip of a mid-grey (encode_movie_stack: ProRes, 709).
  media::MovieStack still;
  still.canvas.frame_w = 64;
  still.canvas.frame_h = 36;
  still.frames = 4;
  media::MovieLayer pic;
  pic.file = png;
  still.layers.push_back(pic);
  const std::filesystem::path mov = dir / "grey.mov";
  REQUIRE_OK(media::encode_movie_stack(still, {24, 1}, {64, 36}, {}, mov));

  media::MovieStack stack;
  media::MovieLayer clip;
  clip.file = mov;
  clip.video = true;
  stack.layers.push_back(clip);
  std::vector<_Float16> buf(3 * 64 * 36);
  const auto target = [&](std::int64_t) -> Result<media::FrameBuffer> {
    return media::FrameBuffer{buf.data(), buf.size() * 2, {}};
  };
  auto mean_of = [&](const media::ColorInfo* color) -> double {
    double m = -1;
    auto st = media::decode_movie_stack(
        stack, {24, 1}, 1, 1, {64, 36}, target,
        [&](std::int64_t) -> Status {
          double sum = 0;
          for (auto v : buf) { sum += static_cast<double>(v); }
          m = sum / static_cast<double>(buf.size());
          return ok_status();
        },
        nullptr, color);
    CHECK(st.ok());
    return m;
  };
  const double own = mean_of(nullptr);
  const auto rec709 = media::output_color("rec709");
  REQUIRE(rec709.has_value());
  const double out = mean_of(&*rec709);
  REQUIRE(own > 0.2 && own < 0.8);
  CHECK(std::abs(out - own) < 0.004);
}

// A 709 clip drawn into a movie again -- a timeline showing it, a
// continuation's guide, a timeline in a timeline -- keeps its colour.
// Drawn into the named BT.709 space, each pass lifted it, the shadows
// most: a guide two passes deep reached the model about 30 codes in 255
// lighter, and the clip continued from it opened washed out.
TEST(media, a_709_clip_drawn_again_keeps_its_colour)
{
  auto dir = test::temp_dir("media-709-again");
  const auto rec709 = media::output_color("rec709");
  REQUIRE(rec709.has_value());
  std::vector<_Float16> buf(3 * 64 * 36);
  const auto target = [&](std::int64_t) -> Result<media::FrameBuffer> {
    return media::FrameBuffer{buf.data(), buf.size() * 2, {}};
  };
  // A clip's first frame as a model reads it: its codes, in 0..1.
  auto mean_of = [&](const std::filesystem::path& mov) -> double {
    media::MovieStack stack;
    media::MovieLayer clip;
    clip.file = mov;
    clip.video = true;
    stack.layers.push_back(clip);
    double m = -1;
    auto st = media::decode_movie_stack(
        stack, {24, 1}, 0, 1, {64, 36}, target,
        [&](std::int64_t) -> Status {
          double sum = 0;
          for (auto v : buf) { sum += static_cast<double>(v); }
          m = sum / static_cast<double>(buf.size());
          return ok_status();
        },
        nullptr, &*rec709);
    CHECK(st.ok());
    return m;
  };
  // The clip as a layer of a timeline, written out.
  auto again = [&](const std::filesystem::path& src,
                   const std::filesystem::path& out) {
    media::MovieStack stack;
    media::MovieLayer clip;
    clip.file = src;
    clip.video = true;
    stack.layers.push_back(clip);
    return media::encode_movie_stack(stack, {24, 1}, {64, 36}, {}, out);
  };
  for (const int grey : {40, 128}) {
    const std::string g = std::to_string(grey);
    const auto png = dir / ("grey-" + g + ".png");
    REQUIRE(write_png_grey(png, static_cast<std::uint8_t>(grey)));
    media::MovieStack still;
    still.canvas.frame_w = 64;
    still.canvas.frame_h = 36;
    still.frames = 4;
    media::MovieLayer pic;
    pic.file = png;
    still.layers.push_back(pic);
    const auto once = dir / ("once-" + g + ".mov");
    const auto twice = dir / ("twice-" + g + ".mov");
    const auto thrice = dir / ("thrice-" + g + ".mov");
    REQUIRE_OK(media::encode_movie_stack(still, {24, 1}, {64, 36}, {},
                                         once));
    REQUIRE_OK(again(once, twice));
    REQUIRE_OK(again(twice, thrice));
    const double a = mean_of(once), b = mean_of(twice),
                 c = mean_of(thrice);
    REQUIRE(a > 0.05 && a < 0.8);
    CHECK(std::abs(b - a) < 0.004);
    CHECK(std::abs(c - a) < 0.004);
  }
}

// A clip's late frames are read from where they are -- not decoded from
// the clip's start (an hour's last minute decodes a minute) -- and drawn
// the same; a writer's preview comes small, twice as often as asked.
TEST(media, a_stack_reads_from_where_it_starts)
{
  const char* mov = std::getenv("VALTZ_TEST_MOVIE");
  if (!mov) {
    SKIP("set VALTZ_TEST_MOVIE to a video file");
  }
  auto info = media::probe_file(mov);
  REQUIRE_OK(info);
  const Rational rate = info->frame_rate;
  const std::int64_t n = info->frame_count;
  REQUIRE(rate.num > 0 && n > 12);
  const media::PixelSize size{64, 36};
  std::vector<_Float16> buf(3 * 64 * 36);
  const auto mean = [&] {
    double sum = 0;
    for (auto v : buf) { sum += static_cast<double>(v); }
    return sum / static_cast<double>(buf.size());
  };
  const auto target = [&](std::int64_t) -> Result<media::FrameBuffer> {
    return media::FrameBuffer{buf.data(), buf.size() * 2, {}};
  };
  // The clip alone, its last three frames.
  std::vector<double> plain;
  REQUIRE_OK(media::decode_movie(
      mov, {}, {}, rate, n - 3, 3, size, target,
      [&](std::int64_t) -> Status {
        plain.push_back(mean());
        return ok_status();
      }));
  // The same three as a stack (one layer): read from there.
  media::MovieStack stack;
  media::MovieLayer clip;
  clip.file = mov;
  clip.video = true;
  stack.layers.push_back(clip);
  std::vector<std::int64_t> frames;
  std::vector<double> stacked;
  std::vector<media::PreviewPicture> previews;
  media::FramePreview preview;
  preview.edge = 32;
  preview.wants = [](std::int64_t f) { return f % 2 == 0; };
  preview.take = [&](media::PreviewPicture&& p) {
    previews.push_back(std::move(p));
  };
  REQUIRE_OK(media::decode_movie_stack(
      stack, rate, n - 3, 3, size, target,
      [&](std::int64_t f) -> Status {
        frames.push_back(f);
        stacked.push_back(mean());
        return ok_status();
      },
      &preview));
  CHECK((frames == std::vector<std::int64_t>{n - 3, n - 2, n - 1}));
  REQUIRE(stacked.size() == 3 && plain.size() == 3);
  for (std::size_t i = 0; i < 3; ++i) {
    CHECK(std::abs(stacked[i] - plain[i]) < 0.01);
  }
  // Previews of the even frames only, at most 32 px, three planes.
  REQUIRE(!previews.empty());
  for (const auto& p : previews) {
    CHECK(p.frame % 2 == 0);
    CHECK(std::max(p.size.width, p.size.height) <= 32);
    CHECK(p.rgb.size() == 3u * static_cast<std::size_t>(p.size.width) *
                              static_cast<std::size_t>(p.size.height));
  }
  // A plain export's preview: its frame there, read by itself.
  auto pic = media::preview_movie_frame(
      mov, static_cast<double>(n - 3) / rate.to_double(), 48);
  REQUIRE_OK(pic);
  CHECK(std::max(pic->size.width, pic->size.height) <= 48);
  CHECK(pic->rgb.size() == 3u * static_cast<std::size_t>(pic->size.width) *
                               static_cast<std::size_t>(pic->size.height));
}

namespace {

// A 16-bit PCM WAV, `channels` interleaved, sample i of each channel
// `i % 30000` (a ramp: where a cut lands can be read off its first one).
bool
write_ramp_wav(const std::filesystem::path& path, int rate, int frames,
               int channels)
{
  std::ofstream f(path, std::ios::binary);
  auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
  auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
  const std::uint32_t data = static_cast<std::uint32_t>(frames) *
                             static_cast<std::uint32_t>(channels) * 2;
  f.write("RIFF", 4);
  u32(36 + data);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(static_cast<std::uint16_t>(channels));
  u32(static_cast<std::uint32_t>(rate));
  u32(static_cast<std::uint32_t>(rate * channels * 2));
  u16(static_cast<std::uint16_t>(channels * 2));
  u16(16);
  f.write("data", 4);
  u32(data);
  for (int i = 0; i < frames; ++i) {
    for (int c = 0; c < channels; ++c) {
      u16(static_cast<std::uint16_t>(i % 30000));
    }
  }
  return static_cast<bool>(f);
}

// A WAV's 16-bit samples (its "data" chunk, whatever chunks come before
// it) and its channel count.
std::vector<std::int16_t>
read_wav16(const std::filesystem::path& path, int* channels)
{
  std::ifstream f(path, std::ios::binary);
  std::vector<char> b((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  std::vector<std::int16_t> out;
  std::size_t at = 12;
  while (at + 8 <= b.size()) {
    std::uint32_t len = 0;
    std::memcpy(&len, &b[at + 4], 4);
    const std::string id(&b[at], 4);
    if (id == "fmt ") {
      std::uint16_t ch = 0;
      std::memcpy(&ch, &b[at + 10], 2);
      *channels = ch;
    } else if (id == "data") {
      const std::size_t n = std::min<std::size_t>(len, b.size() - at - 8);
      out.resize(n / 2);
      std::memcpy(out.data(), &b[at + 8], out.size() * 2);
      break;
    }
    at += 8 + len + (len & 1);
  }
  return out;
}

}

// A sound written out -- a song saved, trimmed -- is cut to the sample:
// from the mark-in's sample, exactly as many as the span holds, as deep
// as it was; and as AAC.
TEST(media, a_sound_is_cut_to_the_sample)
{
  const auto dir = test::temp_dir("sound-cut");
  const auto src = dir / "ramp.wav";
  REQUIRE(write_ramp_wav(src, 48000, 48000, 2));
  // 1.25 ms in, 12.5 ms long: 60 samples in, 600 kept.
  const auto cut = dir / "cut.wav";
  REQUIRE_OK(media::write_sound(src, cut, "wav", 60.0 / 48000,
                                600.0 / 48000));
  int ch = 0;
  const auto s = read_wav16(cut, &ch);
  CHECK(ch == 2);
  REQUIRE(s.size() == 600u * 2);
  CHECK(s[0] == 60 && s[1] == 60);
  CHECK(s[s.size() - 1] == 659);
  // Untrimmed: all of it.
  const auto whole = dir / "whole.wav";
  REQUIRE_OK(media::write_sound(src, whole, "wav"));
  CHECK(read_wav16(whole, &ch).size() == 48000u * 2);
  // AAC: an .m4a that reads back as the span's length.
  const auto aac = dir / "cut.m4a";
  REQUIRE_OK(media::write_sound(src, aac, "m4a", 0.25, 0.5));
  auto info = media::probe_file(aac);
  REQUIRE_OK(info);
  CHECK(info->type == media::MediaType::Audio);
  CHECK(std::abs(info->duration.seconds() - 0.5) < 0.03);
  CHECK(!media::write_sound(src, dir / "x.mp3", "mp3").ok());
}

namespace {

// The first ProRes frame's alpha_info -- 0 none, 1 8-bit, 2 16-bit --
// as a decoder reads it, after its 'icpf' (byte 17 of the frame header,
// FFmpeg's proresdec); -1 with no frame found.
int
prores_alpha_info(const std::filesystem::path& path)
{
  std::ifstream f(path, std::ios::binary);
  const std::string d((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
  const std::size_t i = d.find("icpf");
  if (i == std::string::npos || i + 4 + 18 > d.size()) {
    return -1;
  }
  return static_cast<unsigned char>(d[i + 4 + 17]) & 0xf;
}

}

// A ProRes INTERMEDIATE (OutputSettings::intermediate): a stack drawn in
// the flavour asked -- 422 HQ for one that is not ProRes -- with no
// alpha plane (drawn over black: a 4444 coded a plane of 1s before), and
// a sound joined as 24-bit PCM, at ANY rate, as a whole and as a span
// (AAC's would be moved to one it encodes: 37.8 kHz to 44.1); a
// rendering with its sound the same.
TEST(media, a_prores_intermediate_keeps_its_flavour_and_pcm_sound)
{
  const auto dir = test::temp_dir("intermediate");
  const auto png = dir / "grey.png";
  REQUIRE(write_png_grey(png, 128));
  media::MovieStack still;
  still.canvas.frame_w = 64;
  still.canvas.frame_h = 48;
  still.frames = 48;
  media::MovieLayer pic;
  pic.file = png;
  still.layers.push_back(pic);
  const std::pair<std::string_view, std::string_view> flavours[] = {
      {"prores4444xq", "ap4x"}, {"prores4444", "ap4h"},
      {"prores422hq", "apch"}, {"prores422", "apcn"}, {"hevc", "apch"}};
  for (const auto& [f, fourcc] : flavours) {
    const auto mov = dir / std::format("{}.mov", f);
    REQUIRE_OK(media::encode_movie_stack(still, {24, 1}, {64, 48}, {}, mov,
                                         f));
    auto r = media::probe_file(mov);
    REQUIRE_OK(r);
    CHECK(r->codec == fourcc);
    CHECK(prores_alpha_info(mov) == 0);
  }
  const auto wav = dir / "s37800.wav";
  REQUIRE(write_ramp_wav(wav, 37800, 37800 * 2, 2));
  const auto mov = dir / "prores4444.mov";
  const auto whole = dir / "whole.mov";
  REQUIRE_OK(media::add_soundtrack(mov, wav, whole, 0, 0,
                                   media::SoundCodec::Pcm));
  auto w = media::probe_file(whole);
  REQUIRE_OK(w);
  CHECK(w->has_audio);
  CHECK(w->audio_sample_rate == 37800);
  CHECK(w->audio_channels == 2);
  CHECK(w->codec == "ap4h");
  CHECK(std::abs(w->duration.seconds() - 2.0) < 0.1);
  const auto part = dir / "part.mov";
  REQUIRE_OK(media::add_soundtrack(mov, wav, part, 0.5, 1.0,
                                   media::SoundCodec::Pcm));
  auto p = media::probe_file(part);
  REQUIRE_OK(p);
  CHECK(p->audio_sample_rate == 37800);
  const auto with = dir / "rendered.mov";
  REQUIRE_OK(media::encode_movie_stack(still, {24, 1}, {64, 48}, wav, with,
                                       "prores4444"));
  auto r = media::probe_file(with);
  REQUIRE_OK(r);
  CHECK(r->codec == "ap4h");
  CHECK(r->audio_sample_rate == 37800);
}

// A sound AAC cannot carry as first asked: 8 kHz mono -- a camcorder's
// AVI, upscaled -- joined to the movie made of it. 96 kb/s a channel is
// more than AAC holds at 8 kHz, and the writer refused the whole movie
// ("Cannot Encode Media"). Each rate is encoded as the encoder allows,
// at the sound's own rate where it takes it (37.8 kHz it does not: the
// next it does), as a whole and as a span; an .m4a the same.
TEST(media, a_low_rate_sound_is_encoded_as_aac_allows)
{
  const auto dir = test::temp_dir("low-rate-sound");
  const auto png = dir / "grey.png";
  REQUIRE(write_png_grey(png, 128));
  media::MovieStack still;
  still.canvas.frame_w = 64;
  still.canvas.frame_h = 48;
  still.frames = 48;
  media::MovieLayer pic;
  pic.file = png;
  still.layers.push_back(pic);
  const auto mov = dir / "picture.mov";
  REQUIRE_OK(media::encode_movie_stack(still, {24, 1}, {64, 48}, {}, mov));
  for (const int rate : {8000, 11025, 37800, 96000}) {
    for (const int ch : {1, 2}) {
      const auto wav = dir / std::format("s{}-{}.wav", rate, ch);
      REQUIRE(write_ramp_wav(wav, rate, rate * 2, ch));
      const auto out = dir / std::format("j{}-{}.mov", rate, ch);
      REQUIRE_OK(media::add_soundtrack(mov, wav, out));
      auto r = media::probe_file(out);
      REQUIRE_OK(r);
      CHECK(r->has_audio);
      CHECK(r->audio_channels == ch);
      CHECK(r->audio_sample_rate == (rate == 37800   ? 44100
                                     : rate > 48000 ? 48000
                                                    : rate));
      CHECK(std::abs(r->duration.seconds() - 2.0) < 0.1);
      const auto part = dir / std::format("p{}-{}.mov", rate, ch);
      REQUIRE_OK(media::add_soundtrack(mov, wav, part, 0.5, 1.0));
      const auto m4a = dir / std::format("s{}-{}.m4a", rate, ch);
      REQUIRE_OK(media::write_sound(wav, m4a, "m4a"));
      auto a = media::probe_file(m4a);
      REQUIRE_OK(a);
      CHECK(std::abs(a->duration.seconds() - 2.0) < 0.1);
    }
  }
}

// A RefMod (DESIGN §4s): made as the pack makes one -- written as version
// 5, its visual sources stacked, each sound a member -- read back by its
// tensors, a member converted to vpipe's tensor file as it lays latents
// out; told apart from weights; the pack's pooling, grid and budget.
TEST(media, a_refmod_is_made_read_and_told_from_weights)
{
  using namespace valtz::media;
  const auto dir = test::temp_dir("refmod");
  auto latent = [](std::vector<std::int64_t> shape, float k) {
    Latent z;
    z.shape = std::move(shape);
    z.data.resize(z.count());
    for (std::size_t i = 0; i < z.data.size(); ++i) {
      z.data[i] = std::sin(k * static_cast<float>(i)) * 1.5f;
    }
    return z;
  };
  // Full Reference: two pictures on one 8 x 12 canvas, a sound.
  RefModRecipe r;
  r.name = "hero";
  r.compressed = false;
  r.max_tokens = 0;
  r.description = "a red fox";
  const std::vector<RefModSource> pics = {
      {latent({24, 1, 8, 12}, 0.01f), false},
      {latent({24, 1, 8, 12}, 0.02f), false}};
  const Latent sound = latent({2, 32, 40}, 0.03f);
  const auto file = dir / "hero.safetensors";
  auto made = make_refmod(r, pics, {sound}, file);
  REQUIRE_OK(made);
  CHECK(made->version == 5 && made->name == "hero");
  REQUIRE(made->members.size() == 2);
  const auto& v = made->members[0];
  CHECK(v.kind == "video" && v.tensor == "ref_0" && v.dtype == "F16");
  CHECK(v.frames() == 2 && v.height() == 8 && v.width() == 12);
  CHECK(v.tokens() == 2 * 4 * 6);
  CHECK(jget<std::string>(v.meta, "mode", "") == "encode");
  CHECK(jget<std::string>(v.meta, "source", "") == "stack");
  const auto& a = made->members[1];
  CHECK(a.kind == "audio" && a.tensor == "ref_1" && a.frames() == 40);
  CHECK(std::abs(a.seconds() - 1.0) < 1e-9 && a.tokens() == 80);
  CHECK(is_refmod(file));

  // The stacked latent, half-rounded, frame by frame.
  auto back = read_member(file, v);
  REQUIRE_OK(back);
  CHECK((back->shape == std::vector<std::int64_t>{24, 2, 8, 12}));
  CHECK(back->data[0] == refmod::half(pics[0].z.data[0]));
  CHECK(back->data[96] == refmod::half(pics[1].z.data[0]));   // frame 1
  CHECK(back->data[2 * 96] == refmod::half(pics[0].z.data[96]));  // ch 1
  // A sound comes back as vpipe lays it: [2, 32, T], the pair outer.
  auto sb = read_member(file, a);
  REQUIRE_OK(sb);
  CHECK((sb->shape == std::vector<std::int64_t>{2, 32, 40}));
  CHECK(sb->data[32 * 40 + 5] == refmod::half(sound.data[32 * 40 + 5]));

  // vpipe's tensor file, there and back.
  const auto vpt = dir / "ref0.vpt";
  REQUIRE_OK(write_vptensor(vpt, *back, Json{{"latent", true}}));
  auto vb = read_vptensor(vpt);
  REQUIRE_OK(vb);
  CHECK(vb->shape == back->shape && vb->data == back->data);

  // Weights are not one, whatever their name: a LoRA's tensors.
  const auto lora = dir / "refmod-lookalike.safetensors";
  {
    const std::string head = R"({"lora_unet_a.lora_down.weight":)"
        R"({"dtype":"F16","shape":[2,2],"data_offsets":[0,8]}})";
    std::ofstream f(lora, std::ios::binary);
    const std::uint64_t n = head.size();
    f.write(reinterpret_cast<const char*>(&n), 8);
    f << head << std::string(8, '\0');
  }
  CHECK(!is_refmod(lora));
  CHECK(read_refmod(lora).error().code == Code::InvalidArgument);

  // Compressed Reference: pooled to the first source's aspect, no
  // refinement asked; a clip's frames pooled to `latent_frames`.
  r.compressed = true;
  r.refine_steps = 0;
  r.pool_h = r.pool_w = 4;
  r.latent_frames = 2;
  const auto small = dir / "small.safetensors";
  auto c = make_refmod(r, {{latent({24, 5, 8, 12}, 0.04f), true}}, {},
                       small);
  REQUIRE_OK(c);
  REQUIRE(c->members.size() == 1);
  CHECK(c->members[0].kind == "video");
  CHECK(c->members[0].frames() == 2 && c->members[0].height() == 2 &&
        c->members[0].width() == 4);   // 4 x (4 / 1.5 = 2.7 -> 2)
  CHECK(jget<std::string>(c->members[0].meta, "mode", "") == "training");

  // The pack's grid, pooling and budget.
  CHECK((refmod::aspect_grid(16, 16, 1.5) == std::pair{16, 10}));
  CHECK((refmod::aspect_grid(16, 16, 0.75) == std::pair{12, 16}));
  CHECK((refmod::aspect_grid(16, 16, 1.0) == std::pair{16, 16}));
  Latent ramp;
  ramp.shape = {1, 1, 2, 4};
  ramp.data = {1, 2, 3, 4, 5, 6, 7, 8};
  const Latent p = refmod::pool(ramp, 1, 1, 2);
  CHECK((p.data == std::vector<float>{3.5f, 5.5f}));
  // An enlargement of what is already the size is itself.
  CHECK(refmod::enlarge(ramp, 1, 2, 4).data == ramp.data);
  // Ten identical frames and one other: the budget keeps the two.
  Latent same;
  same.shape = {24, 11, 4, 4};
  same.data.assign(same.count(), 1.0f);
  for (int ch = 0; ch < 24; ++ch) {
    for (int i = 0; i < 16; ++i) {
      same.data[(static_cast<std::size_t>(ch) * 11 + 10) * 16 + i] = -1.0f;
    }
  }
  CHECK((refmod::distinct_frames(same) == std::vector<int>{0, 10}));
  auto fit = refmod::fit_tokens(same, 8);   // 4 a frame: 2 fit
  REQUIRE_OK(fit);
  CHECK(fit->shape[1] == 2);
  CHECK(refmod::fit_tokens(same, 3).error().code == Code::Unsupported);
  // Refinement brings a pooled latent's enlargement nearer the full one.
  const Latent full = latent({2, 3, 8, 8}, 0.37f);
  const Latent coarse = refmod::pool(full, 2, 2, 2);
  auto err = [&](const Latent& s) {
    const Latent up = refmod::enlarge(s, 3, 8, 8);
    double e = 0;
    for (std::size_t i = 0; i < up.data.size(); ++i) {
      e += (up.data[i] - full.data[i]) * (up.data[i] - full.data[i]);
    }
    return e;
  };
  CHECK(err(refmod::refine(coarse, full, 100)) < err(coarse));
}

// The pack's arithmetic against PyTorch's own (VALTZ_TEST_REFMOD_GOLDEN:
// a folder of full / pooled / up / small / refined .vpt files, made by
// adaptive_avg_pool3d, trilinear interpolate and 50 steps of Adam at 0.02
// on the half-rounded pool -- the pack's optimize_latent).
TEST(media, a_refmods_arithmetic_is_pytorchs)
{
  using namespace valtz::media;
  const char* dir = std::getenv("VALTZ_TEST_REFMOD_GOLDEN");
  if (!dir || !*dir) {
    SKIP("VALTZ_TEST_REFMOD_GOLDEN not set");
  }
  auto load = [&](const char* n) -> Latent {
    auto t = read_vptensor(std::filesystem::path(dir) / (std::string(n) +
                                                         ".vpt"));
    CHECK(t.ok());
    return t.ok() ? *t : Latent{};
  };
  const Latent full = load("full"), pooled = load("pooled"), up = load("up"),
               small = load("small"), refined = load("refined");
  auto worst = [](const Latent& a, const Latent& b) {
    double w = 0;
    for (std::size_t i = 0; i < a.data.size(); ++i) {
      w = std::max(w, static_cast<double>(std::fabs(a.data[i] -
                                                    b.data[i])));
    }
    return a.shape == b.shape ? w : 1e9;
  };
  const Latent p = refmod::pool(full, 2, 6, 10);
  CHECK(worst(p, pooled) < 1e-5);
  CHECK(worst(refmod::enlarge(pooled, 5, 16, 24), up) < 1e-5);
  Latent h = pooled;
  for (auto& v : h.data) { v = refmod::half(v); }
  CHECK(worst(h, small) == 0.0);
  const Latent r = refmod::refine(small, full, 50);
  std::fprintf(stderr, "refine against PyTorch: worst %.3g\n",
               worst(r, refined));
  CHECK(worst(r, refined) < 1e-4);
}


// A REVERB's response (media/reverb.h): its pre-delay silent, its energy
// 1, fading to -60 dB at its decay (Schroeder's backward integral, the
// slope between -5 and -35 dB), its highs dying sooner with damping, its
// echoes sparse at first in a large room and dense in a small one; the
// same room the same echoes, its two channels apart.
TEST(media, a_reverb_response_rings_as_set)
{
  using media::ReverbVoice;
  const int fs = 48000;
  const ReverbVoice v{1.2, 0.0, 0.05};
  const auto h = media::reverb_impulse(v, 0.5, 0, fs);
  REQUIRE(h.size() == static_cast<std::size_t>(std::ceil(1.25 * fs)));
  double e = 0, before = 0;
  for (std::size_t i = 0; i < h.size(); ++i) {
    e += double(h[i]) * h[i];
    if (i < static_cast<std::size_t>(0.05 * fs)) {
      before += std::abs(h[i]);
    }
  }
  CHECK(std::abs(e - 1) < 1e-4);
  CHECK(before == 0);
  // RT60 by the slope of the decay curve.
  auto rt60 = [&](const std::vector<float>& r) {
    std::vector<double> edc(r.size() + 1, 0);
    for (std::size_t i = r.size(); i-- > 0;) {
      edc[i] = edc[i + 1] + double(r[i]) * r[i];
    }
    auto at = [&](double db) {
      for (std::size_t i = 0; i < r.size(); ++i) {
        if (10 * std::log10(std::max(1e-30, edc[i] / edc[0])) <= db) {
          return static_cast<double>(i) / fs;
        }
      }
      return static_cast<double>(r.size()) / fs;
    };
    return 2 * (at(-35) - at(-5));
  };
  const double measured = rt60(h);
  const double longer = rt60(media::reverb_impulse({3.0, 0.0, 0.05}, 0.5,
                                                   0, fs));
  std::printf("[media] reverb: RT60 set 1.2 s -> %.2f s, 3.0 s -> %.2f s\n",
              measured, longer);
  CHECK(std::abs(measured - 1.2) < 0.15);
  CHECK(std::abs(longer - 3.0) < 0.35);
  // Damping: the late response's highs (its sample-to-sample steps)
  // against its lows, darker with damping.
  auto brightness = [&](const std::vector<float>& r) {
    double hi = 0, lo = 0;
    for (std::size_t i = r.size() / 2; i + 1 < r.size(); ++i) {
      hi += double(r[i + 1] - r[i]) * (r[i + 1] - r[i]);
      lo += double(r[i]) * r[i];
    }
    return hi / std::max(1e-30, lo);
  };
  const double bright = brightness(h);
  const double dark =
      brightness(media::reverb_impulse({1.2, 1.0, 0.05}, 0.5, 0, fs));
  std::printf("[media] reverb: late brightness %.4f undamped, %.4f damped\n",
              bright, dark);
  CHECK(dark < bright * 0.1);
  // Size: the first 30 ms's crest (peak over rms) -- sparse echoes stand
  // out of a large room's, a small room's are already dense.
  auto crest = [&](double size) {
    const auto r = media::reverb_impulse({1.2, 0.0, 0.05}, size, 0, fs);
    double peak = 0, sum = 0;
    const auto a = static_cast<std::size_t>(0.05 * fs);
    const auto b = static_cast<std::size_t>(0.08 * fs);
    for (std::size_t i = a; i < b; ++i) {
      peak = std::max(peak, double(std::abs(r[i])));
      sum += double(r[i]) * r[i];
    }
    return peak / std::sqrt(sum / static_cast<double>(b - a));
  };
  const double small = crest(0), large = crest(1);
  std::printf("[media] reverb: early crest %.1f small room, %.1f large\n",
              small, large);
  CHECK(large > 2 * small);
  // The same room, the same echoes; the right channel apart.
  CHECK(media::reverb_impulse(v, 0.5, 0, fs) == h);
  const auto right = media::reverb_impulse(v, 0.5, 1, fs);
  double c = 0;
  for (std::size_t i = 0; i < h.size(); ++i) {
    c += double(h[i]) * right[i];
  }
  CHECK(std::abs(c) < 0.1);
}

// The CONVOLUTION (media::convolve_reverb): partitioned and summed on the
// GPU as on the CPU, and either the same as a direct convolution; a send
// hearing two responses by its picks hears each block's mix.
TEST(media, a_reverb_is_convolved_on_the_gpu_as_on_the_cpu)
{
  const int fs = 48000;
  std::uint64_t seed = 7;
  auto noise = [&] {
    seed = seed * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<float>(static_cast<double>(seed >> 40) /
                                  static_cast<double>(1ull << 24) -
                              0.5);
  };
  // Direct: a short send, one response.
  std::vector<float> send(static_cast<std::size_t>(0.7 * fs));
  for (auto& x : send) {
    x = noise();
  }
  const media::ReverbVoice v{0.3, 0.4, 0.01};
  std::array<std::vector<float>, 2> r{media::reverb_impulse(v, 0.4, 0, fs),
                                      media::reverb_impulse(v, 0.4, 1, fs)};
  const std::size_t out_frames = send.size() + r[0].size();
  media::ReverbStats gs, cs;
  auto gpu = media::convolve_reverb(send.data(), send.size(), {r}, {},
                                    out_frames, true, &gs);
  auto cpu = media::convolve_reverb(send.data(), send.size(), {r}, {},
                                    out_frames, false, &cs);
  REQUIRE_OK(gpu);
  REQUIRE_OK(cpu);
  REQUIRE((*gpu)[0].size() == out_frames);
  double worst_direct = 0, worst_gpu = 0, peak = 0;
  for (std::size_t i = 0; i < out_frames; i += 37) {
    for (int c = 0; c < 2; ++c) {
      double d = 0;
      for (std::size_t k = 0; k < r[c].size() && k <= i; ++k) {
        if (i - k < send.size()) {
          d += double(send[i - k]) * r[c][k];
        }
      }
      peak = std::max(peak, std::abs(d));
      worst_direct = std::max(worst_direct, std::abs((*cpu)[c][i] - d));
      worst_gpu = std::max(worst_gpu,
                           double(std::abs((*gpu)[c][i] - (*cpu)[c][i])));
    }
  }
  std::printf("[media] reverb: direct vs CPU %.2e, GPU vs CPU %.2e (peak "
              "%.2f; GPU %s)\n", worst_direct, worst_gpu, peak,
              gs.gpu ? "ran" : "did not run");
  CHECK(!cs.gpu);
  CHECK(worst_direct < 1e-4 * std::max(1.0, peak));
  CHECK(worst_gpu < 1e-4 * std::max(1.0, peak));

  // Two responses, picked along the send: each block of it hears its
  // mix -- the same as the send cut in two, each half convolved with
  // its own.
  const media::ReverbVoice w{1.0, 0.0, 0.0};
  std::array<std::vector<float>, 2> r2{media::reverb_impulse(w, 0.4, 0, fs),
                                       media::reverb_impulse(w, 0.4, 1, fs)};
  const std::size_t half = 8 * media::kReverbBlock;
  std::vector<float> two(2 * half);
  for (auto& x : two) {
    x = noise();
  }
  std::vector<media::ReverbPick> picks(16);
  for (std::size_t k = 8; k < 16; ++k) {
    picks[k] = {1, 1, 0};
  }
  const std::size_t n2 = two.size() + r2[0].size();
  auto both = media::convolve_reverb(two.data(), two.size(), {r, r2}, picks,
                                     n2, true);
  REQUIRE_OK(both);
  std::vector<float> first(two.begin(), two.begin() + half);
  std::vector<float> second(two.size(), 0.0f);
  std::copy(two.begin() + half, two.end(), second.begin() + half);
  auto a = media::convolve_reverb(first.data(), first.size(), {r}, {}, n2,
                                  false);
  auto b = media::convolve_reverb(second.data(), second.size(), {r2}, {},
                                  n2, false);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  double worst_split = 0;
  for (std::size_t i = 0; i < n2; ++i) {
    worst_split = std::max(worst_split,
                           double(std::abs((*both)[0][i] - (*a)[0][i] -
                                           (*b)[0][i])));
  }
  std::printf("[media] reverb: two responses picked vs halves %.2e\n",
              worst_split);
  CHECK(worst_split < 1e-3);

  // A minute through a ten-second room: how long it takes.
  std::vector<float> minute(static_cast<std::size_t>(60 * fs));
  for (auto& x : minute) {
    x = noise();
  }
  const media::ReverbVoice big{10.0, 0.5, 0.05};
  std::array<std::vector<float>, 2> r3{
      media::reverb_impulse(big, 0.5, 0, fs),
      media::reverb_impulse(big, 0.5, 1, fs)};
  media::ReverbStats ms;
  auto long_one = media::convolve_reverb(minute.data(), minute.size(), {r3},
                                         {}, minute.size() + r3[0].size(),
                                         true, &ms);
  REQUIRE_OK(long_one);
  std::printf("[media] reverb: 60 s through a 10 s room in %.3f s (GPU "
              "%.3f s, %s)\n", ms.seconds, ms.gpu_seconds,
              ms.gpu ? "on the GPU" : "on the CPU");
}

// Where a sound SITS (media/space.h), each on its own: a constant-power
// pan, a width over the side alone, a distance 6 dB a doubling with its
// highs dulled, the lows' side taken out below the bass mono's corner, and
// a crossfeed that leaves what both sides share exactly as it was.
// A sound's BEATS (DESIGN §4v) where a layer showing it plays them:
// through its marks (none before its mark-in or past its mark-out), its
// start, its speed -- a ramp's too -- and none on a picture; kept and read
// back as data, and told to a person a bar of four to a line.
TEST(media, beats_land_where_a_layer_plays)
{
  using namespace valtz::media;
  Beats b;
  b.tempo = 120;
  b.seconds = 4;
  b.times = {0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5};
  // From 2 s on the timeline, its marks 1 s to 3 s of the sound (24 fps).
  LayerTiming t = resolve_timing(48, 0, 24, 71, {24, 1}, {24, 1}, 4.0, {});
  auto on = beats_on_timeline(b.times, t);
  REQUIRE(on.size() == 4);
  const double at[] = {2.0, 2.5, 3.0, 3.5};
  for (std::size_t i = 0; i < on.size(); ++i) {
    CHECK(std::abs(on[i] - at[i]) < 1e-9);
  }
  // Twice as fast: half as far apart, the span half as long.
  KeyedSpeed twice;
  twice.keys = {{0, Speed{2}}};
  LayerTiming f = resolve_timing(48, 0, 24, 71, {24, 1}, {24, 1}, 4.0,
                                 twice);
  on = beats_on_timeline(b.times, f);
  REQUIRE(on.size() == 4);
  CHECK(std::abs(on[1] - 2.25) < 1e-9);
  CHECK(std::abs(on[3] - 2.75) < 1e-9);
  // A ramp from 1 to 3 over its first second: the beat 2 s into the
  // sound where the speed brings it (timing's own inverse).
  KeyedSpeed ramp;
  ramp.keys = {{0, Speed{1}}, {24, Speed{3}}};
  LayerTiming r = resolve_timing(0, 0, -1, -1, {24, 1}, {24, 1}, 4.0, ramp);
  on = beats_on_timeline({2.0, 3.5}, r);
  REQUIRE(on.size() == 2);
  CHECK(std::abs(on[0] - 1.0) < 1e-6);
  CHECK(std::abs(on[1] - 1.5) < 1e-6);
  // A picture keeps no time.
  LayerTiming pic = resolve_timing(0, 0, -1, -1, {}, {24, 1}, 0, {});
  CHECK(beats_on_timeline(b.times, pic).empty());

  // Kept and read back, in order whatever order they were kept in.
  Json j = to_json(b);
  std::swap(j["times"][0], j["times"][6]);
  const Beats back = beats_from_json(j);
  CHECK(back.times == b.times);
  CHECK(back.tempo == 120 && back.seconds == 4);
  // Told: the tempo, how many, from where to where, four to a line.
  const std::string text = beats_text("Song", b);
  CHECK(text.starts_with("# Beats of Song\n"));
  CHECK(text.find("120.0 bpm, 7 beats from 0:00.500 to 0:03.500.") !=
        std::string::npos);
  CHECK(text.find("0:00.500  0:01.000  0:01.500  0:02.000\n"
                  "0:02.500  0:03.000  0:03.500\n") != std::string::npos);
  CHECK(beats_text("Hum", Beats{}).find("No beats were found") !=
        std::string::npos);
}

// A sound's BOOKMARKS (DESIGN §4w), read from what a transcript and a
// video summary keep: each line and each sound heard, each scene, in
// order of start; on a layer, those that start in what it shows, where
// its start and speed bring them, ending no later than it does.
TEST(media, bookmarks_are_read_and_land_where_a_layer_plays)
{
  using namespace valtz::media;
  valtz::assist::Transcript tr;
  tr.seconds = 4;
  tr.tagged = true;
  tr.lines = {{2.5, 3.5, "Goodbye.", "English"},
              {0.2, 0.9, "Before.", "English"},
              {1.25, 1.75, "Hello there.", "English"}};
  tr.events = {{1.0, 4.0, "Music", 0.8}};
  Json tj = valtz::assist::to_json(tr);
  // One with no start is no moment.
  tj["lines"].push_back({{"text", "Nowhere."}});
  const auto said = bookmarks_from_transcript(tj);
  REQUIRE(said.size() == 4);
  CHECK(said[0].text == "Before." && said[0].kind == Bookmark::Kind::Speech);
  CHECK(said[1].text == "Music" && said[1].kind == Bookmark::Kind::Sound);
  CHECK(said[2].text == "Hello there.");
  CHECK(said[3].text == "Goodbye." && said[3].end == 3.5);
  CHECK(kind_name(said[1].kind) == "sound");

  valtz::assist::VideoSummary vs;
  vs.seconds = 4;
  vs.scenes = {{0, 1.5, 12, true, "A fox wakes."},
               {1.5, 4, 20, false, "It trots off."}};
  const auto seen = bookmarks_from_summary(valtz::assist::to_json(vs));
  REQUIRE(seen.size() == 2);
  CHECK(seen[1].kind == Bookmark::Kind::Scene);
  CHECK(kind_name(seen[1].kind) == "scene");
  CHECK(bookmarks_from_summary(tj).empty());

  // From 2 s on the timeline, its marks 1 s to 3 s of the sound: "Before"
  // and the first scene start before what it shows; "Goodbye." starts in
  // it and ends where the layer does.
  LayerTiming t = resolve_timing(48, 0, 24, 71, {24, 1}, {24, 1}, 4.0, {});
  auto on = bookmarks_on_timeline(said, t);
  REQUIRE(on.size() == 3);
  CHECK(on[0].text == "Music");
  CHECK(std::abs(on[0].start - 2.0) < 1e-9);
  CHECK(std::abs(on[0].end - 4.0) < 1e-9);
  CHECK(std::abs(on[1].start - 2.25) < 1e-9);
  CHECK(std::abs(on[1].end - 2.75) < 1e-9);
  CHECK(on[2].text == "Goodbye.");
  CHECK(std::abs(on[2].start - 3.5) < 1e-9);
  CHECK(std::abs(on[2].end - 4.0) < 1e-9);
  auto scenes = bookmarks_on_timeline(seen, t);
  REQUIRE(scenes.size() == 1);
  CHECK(std::abs(scenes[0].start - 2.5) < 1e-9);
  CHECK(std::abs(scenes[0].end - 4.0) < 1e-9);
  // Twice as fast: "Hello there." a quarter of a second long.
  KeyedSpeed twice;
  twice.keys = {{0, Speed{2}}};
  LayerTiming f = resolve_timing(48, 0, 24, 71, {24, 1}, {24, 1}, 4.0,
                                 twice);
  on = bookmarks_on_timeline(said, f);
  REQUIRE(on.size() == 3);
  CHECK(std::abs(on[1].start - 2.125) < 1e-9);
  CHECK(std::abs(on[1].end - 2.375) < 1e-9);
  CHECK(std::abs(on[2].start - 2.75) < 1e-9);
  CHECK(std::abs(on[2].end - 3.0) < 1e-9);
  // A picture keeps no time.
  LayerTiming pic = resolve_timing(0, 0, -1, -1, {}, {24, 1}, 0, {});
  CHECK(bookmarks_on_timeline(said, pic).empty());
}

TEST(media, a_sound_is_panned_widened_and_placed)
{
  const int fs = 48000;
  const std::size_t n = fs;  // a second
  auto tone = [&](double hz, double amp) {
    std::vector<float> v(n);
    for (std::size_t i = 0; i < n; ++i) {
      v[i] = static_cast<float>(amp * std::sin(2 * M_PI * hz * i / fs));
    }
    return v;
  };
  // The level of `hz` in `s` after its first fifth (a filter settled).
  auto level = [&](const std::vector<float>& s, double hz) {
    double re = 0, im = 0;
    const std::size_t a = s.size() / 5;
    for (std::size_t i = a; i < s.size(); ++i) {
      re += s[i] * std::cos(2 * M_PI * hz * i / fs);
      im += s[i] * std::sin(2 * M_PI * hz * i / fs);
    }
    return 2 * std::hypot(re, im) / static_cast<double>(s.size() - a);
  };
  auto keyed = [&](float v) { return std::vector<float>(n, v); };

  // Pan: a centred tone hard left at sqrt2, nothing right; half right.
  auto l = tone(1000, 0.5), r = tone(1000, 0.5);
  media::pan_sound(l, r, keyed(-1));
  CHECK(std::abs(level(l, 1000) - 0.5 * std::sqrt(2.0)) < 1e-3);
  CHECK(level(r, 1000) < 1e-6);
  l = tone(1000, 0.5);
  r = tone(1000, 0.5);
  media::pan_sound(l, r, keyed(0));
  CHECK(l == tone(1000, 0.5));
  media::pan_sound(l, r, keyed(0.5));
  const double pl = level(l, 1000), pr = level(r, 1000);
  CHECK(std::abs(pl * pl + pr * pr - 0.5) < 1e-3);  // as loud
  CHECK(pr > 2 * pl);

  // Width: 0 one channel's worth on both; 2 the side doubled.
  l = tone(1000, 0.4);
  r = tone(300, 0.2);
  media::widen_sound(l, r, keyed(0));
  CHECK(l == r);
  CHECK(std::abs(level(l, 1000) - 0.2) < 1e-3);
  l = tone(1000, 0.4);
  r = std::vector<float>(n, 0.0f);
  media::widen_sound(l, r, keyed(2));
  CHECK(std::abs(level(l, 1000) - 0.6) < 1e-3);   // mid 0.2 + side 0.4
  CHECK(std::abs(level(r, 1000) - 0.2) < 1e-3);   // mid - side, opposed

  // Distance: 4 m, 12 dB down at 1 kHz (its air's corner 8.7 kHz); its
  // highs duller still; a metre, nothing done.
  l = tone(1000, 0.5);
  r = tone(8000, 0.5);
  media::place_sound_far(l, r, keyed(4), fs);
  const double far = 20 * std::log10(level(l, 1000) / 0.5);
  const double high = 20 * std::log10(level(r, 8000) / 0.5);
  std::printf("[media] space: 4 m -> 1 kHz %+.2f dB, 8 kHz %+.2f dB\n",
              far, high);
  CHECK(std::abs(far + 12.04) < 0.2);
  CHECK(high < far - 2);
  l = tone(1000, 0.5);
  r = tone(1000, 0.5);
  media::place_sound_far(l, r, keyed(1), fs);
  CHECK(l == tone(1000, 0.5));

  // Bass mono at 150 Hz: 50 Hz on the left alone comes out in the middle,
  // 2 kHz on the left alone stays there.
  l = tone(50, 0.4);
  const auto hi = tone(2000, 0.4);
  for (std::size_t i = 0; i < n; ++i) {
    l[i] += hi[i];
  }
  r = std::vector<float>(n, 0.0f);
  media::mono_bass(l, r, 150, fs);
  std::printf("[media] space: bass mono 150 Hz -> 50 Hz L %.3f R %.3f, "
              "2 kHz L %.3f R %.3f\n", level(l, 50), level(r, 50),
              level(l, 2000), level(r, 2000));
  CHECK(std::abs(level(l, 50) - 0.2) < 0.01);
  CHECK(std::abs(level(r, 50) - 0.2) < 0.01);
  CHECK(std::abs(level(l, 2000) - 0.4) < 0.01);
  CHECK(level(r, 2000) < 0.01);

  // Crossfeed: a centred sound exactly as it was; a left one's lows heard
  // on the right far more than its highs.
  l = tone(100, 0.5);
  r = tone(100, 0.5);
  media::crossfeed_sound(l, r, 1, fs);
  CHECK(l == tone(100, 0.5) && r == tone(100, 0.5));
  l = tone(100, 0.5);
  r = std::vector<float>(n, 0.0f);
  media::crossfeed_sound(l, r, 1, fs);
  const double low_over = level(r, 100) / level(l, 100);
  l = tone(5000, 0.5);
  r = std::vector<float>(n, 0.0f);
  media::crossfeed_sound(l, r, 1, fs);
  const double high_over = level(r, 5000) / level(l, 5000);
  std::printf("[media] space: crossfeed -> right/left %.3f at 100 Hz, "
              "%.3f at 5 kHz\n", low_over, high_over);
  CHECK(low_over > 0.4);
  CHECK(high_over < 0.15);
}
