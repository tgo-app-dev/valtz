// The contract between Valtz and its vpipe plugin, valtz-vpipe.so.
//
// The plugin adds two stages that move data across the boundary through
// vpipe's STAGE COMMANDS (vpipe docs/STAGE-COMMANDS.md), by reference in
// both directions:
//
//   valtz-source  Valtz -> graph. `lease` hands Valtz a writable beat in
//                 the engine's own memory (a Metal shared buffer, so a
//                 GPU writer can fill it too); closing the lease emits it
//                 with no copy. `push` copies a Valtz tensor into a beat,
//                 `emit` sends a JSON beat, `finish` ends the stream.
//   valtz-sink    graph -> Valtz. Every beat that reaches it is queued
//                 and handed out by `next`, IN PLACE: the reply's buffer
//                 is the beat's own bytes, kept alive by the buffer's
//                 owner. A sink has no downstream, so nothing waits on
//                 the caller -- the reply never holds the pipeline, and
//                 the sink never writes or reuses a beat it handed out.
//                 Its bytes stay as replied for as long as any owner
//                 lives: after close, after stop, after unload.
//
// Beside them, valtz-tap (frames counted as they go by, so a host feeds
// by what has come out) and valtz-video-summary (a clip watched by a
// vision-language model, scene by scene) -- stages of Valtz's own that
// need nothing vpipe does not export.
//
// Both sides compile this header -- the plugin declaring and serving the
// commands, Valtz's engine sending them -- so a renamed key is a compile
// error on both sides instead of a silent default on one. It includes
// nothing from vpipe on purpose: it is vocabulary, not API.

#ifndef VALTZ_VPIPE_EXCHANGE_H
#define VALTZ_VPIPE_EXCHANGE_H

namespace valtz::exchange {

// ---- the plugin -----------------------------------------------------------

inline constexpr char kPluginName[] = "valtz";
inline constexpr char kPluginFile[] = "valtz-vpipe.so";

inline constexpr char kSourceType[] = "valtz-source";
inline constexpr char kSinkType[] = "valtz-sink";

// The one buffer every command names.
inline constexpr char kBufData[] = "data";

// ---- valtz-sink ------------------------------------------------------------

// Config. `policy`:
//   queue   keep every beat. At `depth` queued the sink stops reading,
//           so the producer backs up and nothing is lost (results,
//           streamed text, video frames).
//   latest  keep the newest `depth`; an older beat is dropped as a newer
//           one arrives, and the producer never waits (live previews).
// `drain` (default true): at the end of the stream, hand out what is
// queued, and end only once a `next` has been ANSWERED with `eos` -- so a
// reader always learns the end from a reply, never from a refusal by a
// stage that already ended. false: drop the queue and end with the
// input (a graph no host reads).
inline constexpr char kSinkPolicy[] = "policy";
inline constexpr char kSinkPolicyQueue[] = "queue";
inline constexpr char kSinkPolicyLatest[] = "latest";
inline constexpr char kSinkDepth[] = "depth";
inline constexpr char kSinkDrain[] = "drain";

// `next`: the oldest queued beat, waiting for one when the queue is
// empty. Replied with `eos` once the stream has ended and the queue is
// drained. Result keys below; the tensor, if any, is buffer `data`.
inline constexpr char kCmdNext[] = "next";
// `stats`: counters, for diagnostics and tests.
inline constexpr char kCmdStats[] = "stats";

// ---- valtz-tap -------------------------------------------------------------
// A passthrough that COUNTS the frames going by -- a [F, C, H, W] tensor
// beat is F of them, any other tensor beat one, anything else none -- so
// the host can feed a graph by what has COME OUT of it (an upscaler
// holding one chunk at a time, DESIGN §4f), and that CUTS the stream at
// `max_frames` (0: never): a beat past it is dropped, not passed. Beats
// go on by reference, never copied; the tap's own port backpressures as
// its consumer does.
inline constexpr char kTapType[] = "valtz-tap";
inline constexpr char kTapMaxFrames[] = "max_frames";
// `wait` {frames} -> replied once that many frames have gone by, or the
// stream ended first (`eos`): {passed, cut, eos}. Never holds the
// pipeline: the reply waits, the beats do not.
inline constexpr char kCmdWait[] = "wait";
inline constexpr char kFrames[] = "frames";
inline constexpr char kPassed[] = "passed";   // uint: frames gone by
inline constexpr char kCut[] = "cut";         // uint: frames dropped
// (`stats`, as the sink's: {passed, cut, eos}.)

// ---- valtz-video-summary ---------------------------------------------------
// A clip SUMMARIZED by a vision-language model (DESIGN §4i), scene by
// scene. Its frames come in as Valtz decodes them -- planar 8-bit RGB
// [3, h, w], sparse and small, each with its time in the clip (sideband
// `pts_us`, vpipe's own key). They are cut into SCENES where the picture
// changes (src/scene-cut.h) or a scene runs `scene_frames` long; each
// scene's frames go through the model as one video, and what it says
// leaves as a beat. At the end of the stream, the clip in a paragraph,
// written from the scenes' words. Port 1 (optional): a sampler-select's
// spec; unwired, greedy.
inline constexpr char kSummaryType[] = "valtz-video-summary";
// Config: the model (as text-chat takes it: its directory, a drafter
// shipped apart, a DFlash drafter and its bits -- the same as the helper's
// chat, so a model kept warm by one is handed to the other), how long it
// stays loaded after (s), its buffers wired as they load.
inline constexpr char kSummaryModel[] = "hf_dir";
inline constexpr char kSummaryMtpModel[] = "mtp_model";
inline constexpr char kSummaryDraftModel[] = "draft_model";
inline constexpr char kSummaryDraftBits[] = "draft_bits";
inline constexpr char kSummaryKeepLoaded[] = "keep_loaded";
inline constexpr char kSummaryWireWeights[] = "wire_weights";
// The K/V pages, as the chat sizes them (the cache key: shared only
// alike).
inline constexpr char kSummaryPageTokens[] = "page_tokens";
inline constexpr char kSummaryMaxPages[] = "max_pages";
// What the model is told. `scene_prompt` follows a scene's frames,
// `overall_prompt` the scenes' words; in them {start} {end} (m:ss),
// {every} (seconds between frames), {language} and -- the overall's --
// {scenes} (a line each: "[m:ss-m:ss] words") are filled in.
inline constexpr char kSummaryScenePrompt[] = "scene_prompt";
inline constexpr char kSummaryOverallPrompt[] = "overall_prompt";
inline constexpr char kSummaryLanguage[] = "language";  // "English", ...
inline constexpr char kSummaryEvery[] = "every";        // s between frames
// A scene's frames at most (a longer one goes on in the next, its last
// frame shown again first) and at least (but at the end), and the
// distance that may cut (scene-cut.h).
inline constexpr char kSummarySceneFrames[] = "scene_frames";
inline constexpr char kSummaryMinFrames[] = "min_frames";
inline constexpr char kSummaryCutAt[] = "cut_threshold";
inline constexpr char kSummaryMaxTokens[] = "max_new_tokens";
inline constexpr char kSummaryOverall[] = "overall";    // bool
// Out: a data beat a scene, {kind: "scene", index, start, end (s), frames,
// at_cut (it ended where the picture changed), text}; then, for a clip of
// two scenes or more, one {kind: "overall", text}.
inline constexpr char kSummaryKindScene[] = "scene";
inline constexpr char kSummaryKindOverall[] = "overall";
inline constexpr char kIndex[] = "index";
inline constexpr char kStart[] = "start";
inline constexpr char kEnd[] = "end";
inline constexpr char kAtCut[] = "at_cut";
inline constexpr char kText[] = "text";

// ---- valtz-source ----------------------------------------------------------

// `lease` {type, shape, sideband?, storage?} -> writable buffer `data`;
// the beat is emitted when the command is closed (dropped if the
// pipeline stops first).
inline constexpr char kCmdLease[] = "lease";
// `push` {sideband?} + buffer `data` -> a copy emitted as one beat.
inline constexpr char kCmdPush[] = "push";
// `emit` {data} -> the JSON value emitted as one data beat.
inline constexpr char kCmdEmit[] = "emit";
// `finish` -> end of stream.
inline constexpr char kCmdFinish[] = "finish";

// ---- argument and result keys -----------------------------------------------

inline constexpr char kEos[] = "eos";            // bool: stream ended
inline constexpr char kSeq[] = "seq";            // uint: 1-based index
inline constexpr char kKind[] = "kind";          // see kKind* below
inline constexpr char kDropped[] = "dropped";    // uint: see below
inline constexpr char kType[] = "type";          // "u8", "f16", ...
inline constexpr char kShape[] = "shape";        // [int]
inline constexpr char kStrides[] = "strides";    // [int] BYTES; [] = C order
inline constexpr char kSideband[] = "sideband";  // the beat's sideband
inline constexpr char kData[] = "data";          // a data beat's value
inline constexpr char kDescribe[] = "describe";  // any other beat's name
inline constexpr char kStorage[] = "storage";    // kStorage* below
// In-process only: the MTL::Buffer* holding the bytes (as an integer)
// and the byte offset of buffer `data` in it; 0 when the bytes are not
// in a Metal buffer. Valid exactly as long as the buffer's owner is.
inline constexpr char kMtlBuffer[] = "mtl_buffer";
inline constexpr char kMtlOffset[] = "mtl_offset";

// `dropped` on a `next` reply: beats the sink dropped (policy `latest`)
// since the previous reply. On `stats`: in total.
inline constexpr char kReceived[] = "received";
inline constexpr char kDelivered[] = "delivered";
inline constexpr char kQueued[] = "queued";

inline constexpr char kKindTensor[] = "tensor";
inline constexpr char kKindData[] = "data";
inline constexpr char kKindOther[] = "other";

inline constexpr char kStorageCpu[] = "cpu";
inline constexpr char kStorageShared[] = "shared";

}

#endif
