// RefMods (DESIGN §4s): MiniMax H3 REFERENCE LATENTS kept in a
// .safetensors, as ComfyUI-MiniMaxH3Mod writes them -- pictures and clips
// VAE-encoded once by H3's video VAE (several stacked along time), a sound
// by its audio VAE -- so a generation reads the latents instead of
// encoding the references again, and a compressed one (pooled to a small
// grid) costs the DiT a fraction of the tokens.
//
// THE FORMAT. The header's `__metadata__` holds `refmod_meta`, JSON
// (`audio_refmod_meta` in H3AudioMod's audio files). VERSION 5, the main
// target, is a BUNDLE -- {"_format_version": 5, "kind": "bundle", "name",
// "members": [...]}, member i's latent the tensor `ref_<i>` -- each member
// an ordinary reference's metadata. Earlier versions (2-4) hold ONE
// reference, its metadata the member's own, its tensor `latent`. A visual
// latent is [1, 24, T, H, W] (T 1 for "image"; H and W even, the DiT's
// 2x2 patch); an audio one [1, 32, 2, T] at 32 kHz, 40 latents a second.
// Both whitened, the VAE's posterior mean -- the space vpipe's H3 encoders
// write and its DiT reads (a refmod decodes through vpipe's VAE as the
// picture it was made from). Stored F16; BF16 and F32 read too.
//
// TOLD BY ITS TENSORS, not its name: a .safetensors is a RefMod when its
// tensors are exactly `ref_0` ... `ref_<n-1>` (or `latent` alone) in those
// layouts and its metadata says what they are -- a LoRA, a DiT or a VAE
// has hundreds of named weights and none of this.
//
// MADE (make_refmod) as the pack's creators make one: each source's latent
// either kept (Full Reference, "encode") or average-pooled to a grid of
// the first source's aspect and refined -- Adam on the small latent so its
// trilinear enlargement matches the full one, no model involved
// (Compressed Reference, "training", the pack's default); stacked along
// time, fitted to a token budget (near-duplicate latent frames dropped,
// then the rest resampled); each sound a member of its own. Always
// written as version 5.

#ifndef VALTZ_MEDIA_REFMOD_H
#define VALTZ_MEDIA_REFMOD_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace valtz::media {

struct RefModMember {
  std::string name;
  std::string kind;     // "image" | "video" | "audio"
  std::string tensor;   // "ref_<i>" (a bundle's) or "latent"
  std::string dtype;    // "F16" | "BF16" | "F32"
  std::vector<std::int64_t> shape;   // as stored, the batch axis first
  std::uint64_t offset = 0;          // its bytes, from the file's start
  std::uint64_t bytes = 0;
  Json meta;            // the member's own metadata, as written

  bool visual() const { return kind != "audio"; }
  // The latent's extents: T, and a visual one's H and W (0 for audio).
  int frames() const;
  int height() const;
  int width() const;
  // What it costs the DiT: T x (H/2) x (W/2), or 2 T for a sound.
  std::uint64_t tokens() const;
  // A sound's length: T / 40 s (0 for a visual one).
  double seconds() const;
};

struct RefMod {
  std::string name;
  int         version = 0;   // 5 a bundle; 2-4 one reference
  std::vector<RefModMember> members;
  Json        meta;          // refmod_meta as read

  std::uint64_t tokens() const;
  int visuals() const;
  int sounds() const;
  // {"name", "version", "tokens", "members": [{"name", "kind", "frames",
  //  "height", "width", "tokens", "seconds", "description",
  //  "concept_type", "mode"}]} -- what Valtz shows of it.
  Json to_json() const;
};

// The file read as a RefMod by its header alone: InvalidArgument where it
// is not one (no refmod metadata, other tensors -- a LoRA, a DiT), Corrupt
// where it says it is and its tensors disagree, Io where it cannot be
// read.
Result<RefMod> read_refmod(const std::filesystem::path& file);
// Its header says RefMod: what the weights classifier asks first.
bool is_refmod(const std::filesystem::path& file);

// A latent as f32, row-major.
struct Latent {
  std::vector<std::int64_t> shape;
  std::vector<float>        data;
  std::size_t count() const;
};

// A member's latent, f32, the batch axis dropped -- a visual one
// [24, T, H, W], an audio one [2, 32, T]: the stereo pair OUTER, as vpipe
// lays a soundtrack's latent (the pack's [1, 32, 2, T] transposed).
Result<Latent> read_member(const std::filesystem::path& file,
                           const RefModMember& m);

// vpipe's tensor file (save-tensor / load-tensor: "VPTENSOR", version 1,
// dtype, rank, the extents, the bytes, then the sideband's JSON) -- f32.
Status write_vptensor(const std::filesystem::path& file, const Latent& t,
                      const Json& sideband = Json());
Result<Latent> read_vptensor(const std::filesystem::path& file);

// How a RefMod is made (the pack's Create H3 RefMod / Master).
struct RefModRecipe {
  std::string name;
  // Compressed Reference ("training", the pack's default) or Full
  // Reference ("encode").
  bool compressed = true;
  int  pool_h = 16, pool_w = 16;      // the grid's long edge from these
  int  latent_frames = 16;            // a clip's latent frames, pooled
  int  refine_steps = 500;            // the pack's "Refinement Steps"
  std::int64_t max_tokens = 5120;     // the visual member's; 0 none
  int  resolution = 1024;             // the short edge encoded at
  std::string concept_type = "generic";
  std::string description;
};

// One source's full latent, [24, T, H, W] (Full Reference: every source on
// one canvas, the first's).
struct RefModSource {
  Latent z;
  bool   video = false;
};

// Writes `out`, a version-5 bundle: the visual sources stacked into one
// member (when any), each sound ([2, 32, T]) one of its own. `stop`, asked
// between refinement steps, ends it Cancelled; `progress(done, total)`
// counts the steps.
Result<RefMod> make_refmod(const RefModRecipe& r,
                           const std::vector<RefModSource>& visual,
                           const std::vector<Latent>& sounds,
                           const std::filesystem::path& out,
                           const std::function<bool()>& stop = {},
                           const std::function<void(int, int)>& progress =
                               {});

// The pieces, apart for their tests.
namespace refmod {
// The pool grid: its long edge max(pool_h, pool_w), its short one from
// `aspect` (h / w), both even (the pack's aspect_grid).
std::pair<int, int> aspect_grid(int pool_h, int pool_w, double aspect);
// PyTorch's adaptive_avg_pool3d of [C, T, H, W] to [C, t, h, w].
Latent pool(const Latent& z, int t, int h, int w);
// PyTorch's trilinear interpolate (align_corners false) of [C, t, h, w]
// to [C, T, H, W].
Latent enlarge(const Latent& small, int t, int h, int w);
// The pack's optimize_latent: `steps` of Adam (lr 0.02) on `small` so its
// enlargement matches `full` in mean squared error.
Latent refine(const Latent& small, const Latent& full, int steps,
              const std::function<bool()>& stop = {});
// The latent frames kept by the pack's temporal dedup (0.02).
std::vector<int> distinct_frames(const Latent& z);
// The pack's fit_token_budget, truncating: Unsupported where one frame
// alone is over it.
Result<Latent> fit_tokens(const Latent& z, std::int64_t budget);
// A value as the file keeps it: rounded through IEEE half.
float half(float v);
}  // namespace refmod

}  // namespace valtz::media

#endif
