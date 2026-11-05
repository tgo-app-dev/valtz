// A file handed out names the software that wrote it (DESIGN §7 Export).
//
// vpipe names a picture's maker only when a model made it (save-image's
// Software, "... with <model>"): a photo exported, a timeline written, a
// song cut came out saying nothing of Valtz -- or, a photo, still its
// camera's firmware. Every export, and every picture shared or dragged
// out, is stamped once written, in the place its format keeps it:
//
//   PNG    the eXIf block's Software (made when there is none) and a
//          tEXt "Software" chunk -- chunks rewritten, the pixels' bytes
//          as they were (ImageIO's lossless copy keeps a PNG's old tags);
//   JPEG   its EXIF block's Software set in the block itself -- a block
//          made, an APP1 segment, where it has none -- the bytes around
//          it as they were; then XMP's CreatorTool by ImageIO's lossless
//          copy. ImageIO's copy alone gives a JPEG without EXIF XMP only,
//          and leaves an IFD 0 of Software beside the Exif pointer, which
//          it then does not read: Preview and Spotlight showed nothing;
//   TIFF   TIFF Software and XMP's CreatorTool, by ImageIO's lossless
//          metadata copy (no decode);
//   OpenEXR  a "software" string attribute in the header, the chunk
//          offsets moved by what it adds (single-part scanline files, as
//          ImageIO writes them);
//   .mov   QuickTime's com.apple.quicktime.software and ©swr; .mp4, .m4v,
//          .m4a iTunes' ©too ("encoder" to ffprobe) -- every track passed
//          through as stored into a new file;
//   WAV    a LIST/INFO chunk's ISFT.
//
// A picture a model made keeps its model: "Valtz 0.1.0 build 141 (Vpipe
// 0.1 abc with Krea 2)" stays "... with Krea 2" whoever exports it after.
// What the file is not trusted to say -- an export, a composition flattened
// -- the caller names (`keep_model` false: Controller::made_with, the one
// model every layer was made with, or none).
// Never in place: a new file is renamed over the old, so a clone (APFS)
// of it shares nothing that changed.

#ifndef VALTZ_MEDIA_STAMP_H
#define VALTZ_MEDIA_STAMP_H

#include "valtz/base/result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::media {

// `file` stamped with `software` (base/version.h's software_stamp()), as
// its extension says -- with `keep_model`, the model the file names kept
// (stamped_software), else `software` as it is. Code::Unsupported for a
// kind it does not stamp (a PDF is stamped as it is written: write_pdf's
// creator), or a layout it does not read (a tiled or multi-part OpenEXR,
// an RF64 WAV): the file is left as it was either way.
Status stamp_software(const std::filesystem::path& file,
                      std::string_view software, bool keep_model = true);

// What a picture says it was written by, once stamped: `software`, the
// model `was` names kept -- "<...> (Vpipe 0.1 abc with Krea 2)" gives
// "with Krea 2" to `software`'s brackets.
std::string stamped_software(std::string_view was, std::string_view software);

// The model a writer's name names -- "<...> (Vpipe 0.1 abc with Krea 2)":
// "Krea 2" -- or "" (a camera's firmware, Valtz's own name alone).
std::string stamped_model(std::string_view was);

// `software` naming `model` in its brackets ("" names none): "<...> (Vpipe
// 0.1 abc)" with "Krea 2" is "<...> (Vpipe 0.1 abc with Krea 2)".
std::string with_model(std::string_view software, std::string_view model);

// A TIFF block's (EXIF's) Software -- tag 0x0131 in IFD 0 -- read, or
// set: the entries kept, a new IFD 0 with it appended (the old left
// unreferenced, every offset still good). nullopt: no TIFF block. An IFD
// 0 of Software and no other TIFF field (alone, or beside the pointers to
// Exif's or GPS's IFD) ImageIO ignores -- Preview, Spotlight with it: one
// that would be is given Orientation 1 (as stored) beside it.
std::optional<std::string> tiff_software(const std::vector<std::uint8_t>&);
std::optional<std::vector<std::uint8_t>>
tiff_with_software(const std::vector<std::uint8_t>& block,
                   std::string_view software);

}

#endif
