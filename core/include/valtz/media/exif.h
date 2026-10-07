// EXIF: what a camera, or an editor, recorded about a still. Read with
// ImageIO, so it covers every container ImageIO reads (JPEG, HEIC, PNG's
// eXIf chunk, TIFF, camera raw), not only the ones whose bytes we could
// scan ourselves.
//
// Two uses:
//   * fields to SHOW -- kept with the media info (probe), so the app's
//     inspector and `valtzctl info` read the same thing;
//   * a block to CARRY -- an edit's result keeps its base's EXIF. The
//     engine hands the block to vpipe's save-image (its `metadata` port),
//     which writes it into the file and sets Software to who made it.

#ifndef VALTZ_MEDIA_EXIF_H
#define VALTZ_MEDIA_EXIF_H

#include "valtz/base/json.h"
#include "valtz/media/model-input.h"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace valtz::media {

// The fields worth showing, under their EXIF names: Make, Model,
// Software, DateTime, Artist, Copyright, ImageDescription;
// DateTimeOriginal, LensMake, LensModel, ExposureTime, FNumber,
// ISOSpeedRatings, ExposureBiasValue, FocalLength, FocalLenIn35mmFilm,
// ExposureProgram, MeteringMode, Flash, WhiteBalance; GPSLatitude and
// GPSLongitude (signed degrees: south and west are negative),
// GPSAltitude. Strings or numbers, as recorded (ISO is its first value);
// formatting and labels are the reader's. An empty object when there are
// none.
Json exif_fields(const std::filesystem::path&);

// `src`'s EXIF as a raw TIFF block (what JPEG's APP1 "Exif" segment and
// PNG's eXIf chunk hold) for a picture made from it: `size` pixels and
// upright -- Orientation is 1, because the pixels were turned as they
// were read, and PixelXDimension / PixelYDimension are `size`. Empty when
// `src` has none.
//
// ImageIO writes it, into a one-pixel JPEG in memory, and it is read back
// out of that file's APP1 segment: tags ImageIO does not know, maker
// notes among them, may not survive.
std::vector<std::uint8_t> exif_block_for(const std::filesystem::path& src,
                                         PixelSize size);

}

#endif
