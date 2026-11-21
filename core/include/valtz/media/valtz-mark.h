// THE VALTZ MARK as media (DESIGN §10a Insert Valtz Mark): the V mark
// beside the word "valtz", and its SONIC LOGO -- what File › Insert Valtz
// Mark puts on a composition.
//
// The mark is drawn by the app's own Metal renderer (app/macos/Sources/
// Mark), which is Swift: tools/valtz-mark, a process of its own, draws it
// -- the light sweep once, a clip; or settled, a still -- so valtzctl can
// make it too, and it is drawn at each size asked, never scaled. The
// sound is made here: a piano, synthesized.

#ifndef VALTZ_MEDIA_VALTZ_MARK_H
#define VALTZ_MEDIA_VALTZ_MARK_H

#include "valtz/base/rational.h"
#include "valtz/base/result.h"

#include <filesystem>
#include <vector>

namespace valtz::media {

// How long the mark's clip and its sound run: the sweep's second, then
// the chord ringing out.
inline constexpr double kMarkSeconds = 1.5;

// The SONIC LOGO, interleaved stereo at `rate`, kMarkSeconds long: a
// piano's A7(no3) -- A3 E4 G4 -- at 0, resolving at 350 ms to D/A -- A3
// D4 F#4, the first chord damped as the second is struck -- then faded
// out. Synthesized (partials of struck strings, three a note, detuned;
// the hammer's knock), peaking at -3 dBFS.
std::vector<float> sonic_logo(int rate);
// It written as a 32-bit float WAV.
Status write_sonic_logo(const std::filesystem::path& wav, int rate = 48000);

// The mark drawn (tools/valtz-mark): `frames` 0 a still (.png, its
// transparency kept), else a clip of that many frames at `fps` (.mov,
// ProRes 4444 with alpha) -- `sound` joined to it when given. The mark
// and the word fill the picture as one lockup, centred. Blocking.
struct MarkRender {
  int                   width = 0;
  int                   height = 0;
  int                   frames = 0;
  Rational              fps{24, 1};
  std::filesystem::path out;
  std::filesystem::path sound;
};
Status render_valtz_mark(const MarkRender&);

// Where tools/valtz-mark is ($VALTZ_MARK_HELPER, beside the executable,
// its Contents/Helpers, the build's); empty when nowhere.
std::filesystem::path valtz_mark_helper();

}  // namespace valtz::media

#endif  // VALTZ_MEDIA_VALTZ_MARK_H
