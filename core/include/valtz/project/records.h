// The records a project stores.
//
// EVERYTHING IS AN ASSET. A reference photo, the text describing a
// character, a contact sheet, a generated clip and the caption an LLM
// wrote for it are all Assets, so they all get versions, all get
// dependency tracking, and all can be referenced from a Subject.
//
//   Asset         logical identity ("Alice -- turnaround sheet"), with a
//                 HEAD version and, if derived, the Recipe that makes it.
//   AssetVersion  one immutable build: the blob it produced plus, for a
//                 derived asset, the SNAPSHOT of how -- the recipe as
//                 executed (seed materialized), the exact input content
//                 hashes, the engine build and the host that ran it.
//   Recipe        immutable rule, like a makefile target's recipe: an
//                 operation, a model, parameters and inputs (by asset,
//                 following HEAD or pinned to a version). Editing a
//                 derived asset's parameters mints a NEW recipe and
//                 points the asset at it; old versions keep citing the
//                 recipe they were built with.
//   Subject       a category node (character, product, location, style)
//                 whose entries are references -- with a role -- to
//                 source or derived assets.
//   HistoryEntry  a STATE in the project's generation history: a picture
//                 as it was when a generation used it or made it,
//                 frozen.
//
// Staleness is decided by CONTENT, not time: a derived asset is stale
// when its head version was built from a different recipe, or from
// input content that is no longer what its inputs resolve to.
//
// A record KEEPS WHAT IT DOES NOT KNOW (`rest`): the keys a reader did not
// take -- a newer Valtz's fields, an extension's data under "x":
// {"<extension id>": ...} -- are written back as they were, so a project
// opened where an extension is missing loses none of it (DESIGN §5b).

#ifndef VALTZ_PROJECT_RECORDS_H
#define VALTZ_PROJECT_RECORDS_H

#include "valtz/base/hash.h"
#include "valtz/base/id.h"
#include "valtz/base/json.h"
#include "valtz/base/rational.h"
#include "valtz/media/crop.h"
#include "valtz/media/format.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace valtz::project {

// 2: the document model of DESIGN §6a (asset classes, layer time, markup
// assets, one project composition). A schema-1 project is migrated when it
// is read (project/migrate.h).
inline constexpr std::uint32_t kSchemaVersion = 2;

enum class AssetKind : std::uint8_t {
  Text,          // descriptions, prompts, captions
  Image,
  Video,
  Audio,
  ContactSheet,  // an image grid of views / frames of a subject
  Mask,
  Latent,        // cached model latents (derived only)
  Other,
};

enum class Origin : std::uint8_t {
  Source,   // provided by the user
  Derived,  // produced by a recipe
};

// What an asset IS (DESIGN §6a), beside the media it flattens to (its
// AssetKind):
//   Flat         a file as it is -- imported, or another asset flattened
//                (`Asset::from`). No look of its own.
//   Markup       a raster and vector objects at its own size
//                (`Asset::markup`); the objects editable until made pixels.
//   Generated    takes made by a model, each with its recipe; generated
//                again, a new take.
//   Still        a still composition: layers on a frame (kind Image).
//   Composition  layers on a frame and a timeline (kind Video; Audio with
//                a 0 x 0 frame).
// Only compositions have layers; the looks are their layers'.
enum class AssetClass : std::uint8_t {
  Flat,
  Markup,
  Generated,
  Still,
  Composition,
};

const char* to_str(AssetKind);
const char* to_str(Origin);
const char* to_str(AssetClass);
AssetKind asset_kind_from_str(std::string_view);
AssetClass asset_class_from_str(std::string_view);
AssetKind asset_kind_for(media::MediaType);

// A content-addressed file in the project's blob store.
struct BlobRef {
  ContentHash   hash;
  std::uint64_t size = 0;
  std::string   ext;  // file extension without the dot, e.g. "mov"

  bool empty() const noexcept { return hash.is_zero(); }
};

// A source kept in place rather than copied into the project. The
// bookmark finds the file again after a move or rename on its volume;
// size + mtime detect an edit cheaply, and only then is it re-hashed.
struct LinkInfo {
  std::string   bookmark;    // base64 macOS bookmark data
  std::uint64_t size = 0;
  std::int64_t  mtime_ns = 0;
};

// A LAYER's look and tracks (DESIGN §6a), recorded on the composition that
// holds the layer: `layer` is the layer's id. Never applied to any file's
// bytes -- only where the composition is drawn.
//   kind "adjust": the Adjust panel's sliders (media/adjust.h), only those
//                  that moved -- in a composition, a track of keyframes
//                  (media/keyframes.h).
//   kind "crop":   the Crop panel's crop and turn (media/crop.h), after
//                  the adjustments; a track in a composition.
//   kind "speed":  a composition layer's playback rate, keyed
//                  (media/timing.h).
//   kind "audio":  a composition layer's volume and pitch, keyed
//                  (media/timing.h).
//   kind "trim":   from before 2026-10-04 (a clip's marks, now the
//                  layer's own `time`); read by the migration only.
// Keys count from the layer's own start.
struct Modifier {
  std::string kind;
  std::string layer;
  Json        params = Json::object();
  Json        rest = Json::object();  // keys not read, kept
};

// A MARKUP's content (the markup toolbar; media/markup.h): the pixels
// painted on it -- its RASTER, a PNG its size among the project's blobs,
// rewritten whole by each stroke -- and VECTOR objects drawn over them
// (lines, rectangles, ellipses, text), which stay editable until they are
// made pixels. A markup asset holds one (Asset::markup); before 2026-10-04
// a layer did (Layer::markup, read by the migration only).
struct Markup {
  BlobRef      raster;                   // empty: nothing painted yet
  Json         objects = Json::array();  // media/markup.h's objects
  std::int32_t width = 0;                // its size (a markup asset's)
  std::int32_t height = 0;
  Json         rest = Json::object();    // keys not read, kept
};

// Where a layer lies in TIME (a composition's layers; DESIGN §6a). Marks
// are in the source's own frames at `rate` (a sound's: milliseconds);
// `offset` and `duration` in the composition's frames -- a still's PAGES.
//   in / out   the span of the source shown (-1: its start / its end). In
//              a still composition, `in` is the frame shown of a clip or a
//              timeline (the page of a still with pages).
//   offset     where it starts on the timeline (a still's first page).
//   duration   how long it runs; 0: its marked span at its speed -- a
//              picture or a markup, to the timeline's end. In a still:
//              its pages, 0 to the last one.
struct LayerTime {
  std::int64_t in = -1;
  std::int64_t out = -1;
  Rational     rate{0, 1};
  std::int64_t offset = 0;
  std::int64_t duration = 0;

  bool identity() const
  {
    return in < 0 && out < 0 && offset == 0 && duration == 0;
  }
  bool operator==(const LayerTime&) const = default;
};

// One layer of a composition (Asset::layers, bottom first): what it shows
// (`source`, any asset but text), shown or hidden, a MASK of the layer
// beneath it (it is not drawn; that layer shows where it is bright -- its
// brightness times its alpha -- while it is visible), and where it lies in
// time. Its look and tracks are the composition's modifiers with its id.
// A layer holds no pixels of its own: a composition is data, drawn where
// pixels are needed (media/layers.h, model-input.h).
struct Layer {
  std::string            id;       // "" is layer 0, the take layer
  std::string            name;
  bool                   visible = true;
  std::optional<AssetId> source;   // none: blank, waiting for content
  // The version of `source` it shows; 0 follows its head. A capture pins
  // it, so what it froze stays as it was.
  std::uint32_t          source_version = 0;
  bool                   mask = false;
  LayerTime              time;
  // The FOLDER it is in (Asset::layer_folders); "" none.
  std::string            folder;
  // From before 2026-10-04, read by the migration only: the layer showed
  // the asset's own image (`own`), or held its markup.
  bool                   own = false;
  std::optional<Markup>  markup;
  Json                   rest = Json::object();  // keys not read, kept

  // Nothing on it yet: a new layer, waiting for content.
  bool empty() const { return !own && !source && !markup; }
};

// A FOLDER of a composition's layers (DESIGN §6a): a name its layers --
// each naming it (Layer::folder) -- are gathered under. It draws nothing
// of its own; its layers lie together in the stack (tidy_layer_folders).
struct LayerFolder {
  std::string id;
  std::string name;
  Json        rest = Json::object();  // keys not read, kept
};

// What happens where two layers' spans OVERLAP in a composition's time
// (DESIGN §6a): `from` ends first, `to` takes over.
//   "cut"       an abrupt change at the middle of the overlap.
//   "dissolve"  picture and sound cross-fade across the overlap.
struct Transition {
  std::string from;
  std::string to;
  std::string kind = "cut";
  Json        rest = Json::object();  // keys not read, kept
};

struct Asset {
  AssetId                  id;
  std::string              name;
  AssetKind                kind = AssetKind::Other;
  Origin                   origin = Origin::Source;
  AssetClass               cls = AssetClass::Flat;
  std::uint32_t            head = 0;  // 0 = no version yet
  RecipeId                 recipe;    // derived: the CURRENT recipe
  std::vector<std::string> tags;
  std::int64_t             created_ms = 0;
  std::int64_t             modified_ms = 0;
  // Source assets: where the bytes came from, and whether the project
  // holds a copy (false) or only references the original (true).
  std::string              source_path;
  bool                     linked = false;
  LinkInfo                 link;  // when linked
  // A composition's: its layers' looks and tracks (Modifier).
  std::vector<Modifier>    modifiers;
  // A composition's layers, bottom first, and the folders they are in.
  std::vector<Layer>       layers;
  std::vector<LayerFolder> layer_folders;
  // A composition's frame and canvas (media::StackCanvas).
  media::StackCanvas       canvas;
  // A composition's length, in its frames; 0: the latest end among its
  // timed layers (from before 2026-10-04: its bottom clip's).
  std::int64_t             timeline_frames = 0;
  // A composition's frame rate (an audio one counts milliseconds,
  // 1000/1); 0: its bottom clip's (from before 2026-10-04).
  Rational                 rate{0, 1};
  // A still's PAGES (DESIGN §6a): 1, a picture; more, a page each, drawn
  // as a timeline's frames are -- its layers spanning pages (`offset`,
  // `duration` in pages), their looks keyed by page -- with no sound.
  std::int64_t             pages = 1;
  std::vector<Transition>  transitions;
  // A markup's content.
  std::optional<Markup>    markup;
  // A flat asset FLATTENED from another: {"asset", "version"} -- where it
  // came from, a note and not a dependency. Null otherwise.
  Json                     from;
  // The folder of the asset list it is in (Project::folders); "" the
  // list's top.
  std::string              folder;
  Json                     extra = Json::object();
  // A kind this Valtz does not know (an extension's): read as Other, its
  // name kept and written back.
  std::string              kind_name;
  Json                     rest = Json::object();  // keys not read, kept
};

// A composition's folders kept TIDY: a folder's layers lie together in
// the stack -- a layer between two of its layers joins it; of its layers
// apart from each other, the longest run keeps it (the topmost of equal
// runs) and the others leave -- a layer naming no folder there names none,
// and a folder with no layer goes. Every write of layers keeps it so.
void tidy_layer_folders(Asset&);

struct RecipeInput {
  std::string   role;         // "reference", "init_image", "mask", ...
  AssetId       asset;
  std::uint32_t version = 0;  // 0 = follow HEAD
};

struct Recipe {
  RecipeId                 id;
  std::string              op;          // "generate-image", "caption", ...
  std::uint32_t            op_version = 1;
  std::string              model;       // catalog model id; "" if none
  Json                     params = Json::object();
  std::vector<RecipeInput> inputs;
  // May the build system re-run this without asking? True for pure
  // transforms (resize, transcode, contact sheet); false for anything
  // sampled, where a rebuild is a new take rather than a refresh.
  bool                     deterministic = false;
  std::int64_t             created_ms = 0;
  Json                     rest = Json::object();  // keys not read, kept
};

// An input as it was at build time.
struct ResolvedInput {
  std::string   role;
  AssetId       asset;
  std::uint32_t version = 0;
  ContentHash   hash;
};

struct AssetVersion {
  AssetId                    asset;
  std::uint32_t              number = 0;
  BlobRef                    blob;      // empty for linked sources
  media::MediaInfo           info;
  std::int64_t               created_ms = 0;
  // Derived only -- the snapshot.
  RecipeId                   recipe;
  Json                       executed = Json::object();  // params as run
  std::vector<ResolvedInput> inputs;
  ContentHash                fingerprint;
  std::string                engine;   // e.g. "vpipe 0.1 (55b66ae2)"
  std::string                host;     // which Mac built it
  ContentHash                content;  // hash of the produced bytes
  // How long it took to make: {"seconds", "phases": {phase: seconds}}
  // (controller/job-timing.h); empty for one made before it was kept.
  Json                       timing = Json::object();
  // What the build made beside its file, kept with it -- a song's
  // "score" (the ABC it followed). Not part of the fingerprint: an
  // output, not a cause.
  Json                       outputs = Json::object();
  Json                       rest = Json::object();  // keys not read, kept
};

struct SubjectEntry {
  std::string   role;     // "description", "reference", "contact-sheet"
  AssetId       asset;
  std::uint32_t version = 0;  // 0 = follow HEAD
  std::string   note;
};

struct Subject {
  SubjectId                 id;
  std::string               name;
  std::string               kind;  // "character", "object", "location"...
  std::vector<SubjectEntry> entries;
  std::vector<std::string>  tags;
  std::int64_t              created_ms = 0;
  std::int64_t              modified_ms = 0;
  Json                      rest = Json::object();  // keys not read, kept
};

// A state in the project's GENERATION HISTORY (Project::history): what an
// edit sent the model as its base, or what a generation made -- kept by
// the time it was, and FROZEN. Later work on the asset it came from (its
// layers, adjustments, crop, a new version) leaves the state as it was:
// it holds its own pixels, a blob of the project. A base that was a file
// of the asset as it is -- an imported picture, an earlier result, as
// they were -- shares that file's blob; one the model got adjusted,
// cropped or developed (a camera RAW) is that picture, rendered when the
// edit started. A state already in the history is not added again.
//
// A CLIP made is a state too (its movie, `frames` > 0), and the picture a
// clip opened on is its base, as for an edit. So is a SONG made (its
// sound, `kind` "audio", `seconds` long).
//
// The picture is the state flattened. A model that makes layers would
// keep the state's layers beside it -- one more field, the picture still
// the whole.
struct HistoryEntry {
  HistoryId     id;               // UUIDv7: made when the state was
  std::int64_t  created_ms = 0;
  std::string   role;             // "base" | "result"
  BlobRef       picture;
  int           width = 0;
  int           height = 0;
  AssetId       asset;            // what it was, or came from
  std::uint32_t version = 0;
  AssetId       generation;       // the result its generation made
  std::uint32_t generation_version = 0;
  std::string   label;            // the asset's name
  // Rendered as the model got it -- not a file of `asset`.
  bool          rendered = false;
  // A CLIP made (`picture` is its movie): its frames and length; 0 for a
  // picture. A sound's length too.
  std::int64_t  frames = 0;
  double        seconds = 0;
  // What `picture` holds: "image", "video" or "audio" (AssetKind's
  // names); a record from before it reads "video" with frames, else
  // "image".
  std::string   kind = "image";
  Json          rest = Json::object();  // keys not read, kept
};

// The canonical fingerprint of a build: what must be equal for two builds
// to be interchangeable. Model identity enters through `model_digest`
// (the catalog's content digest of the weights, when known).
ContentHash fingerprint(const Recipe&,
                        const std::vector<ResolvedInput>&,
                        std::string_view model_digest = {});

// The project's OUTPUT (DESIGN §6b): what exporting the PROJECT writes --
// its colour space, a timeline's frame rate, its sound's channels and
// sample rate -- set up with it, kept in its meta. An asset keeps its
// own; where a choice cannot be avoided (a 24 fps clip after a 30 fps one
// in the project's timeline) the project's wins: its timeline runs at
// `fps`, and a clip at another rate is resampled to it.
struct OutputSettings {
  // rec709 | srgb | display-p3 | rec2020 | rec2100-pq | rec2100-hlg
  // (media::output_color).
  std::string color = "rec709";
  Rational    fps{24, 1};
  int         channels = 2;       // 1 mono, 2 stereo
  int         sample_rate = 48000;

  bool operator==(const OutputSettings&) const = default;
};

std::int64_t now_ms();

// The keys of `j` not among `known`: what a reader keeps and writes back.
Json rest_of(const Json& j, std::initializer_list<std::string_view> known);
// `rest` merged under `j`: a key `j` sets wins.
void put_rest(Json& j, const Json& rest);

// JSON codecs (records are stored as CBOR of these).
void to_json(Json&, const BlobRef&);
void from_json(const Json&, BlobRef&);
void to_json(Json&, const Asset&);
void from_json(const Json&, Asset&);
void to_json(Json&, const RecipeInput&);
void from_json(const Json&, RecipeInput&);
void to_json(Json&, const Recipe&);
void from_json(const Json&, Recipe&);
void to_json(Json&, const ResolvedInput&);
void from_json(const Json&, ResolvedInput&);
void to_json(Json&, const AssetVersion&);
void from_json(const Json&, AssetVersion&);
void to_json(Json&, const SubjectEntry&);
void from_json(const Json&, SubjectEntry&);
void to_json(Json&, const Subject&);
void from_json(const Json&, Subject&);
void to_json(Json&, const HistoryEntry&);
void from_json(const Json&, HistoryEntry&);
void to_json(Json&, const OutputSettings&);
void from_json(const Json&, OutputSettings&);

}

namespace valtz::media {
void to_json(Json&, const ColorInfo&);
void from_json(const Json&, ColorInfo&);
void to_json(Json&, const FrameDesc&);
void from_json(const Json&, FrameDesc&);
void to_json(Json&, const MediaInfo&);
void from_json(const Json&, MediaInfo&);
}

#endif
