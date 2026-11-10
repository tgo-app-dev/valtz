// Where a layer's sound SITS (DESIGN §10a Audio): its place in the
// stereo field, worked on a layer's span in the offline pass
// (media/sound.h) -- the PAN between the equalizer and the room, the
// rest after the room, in the order below.
//
//   * PAN (keyed, -1 left ... 1 right): a constant-power balance -- each
//     side's gain sqrt2 * cos / sin of a quarter turn by the pan, both 1
//     at the centre, so a centred voice is as loud hard left as in the
//     middle, the far side gone;
//   * WIDTH (keyed, 0 mono ... 1 as it is ... 2): the side (L - R) / 2
//     scaled, the middle kept -- the room's width with the source's;
//   * DISTANCE (keyed, metres, 1 as recorded ... kMaxDistance): the
//     inverse-distance law, 6 dB quieter a doubling, and the air's
//     dulling of the highs -- a one-pole low-pass from 20 kHz down by
//     d^0.6 (13 kHz at 2 m, 3.8 kHz at 16), none at 1 m;
//   * BASS MONO (the layer's, a corner in Hz; under kMinBassMono off):
//     the lows below the corner made one -- a Linkwitz-Riley crossover,
//     24 dB an octave: each side's highs, the two sides' middle's lows on
//     both -- so the lows sit in the middle and what is above keeps its
//     side (the crossover's phase on both alike, its level flat);
//   * CROSSFEED (the layer's, 0 ... 1): each ear hears the other side as
//     a head lets it through -- below 700 Hz, 0.3 ms later -- at up to
//     -6 dB, its own lows (as late) taken back by as much, so a centred
//     sound is exactly as it was and only what differs between the sides
//     is blended (for listening on headphones): at 100% a side's lows
//     reach the other about as loud as they stay on their own.
// The keyed numbers come a value a frame (the caller's keys, read along
// the span).

#ifndef VALTZ_MEDIA_SPACE_H
#define VALTZ_MEDIA_SPACE_H

#include <cstddef>
#include <span>

namespace valtz::media {

inline constexpr double kMinBassMono = 20.0;
inline constexpr double kMaxBassMono = 300.0;

// The field's settings, the layer's own -- one for its whole span.
struct SoundField {
  double bass_mono = 0;  // Hz; under kMinBassMono, off
  double crossfeed = 0;  // 0..1

  bool bass_mono_on() const { return bass_mono >= kMinBassMono; }
  bool at_rest() const { return !bass_mono_on() && crossfeed <= 0; }
  bool operator==(const SoundField&) const = default;
};

// Each works on `left` and `right` in place, as many frames as they hold
// (the two of one length); a keyed one's values one a frame, as many.
void pan_sound(std::span<float> left, std::span<float> right,
               std::span<const float> pan);
void widen_sound(std::span<float> left, std::span<float> right,
                 std::span<const float> width);
void place_sound_far(std::span<float> left, std::span<float> right,
                     std::span<const float> metres, int rate);
void mono_bass(std::span<float> left, std::span<float> right, double hz,
               int rate);
void crossfeed_sound(std::span<float> left, std::span<float> right,
                     double amount, int rate);

}

#endif
