// Encodings KEPT between jobs (DESIGN §4l): what a graph's encoders made
// -- a prompt's conditioning, an edit's reference latents, the picture a
// clip opens or ends on, a clip's reference rows -- tapped out of the
// graph by a valtz-sink as the job runs, kept in the managed cache, and
// fed back by a valtz-source to a later job made from the same, in the
// encoder's place: the encoder (H3's 32B prompt reader, Qwen-Image's VLM,
// a VAE) neither loaded nor run again, and its memory left to the DiT.
//
// THE KEY is the SHA-256 of a document naming everything the encoder's
// output is made from, as content: vpipe's build, the model's files as
// they are now (model_stamp: their names, sizes and times), the stage's
// config, the words, each picture's bytes (its hash) and how it was
// fitted. A change to any of them is another key: nothing is ever
// invalidated, an old entry is only no longer asked for, and the cache's
// LRU lets it go. What does not change an encoding -- the seed, the
// steps, the size of a picture made from words, a LoRA, the DiT -- is
// left out, so changing those reuses it.
//
// THE FILE: "VALTZENC", a version, a JSON header -- each port's tensors
// (type, shape, where their bytes are) and sidebands -- then the bytes.
// A sideband is kept in FlexData's own binary form (SinkBeat::
// sideband_bin): fed back exactly as vpipe made it, every number's kind
// and every typed array intact.

#ifndef VALTZ_ENGINE_VPIPE_ENCODING_CACHE_H
#define VALTZ_ENGINE_VPIPE_ENCODING_CACHE_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/engine/tensor.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::engine::vp {

struct SinkBeat;

// An encoder's outputs, as tapped and as fed back: a port each.
struct Kept {
  struct Port {
    std::string              name;
    bool                     list = false;  // a TensorListPayload
    std::vector<TensorPtr>   tensors;       // one, unless a list
    std::vector<std::string> sidebands;     // FlexData binary, a tensor's
    // Made in a Metal buffer (else CPU memory), as it is fed back: a
    // stage may read a beat's CPU bytes alone (H3's generate-video).
    std::vector<bool>        shared;
    std::string              list_sideband; // a list's own
  };
  std::vector<Port> ports;

  std::uint64_t bytes() const;
};

// A port of a Kept from the beat its tap delivered (a list's items in
// order): its bytes COPIED as it arrives -- a producer's storage is not
// the tap's to hold.
Kept::Port kept_port(std::string name, const SinkBeat& beat);

// "enc-<what>-<sha256 hex of made_from>": a managed-cache key.
std::string encoding_key(std::string_view what, const Json& made_from);

// The model's files as they are: each file under `dir` (links followed)
// by its path there, size and modification time, hashed -- part of every
// key, so a model downloaded again, compressed or replaced is another
// model. "" when `dir` cannot be read (nothing then is kept for it).
std::string model_stamp(const std::filesystem::path& dir);
// ...and one file's (a part's weights linked apart: a Draw Things
// checkpoint): its path, size and modification time, hashed.
std::string file_stamp(const std::filesystem::path& file);

Status       write_kept(const std::filesystem::path&, const Kept&);
Result<Kept> read_kept(const std::filesystem::path&);

}

#endif
