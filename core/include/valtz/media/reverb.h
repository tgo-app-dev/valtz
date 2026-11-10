// A layer's REVERB (DESIGN §10a Audio): its sound heard in a room -- the
// span convolved with an IMPULSE RESPONSE made for it, not a recorded one.
//
// The response is made, per channel (left and right apart, so the room is
// wide), as a room rings:
//   * `pre_delay` seconds of nothing -- the first echo's way to the wall;
//   * then echoes, few and far apart at first and thickening as the sound
//     dies away -- how few, and how long they take to fill in, is the
//     room's SIZE (0..1: small, they crowd in at once; large, they come
//     apart and fill in over a third of a second);
//   * fading to -60 dB in `decay` seconds (RT60);
//   * its highs dying sooner by its DAMPING (0 as soon as the lows ... 1
//     everything above 500 Hz gone by the decay's end): two one-pole
//     low-passes whose corner falls from 20 kHz as the response goes on;
//   * scaled to an energy of 1, so the room is as loud at any decay.
// Its echoes fall at the same places whatever its decay and damping (the
// room's own seed): two responses of one room mixed are a response of it
// between them.
//
// The CONVOLUTION is uniformly partitioned, overlap-add, in blocks of
// kReverbBlock frames: each block of what goes into the room (the SEND)
// and each piece of the response FFT'd once (vDSP), the products of every
// block with every piece summed for every output block -- the work of a
// long room -- on the GPU (a Metal kernel, the bins in parallel), then
// each output block inverse-FFT'd and laid over the next. A send block
// hears the response at ITS time -- two responses mixed by its place
// between them -- so a room changed along the span changes what goes into
// it then, and the tails of what went in before ring on as they began.
// Where Metal cannot run, the same sums on the CPU.

#ifndef VALTZ_MEDIA_REVERB_H
#define VALTZ_MEDIA_REVERB_H

#include "valtz/base/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace valtz::media {

inline constexpr double kMaxPreDelay = 0.5;
// A response's length at most (its pre-delay and decay, times a speed
// the span is stretched by), seconds.
inline constexpr double kMaxReverbLength = 30.0;
inline constexpr int kReverbBlock = 4096;

// The room, the layer's own -- one for its whole span, not keyed.
struct ReverbRoom {
  double pre_delay = 0.02;  // seconds, 0..kMaxPreDelay
  double size = 0.5;        // 0..1

  bool operator==(const ReverbRoom&) const = default;
};

// One response the span hears: its decay (RT60, seconds) and damping, and
// the room's pre-delay -- each as the span's own time counts it (a span
// stretched by a speed later hears them stretched by it first).
struct ReverbVoice {
  double decay = 1.5;
  double damping = 0.5;
  double pre_delay = 0.02;

  bool operator==(const ReverbVoice&) const = default;
};

// Its length, seconds: the pre-delay and the decay, at most
// kMaxReverbLength.
double reverb_length(const ReverbVoice&);

// Channel `channel`'s response (0 left, 1 right) at `rate` Hz, its
// room's `size`: reverb_length long, its energy 1.
std::vector<float> reverb_impulse(const ReverbVoice&, double size,
                                  int channel, int rate);

// What a send block hears: response `a` and response `b` mixed, `w` of
// the way to `b` (0: `a` alone).
struct ReverbPick {
  std::uint32_t a = 0;
  std::uint32_t b = 0;
  float         w = 0;
};

// The ROOM's sound of `send` (mono, `frames` frames): left and right,
// `out_frames` long each (the send's length and a response's at least, to
// hear its tail; cut there). `responses[i]` left and right; `picks` one a
// send block of kReverbBlock frames (the last one's for any block after).
// `gpu` false: on the CPU (what the GPU's sums are checked against).
// Blocking.
struct ReverbStats {
  bool   gpu = false;   // the sums ran on the GPU
  double seconds = 0;   // the whole convolution's time
  double gpu_seconds = 0;
};
Result<std::array<std::vector<float>, 2>>
convolve_reverb(const float* send, std::size_t frames,
                const std::vector<std::array<std::vector<float>, 2>>&
                    responses,
                const std::vector<ReverbPick>& picks,
                std::size_t out_frames, bool gpu = true,
                ReverbStats* stats = nullptr);

}

#endif
