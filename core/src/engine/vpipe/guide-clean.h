// A continuation's GUIDE CLEANED (DESIGN §4e): what the steps between
// vpipe's graphs do to it -- each of the guide's frames, restored by an
// image edit, given back its own large-scale tones; the cleaned frames'
// latents ramped in over the kept ones -- and the tensor files they pass
// through. Pure arithmetic on memory and files: no graph, no model.
//
// WHY THE TONES GO BACK. An edit that takes the blotches off a board
// also lifts or darkens it (a 9-level jump at the cut, measured); the
// person's ask was to take the noise a chain of continuations grows, not
// its slow drift in tone. So each restored frame keeps its own detail on
// the frame's own blur (a Gaussian of sigma 24 px): no step in tone.
//
// WHY A RAMP. The cleaned frames' latents alone make the noise vanish
// between two frames where the continuation is cut in. Mixed in by each
// latent frame's place in the guide -- the kept latents at its first
// frame, the cleaned at its last -- the guide's redone frames fade the
// noise out over their 22 frames, shown in place of the clip's own.

#ifndef VALTZ_ENGINE_VPIPE_GUIDE_CLEAN_H
#define VALTZ_ENGINE_VPIPE_GUIDE_CLEAN_H

#include "valtz/base/result.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace valtz::engine::vp {

// A tensor file as vpipe's save-tensor writes it (and load-tensor reads
// it): "VPTENSOR", u32 version, dtype, rank and a zero, u64 count, the
// rank's i64 dims, the samples, then its sideband (JSON text to the end).
struct TensorFile {
  // vpipe's dtype codes for the two Valtz reads: a latent's f32, a
  // decoded frame's f16.
  static constexpr std::uint32_t kF32 = 3;
  static constexpr std::uint32_t kF16 = 4;

  std::uint32_t             version = 1;
  std::uint32_t             dtype = kF32;
  std::vector<std::int64_t> dims;
  std::vector<std::uint8_t> bytes;
  std::string               sideband;

  std::int64_t count() const;
  std::size_t  sample_bytes() const;  // 4 or 2; 0 for another dtype
};

Result<TensorFile> read_tensor_file(const std::filesystem::path&);
Status write_tensor_file(const std::filesystem::path&, const TensorFile&);

// Each latent frame's weight in a guide's ramp, `latents` of them laid
// as MiniMax H3's video VAE lays a tail -- [1, 4, 4, 4, 4] from its first
// frame, chunk after chunk: by the middle of the frames each stands for,
// 0 at the first's, 1 at the last's. (A 22-frame guide: 0, 0.128, 0.333,
// 0.538, 0.744, 0.872, 1.)
std::vector<double> guide_ramp_weights(std::int64_t latents);

// The guide carried: `kept` -- a clip's kept latents, [C, T, H, W] f32,
// its window from latent frame `start` -- and `cleaned`, the cleaned
// frames' latents ([C, n, H, W] f32, the window's shape), mixed latent
// frame by latent frame, (1 - w) kept + w cleaned -- each w
// guide_ramp_weights' with `ramp`, else 1 (the cleaned alone) -- written
// to `out` with the cleaned file's sideband.
Status ramp_guide(const std::filesystem::path& kept, std::int64_t start,
                  const std::filesystem::path& cleaned,
                  const std::filesystem::path& out, bool ramp);

// `detail`'s fine detail on `tone`'s large-scale tones: detail -
// blur(detail) + blur(tone), in place, each a picture's planes [3, h, w]
// of sRGB-encoded samples, 0..1 (clamped back to it). The blur is a
// Gaussian of `sigma` px, as three box blurs of the widths that make it
// up, the edges extended.
void tone_transfer(std::span<float> detail, std::span<const float> tone,
                   int width, int height, double sigma);

// Three box blurs standing for a Gaussian of `sigma` (sigma 24: widths
// 47, 47, 49) -- `plane`, one channel [h, w], blurred in place.
void gaussian_blur(std::span<float> plane, int width, int height,
                   double sigma);

}  // namespace valtz::engine::vp

#endif
