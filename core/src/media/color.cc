#include "valtz/media/color.h"

#include <format>

namespace valtz::media {

ColorInfo
ColorInfo::srgb()
{
  return {Primaries::BT709, Transfer::SRGB, Matrix::Identity, Range::Full,
          std::nullopt, std::nullopt, {}};
}

ColorInfo
ColorInfo::rec709()
{
  return {Primaries::BT709, Transfer::BT709, Matrix::BT709, Range::Limited,
          std::nullopt, std::nullopt, {}};
}

ColorInfo
ColorInfo::display_p3()
{
  return {Primaries::SMPTE432, Transfer::SRGB, Matrix::Identity,
          Range::Full, std::nullopt, std::nullopt, {}};
}

ColorInfo
ColorInfo::rec2020_pq()
{
  return {Primaries::BT2020, Transfer::PQ, Matrix::BT2020NCL,
          Range::Limited, std::nullopt, std::nullopt, {}};
}

ColorInfo
ColorInfo::rec2020_hlg()
{
  return {Primaries::BT2020, Transfer::HLG, Matrix::BT2020NCL,
          Range::Limited, std::nullopt, std::nullopt, {}};
}

ColorInfo
ColorInfo::working()
{
  return {Primaries::BT2020, Transfer::Linear, Matrix::Identity,
          Range::Full, std::nullopt, std::nullopt, {}};
}

std::optional<ColorInfo>
output_color(std::string_view id)
{
  if (id == "rec709") {
    return ColorInfo::rec709();
  }
  if (id == "srgb") {
    return ColorInfo::srgb();
  }
  if (id == "display-p3") {
    return ColorInfo::display_p3();
  }
  if (id == "rec2020") {
    return ColorInfo{Primaries::BT2020, Transfer::BT2020_10,
                     Matrix::BT2020NCL, Range::Limited, std::nullopt,
                     std::nullopt, {}};
  }
  if (id == "rec2100-pq") {
    return ColorInfo::rec2020_pq();
  }
  if (id == "rec2100-hlg") {
    return ColorInfo::rec2020_hlg();
  }
  return std::nullopt;
}

std::string
ColorInfo::describe() const
{
  if (!is_specified()) {
    return "untagged";
  }
  if (primaries == Primaries::Unspecified &&
      transfer == Transfer::Unspecified) {
    return std::format("ICC: {}", icc_name);
  }
  std::string s = std::format("{} {}", to_str(primaries), to_str(transfer));
  if (range != Range::Unspecified) {
    s += std::format(" ({})", to_str(range));
  }
  return s;
}

const char*
to_str(Primaries p)
{
  switch (p) {
  case Primaries::BT709:       return "BT.709";
  case Primaries::Unspecified: return "unspecified";
  case Primaries::BT470M:      return "BT.470M";
  case Primaries::BT470BG:     return "BT.601-625";
  case Primaries::SMPTE170M:   return "BT.601-525";
  case Primaries::SMPTE240M:   return "SMPTE 240M";
  case Primaries::Film:        return "Film";
  case Primaries::BT2020:      return "BT.2020";
  case Primaries::SMPTE428:    return "XYZ";
  case Primaries::SMPTE431:    return "DCI-P3";
  case Primaries::SMPTE432:    return "Display P3";
  case Primaries::EBU3213:     return "EBU 3213";
  }
  return "?";
}

const char*
to_str(Transfer t)
{
  switch (t) {
  case Transfer::BT709:        return "BT.709";
  case Transfer::Unspecified:  return "unspecified";
  case Transfer::Gamma22:      return "gamma 2.2";
  case Transfer::Gamma28:      return "gamma 2.8";
  case Transfer::SMPTE170M:    return "BT.601";
  case Transfer::SMPTE240M:    return "SMPTE 240M";
  case Transfer::Linear:       return "linear";
  case Transfer::IEC61966_2_4: return "xvYCC";
  case Transfer::BT1361:       return "BT.1361";
  case Transfer::SRGB:         return "sRGB";
  case Transfer::BT2020_10:    return "BT.2020 (10-bit)";
  case Transfer::BT2020_12:    return "BT.2020 (12-bit)";
  case Transfer::PQ:           return "PQ";
  case Transfer::SMPTE428:     return "SMPTE 428";
  case Transfer::HLG:          return "HLG";
  }
  return "?";
}

const char*
to_str(Matrix m)
{
  switch (m) {
  case Matrix::Identity:    return "RGB";
  case Matrix::BT709:       return "BT.709";
  case Matrix::Unspecified: return "unspecified";
  case Matrix::BT470BG:     return "BT.601-625";
  case Matrix::SMPTE170M:   return "BT.601-525";
  case Matrix::SMPTE240M:   return "SMPTE 240M";
  case Matrix::YCgCo:       return "YCgCo";
  case Matrix::BT2020NCL:   return "BT.2020 NCL";
  case Matrix::BT2020CL:    return "BT.2020 CL";
  case Matrix::ICtCp:       return "ICtCp";
  }
  return "?";
}

const char*
to_str(Range r)
{
  switch (r) {
  case Range::Unspecified: return "unspecified";
  case Range::Limited:     return "limited";
  case Range::Full:        return "full";
  }
  return "?";
}

const char*
to_str(AlphaMode a)
{
  switch (a) {
  case AlphaMode::None:          return "none";
  case AlphaMode::Straight:      return "straight";
  case AlphaMode::Premultiplied: return "premultiplied";
  }
  return "?";
}

}
