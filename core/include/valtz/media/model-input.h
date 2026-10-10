// Stills into a MODEL's input space, at the model boundary (DESIGN §7).
//
// Models declare their own space, and for the image models Valtz runs it
// is planar [C,H,W], sRGB-encoded BT.709 0..1, straight alpha -- what
// vpipe's load-image emits, but colour-managed: a P3 photo, a 16-bit PNG
// or an ICC-tagged scan is converted from whatever it declared. A camera
// RAW is DEVELOPED (Core Image's RAW engine, the camera's own look), the
// EXIF orientation applied, the user's adjustments laid on top (DESIGN
// §10a: what the stage showed is what the model gets), and the result
// scaled to exactly the size the graph asked for -- written straight into
// the caller's buffer, which for the engine is a vpipe beat leased
// through valtz-source, so the decoded pixels are never copied again.
// F16 keeps what is deeper than 8 bits (16-bit, float, RAW) all the way
// into vpipe.
//
// One Core Image pass on the GPU, read back into the lease. Blocking;
// call off the main thread.

#ifndef VALTZ_MEDIA_MODEL_INPUT_H
#define VALTZ_MEDIA_MODEL_INPUT_H

#include "valtz/base/result.h"
#include "valtz/media/adjust.h"
#include "valtz/media/crop.h"
#include "valtz/media/format.h"
#include "valtz/media/keyframes.h"
#include "valtz/media/layers.h"
#include "valtz/media/timing.h"

#include <CoreVideo/CVPixelBuffer.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace valtz::media {

// How a still fills a target of another aspect ratio.
enum class Fit : std::uint8_t {
  Crop,     // cover the target, centred; the overhang is cut off
  Stretch,  // scale each axis on its own
};

// The size a still DISPLAYS at: its pixel size with the EXIF orientation
// applied (a portrait phone photo stored landscape reports portrait).
Result<PixelSize> oriented_size(const std::filesystem::path& src);

// The sample type decode_planar writes: u8 (0..255) or half float
// (0..1, IEEE binary16 -- vpipe's F16 picture).
enum class Sample : std::uint8_t { U8, F16 };

// A Metal buffer the planes can be written into ON THE GPU -- a vpipe
// lease in shared memory (SourceWriter::Lease::mtl_buffer): Core Image
// draws into a texture and a compute kernel writes the planes straight
// into the buffer, in one command buffer, so no pixel passes through the
// CPU. `buffer` is an id<MTLBuffer> (MTL::Buffer*); null, or a decode the
// GPU cannot take (u8, an offset Metal will not bind), draws into `dst`
// on the CPU -- the same samples.
struct GpuTarget {
  void*       buffer = nullptr;
  std::size_t offset = 0;
};

// The space it writes in. Srgb: a model's input -- sRGB-encoded, clamped
// to 0..1, a RAW in the camera's look. Linear: for a file that keeps
// light (OpenEXR) -- extended LINEAR sRGB (BT.709 primaries), nothing
// clamped, a RAW developed scene-linear (no tone curve, its highlight
// headroom kept).
enum class Space : std::uint8_t { Srgb, Linear };

// Decode `src` into `dst` as planar [channels, height, width], sRGB-
// encoded, 0..1 (clamped -- the model's range), straight alpha: a RAW
// developed, upright, `adjust` applied in linear light, then `crop`
// (media/crop.h: the picture placed on its canvas, padded), scaled to
// `size` per `fit` -- for a crop, `size` is the CANVAS's shape.
// `channels` is 3 (RGB: alpha is dropped, transparent pixels read as
// black, as with vpipe's load-image) or 4. `dst_size` is in bytes, and
// must hold channels * width * height samples.
Status decode_planar(const std::filesystem::path& src, PixelSize size,
                     int channels, Fit fit, const Adjustments& adjust,
                     Sample sample, void* dst, std::size_t dst_size,
                     Space space = Space::Srgb, const Crop& crop = {},
                     const GpuTarget& gpu = {});

// A CLIP's frames, each with its look -- the adjustment and crop tracks
// (media/keyframes.h) at its own frame -- for a writer: read from `src`
// (frames [first, first + count) at `rate`; count < 0 runs to the end),
// adjusted and cropped as a still is, scaled to `size`, and written as
// planar F16 [3, h, w] in the clip's OWN colour (as it declares it; not
// clamped) into the buffer `target` hands out for that frame, then
// `done` -- one frame at a time, in order. The track's own frame
// numbers count from the clip's start. Blocking.
//
// A clip is read in its decoder's own format -- YUV at its depth (8-bit
// 4:2:0 is 1.5 bytes a pixel, F16 RGBA 8), RGBA only with alpha -- so
// a long 4K clip, or two, decodes in a few frames' memory: what
// AVFoundation reads ahead is small too.
struct FrameBuffer {
  void*       data = nullptr;
  std::size_t size = 0;
  GpuTarget   gpu;  // where the GPU can write it, if it can
};
using FrameTarget = std::function<Result<FrameBuffer>(std::int64_t)>;
using FrameDone = std::function<Status(std::int64_t)>;

// A frame drawn SMALL for the screen as it is written -- a long export's
// preview on the stage: sRGB, 8-bit, planar [3, h, w], its long edge at
// most `edge` px. `wants` is asked for each frame (it knows when the
// next is due); `take` gets the picture. A preview costs a scaled draw,
// never the frame's own.
struct PreviewPicture {
  std::int64_t              frame = 0;
  PixelSize                 size;
  std::vector<std::uint8_t> rgb;  // planar, 3 planes
};
struct FramePreview {
  int                                   edge = 480;
  std::function<bool(std::int64_t)>     wants;
  std::function<void(PreviewPicture&&)> take;
};

// `sample`: F16 (the default), or 8-bit -- an upscaler's source, which
// its projection reads in bytes.
Status decode_movie(const std::filesystem::path& src,
                    const KeyedAdjustments& adjust, const KeyedCrop& crop,
                    Rational rate, std::int64_t first, std::int64_t count,
                    PixelSize size, const FrameTarget& target,
                    const FrameDone& done,
                    const FramePreview* preview = nullptr,
                    Sample sample = Sample::F16);

// The clip's frame showing at `seconds` (or a key frame within half a
// second of it: fast, for a preview), small as a FramePreview draws it.
// What an export vpipe reads by itself shows while it is written.
Result<PreviewPicture> preview_movie_frame(const std::filesystem::path& src,
                                           double seconds, int edge);

// A clip READ SPARSE, for a model that watches it (a video summary,
// DESIGN §4i): the frame showing at every `every` seconds from its start
// -- the first frame at or after each mark -- upright (the track's
// transform applied), in sRGB, 8-bit, planar [3, h, w] at `size`, into
// the buffer `target` hands out for its index and its time in the clip,
// then `done`. Every frame is decoded (a file is read in order), only the
// marked ones drawn. Blocking.
using SampleTarget =
    std::function<Result<FrameBuffer>(std::int64_t, double)>;
Status sample_movie(const std::filesystem::path& src, double every,
                    PixelSize size, const SampleTarget& target,
                    const FrameDone& done);

// The size sample_movie draws a clip at: its upright shape within `most`
// -- its long edge at most the larger of the two, its short at most the
// smaller -- each side a multiple of `align` (a vision tower's patches
// and their merge: 32 for Qwen3-VL), never past `most`.
Result<PixelSize> sampled_size(const std::filesystem::path& src,
                               PixelSize most, int align);

// A COMPOSITION's layers on its timeline (DESIGN §6a), as a writer or the
// screen draws them: bottom first -- clips, pictures, markup, sounds --
// each with its own tracks of keyframes (adjust, crop, turn) counted from
// its own start, where it lies in time (`timing`: its start, its length,
// its source time at each moment, its speed), and the canvas it is shown
// on. A clip shows its frame at its source time; a picture shows while it
// is on; a SOUND (`audio_only`) draws nothing. TRANSITIONS weigh two
// overlapping layers (media/timing.h). A stack with a frame of its own
// (StackCanvas::framed) places every layer on it; one from before has its
// bottom clip set the frame, through its crop, as a still's does.
struct MovieLayer {
  std::string           id;     // its layer's (not written: a job's stack
                                // has no use for it)
  std::filesystem::path file;   // a clip, a still, a markup PNG, a sound
  DrawnPicture          drawn;  // markup drawn in memory, in place of it
  bool                  video = false;
  bool                  audio_only = false;
  KeyedAdjustments      adjust;
  KeyedCrop             crop;
  bool                  visible = true;
  bool                  mask = false;
  // Its place in time; the default -- from the start, forever, its source
  // at the timeline's time -- is how a stack from before plays.
  LayerTiming           timing;
};

struct MovieStack {
  std::vector<MovieLayer>     layers;
  StackCanvas                 canvas;
  // The timeline's length, in frames at its rate; 0: the latest end among
  // its timed layers (one from before: its bottom clip's own).
  std::int64_t                frames = 0;
  // Its frame rate; 0: the one it is drawn at (the bottom clip's).
  Rational                    rate{0, 1};
  std::vector<TransitionSpec> transitions;
};

// {"layers": [{"file", "video", "audio_only", "adjust", "crop", "visible",
//  "mask", "timing"}], "canvas", "frames", "rate_num", "rate_den",
//  "transitions"} -- files only: a layer drawn in memory is not written (a
// job's stack names files).
Json to_json(const MovieStack&);
MovieStack movie_stack_from_json(const Json&);

// The timeline's frames: as set, else the latest end among its timed
// layers, else (a stack from before) the bottom clip's own count.
Result<std::int64_t> timeline_frames(const MovieStack&, Rational rate);

// The stack written as a MOVIE -- `size`, at `rate`, ProRes 422 HQ in a
// .mov -- with `sound` (a file; empty: none) as its soundtrack: a nested
// composition's rendering, a clip-like reference's, a flattened one.
// Blocking.
Status encode_movie_stack(const MovieStack& stack, Rational rate,
                          PixelSize size,
                          const std::filesystem::path& sound,
                          const std::filesystem::path& out);

// The frame of `src` (a clip) showing at `seconds`, written as a 16-bit
// PNG: a still composition's layer showing a frame of a clip.
Status write_movie_frame(const std::filesystem::path& src, double seconds,
                         const std::filesystem::path& out);

// One frame of the stack (timeline frame `frame`), written as a 16-bit
// PNG at the stack's own size.
Status write_stack_frame(const MovieStack& stack, Rational rate,
                         std::int64_t frame,
                         const std::filesystem::path& out);

// A clip's STACK, frame by frame, for a writer -- decode_movie's way:
// timeline frames [first, first + count) (count < 0: to the end), each
// layer at its own frame, composed, on the canvas, scaled to `size`, in
// the bottom clip's colour, as planar F16 [3, h, w] into the buffer
// `target` hands out (on the GPU when it is a Metal buffer), then
// `done`. Blocking.
// `color`: the space to draw in -- an export's (a project's output) --
// else the bottom clip's.
Status decode_movie_stack(const MovieStack& stack, Rational rate,
                          std::int64_t first, std::int64_t count,
                          PixelSize size, const FrameTarget& target,
                          const FrameDone& done,
                          const FramePreview* preview = nullptr,
                          const ColorInfo* color = nullptr);

// A picture as `color` -- a project's output colour (color.h
// output_color) -- written as a 16-bit PNG carrying that space's profile:
// what a still project exports from.
Status convert_picture(const std::filesystem::path& in,
                       const std::filesystem::path& out,
                       const ColorInfo& color);

// A clip's stack drawn for the SCREEN, frame after frame -- what a
// player's compositor asks for. Its stills are read once; a frame takes
// the clips' frames as the player decoded them (one per VIDEO layer, in
// order; null where a clip has none there), composes them as a writer
// does, places them on the canvas, and draws them into `out` on the GPU
// -- scaled to `out`'s size: a player asks for no more pixels than the
// screen shows (a 4K canvas on a 1500-pixel stage is drawn at 1500).
class StackRenderer {
public:
  static Result<std::unique_ptr<StackRenderer>> make(MovieStack stack,
                                                      Rational rate);
  ~StackRenderer();

  // What it draws: the canvas, else the bottom's frame through its crop.
  PixelSize size() const;
  // The timeline's frames, and the clips the video layers show, in order.
  std::int64_t frames() const;
  std::vector<std::filesystem::path> clips() const;

  Status render(std::int64_t frame, const std::vector<CVPixelBufferRef>& clips,
                CVPixelBufferRef out) const;
  // Frame `frame` as a picture (its clips read from their files): a
  // poster, for the stage's layout. Retained: the caller releases it.
  Result<CGImageRef> still(std::int64_t frame) const;

private:
  StackRenderer();
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

// decode_planar, u8, unadjusted.
Status decode_planar_srgb(const std::filesystem::path& src, PixelSize size,
                          int channels, Fit fit, std::uint8_t* dst,
                          std::size_t dst_size);

}

#endif
