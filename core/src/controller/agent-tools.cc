// THE AGENT's TOOLS (DESIGN §9b): Valtz's editor as the agent's model is
// given it -- each tool a name, what it does in a sentence, its arguments
// as a JSON Schema -- and what each does, through the Controller's own
// calls: an edit is one undoable command ("agent", its title), as the
// person's own would be.
//
// What the model reads and writes is the document as a person thinks of
// it: canvas PIXELS from the top-left, pages from 1, layers by id ("0"
// the bottom one), colours as "#rrggbb[aa]".

#include "controller/agent-host.h"

#include "valtz/base/message.h"
#include "valtz/base/text.h"
#include "valtz/media/markup.h"
#include "valtz/media/thumbnail.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace valtz {

namespace fs = std::filesystem;
using project::AssetClass;
using project::AssetKind;

namespace {

// ---- arguments ------------------------------------------------------------

Json
schema(Json props, std::vector<std::string> required = {})
{
  Json s = {{"type", "object"}, {"properties", std::move(props)}};
  if (!required.empty()) {
    s["required"] = required;
  }
  return s;
}

Json
num(const char* doc)
{
  return {{"type", "number"}, {"description", doc}};
}

Json
integer(const char* doc)
{
  return {{"type", "integer"}, {"description", doc}};
}

Json
str(const char* doc)
{
  return {{"type", "string"}, {"description", doc}};
}

Json
boolean(const char* doc)
{
  return {{"type", "boolean"}, {"description", doc}};
}

Json
one_of(std::vector<std::string> values, const char* doc)
{
  return {{"type", "string"}, {"enum", values}, {"description", doc}};
}

const char* kCompDoc =
    "the composition's id; omitted: the one the session works on";
const char* kPageDoc = "the page, from 1; omitted: 1";
const char* kLayerDoc = "the layer's id (\"0\" is the bottom one)";
const char* kStartDoc =
    "on a timeline: when it shows, seconds; 0 by default";
const char* kDurationDoc =
    "on a timeline: for how long, seconds; 3 by default";

double
number(const Json& args, const char* key, double fallback)
{
  if (!args.is_object() || !args.contains(key)) {
    return fallback;
  }
  const Json& v = args[key];
  if (v.is_number()) {
    return v.get<double>();
  }
  if (v.is_string()) {
    try {
      return std::stod(v.get<std::string>());
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

std::string
text_arg(const Json& args, const char* key, std::string fallback = {})
{
  if (!args.is_object() || !args.contains(key)) {
    return fallback;
  }
  const Json& v = args[key];
  if (v.is_string()) {
    return v.get<std::string>();
  }
  if (v.is_number_integer()) {
    return std::to_string(v.get<std::int64_t>());
  }
  return fallback;
}

bool
flag(const Json& args, const char* key, bool fallback = false)
{
  if (!args.is_object() || !args.contains(key)) {
    return fallback;
  }
  const Json& v = args[key];
  if (v.is_boolean()) {
    return v.get<bool>();
  }
  if (v.is_string()) {
    return v == "true" || v == "yes";
  }
  return fallback;
}

// "#rrggbb" / "#rrggbbaa" (or a name of a few) as sRGB 0..1.
std::optional<std::array<double, 4>>
colour(std::string s)
{
  static const std::pair<const char*, const char*> names[] = {
      {"white", "#ffffff"}, {"black", "#000000"}, {"red", "#ff0000"},
      {"green", "#00a000"}, {"blue", "#0040ff"},  {"yellow", "#ffd400"},
      {"gray", "#808080"},  {"grey", "#808080"},  {"transparent", "#00000000"}};
  for (const auto& [n, hex] : names) {
    if (s == n) {
      s = hex;
    }
  }
  if (s.empty() || s.front() != '#' || (s.size() != 7 && s.size() != 9)) {
    return std::nullopt;
  }
  std::array<double, 4> c{0, 0, 0, 1};
  for (std::size_t i = 0; i < (s.size() - 1) / 2; ++i) {
    const std::string byte = s.substr(1 + 2 * i, 2);
    char* end = nullptr;
    const long v = std::strtol(byte.c_str(), &end, 16);
    if (end == nullptr || *end != '\0') {
      return std::nullopt;
    }
    c[i] = static_cast<double>(v) / 255.0;
  }
  return c;
}

std::string
hex(const Json& c)
{
  if (!c.is_array() || c.size() < 3) {
    return {};
  }
  auto b = [&](std::size_t i) {
    return static_cast<int>(std::lround(
        std::clamp(c[i].get<double>(), 0.0, 1.0) * 255));
  };
  const int a = c.size() > 3 ? b(3) : 255;
  return a == 255 ? std::format("#{:02x}{:02x}{:02x}", b(0), b(1), b(2))
                  : std::format("#{:02x}{:02x}{:02x}{:02x}", b(0), b(1),
                                b(2), a);
}

std::string
model_layer(const std::string& id)
{
  return id.empty() ? "0" : id;
}

bool
on_page(const project::LayerTime& t, std::int64_t page)
{
  return t.offset <= page &&
         (t.duration <= 0 || page < t.offset + t.duration);
}

std::string
object_id()
{
  return "a" + JobId::make().str().substr(24);
}

double
round_to(double v, int grid)
{
  return std::max<double>(grid, std::round(v / grid) * grid);
}

// The size a picture for a box is made at: the box's shape, about a
// megapixel (what the picture models are made for), on a 32-pixel grid.
std::pair<int, int>
picture_size(double w, double h)
{
  const double aspect = std::clamp(w / std::max(1.0, h), 0.25, 4.0);
  const double area = 1024.0 * 1024.0;
  const double pw = round_to(std::sqrt(area * aspect), 32);
  const double ph = round_to(area / pw, 32);
  return {static_cast<int>(std::clamp(pw, 256.0, 2048.0)),
          static_cast<int>(std::clamp(ph, 256.0, 2048.0))};
}

// A text box's words as they lie in it: whether all show, the height
// they need, the widest line (to centre a box on its words).
void
tell_fit(Json& out, const Json& object)
{
  const media::TextFit f = media::text_fit(object);
  out["lines"] = f.lines;
  out["text_width"] = std::lround(f.widest);
  if (!f.fits) {
    out["fits"] = false;
    out["needs_height"] = std::lround(f.needed_height);
    out["note"] = "Not every word shows: make the box taller (or wider), "
                  "or the text smaller.";
  }
}

// ---- time ------------------------------------------------------------------

std::int64_t
frames_of(double seconds, double fps)
{
  return static_cast<std::int64_t>(std::llround(std::max(0.0, seconds) *
                                                fps));
}

double
secs(double v)
{
  return std::round(v * 100) / 100;
}

// A sound's level and fades read back from its keys (as shape_sound_
// lays them), so a trim can lay them again over its new length.
struct SoundShape {
  double volume = 1;
  double fade_in = 0;
  double fade_out = 0;
};

SoundShape
shape_of(const media::KeyedSound& k, double fps)
{
  SoundShape s;
  if (k.keys.empty()) {
    return s;
  }
  double top = 0;
  for (const auto& key : k.keys) {
    top = std::max(top, key.value.volume);
  }
  s.volume = top;
  const auto& ks = k.keys;
  if (ks.size() >= 2 && ks.front().value.volume == 0) {
    s.fade_in = static_cast<double>(ks[1].frame - ks[0].frame) / fps;
  }
  if (ks.size() >= 2 && ks.back().value.volume == 0) {
    s.fade_out =
        static_cast<double>(ks.back().frame - ks[ks.size() - 2].frame) /
        fps;
  }
  return s;
}

// A source's own length in seconds: a clip's or a sound's, a
// composition's.
double
source_seconds(Controller& c, project::Project& p, const project::Asset& a)
{
  if (a.cls == AssetClass::Composition) {
    auto n = c.composition_length(p.id(), a.id);
    auto m = c.movie_stack(p.id(), a.id);
    if (n.ok() && m.ok() && m->rate.num > 0) {
      return static_cast<double>(*n) / m->rate.to_double();
    }
    return 0;
  }
  auto v = p.version(a.id);
  return v.ok() ? v->info.duration.seconds() : 0;
}

}  // namespace

// ---- the table ------------------------------------------------------------

const std::vector<Controller::AgentHost::Tool>&
Controller::AgentHost::tools()
{
  using H = AgentHost;
  static const std::vector<Tool> table = [] {
    const Json box_props = {
        {"x", num("the box's left edge, canvas pixels")},
        {"y", num("its top edge, canvas pixels (y grows down)")},
        {"width", num("its width, pixels")},
        {"height", num("its height, pixels")}};
    auto with = [](Json a, const Json& b) {
      for (const auto& [k, v] : b.items()) {
        a[k] = v;
      }
      return a;
    };
    std::vector<Tool> t;
    t.push_back({"get_project", "read", "Look at the project",
                 "The project: its compositions (size, pages), its other "
                 "assets, which composition is active, and the picture "
                 "models that run here.",
                 schema(Json::object()), &H::t_get_project});
    t.push_back({"get_composition", "read", "Look at a composition",
                 "A composition's size, pages and layers, bottom first: "
                 "what each shows, its box, its pages, and a markup "
                 "layer's text boxes and shapes. On a timeline: its "
                 "length, each layer's time in seconds, its volume, the "
                 "transitions.",
                 schema({{"composition", str(kCompDoc)}}),
                 &H::t_get_composition});
    t.push_back({"view_page", "read", "Look at a page",
                 "A page drawn as it is now, shown to you as a picture "
                 "(when you can see pictures; else told in words).",
                 schema({{"composition", str(kCompDoc)},
                         {"page", integer(kPageDoc)}}),
                 &H::t_view_page});
    t.push_back(
        {"create_composition", "edit", "Make a composition",
         "A new composition, which the session then works on: a STILL -- "
         "a canvas with pages, e.g. a slide deck -- a TIMELINE of clips "
         "in time (a video), or a SOUND composition (sounds alone).",
         schema({{"name", str("its name")},
                 {"kind", one_of({"still", "timeline", "sound"},
                                 "still by default")},
                 {"width", integer("pixels, e.g. 1920 (not for sound)")},
                 {"height", integer("pixels, e.g. 1080 (not for sound)")},
                 {"pages", integer("a still's pages; 1 by default")},
                 {"fps", num("a timeline's frame rate: its clips' "
                             "(e.g. 30); 24 by default")}},
                {"name"}),
         &H::t_create_composition});
    t.push_back({"add_pages", "edit", "Add pages",
                 "Pages added to a composition, after a page or at the "
                 "end. What runs across the pages continues onto them.",
                 schema({{"composition", str(kCompDoc)},
                         {"count", integer("how many; 1 by default")},
                         {"after", integer("after this page (from 1); "
                                           "omitted: at the end")}}),
                 &H::t_add_pages});
    t.push_back({"remove_page", "edit", "Remove a page",
                 "A page removed, with the layers only it had.",
                 schema({{"composition", str(kCompDoc)},
                         {"page", integer("the page, from 1")}},
                        {"page"}),
                 &H::t_remove_page});
    t.push_back(
        {"add_text", "edit", "Add text",
         "A TEXT BOX on a page: its words wrapped in the box's width, the "
         "lines that fit its height shown. Goes on the page's markup layer "
         "above its pictures. On a timeline: a title over the clips from "
         "`start` for `duration` seconds.",
         schema(with({{"composition", str(kCompDoc)},
                      {"page", integer(kPageDoc)},
                      {"start", num(kStartDoc)},
                      {"duration", num(kDurationDoc)},
                      {"text", str("the words; \\n starts a new line")},
                      {"size", num("the font's size in pixels; 48 by "
                                   "default")},
                      {"font", str("the font family; \"Helvetica Neue\" "
                                   "by default")},
                      {"bold", boolean("bold")},
                      {"italic", boolean("italic")},
                      {"color", str("the text's colour, \"#rrggbb[aa]\"; "
                                    "white by default")}},
                     box_props),
                {"text", "x", "y", "width", "height"}),
         &H::t_add_text});
    t.push_back(
        {"add_shape", "edit", "Add a shape",
         "A rectangle, an ellipse or a line on a page's markup layer -- a "
         "panel behind words, an accent, a divider. A line runs from "
         "(x, y) to (x + width, y + height).",
         schema(with({{"composition", str(kCompDoc)},
                      {"page", integer(kPageDoc)},
                      {"start", num(kStartDoc)},
                      {"duration", num(kDurationDoc)},
                      {"shape", one_of({"rect", "ellipse", "line"},
                                       "which shape")},
                      {"fill", str("its inside, \"#rrggbb[aa]\"; none by "
                                   "default")},
                      {"stroke", str("its outline or line colour")},
                      {"line_width", num("the outline's width, pixels; 0 "
                                         "none")}},
                     box_props),
                {"shape", "x", "y", "width", "height"}),
         &H::t_add_shape});
    t.push_back(
        {"update_object", "edit", "Change a text or shape",
         "A text box or shape changed: only the fields given.",
         schema(with({{"composition", str(kCompDoc)},
                      {"layer", str(kLayerDoc)},
                      {"object", str("the object's id")},
                      {"text", str("new words")},
                      {"size", num("font size, pixels")},
                      {"font", str("font family")},
                      {"bold", boolean("bold")},
                      {"italic", boolean("italic")},
                      {"color", str("text colour")},
                      {"fill", str("a shape's inside")},
                      {"stroke", str("a shape's outline")},
                      {"line_width", num("outline width")}},
                     box_props),
                {"layer", "object"}),
         &H::t_update_object});
    t.push_back({"remove_object", "edit", "Remove a text or shape",
                 "A text box or shape removed from its markup layer.",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"object", str("the object's id")}},
                        {"layer", "object"}),
                 &H::t_remove_object});
    t.push_back(
        {"place_asset", "edit", "Place an asset",
         "An asset of the project (a picture, a clip's frame, another "
         "composition) on a page as a new layer, fitted in a box (whole, "
         "centred); omitted, at its own size in the middle.",
         schema(with({{"composition", str(kCompDoc)},
                      {"asset", str("the asset's id")},
                      {"page", integer(kPageDoc)},
                      {"below_all", boolean("under every layer of the "
                                            "page (a background)")}},
                     box_props),
                {"asset"}),
         &H::t_place_asset});
    t.push_back({"set_layer_box", "edit", "Move a layer",
                 "A picture layer moved and sized to a box (whole, "
                 "centred in it).",
                 schema(with({{"composition", str(kCompDoc)},
                              {"layer", str(kLayerDoc)}},
                             box_props),
                        {"layer", "x", "y", "width", "height"}),
                 &H::t_set_layer_box});
    t.push_back({"set_layer_pages", "edit", "Set a layer's pages",
                 "The pages a layer lies on: `count` from `first`; 0 -- "
                 "on to the last page (a logo on every page).",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"first", integer("its first page, from 1")},
                         {"count", integer("how many pages; 0: to the "
                                           "last")}},
                        {"layer", "first"}),
                 &H::t_set_layer_pages});
    t.push_back({"arrange_layer", "edit", "Arrange a layer",
                 "A layer moved in the stack: to the top, the bottom, one "
                 "up or one down.",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"to", one_of({"top", "bottom", "up", "down"},
                                       "where")}},
                        {"layer", "to"}),
                 &H::t_arrange_layer});
    t.push_back({"remove_layer", "edit", "Remove a layer",
                 "A layer removed (what it showed stays in the project).",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)}},
                        {"layer"}),
                 &H::t_remove_layer});
    t.push_back({"rename_layer", "edit", "Rename a layer",
                 "A layer's name in the Layers list.",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"name", str("its new name")}},
                        {"layer", "name"}),
                 &H::t_rename_layer});
    t.push_back({"set_layer_visible", "edit", "Show or hide a layer",
                 "A layer shown or hidden.",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"visible", boolean("shown")}},
                        {"layer", "visible"}),
                 &H::t_set_layer_visible});
    t.push_back(
        {"adjust_layer", "edit", "Adjust a layer",
         "A layer's look, as the Adjust panel's sliders: each 0 is as it "
         "is. exposure in stops (-3..3); the others -1..1.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str(kLayerDoc)},
                 {"exposure", num("stops")},
                 {"contrast", num("-1..1")},
                 {"highlights", num("-1..1")},
                 {"shadows", num("-1..1")},
                 {"saturation", num("-1..1")},
                 {"vibrance", num("-1..1")},
                 {"temperature", num("-1..1, + warmer")},
                 {"tint", num("-1..1, + magenta")}},
                {"layer"}),
         &H::t_adjust_layer});
    t.push_back({"create_prompt", "edit", "Save a prompt",
                 "Words kept in the project as a PROMPT asset the person "
                 "can use again (a style, a scene).",
                 schema({{"text", str("the prompt")},
                         {"name", str("its name; omitted: its words")}},
                        {"text"}),
                 &H::t_create_prompt});
    t.push_back(
        {"generate_image", "generate", "Generate a picture",
         "A picture made from a prompt by a picture model, in the "
         "background: it LANDS on the page, fitted to the box, as a new "
         "layer, when it is done. Made at the box's shape. Without a "
         "composition, page or box: only an asset of the project.",
         schema(with({{"prompt", str("what the picture shows: subject, "
                                     "setting, light, style; no words")},
                      {"composition", str(kCompDoc)},
                      {"page", integer(kPageDoc)},
                      {"below_all", boolean("under every layer of the "
                                            "page (a background)")},
                      {"model", str("a picture model's id from "
                                    "get_project; omitted: Auto's")},
                      {"name", str("the asset's name")}},
                     box_props),
                {"prompt"}),
         &H::t_generate_image});
    t.push_back({"wait_for_generations", "read", "Wait for pictures",
                 "Waits until the pictures and sounds you asked for have "
                 "landed; says where each went.",
                 schema(Json::object()), &H::t_wait_for_generations});
    // ---- timelines: clips, sounds, music
    const Json seconds_list = {
        {"type", "array"},
        {"items", {{"type", "number"}}},
        {"description", "the seconds to look at (at most 6)"}};
    t.push_back(
        {"describe_clip", "read", "Watch a clip",
         "What a clip shows, scene by scene, each scene's span in the "
         "clip's own seconds -- told by a model that watches video (made "
         "once, kept in the project as the clip's summary). For choosing "
         "the parts worth keeping.",
         schema({{"asset", str("the clip's id")}}, {"asset"}),
         &H::t_describe_clip});
    t.push_back(
        {"view_frames", "read", "Look at frames",
         "Frames of a clip or a timeline as they are drawn, shown to you "
         "as pictures (when you can see pictures).",
         schema({{"asset", str("a clip's or a composition's id; omitted: "
                               "the one the session works on")},
                 {"times", seconds_list},
                 {"count", integer("else this many, spread over it; 4 by "
                                   "default")}}),
         &H::t_view_frames});
    t.push_back(
        {"add_clip", "edit", "Add a clip",
         "A clip (or a picture) on a timeline as a new layer: its part "
         "from `in` to `out` (the clip's own seconds) from `start` (the "
         "timeline's seconds; omitted: right after the last clip), "
         "fitted to the frame. A picture runs `duration` seconds. With "
         "a dissolve it overlaps the clip before by `transition_seconds`.",
         schema({{"composition", str(kCompDoc)},
                 {"asset", str("the clip's or picture's id")},
                 {"in", num("its part's start, its own seconds; 0 by "
                            "default")},
                 {"out", num("its part's end, its own seconds; its end by "
                             "default")},
                 {"start", num("where it starts on the timeline, seconds; "
                               "omitted (best): right after the last "
                               "clip")},
                 {"duration", num("a picture's seconds; 3 by default")},
                 {"speed", num("playback speed, 0.25-4; 1 as shot")},
                 {"volume", num("its own sound, 0 (mute) to 1 (as "
                                "recorded); 1 by default")},
                 {"fit", one_of({"fit", "fill"},
                                "fit: whole, bars where shapes differ (by "
                                "default); fill: the frame covered, its "
                                "edges cut")},
                 {"transition", one_of({"dissolve", "cut"},
                                       "how it follows the clip before")},
                 {"transition_seconds", num("a dissolve's length; 0.5 by "
                                            "default")}},
                {"asset"}),
         &H::t_add_clip});
    t.push_back(
        {"add_sound", "edit", "Add a sound",
         "A sound of the project (music, a recording) on a timeline as a "
         "new layer: its part from `in` to `out` (its own seconds) from "
         "`start`, at a volume, faded in and out.",
         schema({{"composition", str(kCompDoc)},
                 {"asset", str("the sound's id")},
                 {"start", num("timeline seconds; 0 by default")},
                 {"in", num("its part's start, its own seconds")},
                 {"out", num("its part's end, its own seconds")},
                 {"volume", num("0 to 1; 1 by default")},
                 {"fade_in", num("seconds from silence")},
                 {"fade_out", num("seconds to silence at its end")}},
                {"asset"}),
         &H::t_add_sound});
    t.push_back(
        {"set_layer_time", "edit", "Trim a layer",
         "A timeline's layer moved or trimmed: only what is given "
         "changes.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str(kLayerDoc)},
                 {"start", num("where it starts, timeline seconds")},
                 {"in", num("a clip's or sound's part: from, its own "
                            "seconds")},
                 {"out", num("to, its own seconds")},
                 {"duration", num("a picture's or a title's seconds")},
                 {"speed", num("playback speed, 0.25-4")}},
                {"layer"}),
         &H::t_set_layer_time});
    t.push_back(
        {"set_layer_sound", "edit", "Set a layer's volume",
         "A clip's or a sound's volume over its time: a level, faded in "
         "from silence and out to silence.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str(kLayerDoc)},
                 {"volume", num("0 (mute) to 1 (as it is); up to 2 "
                                "louder")},
                 {"fade_in", num("seconds from silence at its start")},
                 {"fade_out", num("seconds to silence at its end")}},
                {"layer"}),
         &H::t_set_layer_sound});
    t.push_back(
        {"set_transition", "edit", "Set a transition",
         "How one clip goes to the next: dissolve (picture and sound "
         "cross-fade over `seconds`; where the two only meet, the later "
         "one and all after it move earlier to overlap), cut, or none.",
         schema({{"composition", str(kCompDoc)},
                 {"from", str("the earlier layer's id")},
                 {"to", str("the later layer's id")},
                 {"kind", one_of({"dissolve", "cut", "none"}, "which")},
                 {"seconds", num("a dissolve's length; 0.5 by default")}},
                {"from", "to", "kind"}),
         &H::t_set_transition});
    t.push_back(
        {"set_timeline_length", "edit", "Set the length",
         "A timeline's length, what plays and exports: seconds; 0 -- as "
         "long as its layers run.",
         schema({{"composition", str(kCompDoc)},
                 {"seconds", num("its length")}},
                {"seconds"}),
         &H::t_set_timeline_length});
    t.push_back(
        {"generate_music", "generate", "Generate music",
         "Music made by a music model from a STYLE (and lyrics; none: "
         "instrumental), in the background: it LANDS on the timeline from "
         "`start` as a new sound layer, cut at `seconds` -- or where the "
         "clips end, if sooner -- and faded out. Takes about as long as it "
         "runs.",
         schema({{"style", str("the music's style as tags: genre, mood, "
                               "instruments, tempo, instrumental or the "
                               "voice -- e.g. \"upbeat acoustic folk, "
                               "ukulele, hand claps, warm, "
                               "instrumental\"")},
                 {"lyrics", str("words to sing under [Verse] / [Chorus] "
                                "headers; omitted: instrumental")},
                 {"seconds", num("how long it runs on the timeline; at "
                                 "most 240")},
                 {"start", num("timeline seconds; 0 by default")},
                 {"volume", num("0 to 1; 0.8 by default")},
                 {"fade_in", num("seconds; 0 by default")},
                 {"fade_out", num("seconds; 2 by default")},
                 {"composition", str(kCompDoc)},
                 {"name", str("the asset's name")}},
                {"style", "seconds"}),
         &H::t_generate_music});
    t.push_back(
        {"generate_voiceover", "generate", "Generate a voice-over",
         "Words spoken by a speech model, in the background: they LAND "
         "on the timeline from `start` as a new sound layer.",
         schema({{"text", str("the words; [pause 1s] pauses")},
                 {"voice", str("how it speaks, e.g. \"a warm, unhurried "
                               "female narrator\"")},
                 {"language", str("English, Chinese, ...; omitted: the "
                                  "words'")},
                 {"start", num("timeline seconds; 0 by default")},
                 {"volume", num("0 to 1; 1 by default")},
                 {"composition", str(kCompDoc)},
                 {"name", str("the asset's name")}},
                {"text"}),
         &H::t_generate_voiceover});
    t.push_back({"ask_user", "read", "Ask the person",
                 "A question for the person, waiting for their answer. "
                 "Only when you cannot go on without it.",
                 schema({{"question", str("the question")}}, {"question"}),
                 &H::t_ask_user});
    return t;
  }();
  return table;
}

std::vector<agent::ToolSpec>
Controller::AgentHost::tool_specs()
{
  std::vector<agent::ToolSpec> out;
  for (const auto& t : tools()) {
    out.push_back({t.name, t.description, t.parameters});
  }
  return out;
}

Json
Controller::AgentHost::tools_json() const
{
  Json list = Json::array();
  for (const auto& t : tools()) {
    list.push_back({{"name", t.name}, {"kind", t.kind}, {"title", t.title},
                    {"description", t.description},
                    {"parameters", t.parameters}});
  }
  return {{"tools", std::move(list)},
          {"skill", _c.skill_text("valtz-agent")}};
}

// ---- helpers ----------------------------------------------------------------

Result<AssetId>
Controller::AgentHost::composition_(const Session& s, const Json& args)
{
  std::optional<AssetId> id;
  if (const auto t = text_arg(args, "composition"); !t.empty()) {
    id = AssetId::parse(t);
    if (!id) {
      return make_error(Code::InvalidArgument,
                        std::format("'{}' is not an asset id", t));
    }
  } else {
    std::lock_guard lk(s.mu);
    id = s.target;
  }
  if (!id) {
    return make_error(Code::InvalidArgument,
                      "no composition is open: make one with "
                      "create_composition, or name one");
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(*id));
  if (a.cls != AssetClass::Still && a.cls != AssetClass::Composition) {
    return make_error(Code::InvalidArgument,
                      std::format("\"{}\" is not a composition", a.name));
  }
  return *id;
}

Result<std::string>
Controller::AgentHost::layer_(const Json& args)
{
  std::string l = text_arg(args, "layer");
  if (l.empty()) {
    return make_error(Code::InvalidArgument, "which layer?");
  }
  return l == "0" ? std::string() : l;
}

Result<std::int64_t>
Controller::AgentHost::page_(ProjectId pid, AssetId comp, const Json& args,
                             const char* key)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const auto page = static_cast<std::int64_t>(number(args, key, 1));
  if (page < 1 || page > a.pages) {
    return make_error(Code::InvalidArgument, std::format(
        "page {} is not one of its {} page{}", page, a.pages,
        a.pages == 1 ? "" : "s"));
  }
  return page - 1;
}

media::Crop
Controller::AgentHost::box_crop_(media::PixelSize content,
                                 media::PixelSize frame,
                                 const std::array<double, 4>& box)
{
  media::Crop c;
  c.content = content;
  const double sx = box[2] / std::max(1, content.width);
  const double sy = box[3] / std::max(1, content.height);
  c.scale_x = c.scale_y = std::min(sx, sy);
  c.offset_x =
      (box[0] + box[2] / 2 - frame.width / 2.0) / std::max(1, frame.width);
  c.offset_y =
      (box[1] + box[3] / 2 - frame.height / 2.0) / std::max(1, frame.height);
  return c;
}

Result<std::array<double, 4>>
Controller::AgentHost::box_(const Json& args, media::PixelSize frame)
{
  std::array<double, 4> b = {
      number(args, "x", 0), number(args, "y", 0),
      number(args, "width", frame.width),
      number(args, "height", frame.height)};
  if (b[2] <= 0 || b[3] <= 0) {
    return make_error(Code::InvalidArgument,
                      "a box needs a width and a height above 0");
  }
  return b;
}

Json
Controller::AgentHost::layer_box_(ProjectId pid, const project::Asset& comp,
                                  const project::Layer& l)
{
  auto* p = _c.project(pid);
  auto frame = _c.own_frame(pid, comp.id);
  if (!p || !frame.ok() || !l.source) {
    return Json();
  }
  auto content = _c.shown_size_(*p, *l.source, l.source_version, 0);
  if (!content.ok()) {
    return Json();
  }
  const media::Crop c = crop_of(comp, l.id);
  const double w = content->width * (c.identity() ? 1.0 : c.scale_x);
  const double h = content->height * (c.identity() ? 1.0 : c.scale_y);
  const double cx = frame->width / 2.0 + c.offset_x * frame->width;
  const double cy = frame->height / 2.0 + c.offset_y * frame->height;
  return {{"x", std::lround(cx - w / 2)}, {"y", std::lround(cy - h / 2)},
          {"width", std::lround(w)}, {"height", std::lround(h)}};
}

Result<std::string>
Controller::AgentHost::markup_on_(ProjectId pid, AssetId comp,
                                  std::int64_t page)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  // The page's topmost layer: drawn on when it is a markup; else a new
  // markup layer above it, the page's alone.
  std::vector<std::string> selected;
  for (auto it = a.layers.rbegin(); it != a.layers.rend(); ++it) {
    if (a.pages > 1 && !on_page(it->time, page)) {
      continue;
    }
    if (it->source) {
      auto src = p->asset(*it->source);
      if (src.ok() && src->cls == AssetClass::Markup) {
        selected.push_back(it->id);
      }
    }
    break;
  }
  return _c.markup_layer(pid, comp, selected, page);
}

Result<Json>
Controller::AgentHost::objects_(ProjectId pid, AssetId comp,
                                const std::string& layer)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  VALTZ_ASSIGN(auto m, _c.markup_at_(*p, a, layer));
  return m.second.objects.is_array() ? m.second.objects : Json::array();
}

Result<std::pair<std::string, Json>>
Controller::AgentHost::markup_for_(ProjectId pid, AssetId comp,
                                   const Json& args)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  if (a.cls != AssetClass::Composition) {
    VALTZ_ASSIGN(std::int64_t page, page_(pid, comp, args));
    VALTZ_ASSIGN(std::string layer, markup_on_(pid, comp, page));
    return std::pair{layer, Json{{"page", page + 1}}};
  }
  if (a.kind == AssetKind::Audio) {
    return make_error(Code::InvalidArgument,
                      "a composition of sound shows no words");
  }
  const double fps = rate_(pid, comp).to_double();
  const double start = std::max(0.0, number(args, "start", 0));
  const double dur = std::max(0.1, number(args, "duration", 3));
  VALTZ_ASSIGN(std::string layer,
               title_layer_(pid, comp, std::llround(start * fps),
                            std::llround(dur * fps)));
  return std::pair{layer, Json{{"start", start}, {"end", start + dur}}};
}

// ---- timeline helpers -------------------------------------------------------

Result<AssetId>
Controller::AgentHost::timeline_(const Session& s, const Json& args)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  if (a.cls != AssetClass::Composition) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is a still, not a timeline: make one with "
        "create_composition (kind \"timeline\")", a.name));
  }
  return comp;
}

Rational
Controller::AgentHost::rate_(ProjectId pid, AssetId comp)
{
  auto* p = _c.project(pid);
  if (p) {
    if (auto a = p->asset(comp); a.ok()) {
      return _c.rate_of_(*p, *a);
    }
  }
  return {24, 1};
}

Result<std::map<std::string, media::LayerTiming>>
Controller::AgentHost::timings_(ProjectId pid, AssetId comp)
{
  VALTZ_ASSIGN(media::MovieStack m, _c.movie_stack(pid, comp, true));
  std::map<std::string, media::LayerTiming> out;
  for (const auto& l : m.layers) {
    out[l.id] = l.timing;
  }
  return out;
}

std::string
Controller::AgentHost::layer_kind_(ProjectId pid, const project::Layer& l)
{
  auto* p = _c.project(pid);
  if (!p || !l.source) {
    return "nothing";
  }
  auto s = p->asset(*l.source);
  if (!s.ok()) {
    return "nothing";
  }
  if (s->cls == AssetClass::Markup) {
    return "title";
  }
  if (s->kind == AssetKind::Audio) {
    return "sound";
  }
  return s->kind == AssetKind::Video ? "clip" : "picture";
}

Rational
Controller::AgentHost::mark_rate_(ProjectId pid, AssetId source,
                                  bool sound_only)
{
  auto* p = _c.project(pid);
  auto a = p ? p->asset(source) : Result<project::Asset>(make_error(
                                      Code::NotFound, "no project"));
  if (!a.ok()) {
    return {24, 1};
  }
  if (a->cls == AssetClass::Composition) {
    return sound_only && a->kind == AssetKind::Video ? Rational{1000, 1}
                                                     : _c.rate_of_(*p, *a);
  }
  if (a->kind == AssetKind::Audio ||
      (sound_only && a->kind == AssetKind::Video)) {
    return {1000, 1};
  }
  if (auto v = p->version(source); v.ok() && v->info.frame_rate.num > 0) {
    return v->info.frame_rate;
  }
  return {24, 1};
}

Result<std::string>
Controller::AgentHost::title_layer_(ProjectId pid, AssetId comp,
                                    std::int64_t start, std::int64_t length)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  length = std::max<std::int64_t>(1, length);
  for (auto it = a.layers.rbegin(); it != a.layers.rend(); ++it) {
    if (it->time.offset == start && it->time.duration == length &&
        layer_kind_(pid, *it) == "title") {
      return it->id;
    }
  }
  // A blank on top, made a markup there, then given its span.
  VALTZ_ASSIGN(std::string blank,
               _c.add_layer(pid, comp,
                            a.layers.empty() ? std::string()
                                             : a.layers.back().id));
  VALTZ_ASSIGN(std::string layer,
               _c.markup_layer(pid, comp, {blank}, start));
  project::LayerTime t;
  t.offset = start;
  t.duration = length;
  VALTZ_TRY(_c.set_layer_time(pid, comp, layer, t));
  return layer;
}

Status
Controller::AgentHost::shape_sound_(ProjectId pid, AssetId comp,
                                    const std::string& layer, double volume,
                                    double fade_in, double fade_out,
                                    double seconds)
{
  const Rational rate = rate_(pid, comp);
  const double fps = rate.to_double();
  const double v = std::clamp(volume, 0.0, 2.0);
  const bool timed = std::isfinite(seconds) && seconds > 0;
  const double half = timed ? seconds / 2 : 0;
  const double fi = timed ? std::clamp(fade_in, 0.0, half) : 0;
  const double fo = timed ? std::clamp(fade_out, 0.0, half) : 0;
  media::KeyedSound k;
  k.rate = rate;
  auto key = [&](double at, double gain) {
    const auto f = static_cast<std::int64_t>(std::llround(at * fps));
    if (!k.keys.empty() && k.keys.back().frame >= f) {
      k.keys.back().value.volume = gain;
      return;
    }
    k.keys.push_back({f, media::Sound{gain, 0}});
  };
  key(0, fi > 0 ? 0.0 : v);
  if (fi > 0) {
    key(fi, v);
  }
  if (fo > 0) {
    key(seconds - fo, v);
    key(seconds, 0);
  }
  return _c.set_sound_keys(pid, comp, k, layer);
}

double
Controller::AgentHost::shown_end_(ProjectId pid, AssetId comp)
{
  auto* p = _c.project(pid);
  auto times = timings_(pid, comp);
  if (!p || !times.ok()) {
    return 0;
  }
  auto a = p->asset(comp);
  if (!a.ok()) {
    return 0;
  }
  double end = 0;
  for (const auto& l : a->layers) {
    const std::string k = layer_kind_(pid, l);
    const auto it = times->find(l.id);
    if ((k == "clip" || k == "picture") && it != times->end() &&
        std::isfinite(it->second.length)) {
      end = std::max(end, it->second.end());
    }
  }
  return end;
}

Status
Controller::AgentHost::cut_at_(ProjectId pid, AssetId comp,
                               const std::string& layer, double end)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const auto it = std::ranges::find(a.layers, layer, &project::Layer::id);
  if (it == a.layers.end() || !it->source) {
    return make_error(Code::NotFound, "no such layer");
  }
  VALTZ_ASSIGN(auto times, timings_(pid, comp));
  const media::LayerTiming was = times[layer];
  if (!was.timed || end <= was.start) {
    return ok_status();
  }
  // Where its source is when the timeline reaches `end`: its mark-out.
  const Rational mr =
      mark_rate_(pid, *it->source, a.kind == AssetKind::Audio);
  project::LayerTime t = it->time;
  t.in = frames_of(was.in, mr.to_double());
  t.out = std::max(t.in, frames_of(was.source_at(end), mr.to_double()) -
                             1);
  t.rate = mr;
  VALTZ_TRY(_c.set_layer_time(pid, comp, layer, t));
  const double fps = rate_(pid, comp).to_double();
  const SoundShape sh = shape_of(Controller::sound_keys_of(a, layer), fps);
  VALTZ_ASSIGN(times, timings_(pid, comp));
  if (!Controller::sound_keys_of(a, layer).identity()) {
    VALTZ_TRY(shape_sound_(pid, comp, layer, sh.volume, sh.fade_in,
                           sh.fade_out, times[layer].length));
  }
  return ok_status();
}

Json
Controller::AgentHost::lay_sound_(const Landing& l, AssetId comp,
                                  const std::string& layer, AssetId asset)
{
  Json out = Json::object();
  auto* p = _c.project(l.project);
  if (!p) {
    return out;
  }
  double made = 0;
  if (auto v = p->version(asset); v.ok()) {
    made = v->info.duration.seconds();
  }
  const double fps = rate_(l.project, comp).to_double();
  const double start = static_cast<double>(l.start) / fps;
  // Music ends with the clips when they end first.
  double want = l.seconds;
  const double shown = l.fit ? shown_end_(l.project, comp) : 0;
  if (shown > start + 1 && (want <= 0 || start + want > shown + 0.05)) {
    want = shown - start;
    out["fitted"] = std::format("cut at {:.2f} s, where the clips end",
                                shown);
  }
  double runs = made;
  if (want > 0 && made > want + 0.05) {
    // Cut at its length: a sound's marks count milliseconds.
    if (auto a = p->asset(comp); a.ok()) {
      const auto it = std::ranges::find(a->layers, layer,
                                        &project::Layer::id);
      if (it != a->layers.end()) {
        project::LayerTime t = it->time;
        t.in = 0;
        t.out = std::llround(want * 1000) - 1;
        t.rate = {1000, 1};
        if (_c.set_layer_time(l.project, comp, layer, t).ok()) {
          runs = want;
        }
      }
    }
  }
  (void)shape_sound_(l.project, comp, layer, l.volume, l.fade_in,
                     l.fade_out, runs);
  out["made_seconds"] = std::round(made * 10) / 10;
  out["start"] = std::round(start * 100) / 100;
  out["end"] = std::round((start + runs) * 100) / 100;
  if (l.seconds > 0 && made + 0.5 < l.seconds) {
    out["note"] = std::format(
        "It came out {:.1f} s long, shorter than the {:.1f} s asked: "
        "the timeline's end is silent unless you add more.", made,
        l.seconds);
  }
  return out;
}

bool
Controller::AgentHost::wait_job_(Session& s, JobId job)
{
  std::unique_lock lk(s.mu);
  for (;;) {
    if (s.stop_asked) {
      lk.unlock();
      (void)_c.cancel(job);
      return false;
    }
    const auto rec = _c._jobs.get(job);
    if (!rec || rec->state == JobState::Finished ||
        rec->state == JobState::Failed ||
        rec->state == JobState::Cancelled) {
      return true;
    }
    s.cv.wait_for(lk, std::chrono::milliseconds(200));
  }
}

// ---- reading --------------------------------------------------------------

Result<Json>
Controller::AgentHost::t_get_project(Session& s, Step&, const Json&,
                                     std::vector<fs::path>&)
{
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(auto all, p->assets());
  std::ranges::sort(all, [](const auto& a, const auto& b) {
    return a.created_ms > b.created_ms;
  });
  Json comps = Json::array();
  Json assets = Json::array();
  // A clip's summary: the text asset told from it (describe_clip).
  std::map<AssetId, AssetId> told;
  for (const auto& a : all) {
    if (a.kind != AssetKind::Text || a.head == 0 ||
        a.origin != project::Origin::Derived) {
      continue;
    }
    auto r = p->recipe(a.recipe);
    if (r.ok() && r->op == engine::kOpSummarizeVideo && !r->inputs.empty()) {
      told.emplace(r->inputs.front().asset, a.id);
    }
  }
  for (const auto& a : all) {
    if (a.cls == AssetClass::Still || a.cls == AssetClass::Composition) {
      auto f = _c.own_frame(s.project, a.id);
      const bool sound = a.kind == AssetKind::Audio;
      Json j = {{"id", a.id.str()},
                {"name", a.name},
                {"kind", a.cls == AssetClass::Still ? "still"
                         : sound                    ? "sound"
                                                    : "timeline"},
                {"layers", a.layers.size()}};
      if (!sound) {
        j["width"] = f.ok() ? f->width : 0;
        j["height"] = f.ok() ? f->height : 0;
      }
      if (a.cls == AssetClass::Still) {
        j["pages"] = a.pages;
      } else {
        const Rational r = _c.rate_of_(*p, a);
        if (!sound) {
          j["fps"] = std::round(r.to_double() * 1000) / 1000;
        }
        if (auto n = _c.composition_length(s.project, a.id); n.ok()) {
          j["seconds"] =
              std::round(static_cast<double>(*n) / r.to_double() * 100) /
              100;
        }
      }
      comps.push_back(std::move(j));
    } else if (a.cls != AssetClass::Markup && assets.size() < 80) {
      Json j = {{"id", a.id.str()},
                {"name", a.name},
                {"kind", project::to_str(a.kind)}};
      if (a.head > 0) {
        if (auto v = p->version(a.id); v.ok()) {
          if (v->info.frame.width > 0) {
            j["width"] = v->info.frame.width;
            j["height"] = v->info.frame.height;
          }
          if (a.kind == AssetKind::Video || a.kind == AssetKind::Audio) {
            j["seconds"] =
                std::round(v->info.duration.seconds() * 100) / 100;
          }
          if (a.kind == AssetKind::Video) {
            j["fps"] =
                std::round(v->info.frame_rate.to_double() * 1000) / 1000;
            j["has_sound"] = v->info.has_audio;
          }
        }
      } else {
        j["state"] = "being made";
      }
      if (auto it = told.find(a.id); it != told.end()) {
        j["summary"] = it->second.str();
      }
      if (is_prompt(a)) {
        j["kind"] = "prompt";
      }
      assets.push_back(std::move(j));
    }
  }
  Json models = Json::array();
  for (const auto& m : _c._catalog.models()) {
    if (m.has(models::Capability::TextToImage) &&
        _c.runs(m, "image", "generate")) {
      models.push_back({{"id", m.id}, {"name", m.name}});
    }
  }
  std::string auto_pick;
  for (const auto& pick : _c.auto_models()) {
    if (pick.modality == "image" && pick.op == "generate") {
      auto_pick = pick.chosen;
    }
  }
  // What makes sound here, and whether a clip can be watched.
  bool music = false, speech = false, watch = false;
  for (const auto& m : _c._catalog.models()) {
    const bool sounds = _c.runs(m, "audio", "generate");
    music = music || (sounds && m.has(models::Capability::TextToAudio));
    speech = speech || (sounds && m.has(models::Capability::TextToSpeech));
    watch = watch ||
            (m.has(models::Capability::VideoSummary) &&
             _c._store->info(m).state == models::InstallState::Installed);
  }
  watch = watch && _c.engine_runs_(models::Capability::VideoSummary);
  std::lock_guard lk(s.mu);
  return Json{{"name", p->name()},
              {"active", s.target ? s.target->str() : std::string()},
              {"compositions", std::move(comps)},
              {"assets", std::move(assets)},
              {"picture_models", std::move(models)},
              {"auto_picture_model", auto_pick},
              {"can_make_music", music},
              {"can_make_voiceover", speech},
              {"can_describe_clips", watch}};
}

Result<Json>
Controller::AgentHost::t_get_composition(Session& s, Step&, const Json& args,
                                         std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  auto frame = _c.own_frame(s.project, comp);
  const bool timeline = a.cls == AssetClass::Composition;
  const bool sound_only = a.kind == AssetKind::Audio;
  std::map<std::string, media::LayerTiming> times;
  if (timeline) {
    VALTZ_ASSIGN(times, timings_(s.project, comp));
  }
  const double fps = timeline ? rate_(s.project, comp).to_double() : 1;
  Json layers = Json::array();
  for (const auto& l : a.layers) {
    Json j = {{"id", model_layer(l.id)}, {"name", l.name},
              {"visible", l.visible}};
    const std::string kind = timeline ? layer_kind_(s.project, l) : "";
    if (timeline) {
      // Where it is in time: the timeline's seconds, its source's.
      j["kind"] = kind;
      if (auto it = times.find(l.id); it != times.end()) {
        const media::LayerTiming& t = it->second;
        j["start"] = secs(t.start);
        j["end"] = std::isfinite(t.length) ? Json(secs(t.end()))
                                           : Json("the end");
        if (t.timed) {
          j["in"] = secs(t.in);
          j["out"] = secs(t.out);
        }
        if (!t.speed.identity()) {
          j["speed"] = t.speed.keys.empty()
                           ? 1.0 : t.speed.keys.front().value.rate;
        }
      }
      const media::KeyedSound k = Controller::sound_keys_of(a, l.id);
      if (!k.identity()) {
        Json vol = Json::array();
        for (const auto& key : k.keys) {
          vol.push_back({secs(static_cast<double>(key.frame) / fps),
                         std::round(key.value.volume * 100) / 100});
        }
        j["volume"] = std::move(vol);
      }
    }
    if (a.pages > 1) {
      const std::int64_t first = l.time.offset + 1;
      j["pages"] = l.time.duration <= 0
                       ? Json{first, a.pages}
                       : Json{first, l.time.offset + l.time.duration};
    }
    if (!l.source) {
      j["shows"] = "nothing";
    } else if (auto src = p->asset(*l.source); src.ok()) {
      if (src->cls == AssetClass::Markup) {
        j["shows"] = "markup";
        Json objs = Json::array();
        const Json raw = src->markup ? src->markup->objects : Json();
        for (const auto& o : raw.is_array() ? raw : Json::array()) {
          Json x = {{"id", jget<std::string>(o, "id", "")},
                    {"kind", jget<std::string>(o, "kind", "")}};
          const double x0 = jget(o, "x0", 0.0), y0 = jget(o, "y0", 0.0);
          const double x1 = jget(o, "x1", 0.0), y1 = jget(o, "y1", 0.0);
          x["x"] = std::lround(std::min(x0, x1));
          x["y"] = std::lround(std::min(y0, y1));
          x["width"] = std::lround(std::abs(x1 - x0));
          x["height"] = std::lround(std::abs(y1 - y0));
          if (x["kind"] == "text") {
            x["text"] = jget<std::string>(o, "text", "");
            const Json f = jget(o, "font", Json::object());
            x["size"] = jget(f, "size", 0.0);
            x["font"] = jget<std::string>(f, "family", "");
            if (jget(f, "bold", false)) {
              x["bold"] = true;
            }
            if (jget(f, "italic", false)) {
              x["italic"] = true;
            }
            x["color"] = hex(jget(o, "stroke", Json()));
            if (!media::text_fit(o).fits) {
              x["fits"] = false;
            }
          } else {
            if (auto f = hex(jget(o, "fill", Json())); !f.empty()) {
              x["fill"] = f;
            }
            if (jget(o, "width", 0.0) > 0) {
              x["stroke"] = hex(jget(o, "stroke", Json()));
              x["line_width"] = jget(o, "width", 0.0);
            }
          }
          objs.push_back(std::move(x));
        }
        j["objects"] = std::move(objs);
      } else {
        j["shows"] = {{"asset", src->id.str()},
                      {"name", src->name},
                      {"kind", src->cls == AssetClass::Still ||
                                       src->cls == AssetClass::Composition
                                   ? "composition"
                                   : project::to_str(src->kind)}};
        if (src->head == 0 && src->cls == AssetClass::Generated) {
          j["shows"]["state"] = "being made";
        }
        if (!sound_only && kind != "sound") {
          j["box"] = layer_box_(s.project, a, l);
        }
      }
    }
    if (!Controller::adjustments_of(a, l.id).identity()) {
      j["adjusted"] = to_json(Controller::adjustments_of(a, l.id));
    }
    layers.push_back(std::move(j));
  }
  Json out = {{"id", comp.str()},
              {"name", a.name},
              {"layers", std::move(layers)}};
  if (!sound_only) {
    out["width"] = frame.ok() ? frame->width : 0;
    out["height"] = frame.ok() ? frame->height : 0;
  }
  if (!timeline) {
    out["kind"] = "still";
    out["pages"] = a.pages;
    return out;
  }
  out["kind"] = sound_only ? "sound" : "timeline";
  if (!sound_only) {
    out["fps"] = std::round(fps * 1000) / 1000;
  }
  if (auto n = _c.composition_length(s.project, comp); n.ok()) {
    out["seconds"] = secs(static_cast<double>(*n) / fps);
  }
  Json trans = Json::array();
  for (const auto& t : a.transitions) {
    trans.push_back({{"from", model_layer(t.from)}, {"to", model_layer(t.to)},
                     {"kind", t.kind}});
  }
  out["transitions"] = std::move(trans);
  return out;
}

Result<Json>
Controller::AgentHost::t_view_page(Session& s, Step& step, const Json& args,
                                   std::vector<fs::path>& shown)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  if (auto a = _c.project(s.project)->asset(comp);
      a.ok() && a->cls == AssetClass::Composition) {
    return make_error(Code::InvalidArgument,
                      "a timeline has frames, not pages: view_frames");
  }
  VALTZ_ASSIGN(std::int64_t page, page_(s.project, comp, args));
  if (!s.backend->sees()) {
    Json words;
    VALTZ_ASSIGN(words, t_get_composition(s, step, args, shown));
    Json on = Json::array();
    for (const auto& l : jget(words, "layers", Json::array())) {
      const Json pg = jget(l, "pages", Json());
      if (!pg.is_array() ||
          (pg[0].get<std::int64_t>() <= page + 1 &&
           page + 1 <= pg[1].get<std::int64_t>())) {
        on.push_back(l);
      }
    }
    return Json{{"shown", false},
                {"note", "You cannot see pictures; here is the page's "
                         "layout."},
                {"layers", std::move(on)}};
  }
  const fs::path drawn = s.dir / std::format("page-{}.png", step.n);
  VALTZ_TRY(_c.flatten(s.project, comp, drawn, std::nullopt, std::nullopt,
                       {}, page));
  const fs::path small = s.dir / std::format("page-{}.jpg", step.n);
  VALTZ_TRY(media::write_thumbnail(drawn, media::MediaType::Image, 1024,
                                   small));
  std::error_code ec;
  fs::remove(drawn, ec);
  shown.push_back(small);
  return Json{{"shown", true}, {"page", page + 1},
              {"note", "The page follows, as a picture."}};
}

// ---- compositions and pages -------------------------------------------------

Result<Json>
Controller::AgentHost::t_create_composition(Session& s, Step& step,
                                            const Json& args,
                                            std::vector<fs::path>&)
{
  const std::string kind = text_arg(args, "kind", "still");
  if (kind != "still" && kind != "timeline" && kind != "sound") {
    return make_error(Code::InvalidArgument,
                      "a composition is a still, a timeline or a sound");
  }
  const bool sound = kind == "sound";
  const int w = sound ? 0
                      : std::clamp(static_cast<int>(
                                       number(args, "width", 1920)),
                                   64, 8192);
  const int h = sound ? 0
                      : std::clamp(static_cast<int>(
                                       number(args, "height", 1080)),
                                   64, 8192);
  const int pages =
      std::clamp(static_cast<int>(number(args, "pages", 1)), 1, 200);
  // A timeline's rate: a whole number of frames a second, or NTSC's.
  Rational rate{0, 1};
  if (kind == "timeline") {
    const double fps = std::clamp(number(args, "fps", 24), 1.0, 240.0);
    const double whole = std::round(fps);
    rate = std::abs(fps - whole) < 0.01
               ? Rational(static_cast<std::int64_t>(whole), 1)
               : Rational(static_cast<std::int64_t>(whole) * 1000, 1001);
  }
  std::string name = one_line(text_arg(args, "name", "Composition"));
  auto undo = _c.command_(s.project, "agent", {}, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(AssetId comp,
               _c.create_composition(
                   s.project,
                   kind == "still" ? AssetClass::Still
                                   : AssetClass::Composition,
                   {w, h}, rate, name, false, true));
  for (int i = 1; kind == "still" && i < pages; ++i) {
    VALTZ_TRY(_c.add_page(s.project, comp));
  }
  {
    std::lock_guard lk(s.mu);
    s.target = comp;
  }
  Json out = {{"composition", comp.str()}, {"kind", kind}};
  if (!sound) {
    out["width"] = w;
    out["height"] = h;
  }
  if (kind == "still") {
    out["pages"] = pages;
  } else if (!sound) {
    out["fps"] = std::round(rate.to_double() * 1000) / 1000;
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_add_pages(Session& s, Step& step, const Json& args,
                                   std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  const int count =
      std::clamp(static_cast<int>(number(args, "count", 1)), 1, 100);
  std::optional<std::int64_t> after;
  if (args.contains("after")) {
    VALTZ_ASSIGN(std::int64_t a, page_(s.project, comp, args, "after"));
    after = a;
  }
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  Json added = Json::array();
  for (int i = 0; i < count; ++i) {
    VALTZ_ASSIGN(std::int64_t at, _c.add_page(s.project, comp, after));
    added.push_back(at + 1);
    if (after) {
      *after = at;
    }
  }
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  return Json{{"added", std::move(added)}, {"pages", a.pages}};
}

Result<Json>
Controller::AgentHost::t_remove_page(Session& s, Step& step,
                                     const Json& args,
                                     std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::int64_t page, page_(s.project, comp, args));
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.remove_page(s.project, comp, page));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  return Json{{"pages", a.pages}};
}

// ---- words and shapes -------------------------------------------------------

Result<Json>
Controller::AgentHost::t_add_text(Session& s, Step& step, const Json& args,
                                  std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
  VALTZ_ASSIGN(auto box, box_(args, frame));
  const std::string text = text_arg(args, "text");
  if (text.empty()) {
    return make_error(Code::InvalidArgument, "no words to add");
  }
  auto c = colour(text_arg(args, "color", "#ffffff"));
  if (!c) {
    return make_error(Code::InvalidArgument,
                      "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
  }
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(auto on, markup_for_(s.project, comp, args));
  const std::string layer = on.first;
  VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
  const std::string id = object_id();
  objs.push_back(
      {{"id", id},
       {"kind", "text"},
       {"x0", box[0]},
       {"y0", box[1]},
       {"x1", box[0] + box[2]},
       {"y1", box[1] + box[3]},
       {"box", true},
       {"text", text},
       {"stroke", {(*c)[0], (*c)[1], (*c)[2], (*c)[3]}},
       {"width", 0},
       {"font",
        {{"family", text_arg(args, "font", "Helvetica Neue")},
         {"size", std::clamp(number(args, "size", 48), 4.0, 2000.0)},
         {"bold", flag(args, "bold")},
         {"italic", flag(args, "italic")},
         {"underline", false}}}});
  VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
  Json out = {{"layer", model_layer(layer)}, {"object", id}};
  out.update(on.second);
  tell_fit(out, objs.back());
  return out;
}

Result<Json>
Controller::AgentHost::t_add_shape(Session& s, Step& step, const Json& args,
                                   std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
  VALTZ_ASSIGN(auto box, box_(args, frame));
  const std::string shape = text_arg(args, "shape", "rect");
  if (shape != "rect" && shape != "ellipse" && shape != "line") {
    return make_error(Code::InvalidArgument,
                      "a shape is rect, ellipse or line");
  }
  auto fill = colour(text_arg(args, "fill", "#00000000"));
  auto stroke = colour(text_arg(args, "stroke", "#ffffffff"));
  if (!fill || !stroke) {
    return make_error(Code::InvalidArgument,
                      "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
  }
  double lw = std::clamp(number(args, "line_width",
                                shape == "line" ? 4.0 : 0.0), 0.0, 500.0);
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(auto on, markup_for_(s.project, comp, args));
  const std::string layer = on.first;
  VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
  const std::string id = object_id();
  Json made = {{"id", id},
               {"kind", shape == "rect"      ? "rect"
                        : shape == "ellipse" ? "ellipse"
                                             : "line"},
               {"x0", box[0]},
               {"y0", box[1]},
               {"x1", box[0] + box[2]},
               {"y1", box[1] + box[3]},
               {"stroke", {(*stroke)[0], (*stroke)[1], (*stroke)[2],
                           (*stroke)[3]}},
               {"fill", {(*fill)[0], (*fill)[1], (*fill)[2], (*fill)[3]}},
               {"width", lw}};
  // Under the layer's words, whenever it is added: a panel is behind
  // the text it was made for (objects are drawn in order).
  const auto words = std::ranges::find_if(objs, [](const Json& o) {
    return jget<std::string>(o, "kind", "") == "text";
  });
  objs.insert(words, std::move(made));
  VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
  Json out = {{"layer", model_layer(layer)}, {"object", id}};
  out.update(on.second);
  return out;
}

Result<Json>
Controller::AgentHost::t_update_object(Session& s, Step& step,
                                       const Json& args,
                                       std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  const std::string id = text_arg(args, "object");
  VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
  auto it = std::ranges::find_if(objs, [&](const Json& o) {
    return jget<std::string>(o, "id", "") == id;
  });
  if (it == objs.end()) {
    return make_error(Code::NotFound,
                      std::format("layer {} has no object '{}'",
                                  model_layer(layer), id));
  }
  Json& o = *it;
  const double x0 = std::min(jget(o, "x0", 0.0), jget(o, "x1", 0.0));
  const double y0 = std::min(jget(o, "y0", 0.0), jget(o, "y1", 0.0));
  const double w = std::abs(jget(o, "x1", 0.0) - jget(o, "x0", 0.0));
  const double h = std::abs(jget(o, "y1", 0.0) - jget(o, "y0", 0.0));
  if (args.contains("x") || args.contains("y") || args.contains("width") ||
      args.contains("height")) {
    const double nx = number(args, "x", x0), ny = number(args, "y", y0);
    const double nw = number(args, "width", w), nh = number(args, "height", h);
    o["x0"] = nx;
    o["y0"] = ny;
    o["x1"] = nx + nw;
    o["y1"] = ny + nh;
  }
  if (args.contains("text")) {
    o["text"] = text_arg(args, "text");
  }
  Json font = jget(o, "font", Json::object());
  if (args.contains("size")) {
    font["size"] = std::clamp(number(args, "size", 48), 4.0, 2000.0);
  }
  if (args.contains("font")) {
    font["family"] = text_arg(args, "font");
  }
  if (args.contains("bold")) {
    font["bold"] = flag(args, "bold");
  }
  if (args.contains("italic")) {
    font["italic"] = flag(args, "italic");
  }
  if (o.contains("font")) {
    o["font"] = font;
  }
  for (const char* k : {"color", "stroke", "fill"}) {
    if (!args.contains(k)) {
      continue;
    }
    auto c = colour(text_arg(args, k));
    if (!c) {
      return make_error(Code::InvalidArgument,
                        "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
    }
    o[std::string(k) == "fill" ? "fill" : "stroke"] = {(*c)[0], (*c)[1],
                                                       (*c)[2], (*c)[3]};
  }
  if (args.contains("line_width")) {
    o["width"] = std::clamp(number(args, "line_width", 0), 0.0, 500.0);
  }
  const Json changed = o;
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
  Json out = {{"layer", model_layer(layer)}, {"object", id}};
  if (jget<std::string>(changed, "kind", "") == "text") {
    tell_fit(out, changed);
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_remove_object(Session& s, Step& step,
                                       const Json& args,
                                       std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  const std::string id = text_arg(args, "object");
  VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
  const auto n = objs.size();
  objs.erase(std::remove_if(objs.begin(), objs.end(),
                            [&](const Json& o) {
                              return jget<std::string>(o, "id", "") == id;
                            }),
             objs.end());
  if (objs.size() == n) {
    return make_error(Code::NotFound,
                      std::format("layer {} has no object '{}'",
                                  model_layer(layer), id));
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
  return Json{{"removed", id}};
}

// ---- layers -----------------------------------------------------------------

Result<Json>
Controller::AgentHost::t_place_asset(Session& s, Step& step,
                                     const Json& args,
                                     std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::int64_t page, page_(s.project, comp, args));
  const auto asset = AssetId::parse(text_arg(args, "asset"));
  if (!asset) {
    return make_error(Code::InvalidArgument, "which asset?");
  }
  VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(auto placed, _c.instantiate(s.project, *asset, comp,
                                           std::nullopt, page));
  const std::string layer = placed.second;
  if (args.contains("width") || args.contains("height") ||
      args.contains("x") || args.contains("y")) {
    VALTZ_ASSIGN(auto box, box_(args, frame));
    auto* p = _c.project(s.project);
    VALTZ_ASSIGN(media::PixelSize content,
                 _c.shown_size_(*p, *asset, 0, 0));
    VALTZ_TRY(_c.set_crop(s.project, comp, box_crop_(content, frame, box),
                          layer));
  }
  if (flag(args, "below_all")) {
    VALTZ_TRY(_c.place_layers(s.project, comp, {layer}, std::nullopt, ""));
  }
  return Json{{"layer", model_layer(layer)}, {"page", page + 1}};
}

Result<Json>
Controller::AgentHost::t_set_layer_box(Session& s, Step& step,
                                       const Json& args,
                                       std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
  VALTZ_ASSIGN(auto box, box_(args, frame));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const auto it = std::ranges::find_if(
      a.layers, [&](const project::Layer& l) { return l.id == layer; });
  if (it == a.layers.end() || !it->source) {
    return make_error(Code::NotFound, std::format(
        "no layer {} showing a picture", model_layer(layer)));
  }
  VALTZ_ASSIGN(media::PixelSize content,
               _c.shown_size_(*p, *it->source, it->source_version, 0));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_crop(s.project, comp, box_crop_(content, frame, box),
                        layer));
  return Json{{"layer", model_layer(layer)},
              {"box", layer_box_(s.project, *p->asset(comp), *it)}};
}

Result<Json>
Controller::AgentHost::t_set_layer_pages(Session& s, Step& step,
                                         const Json& args,
                                         std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  VALTZ_ASSIGN(std::int64_t first, page_(s.project, comp, args, "first"));
  const auto count = static_cast<std::int64_t>(
      std::max(0.0, number(args, "count", 0)));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_layer_pages(s.project, comp, layer, first, count));
  return Json{{"layer", model_layer(layer)}, {"first", first + 1},
              {"count", count}};
}

Result<Json>
Controller::AgentHost::t_arrange_layer(Session& s, Step& step,
                                       const Json& args,
                                       std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  const std::string to = text_arg(args, "to");
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  if (to == "up" || to == "down") {
    VALTZ_TRY(_c.move_layer(s.project, comp, layer, to == "up" ? 1 : -1));
  } else if (to == "bottom") {
    VALTZ_TRY(_c.place_layers(s.project, comp, {layer}, std::nullopt, ""));
  } else if (to == "top") {
    auto* p = _c.project(s.project);
    VALTZ_ASSIGN(project::Asset a, p->asset(comp));
    if (!a.layers.empty() && a.layers.back().id != layer) {
      VALTZ_TRY(_c.place_layers(s.project, comp, {layer},
                                a.layers.back().id, ""));
    }
  } else {
    return make_error(Code::InvalidArgument,
                      "to is top, bottom, up or down");
  }
  return Json{{"layer", model_layer(layer)}, {"to", to}};
}

Result<Json>
Controller::AgentHost::t_remove_layer(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.remove_layer(s.project, comp, layer));
  return Json{{"removed", model_layer(layer)}};
}

Result<Json>
Controller::AgentHost::t_rename_layer(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.rename_layer(s.project, comp, layer,
                            one_line(text_arg(args, "name"))));
  return Json{{"layer", model_layer(layer)}};
}

Result<Json>
Controller::AgentHost::t_set_layer_visible(Session& s, Step& step,
                                           const Json& args,
                                           std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_layer_visible(s.project, comp, layer,
                                 flag(args, "visible", true)));
  return Json{{"layer", model_layer(layer)},
              {"visible", flag(args, "visible", true)}};
}

Result<Json>
Controller::AgentHost::t_adjust_layer(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  Json a = Json::object();
  for (const char* k : {"exposure", "contrast", "highlights", "shadows",
                        "vibrance", "saturation", "temperature", "tint"}) {
    if (args.contains(k)) {
      a[k] = number(args, k, 0);
    }
  }
  const media::Adjustments adj = media::adjustments_from_json(a);
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_adjustments(s.project, comp, adj, layer));
  return Json{{"layer", model_layer(layer)}, {"look", media::to_json(adj)}};
}

Result<Json>
Controller::AgentHost::t_create_prompt(Session& s, Step& step,
                                       const Json& args,
                                       std::vector<fs::path>&)
{
  const std::string text = text_arg(args, "text");
  if (one_line(text).empty()) {
    return make_error(Code::InvalidArgument, "the prompt is empty");
  }
  auto undo = _c.command_(s.project, "agent", {}, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(AssetId id,
               _c.capture_prompt(s.project, text, {}, {}, std::nullopt,
                                 one_line(text_arg(args, "name"))));
  return Json{{"asset", id.str()}};
}

// ---- generating -------------------------------------------------------------

Result<Json>
Controller::AgentHost::t_generate_image(Session& s, Step& step,
                                        const Json& args,
                                        std::vector<fs::path>&)
{
  const std::string prompt = text_arg(args, "prompt");
  if (one_line(prompt).empty()) {
    return make_error(Code::InvalidArgument, "the prompt is empty");
  }
  Landing l;
  l.session = s.id;
  l.step = step.n;
  l.project = s.project;
  // Where it lands: a box on a page of a composition -- unless none is
  // named and none is open, when it is an asset alone.
  const bool placed = args.contains("composition") ||
                      args.contains("page") || args.contains("x") ||
                      args.contains("width");
  bool has_target = false;
  {
    std::lock_guard lk(s.mu);
    has_target = s.target.has_value();
  }
  std::pair<int, int> size{1024, 1024};
  if (placed || has_target) {
    VALTZ_ASSIGN(AssetId comp, composition_(s, args));
    VALTZ_ASSIGN(l.page, page_(s.project, comp, args));
    VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
    VALTZ_ASSIGN(auto box, box_(args, frame));
    l.composition = comp;
    l.box = box;
    l.bottom = flag(args, "below_all");
    size = picture_size(box[2], box[3]);
  }
  GenerateImageRequest req;
  req.project = s.project;
  req.prompt = prompt;
  // "Auto" as the person would say it; a model by its name as shown.
  req.model = text_arg(args, "model");
  {
    std::string low = req.model;
    for (auto& ch : low) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    if (low == "auto" || low == "default" || low == "auto's") {
      req.model.clear();
    } else if (!req.model.empty() && !_c._catalog.find(req.model)) {
      for (const auto& m : _c._catalog.models()) {
        if (m.name == req.model) {
          req.model = m.id;
        }
      }
    }
  }
  req.name = one_line(text_arg(args, "name"));
  req.width = size.first;
  req.height = size.second;
  req.mode = "generate";
  auto undo = _c.command_(s.project, "agent",
                          l.composition.value_or(AssetId{}), "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(JobId job, _c.generate_image(std::move(req)));
  {
    std::lock_guard lk(_mu);
    _landings[job] = l;
  }
  {
    std::lock_guard lk(s.mu);
    s.pending.insert(job);
  }
  step.job = job;
  Json r = {{"job", job.str()},
            {"queued", true},
            {"size", {size.first, size.second}}};
  if (auto rec = _c._jobs.get(job); rec && !rec->asset.is_nil()) {
    r["asset"] = rec->asset.str();
  }
  if (l.composition) {
    r["lands_on"] = {{"composition", l.composition->str()},
                     {"page", l.page + 1},
                     {"box", {(*l.box)[0], (*l.box)[1], (*l.box)[2],
                              (*l.box)[3]}}};
  }
  return r;
}

Result<Json>
Controller::AgentHost::t_wait_for_generations(Session& s, Step&,
                                              const Json&,
                                              std::vector<fs::path>&)
{
  if (!wait_pending_(s)) {
    return make_error(Code::Cancelled, "stopped");
  }
  std::lock_guard lk(s.mu);
  Json landed = Json::array();
  for (auto& x : s.landed) {
    landed.push_back(std::move(x));
  }
  s.landed.clear();
  return Json{{"landed", std::move(landed)}};
}

Result<Json>
Controller::AgentHost::t_ask_user(Session& s, Step&, const Json& args,
                                  std::vector<fs::path>&)
{
  const std::string q = text_arg(args, "question");
  {
    std::lock_guard lk(s.mu);
    s.answer.reset();
    s.state = "asking";
  }
  post_(s);
  std::unique_lock lk(s.mu);
  s.cv.wait(lk, [&] { return s.stop_asked || s.answer.has_value(); });
  if (s.stop_asked) {
    return make_error(Code::Cancelled, "stopped");
  }
  Json r = {{"question", q}, {"answer", *s.answer}};
  s.answer.reset();
  return r;
}

// ---- timelines: watching ---------------------------------------------------

Result<Json>
Controller::AgentHost::t_describe_clip(Session& s, Step&, const Json& args,
                                       std::vector<fs::path>&)
{
  const auto asset = AssetId::parse(text_arg(args, "asset"));
  if (!asset) {
    return make_error(Code::InvalidArgument, "which clip?");
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset clip, p->asset(*asset));
  if (clip.kind != AssetKind::Video) {
    return make_error(Code::InvalidArgument,
                      std::format("\"{}\" is not a clip", clip.name));
  }
  // Told before -- by the person, or an earlier step -- and still about
  // it as it is: that summary.
  std::optional<AssetId> text;
  if (auto deps = p->dependents(*asset, false); deps.ok()) {
    for (const auto& d : *deps) {
      auto a = p->asset(d);
      auto r = a.ok() && a->head > 0 ? p->recipe(a->recipe)
                                     : Result<project::Recipe>(make_error(
                                           Code::NotFound, "none"));
      auto st = p->staleness(d);
      if (r.ok() && r->op == engine::kOpSummarizeVideo && st.ok() &&
          st->fresh()) {
        text = d;
      }
    }
  }
  if (!text) {
    VALTZ_ASSIGN(auto made, _c.summarize_video(s.project, *asset));
    text = made.first;
    if (!wait_job_(s, made.second)) {
      return make_error(Code::Cancelled, "stopped");
    }
    const auto rec = _c._jobs.get(made.second);
    if (rec && rec->state != JobState::Finished) {
      return make_error(Code::Engine, rec->message.empty()
                                          ? std::string("it could not be "
                                                        "watched")
                                          : rec->message);
    }
  }
  VALTZ_ASSIGN(project::AssetVersion v, p->version(*text));
  const Json sum = jget(v.outputs, "summary", Json::object());
  Json out = {{"clip", asset->str()},
              {"name", clip.name},
              {"summary", text->str()}};
  if (!sum.is_object() || !sum.contains("scenes")) {
    // Its words as they are kept (edited, perhaps).
    auto words = p->read_text(*text);
    out["text"] = words.ok() ? std::string(utf8_prefix(*words, 8192))
                             : std::string();
    return out;
  }
  out["seconds"] = secs(jget(sum, "seconds", 0.0));
  out["overall"] = jget<std::string>(sum, "overall", "");
  Json scenes = Json::array();
  for (const auto& sc : jget(sum, "scenes", Json::array())) {
    scenes.push_back({{"start", secs(jget(sc, "start", 0.0))},
                      {"end", secs(jget(sc, "end", 0.0))},
                      {"text", jget<std::string>(sc, "text", "")}});
  }
  out["scenes"] = std::move(scenes);
  return out;
}

Result<Json>
Controller::AgentHost::t_view_frames(Session& s, Step& step, const Json& args,
                                     std::vector<fs::path>& shown)
{
  std::optional<AssetId> id;
  if (const auto t = text_arg(args, "asset"); !t.empty()) {
    id = AssetId::parse(t);
    if (!id) {
      return make_error(Code::InvalidArgument,
                        std::format("'{}' is not an asset id", t));
    }
  } else {
    std::lock_guard lk(s.mu);
    id = s.target;
  }
  if (!id) {
    return make_error(Code::InvalidArgument, "which clip?");
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(*id));
  if (a.kind != AssetKind::Video) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is not a clip or a timeline with a picture: a still's "
        "pages are view_page's", a.name));
  }
  Rational rate{24, 1};
  if (a.cls == AssetClass::Composition) {
    rate = _c.rate_of_(*p, a);
  } else if (auto v = p->version(*id); v.ok() &&
                                       v->info.frame_rate.num > 0) {
    rate = v->info.frame_rate;
  }
  const double fps = rate.to_double();
  const double seconds = source_seconds(_c, *p, a);
  if (!s.backend->sees()) {
    return Json{{"shown", false},
                {"seconds", secs(seconds)},
                {"note", "You cannot see pictures: describe_clip tells a "
                         "clip in words, get_composition the timeline."}};
  }
  std::vector<double> times;
  if (args.contains("times") && args["times"].is_array()) {
    for (const auto& t : args["times"]) {
      if (t.is_number() && times.size() < 6) {
        times.push_back(t.get<double>());
      }
    }
  }
  if (times.empty()) {
    const int n = std::clamp(static_cast<int>(number(args, "count", 4)),
                             1, 6);
    for (int i = 0; i < n; ++i) {
      times.push_back(seconds * (i + 0.5) / n);
    }
  }
  Json told = Json::array();
  for (std::size_t i = 0; i < times.size(); ++i) {
    const double t =
        std::clamp(times[i], 0.0, std::max(0.0, seconds - 1 / fps));
    VALTZ_ASSIGN(fs::path drawn,
                 _c.rendered_frame_(s.project, *id, 0,
                                    static_cast<std::int64_t>(t * fps), 0));
    const fs::path small =
        s.dir / std::format("frame-{}-{}.jpg", step.n, i);
    VALTZ_TRY(media::write_thumbnail(drawn, media::MediaType::Image, 512,
                                     small));
    shown.push_back(small);
    told.push_back(secs(t));
  }
  return Json{{"shown", true},
              {"seconds", secs(seconds)},
              {"times", std::move(told)},
              {"note", "The frames follow, as pictures, in this order."}};
}

// ---- timelines: clips and sounds -------------------------------------------

Result<Json>
Controller::AgentHost::t_add_clip(Session& s, Step& step, const Json& args,
                                  std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, timeline_(s, args));
  const auto asset = AssetId::parse(text_arg(args, "asset"));
  if (!asset) {
    return make_error(Code::InvalidArgument, "which clip?");
  }
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset src, p->asset(*asset));
  VALTZ_ASSIGN(project::Asset tl, p->asset(comp));
  const bool sound_tl = tl.kind == AssetKind::Audio;
  const bool picture = src.kind == AssetKind::Image;
  if (src.kind == AssetKind::Audio) {
    return make_error(Code::InvalidArgument,
                      "a sound goes on with add_sound");
  }
  if (!picture && src.kind != AssetKind::Video) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is not a clip or a picture", src.name));
  }
  if (picture && sound_tl) {
    return make_error(Code::InvalidArgument,
                      "a composition of sound shows no pictures");
  }
  const std::string transition = text_arg(args, "transition");
  if (!transition.empty() && transition != "dissolve" &&
      transition != "cut") {
    return make_error(Code::InvalidArgument,
                      "a transition is dissolve or cut");
  }
  const Rational rate = rate_(s.project, comp);
  const double fps = rate.to_double();
  VALTZ_ASSIGN(auto times, timings_(s.project, comp));
  // The clip before it -- the one that ends last -- and the topmost clip
  // or picture, which it goes above: titles stay over the clips.
  std::optional<std::string> before, top;
  double before_end = 0;
  for (const auto& l : tl.layers) {
    const std::string k = layer_kind_(s.project, l);
    if (k != "clip" && k != "picture") {
      continue;
    }
    top = l.id;
    const auto it = times.find(l.id);
    if (it != times.end() && std::isfinite(it->second.length) &&
        it->second.end() >= before_end) {
      before = l.id;
      before_end = it->second.end();
    }
  }
  const double overlap =
      transition == "dissolve"
          ? std::clamp(number(args, "transition_seconds", 0.5), 0.0, 10.0)
          : 0.0;
  double start = args.contains("start")
                     ? std::max(0.0, number(args, "start", 0))
                     : std::max(0.0, before_end - overlap);
  if (args.contains("start")) {
    // Given: the clip it follows is the one starting before it that
    // ends last.
    before.reset();
    before_end = 0;
    for (const auto& l : tl.layers) {
      const std::string k = layer_kind_(s.project, l);
      const auto it = times.find(l.id);
      if ((k == "clip" || k == "picture") && it != times.end() &&
          std::isfinite(it->second.length) &&
          it->second.start < start - 1e-3 &&
          it->second.end() >= before_end) {
        before = l.id;
        before_end = it->second.end();
      }
    }
  }
  // A dissolve overlaps the clip before by its length, whatever start
  // was given -- unless that start overlaps it already.
  if (before && overlap > 0 && start > before_end - overlap) {
    start = std::max(0.0, before_end - overlap);
  }
  // Right after the clip before: from the frame it has ended by.
  const std::int64_t start_f =
      !args.contains("start") && overlap == 0
          ? static_cast<std::int64_t>(std::ceil(start * fps - 1e-6))
          : frames_of(start, fps);
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(auto placed, _c.instantiate(s.project, *asset, comp, top,
                                           start_f, false));
  const std::string layer = placed.second;
  if (!top && tl.layers.size() > 0) {
    // The first clip: under what is there (titles, sounds).
    VALTZ_TRY(_c.place_layers(s.project, comp, {layer}, std::nullopt, ""));
  }
  project::LayerTime t;
  t.offset = start_f;
  std::string clamped;
  if (picture) {
    t.duration = std::max<std::int64_t>(
        1, frames_of(number(args, "duration", 3), fps));
  } else if (args.contains("in") || args.contains("out")) {
    const Rational mr = mark_rate_(s.project, *asset, sound_tl);
    const double whole = source_seconds(_c, *p, src);
    const double in = std::clamp(number(args, "in", 0), 0.0, whole);
    const double out =
        std::clamp(number(args, "out", whole), in, whole);
    if (number(args, "out", 0) > whole + 0.05) {
      clamped = std::format("\"{}\" is {:.2f} s long: its part ends "
                            "there.", src.name, whole);
    }
    t.in = frames_of(in, mr.to_double());
    t.out = std::max(t.in, frames_of(out, mr.to_double()) - 1);
    t.rate = mr;
  }
  VALTZ_TRY(_c.set_layer_time(s.project, comp, layer, t));
  const double speed = std::clamp(number(args, "speed", 1), 0.1, 10.0);
  if (!picture && std::abs(speed - 1) > 1e-3) {
    media::KeyedSpeed k;
    k.rate = rate;
    k.keys.push_back({0, media::Speed{speed}});
    VALTZ_TRY(_c.set_speed_keys(s.project, comp, k, layer));
  }
  if (!sound_tl) {
    // Fitted to the frame: whole, or covering it.
    VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
    VALTZ_ASSIGN(media::PixelSize content,
                 _c.shown_size_(*p, *asset, 0, 0));
    if (content.width > 0 && content.height > 0 &&
        (content.width != frame.width || content.height != frame.height)) {
      media::Crop c = box_crop_(
          content, frame,
          {0, 0, static_cast<double>(frame.width),
           static_cast<double>(frame.height)});
      if (text_arg(args, "fit") == "fill") {
        c.scale_x = c.scale_y = std::max(
            static_cast<double>(frame.width) / content.width,
            static_cast<double>(frame.height) / content.height);
      }
      VALTZ_TRY(_c.set_crop(s.project, comp, c, layer));
    }
  }
  VALTZ_ASSIGN(times, timings_(s.project, comp));
  const media::LayerTiming mine = times[layer];
  const double volume = std::clamp(number(args, "volume", 1), 0.0, 2.0);
  if (!picture && std::abs(volume - 1) > 1e-3) {
    VALTZ_TRY(shape_sound_(s.project, comp, layer, volume, 0, 0,
                           mine.length));
  }
  Json out = {{"layer", model_layer(layer)},
              {"start", secs(mine.start)},
              {"end", secs(mine.end())}};
  if (mine.timed) {
    out["in"] = secs(mine.in);
    out["out"] = secs(mine.out);
  }
  if (transition == "dissolve") {
    const auto b = before ? times.find(*before) : times.end();
    if (b != times.end() && mine.start < b->second.end() - 1e-3) {
      VALTZ_TRY(_c.set_transition(s.project, comp, *before, layer,
                                  "dissolve"));
      out["transition"] = {{"from", model_layer(*before)},
                           {"kind", "dissolve"},
                           {"seconds", secs(b->second.end() - mine.start)}};
    } else {
      out["note"] = "It does not overlap a clip before it: a plain cut.";
    }
  }
  if (!clamped.empty()) {
    out["note"] = clamped;
  }
  if (auto n = _c.composition_length(s.project, comp); n.ok()) {
    out["timeline_seconds"] = secs(static_cast<double>(*n) / fps);
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_add_sound(Session& s, Step& step, const Json& args,
                                   std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, timeline_(s, args));
  const auto asset = AssetId::parse(text_arg(args, "asset"));
  if (!asset) {
    return make_error(Code::InvalidArgument, "which sound?");
  }
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset src, p->asset(*asset));
  if (src.kind != AssetKind::Audio) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is not a sound: a clip goes on with add_clip", src.name));
  }
  const double fps = rate_(s.project, comp).to_double();
  const double start = std::max(0.0, number(args, "start", 0));
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(auto placed,
               _c.instantiate(s.project, *asset, comp, std::nullopt,
                              frames_of(start, fps), false));
  const std::string layer = placed.second;
  project::LayerTime t;
  t.offset = frames_of(start, fps);
  std::string clamped;
  if (args.contains("in") || args.contains("out")) {
    const double whole = source_seconds(_c, *p, src);
    const double in = std::clamp(number(args, "in", 0), 0.0, whole);
    const double out = std::clamp(number(args, "out", whole), in, whole);
    if (number(args, "out", 0) > whole + 0.05) {
      clamped = std::format("\"{}\" is {:.2f} s long: its part ends "
                            "there.", src.name, whole);
    }
    const Rational mr = mark_rate_(s.project, *asset, true);
    t.in = frames_of(in, mr.to_double());
    t.out = std::max(t.in, frames_of(out, mr.to_double()) - 1);
    t.rate = mr;
  }
  VALTZ_TRY(_c.set_layer_time(s.project, comp, layer, t));
  VALTZ_ASSIGN(auto times, timings_(s.project, comp));
  const media::LayerTiming mine = times[layer];
  VALTZ_TRY(shape_sound_(s.project, comp, layer,
                         number(args, "volume", 1),
                         number(args, "fade_in", 0),
                         number(args, "fade_out", 0), mine.length));
  Json out = {{"layer", model_layer(layer)},
              {"start", secs(mine.start)},
              {"end", secs(mine.end())},
              {"in", secs(mine.in)},
              {"out", secs(mine.out)}};
  if (!clamped.empty()) {
    out["note"] = clamped;
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_set_layer_time(Session& s, Step& step,
                                        const Json& args,
                                        std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, timeline_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const auto it = std::ranges::find(a.layers, layer, &project::Layer::id);
  if (it == a.layers.end()) {
    return make_error(Code::NotFound,
                      std::format("no layer {}", model_layer(layer)));
  }
  const std::string kind = layer_kind_(s.project, *it);
  const bool timed = kind == "clip" || kind == "sound";
  if (args.contains("duration") && timed) {
    return make_error(Code::InvalidArgument,
                      "a clip's or a sound's length is its part: set its "
                      "in and out");
  }
  if ((args.contains("in") || args.contains("out") ||
       args.contains("speed")) && !timed) {
    return make_error(Code::InvalidArgument,
                      "only a clip or a sound has a part and a speed");
  }
  const Rational rate = rate_(s.project, comp);
  const double fps = rate.to_double();
  VALTZ_ASSIGN(auto times, timings_(s.project, comp));
  const media::LayerTiming was = times[layer];
  project::LayerTime t = it->time;
  if (args.contains("start")) {
    t.offset = frames_of(number(args, "start", 0), fps);
  }
  if (args.contains("duration")) {
    t.duration = std::max<std::int64_t>(
        1, frames_of(number(args, "duration", 3), fps));
  }
  std::string clamped;
  if (args.contains("in") || args.contains("out")) {
    VALTZ_ASSIGN(project::Asset src, p->asset(*it->source));
    const double whole = source_seconds(_c, *p, src);
    if (number(args, "out", 0) > whole + 0.05) {
      clamped = std::format("\"{}\" is {:.2f} s long: its part ends "
                            "there.", src.name, whole);
    }
    const double in =
        std::clamp(number(args, "in", was.in), 0.0, whole);
    const double out = std::clamp(
        number(args, "out", std::isfinite(was.out) ? was.out : whole), in,
        whole);
    const Rational mr = mark_rate_(s.project, *it->source,
                                   a.kind == AssetKind::Audio);
    t.in = frames_of(in, mr.to_double());
    t.out = std::max(t.in, frames_of(out, mr.to_double()) - 1);
    t.rate = mr;
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_layer_time(s.project, comp, layer, t));
  if (args.contains("speed")) {
    const double speed = std::clamp(number(args, "speed", 1), 0.1, 10.0);
    media::KeyedSpeed k;
    k.rate = rate;
    k.keys.push_back({0, media::Speed{speed}});
    VALTZ_TRY(_c.set_speed_keys(s.project, comp, k, layer));
  }
  VALTZ_ASSIGN(times, timings_(s.project, comp));
  const media::LayerTiming now = times[layer];
  // Its fades kept at its ends, wherever they are now.
  const media::KeyedSound keys = Controller::sound_keys_of(a, layer);
  if (timed && !keys.identity() &&
      std::abs(now.length - was.length) > 1e-3) {
    const SoundShape sh = shape_of(keys, fps);
    VALTZ_TRY(shape_sound_(s.project, comp, layer, sh.volume, sh.fade_in,
                           sh.fade_out, now.length));
  }
  Json out = {{"layer", model_layer(layer)},
              {"start", secs(now.start)},
              {"end", std::isfinite(now.length) ? Json(secs(now.end()))
                                                : Json("the end")}};
  if (now.timed) {
    out["in"] = secs(now.in);
    out["out"] = secs(now.out);
  }
  if (!clamped.empty()) {
    out["note"] = clamped;
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_set_layer_sound(Session& s, Step& step,
                                         const Json& args,
                                         std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, timeline_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const auto it = std::ranges::find(a.layers, layer, &project::Layer::id);
  if (it == a.layers.end()) {
    return make_error(Code::NotFound,
                      std::format("no layer {}", model_layer(layer)));
  }
  const std::string kind = layer_kind_(s.project, *it);
  if (kind != "clip" && kind != "sound") {
    return make_error(Code::InvalidArgument,
                      "only a clip or a sound has a volume");
  }
  const double fps = rate_(s.project, comp).to_double();
  const SoundShape was = shape_of(Controller::sound_keys_of(a, layer), fps);
  const double volume = number(args, "volume", was.volume);
  const double fade_in = number(args, "fade_in", was.fade_in);
  const double fade_out = number(args, "fade_out", was.fade_out);
  VALTZ_ASSIGN(auto times, timings_(s.project, comp));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(shape_sound_(s.project, comp, layer, volume, fade_in, fade_out,
                         times[layer].length));
  return Json{{"layer", model_layer(layer)},
              {"volume", std::clamp(volume, 0.0, 2.0)},
              {"fade_in", std::max(0.0, fade_in)},
              {"fade_out", std::max(0.0, fade_out)}};
}

Result<Json>
Controller::AgentHost::t_set_transition(Session& s, Step& step,
                                        const Json& args,
                                        std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, timeline_(s, args));
  std::string from = text_arg(args, "from");
  std::string to = text_arg(args, "to");
  if (from.empty() || to.empty() || from == to) {
    return make_error(Code::InvalidArgument, "between which layers?");
  }
  from = from == "0" ? std::string() : from;
  to = to == "0" ? std::string() : to;
  std::string kind = text_arg(args, "kind", "dissolve");
  if (kind != "dissolve" && kind != "cut" && kind != "none") {
    return make_error(Code::InvalidArgument,
                      "a transition is dissolve, cut or none");
  }
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const double fps = rate_(s.project, comp).to_double();
  VALTZ_ASSIGN(auto times, timings_(s.project, comp));
  if (!times.contains(from) || !times.contains(to)) {
    return make_error(Code::NotFound, "no such layers");
  }
  // The earlier one first, whichever way they were named.
  if (times[to].start < times[from].start) {
    std::swap(from, to);
  }
  auto overlap = [&] {
    return std::min(times[from].end(), times[to].end()) -
           std::max(times[from].start, times[to].start);
  };
  auto undo = _c.command_(s.project, "agent", comp, from,
                          {{"tool", step.tool}, {"title", step.title}});
  Json out = {{"from", model_layer(from)}, {"to", model_layer(to)},
              {"kind", kind}};
  if (kind == "dissolve") {
    // Too short an overlap (none: they only meet): the later one -- and
    // all that starts with or after it -- moves earlier, so the two
    // overlap by the dissolve's length.
    const double want = std::clamp(number(args, "seconds", 0.5), 1 / fps,
                                   10.0);
    const double shift = want - overlap();
    if (shift > 0.5 / fps) {
      const double from_start = times[to].start;
      for (const auto& l : a.layers) {
        const media::LayerTiming& t = times[l.id];
        if (l.id == from || t.start < from_start - 1e-3) {
          continue;
        }
        project::LayerTime lt = l.time;
        lt.offset = frames_of(t.start - shift, fps);
        VALTZ_TRY(_c.set_layer_time(s.project, comp, l.id, lt));
      }
      VALTZ_ASSIGN(times, timings_(s.project, comp));
      out["moved"] = std::format(
          "layer {} and all that starts after it {:.2f} s earlier",
          model_layer(to), shift);
    }
    VALTZ_TRY(_c.set_transition(s.project, comp, from, to, "dissolve"));
    out["seconds"] = secs(std::max(0.0, overlap()));
  } else if (kind == "cut" && overlap() > 0.5 / fps) {
    VALTZ_TRY(_c.set_transition(s.project, comp, from, to, "cut"));
  } else {
    // None -- or a cut where they only meet: one plays, then the other.
    VALTZ_TRY(_c.set_transition(s.project, comp, from, to, ""));
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_set_timeline_length(Session& s, Step& step,
                                             const Json& args,
                                             std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, timeline_(s, args));
  const double fps = rate_(s.project, comp).to_double();
  const double seconds = std::max(0.0, number(args, "seconds", 0));
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_timeline(s.project, comp, frames_of(seconds, fps)));
  VALTZ_ASSIGN(std::int64_t n, _c.composition_length(s.project, comp));
  const double length = static_cast<double>(n) / fps;
  Json out = {{"seconds", secs(length)}};
  // Past its last clip or picture, the picture is black.
  double shown = 0;
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  VALTZ_ASSIGN(auto times, timings_(s.project, comp));
  for (const auto& l : a.layers) {
    const std::string k = layer_kind_(s.project, l);
    const auto it = times.find(l.id);
    if ((k == "clip" || k == "picture") && it != times.end() &&
        std::isfinite(it->second.length)) {
      shown = std::max(shown, it->second.end());
    }
  }
  if (a.kind != AssetKind::Audio && length > shown + 0.5) {
    out["note"] = std::format(
        "The clips end at {:.2f} s: from there to {:.2f} s the picture is "
        "black. Make the video longer with more or longer clips instead.",
        shown, length);
  }
  // A sound running past it is cut at it, its fade-out moved there.
  Json cut = Json::array();
  for (const auto& l : a.layers) {
    const auto it = times.find(l.id);
    if (seconds > 0 && layer_kind_(s.project, l) == "sound" &&
        it != times.end() && it->second.start < length &&
        it->second.end() > length + 0.05) {
      VALTZ_TRY(cut_at_(s.project, comp, l.id, length));
      cut.push_back(model_layer(l.id));
    }
  }
  if (!cut.empty()) {
    out["sounds_cut_at_the_end"] = std::move(cut);
  }
  return out;
}

// ---- timelines: music and voice --------------------------------------------

Result<Json>
Controller::AgentHost::t_generate_music(Session& s, Step& step,
                                        const Json& args,
                                        std::vector<fs::path>&)
{
  const std::string style = one_line(text_arg(args, "style"));
  if (style.empty()) {
    return make_error(Code::InvalidArgument, "the music's style is empty");
  }
  std::string lyrics = text_arg(args, "lyrics");
  if (!one_line(lyrics).empty() && lyrics.find('[') == std::string::npos) {
    lyrics = "[Verse]\n" + lyrics;
  }
  Landing l;
  l.session = s.id;
  l.step = step.n;
  l.project = s.project;
  l.sound = true;
  l.seconds = std::clamp(number(args, "seconds", 30), 1.0, 240.0);
  l.volume = std::clamp(number(args, "volume", 0.8), 0.0, 2.0);
  l.fade_in = std::max(0.0, number(args, "fade_in", 0));
  l.fade_out = std::max(0.0, number(args, "fade_out", 2));
  l.fit = true;
  // Onto the timeline named, or the one open; else an asset alone.
  std::optional<AssetId> comp;
  if (args.contains("composition")) {
    VALTZ_ASSIGN(AssetId c, timeline_(s, args));
    comp = c;
  } else if (auto c = timeline_(s, args); c.ok()) {
    comp = *c;
  }
  if (comp) {
    l.composition = comp;
    l.start = frames_of(number(args, "start", 0),
                        rate_(s.project, *comp).to_double());
  }
  GenerateAudioRequest req;
  req.project = s.project;
  req.prompt = one_line(lyrics).empty() ? style : style + "\n\n" + lyrics;
  req.name = one_line(text_arg(args, "name"));
  // A little past where it is cut: the cut fades out over music.
  req.max_seconds = std::ceil(l.seconds + l.fade_out);
  auto undo = _c.command_(s.project, "agent", comp.value_or(AssetId{}), "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(JobId job, _c.generate_audio(std::move(req)));
  {
    std::lock_guard lk(_mu);
    _landings[job] = l;
  }
  {
    std::lock_guard lk(s.mu);
    s.pending.insert(job);
  }
  step.job = job;
  Json r = {{"job", job.str()}, {"queued", true}};
  if (auto rec = _c._jobs.get(job); rec && !rec->asset.is_nil()) {
    r["asset"] = rec->asset.str();
  }
  if (comp) {
    const double start = number(args, "start", 0);
    r["lands_on"] = {{"composition", comp->str()},
                     {"start", secs(start)},
                     {"end", secs(start + l.seconds)}};
  }
  return r;
}

Result<Json>
Controller::AgentHost::t_generate_voiceover(Session& s, Step& step,
                                            const Json& args,
                                            std::vector<fs::path>&)
{
  const std::string words = text_arg(args, "text");
  if (one_line(words).empty()) {
    return make_error(Code::InvalidArgument, "no words to speak");
  }
  // The model that speaks here (a quantized variant is its source's
  // choice, by Favor).
  const models::ModelEntry* m = nullptr;
  for (const auto& e : _c._catalog.models()) {
    if (e.has(models::Capability::TextToSpeech) &&
        e.quantize_from.empty() && _c.runs(e, "audio", "generate") &&
        (!m || e.rank > m->rank)) {
      m = &e;
    }
  }
  if (!m) {
    return make_error(Code::Unsupported, "no speech model runs here");
  }
  assist::SpeechText st;
  st.instruction = one_line(text_arg(args, "voice"));
  st.language = one_line(text_arg(args, "language"));
  if (st.language.empty()) {
    st.language = assist::speech_language(words);
  }
  st.text = words;
  Landing l;
  l.session = s.id;
  l.step = step.n;
  l.project = s.project;
  l.sound = true;
  l.volume = std::clamp(number(args, "volume", 1), 0.0, 2.0);
  std::optional<AssetId> comp;
  if (args.contains("composition")) {
    VALTZ_ASSIGN(AssetId c, timeline_(s, args));
    comp = c;
  } else if (auto c = timeline_(s, args); c.ok()) {
    comp = *c;
  }
  if (comp) {
    l.composition = comp;
    l.start = frames_of(number(args, "start", 0),
                        rate_(s.project, *comp).to_double());
  }
  GenerateAudioRequest req;
  req.project = s.project;
  req.prompt = assist::speech_prompt(st);
  req.model = m->id;
  req.name = one_line(text_arg(args, "name"));
  auto undo = _c.command_(s.project, "agent", comp.value_or(AssetId{}), "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(JobId job, _c.generate_audio(std::move(req)));
  {
    std::lock_guard lk(_mu);
    _landings[job] = l;
  }
  {
    std::lock_guard lk(s.mu);
    s.pending.insert(job);
  }
  step.job = job;
  Json r = {{"job", job.str()}, {"queued", true}, {"model", m->name}};
  if (auto rec = _c._jobs.get(job); rec && !rec->asset.is_nil()) {
    r["asset"] = rec->asset.str();
  }
  if (comp) {
    r["lands_on"] = {{"composition", comp->str()},
                     {"start", secs(number(args, "start", 0))}};
  }
  return r;
}

}  // namespace valtz
