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
