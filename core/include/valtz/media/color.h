// Color description of an image or video stream.
//
// Code points are ITU-T H.273 (CICP) -- the numbers HEVC/AV1 VUI, PNG
// cICP, CoreVideo attachments and FFmpeg all carry -- so tagging never
// goes through a lossy "closest named space" table. Every asset records
// its color exactly as its source declared it, UNSPECIFIED included:
// guessing at import time bakes the guess into every derivative.
//
// THE WORKING SPACE. Pixels that Valtz processes (composite, resize,
// compare, feed to a model) are converted once, at the I/O boundary, to
//
//   RGBA, half float, PREMULTIPLIED alpha,
//   linear light, BT.2020 primaries, extended range
//
// with SDR reference white at 1.0 (HDR content keeps values above 1.0;
// BT.2408's 203 cd/m2 maps to 1.0). This is the same convention as
// macOS EDR, so the viewer can hand the working buffer to Core Animation
// with an extended-linear color space and let the system tone-map for
// the display. Filtering and blending are correct only in linear,
// premultiplied form; BT.2020 contains P3 and 709, so no source gamut is
// clipped; half float keeps PQ's 10,000-nit range and ProRes 4444's 12
// bits without banding.
//
// Models are the exception: each declares the space it was trained in
// (nearly always sRGB-encoded BT.709, straight alpha) and the model
// adapter converts working <-> model space explicitly at its boundary.

#ifndef VALTZ_MEDIA_COLOR_H
#define VALTZ_MEDIA_COLOR_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace valtz::media {

enum class Primaries : std::uint8_t {
  BT709       = 1,
  Unspecified = 2,
  BT470M      = 4,
  BT470BG     = 5,   // PAL
  SMPTE170M   = 6,   // NTSC / BT.601-525
  SMPTE240M   = 7,
  Film        = 8,
  BT2020      = 9,
  SMPTE428    = 10,  // CIE XYZ (DCDM)
  SMPTE431    = 11,  // DCI-P3 (theatrical white)
  SMPTE432    = 12,  // Display P3 (D65)
  EBU3213     = 22,
};

enum class Transfer : std::uint8_t {
  BT709        = 1,
  Unspecified  = 2,
  Gamma22      = 4,
  Gamma28      = 5,
  SMPTE170M    = 6,
  SMPTE240M    = 7,
  Linear       = 8,
  IEC61966_2_4 = 11,  // xvYCC
  BT1361       = 12,
  SRGB         = 13,  // IEC 61966-2-1
  BT2020_10    = 14,
  BT2020_12    = 15,
  PQ           = 16,  // SMPTE ST 2084
  SMPTE428     = 17,
  HLG          = 18,  // ARIB STD-B67
};

enum class Matrix : std::uint8_t {
  Identity    = 0,    // RGB / GBR
  BT709       = 1,
  Unspecified = 2,
  BT470BG     = 5,
  SMPTE170M   = 6,
  SMPTE240M   = 7,
  YCgCo       = 8,
  BT2020NCL   = 9,
  BT2020CL    = 10,
  ICtCp       = 14,
};

enum class Range : std::uint8_t {
  Unspecified,
  Limited,    // "video" / "TV" range
  Full,
};

enum class AlphaMode : std::uint8_t {
  None,           // opaque
  Straight,       // unassociated: color is not multiplied by alpha
  Premultiplied,  // associated
};

// SMPTE ST 2086 mastering display color volume.
struct MasteringDisplay {
  // CIE 1931 xy chromaticities, in units of 0.00002 (as in HEVC SEI).
  std::uint16_t red_x = 0, red_y = 0;
  std::uint16_t green_x = 0, green_y = 0;
  std::uint16_t blue_x = 0, blue_y = 0;
  std::uint16_t white_x = 0, white_y = 0;
  // Luminance in units of 0.0001 cd/m2.
  std::uint32_t max_luminance = 0;
  std::uint32_t min_luminance = 0;

  bool operator==(const MasteringDisplay&) const = default;
};

// CTA-861.3 content light level.
struct ContentLight {
  std::uint16_t max_cll = 0;   // cd/m2
  std::uint16_t max_fall = 0;  // cd/m2

  bool operator==(const ContentLight&) const = default;
};

struct ColorInfo {
  Primaries primaries = Primaries::Unspecified;
  Transfer  transfer = Transfer::Unspecified;
  Matrix    matrix = Matrix::Unspecified;
  Range     range = Range::Unspecified;
  std::optional<MasteringDisplay> mastering;
  std::optional<ContentLight>     content_light;
  // Name of an embedded ICC profile when the source carried one instead
  // of (or in addition to) CICP tags -- typical for stills.
  std::string icc_name;

  bool is_hdr() const noexcept
  {
    return transfer == Transfer::PQ || transfer == Transfer::HLG;
  }
  bool is_wide_gamut() const noexcept
  {
    return primaries == Primaries::BT2020 ||
           primaries == Primaries::SMPTE431 ||
           primaries == Primaries::SMPTE432;
  }
  bool is_specified() const noexcept
  {
    return primaries != Primaries::Unspecified ||
           transfer != Transfer::Unspecified || !icc_name.empty();
  }

  // Common tags.
  static ColorInfo srgb();          // BT.709 primaries, sRGB curve, full
  static ColorInfo rec709();        // BT.709 video, limited range
  static ColorInfo display_p3();    // P3-D65, sRGB curve, full
  static ColorInfo rec2020_pq();    // HDR10
  static ColorInfo rec2020_hlg();
  static ColorInfo working();       // linear BT.2020, see file comment

  // Short label for UI and logs, e.g. "BT.2020 PQ (limited)".
  std::string describe() const;

  bool operator==(const ColorInfo&) const = default;
};

// An OUTPUT colour space -- what a project exports in (project::
// OutputSettings) -- by its id: "rec709" (BT.709 video), "srgb",
// "display-p3", "rec2020" (BT.2020, SDR), "rec2100-pq" (HDR10),
// "rec2100-hlg". nullopt for one not known.
std::optional<ColorInfo> output_color(std::string_view id);

const char* to_str(Primaries);
const char* to_str(Transfer);
const char* to_str(Matrix);
const char* to_str(Range);
const char* to_str(AlphaMode);

}

#endif
