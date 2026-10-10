// The model catalog: which checkpoints Valtz can offer, and for what.
//
// Built from core/resources/model-catalog.json, compiled into the binary
// (a catalog that can go missing from disk is a support problem), with
// each EXTENSION's contribution laid over it (Catalog::add, DESIGN §8a):
// one schema and one reader for both, the built-in catalog read strictly
// (a mistake there is a build mistake), an extension's tolerantly (a
// mistake there costs the entry, not the app).

#ifndef VALTZ_MODELS_CATALOG_H
#define VALTZ_MODELS_CATALOG_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::models {

enum class Capability : std::uint8_t {
  TextToImage,
  ImageEdit,
  TextToVideo,
  ImageToVideo,
  ReferenceToVideo,
  UpscaleImage,
  UpscaleVideo,
  LivePreview,     // TAE decode of in-flight latents
  PromptEnhance,
  Intent,          // classify what the user is asking for
  Caption,         // describe an image (VLM)
  AlphaOutput,     // generates with transparency
  AudioOutput,     // video with a soundtrack
  TextToAudio,     // a sound -- a song -- from words (YuE2)
  TextToSpeech,    // speech from text, a voice cloned (MOSS-TTS)
  Transcribe,      // speech to text, sound events heard (Qwen3-ASR)
  VideoSummary,    // a clip told scene by scene (a VLM: Qwen3.5)
};

inline constexpr Capability kAllCapabilities[] = {
  Capability::TextToImage,   Capability::ImageEdit,
  Capability::TextToVideo,   Capability::ImageToVideo,
  Capability::ReferenceToVideo, Capability::UpscaleImage,
  Capability::UpscaleVideo,  Capability::LivePreview,
  Capability::PromptEnhance, Capability::Intent,
  Capability::Caption,       Capability::AlphaOutput,
  Capability::AudioOutput,   Capability::TextToAudio,
  Capability::TextToSpeech,  Capability::Transcribe,
  Capability::VideoSummary,
};

const char* to_str(Capability);
const char* label(Capability);  // UI text
std::optional<Capability> capability_from_str(std::string_view);

struct ModelEntry {
  std::string              id;
  std::string              name;
  std::string              family;
  // assistant|image|video|audio|upscale|preview|encoder|lora|branch|vae
  std::string              role;
  std::string              hf_path;
  // A folder inside the repo that IS this model (catalog `subdir`): one
  // repo can publish several -- MiniMax-H3's FL2VA and Ref2VA partitions.
  // Installed means weights under <root>/<hf_path>/<subdir>.
  std::string              subdir;
  // ONE FILE in the repo that IS this model (catalog `file`): a repo can
  // hold several single-file models side by side -- madebyollin/taehv
  // holds taeh3 and taew2_1, a LoRA repo its adapters. Installed means
  // that file exists, and the engine is handed the file, not the folder
  // (which would name more than one).
  std::string              file;
  // vpipe model-fetch's `model_variant` (catalog `fetch_variant`): which
  // of a repo's models a download pulls -- required where one repo
  // publishes several.
  std::string              fetch_variant;
  // A CoreML archive's (vpipe-supplement's: `file` the .tar): the folder
  // model-fetch unpacks it into, beside it, which holds the package the
  // stage loads (catalog `package`).
  std::string              package;
  std::vector<Capability>  capabilities;
  double                   disk_gb = 0;
  bool                     disk_measured = false;
  std::uint32_t            min_ram_gb = 16;
  int                      rank = 0;
  // Per-capability overrides of `rank` (catalog `rank_for`): one model
  // can be the pick for one job and not another -- FLUX.2-klein is the
  // fast text-to-image model, Qwen-Image 2.1 the better editor.
  std::vector<std::pair<Capability, int>> rank_for;
  std::string              preview_with;  // id of a TAE entry
  std::vector<std::string> requires_models;
  std::string              license;
  bool                     gated = false;
  std::string              notes;
  Json                     engine = Json::object();
  // How the model reads a prompt (catalog `prompting`), whatever engine
  // runs it: `reference_tag` -- what the prompt calls an edit's n-th
  // picture ("<image{n}>") -- and `enhance` -- its makers' rewriter
  // instructions, by name, for "generate" and "edit"
  // (assist::rewrite_prompt).
  Json                     prompting = Json::object();
  // The extension it came from (its id and version); "" built in. A
  // recipe made with it records both (params "extension"), so a project
  // opened without it can say what to install.
  std::string              origin;
  std::string              origin_version;
  // A QUANTIZED VARIANT of another model (catalog `quantize`: {"from",
  // "bits", "group_size"}): vpipe's model-quantize makes it from that
  // one, into <download root>/<hf_path> -- nothing to download -- or it
  // is linked where one already is. It runs as its source does: what it
  // leaves out (capabilities, engine, prompting, requires, license, the
  // memory it needs, the family) is its source's.
  std::string              quantize_from;
  int                      quantize_bits = 0;
  int                      quantize_group = 0;

  bool has(Capability c) const;
  int rank_in(Capability c) const;
};

// A FAMILY, as Settings > Capabilities lists models (catalog
// `families`): what it makes -- its FEATURES, each a kind of work a
// person asks for -- and everything it runs with, each listed by a
// label of its own ("FL2VA", "6-step Turbo LoRA (larryvrh)"). Every
// catalog model is a member of one family.
struct Family {
  // "video-gen", "video-edit", "image-gen", "image-edit", "audio-gen",
  // "speech-gen", "helper", "video-upscale", "image-upscale".
  static constexpr const char* kFeatures[] = {
    "video-gen", "video-edit", "image-gen", "image-edit", "audio-gen",
    "speech-gen", "helper", "video-upscale", "image-upscale"};

  struct Member {
    std::string model;  // a catalog id
    std::string label;
  };
  std::string              id;
  std::string              name;
  std::vector<std::string> features;
  std::vector<Member>      members;
};

// The capabilities that make a feature: video-gen is text- or
// image-to-video, video-edit reference-to-video, and so on.
std::vector<Capability> feature_capabilities(std::string_view feature);

// A part of a contribution the catalog did not take: `item`
// ("model:acme-v1", "family:acme", "auto:image/generate:acme-v1",
// "skill:acme-t2v", "recognize:2", "plugin:plugins/acme.so"), `why` a
// stable code ("duplicate-id", "unknown-capability",
// "unknown-reference", "two-families", "unknown-feature", "needs-feature",
// "needs-shape", "bad-entry", "bad-path", "missing-file", "too-large"),
// `detail` English for the log.
struct Withheld {
  std::string item;
  std::string why;
  std::string detail;
};

// Who contributes: an extension ("" = the built-in catalog), where its
// files are (skills), and the language its text is read in (the UI's;
// it is fixed for a run).
struct Contributor {
  std::string           id;
  std::string           version;
  std::filesystem::path dir;
  std::string           language;
};

class Catalog {
public:
  static Result<Catalog> builtin();
  static Result<Catalog> parse(const Json&);

  // An EXTENSION's contribution laid over what is here (its manifest
  // upgraded to the current interface, ext/extension.h): models,
  // families (new ones, or members and features added to one that is
  // here), Auto's lists (appended, or each id `before` another), weight
  // recognition rules (`recognize`, ahead of those already here) and
  // enhancement skills (`skills`, read from the package). Each is checked
  // as the built-in catalog is; what does not hold is withheld and the
  // rest taken -- a capability from a newer Valtz, a broken reference or
  // a taken id costs that entry, not the package. Text (names, notes, a
  // tuning option's label) is read in `who.language`.
  std::vector<Withheld> add(const Json& contribution,
                            const Contributor& who);

  // An enhancement skill's instructions (catalog `skills`, a model's
  // `prompting.enhance`); null when there is none of that id.
  const std::string* skill(std::string_view id) const;
  // The rules that file a dropped weight file (models/tuning.h
  // classify_checkpoint), first match wins: an extension's, in load
  // order. Each carries its "origin".
  const Json& recognize_rules() const noexcept { return _recognize; }

  const std::vector<ModelEntry>& models() const noexcept { return _models; }
  const ModelEntry* find(std::string_view id) const;
  const ModelEntry* find_by_hf_path(std::string_view) const;
  // Entries serving `c`, best rank first.
  std::vector<const ModelEntry*> serving(Capability c) const;

  // AUTO: the models a modality ("image" | "video" | "audio") tries for
  // an op ("generate" | "edit"), in priority order (catalog `auto`). The
  // first that this machine can run is the pick (Controller::
  // auto_models). A list, not a ranking, so it can say what a person
  // would choose; an LLM may choose later.
  const std::vector<std::string>& auto_order(std::string_view modality,
                                             std::string_view op) const;

  const std::vector<Family>& families() const noexcept { return _families; }

private:
  // `strict`: the first problem fails the whole (the built-in catalog).
  Result<std::vector<Withheld>> merge_(const Json& doc,
                                       const Contributor& who,
                                       bool strict);
  std::vector<std::string>& auto_list_(const std::string& modality,
                                       const std::string& op);

  std::vector<ModelEntry> _models;
  std::vector<Family>     _families;
  // {modality, op} -> ids
  std::vector<std::pair<std::pair<std::string, std::string>,
                        std::vector<std::string>>> _auto;
  std::vector<std::pair<std::string, std::string>> _skills;  // id, text
  Json                    _recognize = Json::array();
};

void to_json(Json&, const ModelEntry&);

}

#endif
