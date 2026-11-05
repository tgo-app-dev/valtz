#include "valtz/media/stamp.h"

#include "valtz/media/movie.h"

#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace valtz::media {

namespace fs = std::filesystem;
using Bytes = std::vector<std::uint8_t>;

namespace {

std::string
lower_ext(const fs::path& p)
{
  std::string e = p.extension().string();
  std::ranges::transform(e, e.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return e;
}

Result<Bytes>
read_all(const fs::path& p)
{
  std::ifstream in(p, std::ios::binary);
  if (!in) {
    return make_error(Code::Io, std::format("cannot read {}", p.string()));
  }
  Bytes b((std::istreambuf_iterator<char>(in)),
          std::istreambuf_iterator<char>());
  return b;
}

// `bytes` as `file`, through a file beside it renamed over it: never in
// place, so a clone (APFS) of `file` shares nothing that changed.
Status
replace_with(const fs::path& file, const Bytes& bytes)
{
  const fs::path tmp = file.parent_path() /
      ("." + file.filename().string() + ".stamp");
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out.good()) {
      std::error_code ec;
      fs::remove(tmp, ec);
      return make_error(Code::Io, std::format("cannot write {}",
                                              tmp.string()));
    }
  }
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return make_error(Code::Io, std::format("cannot replace {}: {}",
                                            file.string(), ec.message()));
  }
  return ok_status();
}

// ---- TIFF blocks (EXIF) ------------------------------------------------

struct Tiff {
  const Bytes& t;
  bool le = true;

  std::uint32_t
  rd(std::size_t at, int n) const
  {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      const std::uint32_t b = t[at + static_cast<std::size_t>(
                                         le ? i : n - 1 - i)];
      v |= b << (8 * i);
    }
    return v;
  }
};

void
put(Bytes& t, bool le, std::size_t at, int n, std::uint32_t v)
{
  for (int i = 0; i < n; ++i) {
    t[at + static_cast<std::size_t>(le ? i : n - 1 - i)] =
        static_cast<std::uint8_t>(v >> (8 * i));
  }
}

// IFD 0's place and entry count, checked to lie in the block.
std::optional<std::pair<std::size_t, std::uint32_t>>
ifd0(const Tiff& r)
{
  const Bytes& t = r.t;
  if (t.size() < 8 || r.rd(2, 2) != 42) {
    return std::nullopt;
  }
  const std::size_t at = r.rd(4, 4);
  if (at + 2 > t.size()) {
    return std::nullopt;
  }
  const std::uint32_t n = r.rd(at, 2);
  if (at + 2 + 12 * std::size_t(n) + 4 > t.size()) {
    return std::nullopt;
  }
  return std::pair{at, n};
}

std::optional<bool>
byte_order(const Bytes& t)
{
  if (t.size() >= 2 && t[0] == 'I' && t[1] == 'I') {
    return true;
  }
  if (t.size() >= 2 && t[0] == 'M' && t[1] == 'M') {
    return false;
  }
  return std::nullopt;
}

// ---- PNG ---------------------------------------------------------------

constexpr std::array<std::uint8_t, 8> kPngSig = {0x89, 'P', 'N', 'G',
                                                 '\r', '\n', 0x1A, '\n'};

std::uint32_t
crc32(const std::uint8_t* p, std::size_t n, std::uint32_t c = 0xFFFFFFFFu)
{
  static const auto table = [] {
    std::array<std::uint32_t, 256> t{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      }
      t[i] = c;
    }
    return t;
  }();
  for (std::size_t i = 0; i < n; ++i) {
    c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  }
  return c;
}

struct Chunk {
  std::string type;
  Bytes       data;
};

void
append_chunk(Bytes& out, const Chunk& c)
{
  const auto n = static_cast<std::uint32_t>(c.data.size());
  for (int i = 3; i >= 0; --i) {
    out.push_back(static_cast<std::uint8_t>(n >> (8 * i)));
  }
  const std::size_t from = out.size();
  out.insert(out.end(), c.type.begin(), c.type.end());
  out.insert(out.end(), c.data.begin(), c.data.end());
  const std::uint32_t crc =
      crc32(out.data() + from, out.size() - from) ^ 0xFFFFFFFFu;
  for (int i = 3; i >= 0; --i) {
    out.push_back(static_cast<std::uint8_t>(crc >> (8 * i)));
  }
}

// A tEXt / iTXt / zTXt chunk whose keyword is Software.
bool
software_text(const Chunk& c)
{
  if (c.type != "tEXt" && c.type != "iTXt" && c.type != "zTXt") {
    return false;
  }
  static constexpr std::string_view kKey = "Software";
  return c.data.size() > kKey.size() && c.data[kKey.size()] == 0 &&
         std::equal(kKey.begin(), kKey.end(), c.data.begin());
}

// An iTXt chunk of XMP ("XML:com.adobe.xmp", uncompressed: ImageIO
// writes one beside a PNG's EXIF) with its writer named in it -- XMP's
// CreatorTool and TIFF's Software, as ImageIO reads them -- through
// ImageIO's own XMP. nullopt: no such chunk, or one it does not read.
std::optional<Bytes>
xmp_with_software(const Chunk& c, std::string_view software)
{
  static constexpr std::string_view kKey = "XML:com.adobe.xmp";
  if (c.type != "iTXt" || c.data.size() < kKey.size() + 3 ||
      !std::equal(kKey.begin(), kKey.end(), c.data.begin()) ||
      c.data[kKey.size()] != 0 || c.data[kKey.size() + 1] != 0) {
    return std::nullopt;
  }
  // keyword \0, flag, method, language \0, translated keyword \0, text.
  std::size_t at = kKey.size() + 3;
  for (int z = 0; z < 2; ++z) {
    const auto end = std::find(c.data.begin() + static_cast<std::ptrdiff_t>(
                                                    at),
                               c.data.end(), 0);
    if (end == c.data.end()) {
      return std::nullopt;
    }
    at = static_cast<std::size_t>(end - c.data.begin()) + 1;
  }
  @autoreleasepool {
    NSData* xmp = [NSData dataWithBytes:c.data.data() + at
                                 length:c.data.size() - at];
    CGImageMetadataRef m =
        CGImageMetadataCreateFromXMPData((__bridge CFDataRef)xmp);
    if (!m) {
      return std::nullopt;
    }
    CGMutableImageMetadataRef mm = CGImageMetadataCreateMutableCopy(m);
    CFRelease(m);
    NSString* v = [[NSString alloc] initWithBytes:software.data()
                                           length:software.size()
                                         encoding:NSUTF8StringEncoding];
    CGImageMetadataSetValueWithPath(mm, nullptr, CFSTR("xmp:CreatorTool"),
                                    (__bridge CFStringRef)v);
    CGImageMetadataSetValueWithPath(mm, nullptr, CFSTR("tiff:Software"),
                                    (__bridge CFStringRef)v);
    NSData* out = (__bridge_transfer NSData*)CGImageMetadataCreateXMPData(
        mm, nullptr);
    CFRelease(mm);
    if (!out) {
      return std::nullopt;
    }
    Bytes d(c.data.begin(), c.data.begin() + static_cast<std::ptrdiff_t>(at));
    const auto* p = static_cast<const std::uint8_t*>(out.bytes);
    d.insert(d.end(), p, p + out.length);
    return d;
  }
}

// An empty TIFF block (little-endian, IFD 0 of no entries): what a PNG
// with no eXIf gets its Software into.
Bytes
empty_tiff()
{
  return {'I', 'I', 42, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}

Status
stamp_png(const fs::path& file, std::string_view software, bool keep_model)
{
  VALTZ_ASSIGN(Bytes b, read_all(file));
  if (b.size() < 8 || !std::equal(kPngSig.begin(), kPngSig.end(),
                                  b.begin())) {
    return make_error(Code::Corrupt, std::format("{} is no PNG",
                                                 file.string()));
  }
  std::vector<Chunk> chunks;
  std::size_t i = 8;
  while (i + 12 <= b.size()) {
    const std::size_t n = (std::size_t(b[i]) << 24) |
                          (std::size_t(b[i + 1]) << 16) |
                          (std::size_t(b[i + 2]) << 8) | b[i + 3];
    if (i + 12 + n > b.size()) {
      return make_error(Code::Corrupt, std::format("{} is cut short",
                                                   file.string()));
    }
    Chunk c;
    c.type.assign(b.begin() + static_cast<std::ptrdiff_t>(i + 4),
                  b.begin() + static_cast<std::ptrdiff_t>(i + 8));
    c.data.assign(b.begin() + static_cast<std::ptrdiff_t>(i + 8),
                  b.begin() + static_cast<std::ptrdiff_t>(i + 8 + n));
    const bool end = c.type == "IEND";
    chunks.push_back(std::move(c));
    i += 12 + n;
    if (end) {
      break;
    }
  }
  if (chunks.empty() || chunks.front().type != "IHDR" ||
      chunks.back().type != "IEND") {
    return make_error(Code::Corrupt, std::format("{} is no whole PNG",
                                                 file.string()));
  }
  // Who it says wrote it: its EXIF's Software, else its text's.
  std::string was;
  Chunk* exif = nullptr;
  for (auto& c : chunks) {
    if (c.type == "eXIf") {
      exif = &c;
      if (auto s = tiff_software(c.data)) {
        was = *s;
      }
    } else if (was.empty() && c.type == "tEXt" && software_text(c)) {
      was.assign(c.data.begin() + 9, c.data.end());
    }
  }
  const std::string sw = keep_model ? stamped_software(was, software)
                                    : std::string(software);
  std::optional<Bytes> block =
      tiff_with_software(exif ? exif->data : empty_tiff(), sw);
  if (!block) {
    // An eXIf ImageIO wrote but no TIFF block we read: a fresh one.
    block = tiff_with_software(empty_tiff(), sw);
  }
  // tEXt holds Latin-1: an ASCII name goes there, any other in iTXt
  // (UTF-8, uncompressed, no language).
  const bool ascii = std::ranges::all_of(sw, [](char ch) {
    return static_cast<unsigned char>(ch) < 0x80;
  });
  Chunk text{ascii ? "tEXt" : "iTXt", {}};
  static constexpr std::string_view kKey = "Software";
  text.data.assign(kKey.begin(), kKey.end());
  text.data.push_back(0);
  if (!ascii) {
    text.data.insert(text.data.end(), {0, 0, 0, 0});  // flag, method, "", ""
  }
  text.data.insert(text.data.end(), sw.begin(), sw.end());

  Bytes out(kPngSig.begin(), kPngSig.end());
  bool placed = false;
  for (const auto& c : chunks) {
    if (software_text(c)) {
      continue;
    }
    // Both before the picture: where a reader looks for them.
    if (!placed && (c.type == "IDAT" || c.type == "IEND")) {
      if (!exif) {
        append_chunk(out, {"eXIf", *block});
      }
      append_chunk(out, text);
      placed = true;
    }
    if (c.type == "eXIf") {
      append_chunk(out, {"eXIf", *block});
    } else if (auto x = xmp_with_software(c, sw)) {
      append_chunk(out, {"iTXt", std::move(*x)});
    } else {
      append_chunk(out, c);
    }
  }
  return replace_with(file, out);
}

// ---- JPEG, TIFF: ImageIO's lossless copy -------------------------------

// A JPEG's EXIF segment (an APP1 of "Exif\0\0"): where it starts and its
// length, marker included -- or, none, where one goes (after the SOI, and
// after a JFIF APP0 first in it, as JFIF wants). nullopt: no JPEG this
// reads.
struct JpegExif {
  std::size_t at = 0, size = 0;  // size 0: none, `at` where one goes
};

std::optional<JpegExif>
jpeg_exif(const Bytes& b)
{
  if (b.size() < 4 || b[0] != 0xFF || b[1] != 0xD8) {
    return std::nullopt;
  }
  static constexpr std::array<std::uint8_t, 6> kExif = {'E', 'x', 'i', 'f',
                                                        0, 0};
  JpegExif place{2, 0};
  std::size_t i = 2;
  bool first = true;
  while (i + 4 <= b.size()) {
    if (b[i] != 0xFF) {
      return std::nullopt;
    }
    const std::uint8_t m = b[i + 1];
    if (m == 0xFF) {
      ++i;  // fill
      continue;
    }
    if (m == 0xDA || m == 0xD9) {
      return place;  // the picture's data: no EXIF before it
    }
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
      i += 2;  // no length
      continue;
    }
    const std::size_t len = (std::size_t(b[i + 2]) << 8) | b[i + 3];
    if (len < 2 || i + 2 + len > b.size()) {
      return std::nullopt;
    }
    if (m == 0xE1 && len >= 2 + kExif.size() &&
        std::equal(kExif.begin(), kExif.end(), b.begin() +
                   static_cast<std::ptrdiff_t>(i + 4))) {
      return JpegExif{i, 2 + len};
    }
    if (first && m == 0xE0) {
      place.at = i + 2 + len;
    }
    first = false;
    i += 2 + len;
  }
  return std::nullopt;
}

// A JPEG's EXIF Software set in its own block -- one made where it has
// none -- the bytes around it as they were. ImageIO's copy (below) gives
// a JPEG without one XMP alone, and leaves an IFD 0 of Software and the
// Exif pointer (ImageIO's own) which it then does not read. Returns the
// name written, the model the block named kept with `keep_model`; nullopt
// when the file is no JPEG this reads, or its block is not (ImageIO's copy
// alone then).
Result<std::optional<std::string>>
stamp_jpeg_exif(const fs::path& file, std::string_view software,
                bool keep_model)
{
  VALTZ_ASSIGN(Bytes b, read_all(file));
  const auto seg = jpeg_exif(b);
  if (!seg) {
    return std::optional<std::string>();
  }
  const std::size_t head = 4 + 6;  // marker, length, "Exif\0\0"
  const Bytes block = seg->size > head
      ? Bytes(b.begin() + static_cast<std::ptrdiff_t>(seg->at + head),
              b.begin() + static_cast<std::ptrdiff_t>(seg->at + seg->size))
      : empty_tiff();
  const std::string was =
      seg->size ? tiff_software(block).value_or("") : std::string();
  const std::string sw = keep_model ? stamped_software(was, software)
                                    : std::string(software);
  const std::optional<Bytes> tiff = tiff_with_software(block, sw);
  if (!tiff || tiff->size() + head - 2 > 0xFFFF) {
    return std::optional<std::string>();
  }
  Bytes out = {0xFF, 0xE1, 0, 0, 'E', 'x', 'i', 'f', 0, 0};
  out.insert(out.end(), tiff->begin(), tiff->end());
  const std::size_t len = out.size() - 2;
  out[2] = static_cast<std::uint8_t>(len >> 8);
  out[3] = static_cast<std::uint8_t>(len & 0xFF);
  b.erase(b.begin() + static_cast<std::ptrdiff_t>(seg->at),
          b.begin() + static_cast<std::ptrdiff_t>(seg->at + seg->size));
  b.insert(b.begin() + static_cast<std::ptrdiff_t>(seg->at), out.begin(),
           out.end());
  VALTZ_TRY(replace_with(file, b));
  return std::optional<std::string>(sw);
}

Status
stamp_imageio(const fs::path& file, std::string_view software,
              bool keep_model)
{
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:@(file.c_str())];
    CGImageSourceRef src =
        CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    if (!src || CGImageSourceGetCount(src) < 1) {
      if (src) {
        CFRelease(src);
      }
      return make_error(Code::Corrupt, std::format(
          "{} is no picture ImageIO reads", file.string()));
    }
    NSDictionary* p = (__bridge_transfer NSDictionary*)
        CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr);
    NSString* was =
        p[(id)kCGImagePropertyTIFFDictionary][(id)kCGImagePropertyTIFFSoftware];
    const std::string sw =
        keep_model ? stamped_software(
                         [was isKindOfClass:[NSString class]]
                             ? was.UTF8String : "",
                         software)
                   : std::string(software);
    NSString* value = [[NSString alloc] initWithBytes:sw.data()
                                               length:sw.size()
                                             encoding:NSUTF8StringEncoding];
    CGMutableImageMetadataRef md = CGImageMetadataCreateMutable();
    CGImageMetadataSetValueMatchingImageProperty(
        md, kCGImagePropertyTIFFDictionary, kCGImagePropertyTIFFSoftware,
        (__bridge CFStringRef)value);
    CGImageMetadataSetValueWithPath(md, nullptr, CFSTR("xmp:CreatorTool"),
                                    (__bridge CFStringRef)value);
    const fs::path tmp = file.parent_path() /
        ("." + file.filename().string() + ".stamp");
    NSURL* turl = [NSURL fileURLWithPath:@(tmp.c_str())];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)turl, CGImageSourceGetType(src), 1, nullptr);
    bool ok = false;
    NSString* error = nil;
    if (dst) {
      NSDictionary* opts = @{
        (id)kCGImageDestinationMetadata : (__bridge id)md,
        (id)kCGImageDestinationMergeMetadata : @YES,
      };
      CFErrorRef err = nullptr;
      ok = CGImageDestinationCopyImageSource(
          dst, src, (__bridge CFDictionaryRef)opts, &err);
      if (err) {
        error = [(__bridge NSError*)err localizedDescription];
        CFRelease(err);
      }
      CFRelease(dst);
    }
    CFRelease(md);
    CFRelease(src);
    std::error_code ec;
    if (!ok) {
      fs::remove(tmp, ec);
      return make_error(Code::Unsupported, std::format(
          "{}: {}", file.filename().string(),
          error ? error.UTF8String : "not stamped"));
    }
    fs::rename(tmp, file, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return make_error(Code::Io, std::format("cannot replace {}: {}",
                                              file.string(), ec.message()));
    }
  }
  return ok_status();
}

// ---- OpenEXR -----------------------------------------------------------

std::uint32_t
le32(const Bytes& b, std::size_t at)
{
  return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8) |
         (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
}

// Scanlines a chunk holds, by compression (OpenEXR's numbering).
int
lines_per_chunk(int compression)
{
  switch (compression) {
  case 0: case 1: case 2: return 1;     // none, RLE, ZIPS
  case 3: case 5: return 16;            // ZIP, PXR24
  case 4: case 6: case 7: case 8: return 32;  // PIZ, B44, B44A, DWAA
  case 9: return 256;                   // DWAB
  default: return 0;
  }
}

Status
stamp_exr(const fs::path& file, std::string_view software)
{
  VALTZ_ASSIGN(Bytes b, read_all(file));
  if (b.size() < 8 || le32(b, 0) != 20000630u) {
    return make_error(Code::Corrupt, std::format("{} is no OpenEXR",
                                                 file.string()));
  }
  // Tiled, deep or multi-part: offsets laid out otherwise; left alone.
  if ((le32(b, 4) & (0x200u | 0x800u | 0x1000u)) != 0) {
    return make_error(Code::Unsupported, std::format(
        "{}: only a single-part scanline OpenEXR is stamped",
        file.filename().string()));
  }
  struct Attr {
    std::size_t from, to;  // the whole attribute, name to value
    std::string name;
  };
  std::vector<Attr> attrs;
  int compression = -1;
  std::int64_t y0 = 0, y1 = -1;
  std::int64_t chunk_count = -1;
  std::size_t i = 8;
  auto cstr = [&](std::size_t& at) -> std::optional<std::string> {
    const auto end = std::find(b.begin() + static_cast<std::ptrdiff_t>(at),
                               b.end(), 0);
    if (end == b.end()) {
      return std::nullopt;
    }
    std::string s(b.begin() + static_cast<std::ptrdiff_t>(at), end);
    at = static_cast<std::size_t>(end - b.begin()) + 1;
    return s;
  };
  while (i < b.size() && b[i] != 0) {
    const std::size_t from = i;
    auto name = cstr(i);
    auto type = name ? cstr(i) : std::nullopt;
    if (!type || i + 4 > b.size()) {
      return make_error(Code::Corrupt, std::format("{}: a broken header",
                                                   file.string()));
    }
    const std::size_t n = le32(b, i);
    i += 4;
    if (i + n > b.size()) {
      return make_error(Code::Corrupt, std::format("{}: a broken header",
                                                   file.string()));
    }
    if (*name == "compression" && n == 1) {
      compression = b[i];
    } else if (*name == "dataWindow" && n == 16) {
      y0 = static_cast<std::int32_t>(le32(b, i + 4));
      y1 = static_cast<std::int32_t>(le32(b, i + 12));
    } else if (*name == "chunkCount" && n == 4) {
      chunk_count = static_cast<std::int32_t>(le32(b, i));
    }
    i += n;
    attrs.push_back({from, i, *name});
  }
  if (i >= b.size()) {
    return make_error(Code::Corrupt, std::format("{}: a broken header",
                                                 file.string()));
  }
  const std::size_t header_end = i + 1;  // past the terminating 0
  if (chunk_count < 0) {
    const int lines = lines_per_chunk(compression);
    if (lines <= 0 || y1 < y0) {
      return make_error(Code::Unsupported, std::format(
          "{}: compression {} not stamped", file.filename().string(),
          compression));
    }
    chunk_count = (y1 - y0 + lines) / lines;
  }
  const std::size_t table_end =
      header_end + 8 * static_cast<std::size_t>(chunk_count);
  if (table_end > b.size()) {
    return make_error(Code::Corrupt, std::format("{}: cut short",
                                                 file.string()));
  }
  // The header again, a "software" string attribute in place of any.
  Bytes head(b.begin(), b.begin() + 8);
  for (const auto& a : attrs) {
    if (a.name != "software") {
      head.insert(head.end(), b.begin() + static_cast<std::ptrdiff_t>(a.from),
                  b.begin() + static_cast<std::ptrdiff_t>(a.to));
    }
  }
  static constexpr std::string_view kName = "software", kType = "string";
  head.insert(head.end(), kName.begin(), kName.end());
  head.push_back(0);
  head.insert(head.end(), kType.begin(), kType.end());
  head.push_back(0);
  const auto len = static_cast<std::uint32_t>(software.size());
  for (int k = 0; k < 4; ++k) {
    head.push_back(static_cast<std::uint8_t>(len >> (8 * k)));
  }
  head.insert(head.end(), software.begin(), software.end());
  head.push_back(0);
  // Every chunk moves by what the header grew (or shrank) by.
  const std::int64_t delta = static_cast<std::int64_t>(head.size()) -
                             static_cast<std::int64_t>(header_end);
  Bytes out = std::move(head);
  out.reserve(b.size() + static_cast<std::size_t>(std::max<std::int64_t>(
                             0, delta)));
  for (std::size_t k = header_end; k < table_end; k += 8) {
    std::uint64_t off = 0;
    for (int j = 0; j < 8; ++j) {
      off |= std::uint64_t(b[k + static_cast<std::size_t>(j)]) << (8 * j);
    }
    if (off != 0) {  // 0: a chunk never written
      off = static_cast<std::uint64_t>(static_cast<std::int64_t>(off) +
                                       delta);
    }
    for (int j = 0; j < 8; ++j) {
      out.push_back(static_cast<std::uint8_t>(off >> (8 * j)));
    }
  }
  out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(table_end),
             b.end());
  return replace_with(file, out);
}

// ---- WAV ---------------------------------------------------------------

void
add_le32(Bytes& b, std::uint32_t v)
{
  for (int k = 0; k < 4; ++k) {
    b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
  }
}

// A LIST/INFO chunk of `items` (id, data) and ISFT.
Bytes
info_chunk(const std::vector<std::pair<std::string, Bytes>>& items,
           std::string_view software)
{
  Bytes body = {'I', 'N', 'F', 'O'};
  auto sub = [&](std::string_view id, const Bytes& data) {
    body.insert(body.end(), id.begin(), id.end());
    add_le32(body, static_cast<std::uint32_t>(data.size()));
    body.insert(body.end(), data.begin(), data.end());
    if (data.size() % 2) {
      body.push_back(0);
    }
  };
  for (const auto& [id, data] : items) {
    if (id != "ISFT") {
      sub(id, data);
    }
  }
  Bytes sw(software.begin(), software.end());
  sw.push_back(0);
  sub("ISFT", sw);
  Bytes c = {'L', 'I', 'S', 'T'};
  add_le32(c, static_cast<std::uint32_t>(body.size()));
  c.insert(c.end(), body.begin(), body.end());
  return c;
}

Status
stamp_wav(const fs::path& file, std::string_view software)
{
  VALTZ_ASSIGN(Bytes b, read_all(file));
  if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 ||
      std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
    return make_error(Code::Unsupported, std::format(
        "{}: only a RIFF WAV is stamped", file.filename().string()));
  }
  // Its chunks; an INFO list's items read, to be kept.
  std::optional<std::pair<std::size_t, std::size_t>> info;  // [from, to)
  std::vector<std::pair<std::string, Bytes>> items;
  std::size_t i = 12;
  while (i + 8 <= b.size()) {
    const std::size_t n = le32(b, i + 4);
    const std::size_t end = std::min(b.size(), i + 8 + n + (n % 2));
    if (std::memcmp(b.data() + i, "LIST", 4) == 0 && n >= 4 &&
        std::memcmp(b.data() + i + 8, "INFO", 4) == 0 && !info) {
      info = std::pair{i, end};
      for (std::size_t k = i + 12; k + 8 <= i + 8 + n && k + 8 <= b.size();) {
        const std::size_t m = le32(b, k + 4);
        if (k + 8 + m > b.size()) {
          break;
        }
        items.emplace_back(
            std::string(b.begin() + static_cast<std::ptrdiff_t>(k),
                        b.begin() + static_cast<std::ptrdiff_t>(k + 4)),
            Bytes(b.begin() + static_cast<std::ptrdiff_t>(k + 8),
                  b.begin() + static_cast<std::ptrdiff_t>(k + 8 + m)));
        k += 8 + m + (m % 2);
      }
    }
    i = end;
  }
  const Bytes list = info_chunk(items, software);
  Bytes out;
  if (info) {
    out.assign(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(
                                          info->first));
    out.insert(out.end(), list.begin(), list.end());
    out.insert(out.end(), b.begin() + static_cast<std::ptrdiff_t>(
                                          info->second),
               b.end());
  } else {
    out = std::move(b);
    if (out.size() % 2) {
      out.push_back(0);  // the chunk before it padded to a word
    }
    out.insert(out.end(), list.begin(), list.end());
  }
  if (out.size() - 8 > 0xFFFFFFFFu) {
    return make_error(Code::Unsupported, std::format(
        "{}: too long for RIFF", file.filename().string()));
  }
  put(out, true, 4, 4, static_cast<std::uint32_t>(out.size() - 8));
  return replace_with(file, out);
}

// ---- movies and AAC ----------------------------------------------------

Status
stamp_movie(const fs::path& file, std::string_view software)
{
  const fs::path tmp = file.parent_path() /
      ("." + file.stem().string() + ".stamp" + file.extension().string());
  VALTZ_TRY(remux_with_software(file, tmp, software));
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return make_error(Code::Io, std::format("cannot replace {}: {}",
                                            file.string(), ec.message()));
  }
  return ok_status();
}

}

std::string
stamped_model(std::string_view was)
{
  static constexpr std::string_view kWith = " with ";
  const auto v = was.find("Vpipe ");
  const auto w = v == std::string_view::npos ? v : was.find(kWith, v);
  if (w == std::string_view::npos) {
    return {};
  }
  std::string_view model = was.substr(w + kWith.size());
  while (!model.empty() && model.back() == ')') {
    model.remove_suffix(1);
  }
  return std::string(model);
}

std::string
with_model(std::string_view software, std::string_view model)
{
  std::string out(software);
  if (model.empty()) {
    return out;
  }
  if (!out.empty() && out.back() == ')') {
    out.insert(out.size() - 1, std::format(" with {}", model));
  } else {
    out += std::format(" (with {})", model);
  }
  return out;
}

std::string
stamped_software(std::string_view was, std::string_view software)
{
  return with_model(software, stamped_model(was));
}

std::optional<std::string>
tiff_software(const Bytes& t)
{
  const auto le = byte_order(t);
  if (!le) {
    return std::nullopt;
  }
  const Tiff r{t, *le};
  const auto ifd = ifd0(r);
  if (!ifd) {
    return std::nullopt;
  }
  for (std::uint32_t k = 0; k < ifd->second; ++k) {
    const std::size_t e = ifd->first + 2 + 12 * std::size_t(k);
    if (r.rd(e, 2) != 0x0131 || r.rd(e + 2, 2) != 2) {
      continue;
    }
    const std::size_t n = r.rd(e + 4, 4);
    const std::size_t at = n <= 4 ? e + 8 : r.rd(e + 8, 4);
    if (at + n > t.size()) {
      return std::nullopt;
    }
    std::string s(t.begin() + static_cast<std::ptrdiff_t>(at),
                  t.begin() + static_cast<std::ptrdiff_t>(at + n));
    while (!s.empty() && s.back() == 0) {
      s.pop_back();
    }
    return s;
  }
  return std::string();
}

std::optional<Bytes>
tiff_with_software(const Bytes& block, std::string_view software)
{
  const auto le = byte_order(block);
  if (!le) {
    return std::nullopt;
  }
  const Tiff r{block, *le};
  const auto ifd = ifd0(r);
  if (!ifd) {
    return std::nullopt;
  }
  const auto [at, n] = *ifd;
  // IFD 0's entries but Software's, as they are: an offset in one still
  // points where it did, since the block only grows at its end.
  std::vector<std::array<std::uint8_t, 12>> entries;
  for (std::uint32_t k = 0; k < n; ++k) {
    const std::size_t e = at + 2 + 12 * std::size_t(k);
    if (r.rd(e, 2) == 0x0131) {
      continue;
    }
    std::array<std::uint8_t, 12> x{};
    std::copy_n(block.begin() + static_cast<std::ptrdiff_t>(e), 12,
                x.begin());
    entries.push_back(x);
  }
  // Software with no other TIFF field in IFD 0 -- alone, or beside only
  // the pointers to other IFDs (Exif's, GPS's, interoperability's) --
  // ImageIO does not read (nor so Preview, nor Spotlight): Orientation 1,
  // the pixels as stored, beside it.
  auto tag_of = [&](const std::array<std::uint8_t, 12>& x) {
    return *le ? std::uint32_t(x[0]) | (std::uint32_t(x[1]) << 8)
               : (std::uint32_t(x[0]) << 8) | std::uint32_t(x[1]);
  };
  if (std::ranges::all_of(entries, [&](const auto& x) {
        const std::uint32_t tag = tag_of(x);
        return tag == 0x8769 || tag == 0x8825 || tag == 0xA005;
      })) {
    Bytes e(12, 0);
    put(e, *le, 0, 2, 0x0112);
    put(e, *le, 2, 2, 3);  // SHORT
    put(e, *le, 4, 4, 1);
    put(e, *le, 8, 2, 1);  // left in its field, either order
    std::array<std::uint8_t, 12> x{};
    std::copy(e.begin(), e.end(), x.begin());
    entries.push_back(x);
  }
  const std::uint32_t next = r.rd(at + 2 + 12 * std::size_t(n), 4);
  Bytes t = block;
  if (t.size() % 2) {
    t.push_back(0);  // an IFD starts on a word
  }
  const std::size_t ifd_at = t.size();
  const std::size_t count = entries.size() + 1;
  const std::size_t str_at = ifd_at + 2 + 12 * count + 4;
  const auto len = static_cast<std::uint32_t>(software.size() + 1);
  std::array<std::uint8_t, 12> sw{};
  {
    Bytes e(12, 0);
    put(e, *le, 0, 2, 0x0131);
    put(e, *le, 2, 2, 2);  // ASCII
    put(e, *le, 4, 4, len);
    if (len <= 4) {
      std::copy(software.begin(), software.end(), e.begin() + 8);
    } else {
      put(e, *le, 8, 4, static_cast<std::uint32_t>(str_at));
    }
    std::copy(e.begin(), e.end(), sw.begin());
  }
  // In tag order, as TIFF wants an IFD.
  entries.push_back(sw);
  std::ranges::stable_sort(entries, {}, tag_of);
  t.resize(str_at, 0);
  put(t, *le, ifd_at, 2, static_cast<std::uint32_t>(count));
  for (std::size_t k = 0; k < entries.size(); ++k) {
    std::copy(entries[k].begin(), entries[k].end(),
              t.begin() + static_cast<std::ptrdiff_t>(ifd_at + 2 + 12 * k));
  }
  put(t, *le, ifd_at + 2 + 12 * count, 4, next);
  if (len > 4) {
    t.insert(t.end(), software.begin(), software.end());
    t.push_back(0);
  }
  put(t, *le, 4, 4, static_cast<std::uint32_t>(ifd_at));
  return t;
}

Status
stamp_software(const fs::path& file, std::string_view software,
               bool keep_model)
{
  if (software.empty()) {
    return ok_status();
  }
  const std::string ext = lower_ext(file);
  if (ext == ".png") {
    return stamp_png(file, software, keep_model);
  }
  if (ext == ".jpg" || ext == ".jpeg") {
    // Its EXIF block first, then ImageIO's copy for the XMP -- the same
    // name, the model already settled from the block.
    VALTZ_ASSIGN(std::optional<std::string> sw,
                 stamp_jpeg_exif(file, software, keep_model));
    return sw ? stamp_imageio(file, *sw, false)
              : stamp_imageio(file, software, keep_model);
  }
  if (ext == ".tif" || ext == ".tiff") {
    return stamp_imageio(file, software, keep_model);
  }
  if (ext == ".exr") {
    return stamp_exr(file, software);
  }
  if (ext == ".wav") {
    return stamp_wav(file, software);
  }
  if (ext == ".mov" || ext == ".mp4" || ext == ".m4v" || ext == ".m4a") {
    return stamp_movie(file, software);
  }
  return make_error(Code::Unsupported, std::format(
      "{} files are not stamped", ext.empty() ? "extensionless" : ext));
}

}
