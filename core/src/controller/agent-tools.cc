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
#include "valtz/media/text-flow.h"
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
const char* kPageDoc =
    "the page, from 1 (needed on a still of more than one page)";
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
      {"gray", "#808080"},  {"grey", "#808080"},  {"transparent", "#00000000"},
      {"none", "#00000000"}};
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

// On that page and no other.
bool
alone_on(const project::LayerTime& t, std::int64_t page)
{
  return t.offset == page && t.duration == 1;
}

// Across every page: from the first to the last.
bool
runs_across(const project::LayerTime& t)
{
  return t.offset == 0 && t.duration <= 0;
}

// The markups across every page the agent writes on (front_markup_,
// back_markup_), found again by their names.
const char* kFrontName = "Every Page";
const char* kBackName = "Every Page (Background)";
const char* kPageBackName = "Page Background";
// The folder of the page progress bar's layers (add_page_progress),
// whose layers a new bar replaces.
const char* kProgressName = "Page Progress";

// Text made LOOSE to find a passage the model quotes from the person's
// message: lower case, punctuation gone (typographic quotes and dashes
// too), one space between words -- and where each byte came from.
struct Loose {
  std::string              text;
  std::vector<std::size_t> at;
};

Loose
loosen(std::string_view s)
{
  Loose l;
  bool space = true;
  for (std::size_t i = 0; i < s.size(); ++i) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c == 0xe2 && i + 2 < s.size() &&
        static_cast<unsigned char>(s[i + 1]) == 0x80) {
      i += 2;  // U+2000-U+203F: quotes, dashes, an ellipsis
      continue;
    }
    if (c >= 0x80 || std::isalnum(c)) {
      l.text += static_cast<char>(c >= 0x80 ? c : std::tolower(c));
      l.at.push_back(i);
      space = false;
    } else if (std::isspace(c) && !space) {
      l.text += ' ';
      l.at.push_back(i);
      space = true;
    }
  }
  if (!l.text.empty() && l.text.back() == ' ') {
    l.text.pop_back();
    l.at.pop_back();
  }
  return l;
}

// Where `needle` is in `hay` from byte `from`, loosely: its first byte and
// the byte after its last -- widened to the whole words around it (an
// opening quote, a closing full stop).
std::optional<std::pair<std::size_t, std::size_t>>
find_loose(std::string_view hay, std::string_view needle, std::size_t from)
{
  const Loose h = loosen(hay);
  const Loose n = loosen(needle);
  if (n.text.empty()) {
    return std::nullopt;
  }
  std::size_t start = 0;
  while (start < h.at.size() && h.at[start] < from) {
    ++start;
  }
  const auto at = h.text.find(n.text, start);
  if (at == std::string::npos) {
    return std::nullopt;
  }
  std::size_t a = h.at[at];
  std::size_t b = h.at[at + n.text.size() - 1] + 1;
  auto space = [&](std::size_t i) {
    return std::isspace(static_cast<unsigned char>(hay[i])) != 0;
  };
  while (a > from && !space(a - 1)) {
    --a;
  }
  while (b < hay.size() && !space(b)) {
    ++b;
  }
  return std::pair{a, b};
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

// The size a SHOT for a frame is made at: the frame's own when it is no
// bigger than the model's default size (H3's 960 x 544: what it makes
// best, and what fits the memory), else the frame's shape at that area,
// on the model's grid -- laid filling the frame.
std::pair<int, int>
shot_size(media::PixelSize frame, int w, int h, int grid)
{
  const double area = static_cast<double>(w) * h;
  if (frame.width <= 0 || frame.height <= 0) {
    return {w, h};
  }
  if (static_cast<double>(frame.width) * frame.height <= area * 1.05) {
    return {frame.width, frame.height};
  }
  const double aspect = std::clamp(
      static_cast<double>(frame.width) / frame.height, 0.25, 4.0);
  const double sh = round_to(std::sqrt(area / aspect), grid);
  const double sw = round_to(sh * aspect, grid);
  return {static_cast<int>(sw), static_cast<int>(sh)};
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

// The EFFECTS a tool's arguments ask for (media/markup.h) laid on `fx`
// (an object's own, or none): `shadow` / `glow` true adds or remakes it,
// false takes it off; `shadow_color` / `glow_color` its colour. Sized
// from `base` -- a text's font size, a shape's from its shorter side --
// a glow in the object's own colour unless told. Kept in the core's
// order: the glow inside, the shadow of it all outside.
Result<Json>
effects_arg(const Json& args, Json fx, double base,
            const std::array<double, 4>& own)
{
  if (!fx.is_array()) {
    fx = Json::array();
  }
  auto set = [&](const char* kind, std::optional<Json> made) {
    Json out = Json::array();
    for (const auto& e : fx) {
      if (jget<std::string>(e, "kind", "") != kind) {
        out.push_back(e);
      }
    }
    if (made) {
      out.push_back(*made);
    }
    fx = std::move(out);
  };
  auto rgba = [](const std::array<double, 4>& c) {
    return Json::array({c[0], c[1], c[2], c[3]});
  };
  if (args.contains("shadow") || args.contains("shadow_color")) {
    std::optional<Json> made;
    if (flag(args, "shadow", true)) {
      auto c = colour(text_arg(args, "shadow_color", "#00000080"));
      if (!c) {
        return make_error(Code::InvalidArgument,
                          "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
      }
      const double d = std::max(2.0, std::round(base * 0.07));
      made = Json{{"kind", "shadow"}, {"color", rgba(*c)},
                  {"blur", std::max(3.0, std::round(base * 0.2))},
                  {"dx", d}, {"dy", d}};
    }
    set("shadow", made);
  }
  if (args.contains("glow") || args.contains("glow_color")) {
    std::optional<Json> made;
    if (flag(args, "glow", true)) {
      std::array<double, 4> mine = own;
      mine[3] = std::max(mine[3], 0.8);
      auto c = args.contains("glow_color")
                   ? colour(text_arg(args, "glow_color"))
                   : std::optional(mine);
      if (!c) {
        return make_error(Code::InvalidArgument,
                          "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
      }
      made = Json{{"kind", "glow"}, {"color", rgba(*c)},
                  {"radius", std::max(4.0, std::round(base * 0.35))}};
    }
    set("glow", made);
  }
  Json ordered = Json::array();
  for (const auto& e : fx) {
    if (jget<std::string>(e, "kind", "") == "glow") {
      ordered.push_back(e);
    }
  }
  for (const auto& e : fx) {
    if (jget<std::string>(e, "kind", "") != "glow") {
      ordered.push_back(e);
    }
  }
  return ordered;
}

// A shape's size for its effects: a quarter of its shorter side, held to
// what reads (a hairline rule, a full-page panel).
double
shape_base(double w, double h)
{
  return std::clamp(std::min(std::abs(w), std::abs(h)) * 0.25, 16.0, 120.0);
}

// The GRADIENT a tool's arguments ask for (media/markup.h): `gradient`
// its colours, two or more, spread evenly ([] takes it off),
// `gradient_type` linear (by default) or radial, `gradient_angle` a
// linear one's direction in degrees -- 0 left to right, 90 top to bottom
// (by default) -- across the object's box; a radial one round its middle
// out to its side. Nothing asked: `keep`. Unchanged by an update giving
// only a type or an angle: its colours kept.
Result<Json>
gradient_arg(const Json& args, const Json& keep)
{
  if (!args.contains("gradient") && !args.contains("gradient_type") &&
      !args.contains("gradient_angle")) {
    return keep;
  }
  Json colours = Json::array();
  if (args.contains("gradient")) {
    const Json& g = args["gradient"];
    if (g.is_array()) {
      for (const auto& c : g) {
        if (!c.is_string()) {
          continue;
        }
        auto v = colour(c.get<std::string>());
        if (!v) {
          return make_error(Code::InvalidArgument,
                            "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
        }
        colours.push_back({(*v)[0], (*v)[1], (*v)[2], (*v)[3]});
      }
      if (colours.empty()) {
        return Json();  // [] -- none
      }
    }
  } else if (keep.is_object()) {
    for (const auto& st : jget(keep, "stops", Json::array())) {
      colours.push_back(jget(st, "color", Json::array({0, 0, 0, 1})));
    }
  }
  if (colours.size() < 2) {
    return make_error(Code::InvalidArgument,
                      "a gradient is two colours or more, e.g. "
                      "[\"#ff8a00\", \"#e52e71\"]");
  }
  Json stops = Json::array();
  for (std::size_t i = 0; i < colours.size(); ++i) {
    stops.push_back({{"at", static_cast<double>(i) /
                                (static_cast<double>(colours.size()) - 1)},
                     {"color", colours[i]}});
  }
  const bool radial = text_arg(args, "gradient_type",
                               jget<std::string>(keep, "kind", "linear")) ==
                      "radial";
  Json g = {{"kind", radial ? "radial" : "linear"},
            {"stops", std::move(stops)}};
  if (radial) {
    g["from"] = {0.5, 0.5};
    g["to"] = {1.0, 0.5};
  } else {
    const double a = number(args, "gradient_angle", 90) * M_PI / 180;
    const double c = std::cos(a) / 2, sn = std::sin(a) / 2;
    g["from"] = {0.5 - c, 0.5 - sn};
    g["to"] = {0.5 + c, 0.5 + sn};
  }
  return g;
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
// composition's. A clip's is its picture's frames, as a layer runs (its
// soundtrack may run on past the last frame).
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
  if (!v.ok()) {
    return 0;
  }
  const double fps = v->info.frame_rate.to_double();
  if (a.kind == AssetKind::Video && fps > 0 && v->info.frame_count > 0) {
    return static_cast<double>(v->info.frame_count) / fps;
  }
  return v->info.duration.seconds();
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
                 schema({{"composition", str(kCompDoc)},
                         {"page", integer("a still's page: only the "
                                          "layers on it")}}),
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
                                    "white by default")},
                      {"align", one_of({"left", "center", "right",
                                        "justified"},
                                       "its lines in the box; left by "
                                       "default")},
                      {"line_spacing", num("its lines this many lines "
                                           "apart, 1-3; 1 by default")},
                      {"every_page", boolean("on every page of a still "
                                             "(a header, a footer), made "
                                             "once")},
                      {"gradient", {{"type", "array"},
                                    {"items", {{"type", "string"}}},
                                    {"description",
                                     "a gradient in place of the fill (a "
                                     "text's colour): its colours, two or "
                                     "more, \"#rrggbb[aa]\"; [] takes it "
                                     "off"}}},
                      {"gradient_type", one_of({"linear", "radial"},
                                               "linear by default")},
                      {"gradient_angle", num("a linear one's direction, "
                                             "degrees: 0 left to right, "
                                             "90 top to bottom (by "
                                             "default)")},
                      {"shadow", boolean("a soft drop shadow, down and "
                                         "right; false takes it off")},
                      {"shadow_color", str("its colour; \"#00000080\" by "
                                           "default")},
                      {"glow", boolean("a glow around it; false takes it "
                                       "off")},
                      {"glow_color", str("its colour; the object's own by "
                                         "default")}},
                     box_props),
                {"text", "x", "y", "width", "height"}),
         &H::t_add_text});
    t.push_back(
        {"add_shape", "edit", "Add a shape",
         "A rectangle, an ellipse, a POLYGON (its `points`) or a line on "
         "a page's markup layer -- a panel behind words, an accent, a "
         "badge, a divider; filled flat or with a `gradient`. A line runs "
         "from "
         "(x, y) to (x + width, y + height): a level rule has height 0, "
         "its thickness `line_width`.",
         schema(with({{"composition", str(kCompDoc)},
                      {"page", integer(kPageDoc)},
                      {"start", num(kStartDoc)},
                      {"duration", num(kDurationDoc)},
                      {"shape", one_of({"rect", "ellipse", "polygon", "line"},
                                       "which shape")},
                      {"points", {{"type", "array"},
                                  {"items", {{"type", "array"},
                                             {"items", {{"type",
                                                         "number"}}}}},
                                  {"description",
                                   "a polygon's corners, [[x, y], ...] in "
                                   "canvas pixels, three or more (a star, "
                                   "a badge, an arrow); its box is made "
                                   "from them"}}},
                      {"fill", str("its inside, \"#rrggbb[aa]\"; none by "
                                   "default")},
                      {"stroke", str("its outline or line colour")},
                      {"line_width", num("the outline's width, pixels; 0 "
                                         "none")},
                      {"every_page", boolean("on every page of a still (a "
                                             "rule, a band, a frame), made "
                                             "once")},
                      {"below_all", boolean("under every layer: with "
                                            "every_page the background of "
                                            "every page, else of its "
                                            "page")},
                      {"gradient", {{"type", "array"},
                                    {"items", {{"type", "string"}}},
                                    {"description",
                                     "a gradient in place of the fill (a "
                                     "text's colour): its colours, two or "
                                     "more, \"#rrggbb[aa]\"; [] takes it "
                                     "off"}}},
                      {"gradient_type", one_of({"linear", "radial"},
                                               "linear by default")},
                      {"gradient_angle", num("a linear one's direction, "
                                             "degrees: 0 left to right, "
                                             "90 top to bottom (by "
                                             "default)")},
                      {"shadow", boolean("a soft drop shadow, down and "
                                         "right; false takes it off")},
                      {"shadow_color", str("its colour; \"#00000080\" by "
                                           "default")},
                      {"glow", boolean("a glow around it; false takes it "
                                       "off")},
                      {"glow_color", str("its colour; the object's own by "
                                         "default")}},
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
                      {"align", one_of({"left", "center", "right",
                                        "justified"}, "its lines")},
                      {"line_spacing", num("lines apart, 1-3")},
                      {"fill", str("a shape's inside")},
                      {"stroke", str("a shape's outline")},
                      {"line_width", num("outline width")},
                      {"gradient", {{"type", "array"},
                                    {"items", {{"type", "string"}}},
                                    {"description",
                                     "a gradient in place of the fill (a "
                                     "text's colour): its colours, two or "
                                     "more, \"#rrggbb[aa]\"; [] takes it "
                                     "off"}}},
                      {"gradient_type", one_of({"linear", "radial"},
                                               "linear by default")},
                      {"gradient_angle", num("a linear one's direction, "
                                             "degrees: 0 left to right, "
                                             "90 top to bottom (by "
                                             "default)")},
                      {"shadow", boolean("a soft drop shadow, down and "
                                         "right; false takes it off")},
                      {"shadow_color", str("its colour; \"#00000080\" by "
                                           "default")},
                      {"glow", boolean("a glow around it; false takes it "
                                       "off")},
                      {"glow_color", str("its colour; the object's own by "
                                         "default")}},
                     box_props),
                {"layer", "object"}),
         &H::t_update_object});
    t.push_back(
        {"flow_text", "edit", "Lay out a long text",
         "A LONG TEXT laid out on a still's pages -- the WHOLE text in ONE "
         "call, never a section at a time -- page after page, in an "
         "area of each (in columns): paragraphs set as text boxes, "
         "headings (\"# \", \"## \" lines, or found), lists, "
         "quotations -- each word placed once, a paragraph cut only at a "
         "line, pages added as it needs them. The words come from "
         "`from_words` and `to_words` -- the first and the last few words "
         "of the passage in the person's message, which is copied as it "
         "is (never retype a long text) -- or a text asset, or `text`. "
         "Tells each page's first words, the headings' pages and the room "
         "left on the last page.",
         schema({{"composition", str(kCompDoc)},
                 {"from_words", str("the passage's first words in the "
                                    "person's message, as written (5-10 "
                                    "words)")},
                 {"to_words", str("the WHOLE text's last words, as "
                                  "written -- not a section's; omitted "
                                  "(best): to the message's end")},
                 {"asset", str("else a text asset's id")},
                 {"text", str("else the words themselves (short texts "
                              "only)")},
                 {"first_page", integer("the page it starts on, from 1 "
                                        "(pages+1: a new one); 1 by "
                                        "default -- below what is there. "
                                        "It goes on onto the pages after "
                                        "it that hold nothing of their "
                                        "own, else onto new ones")},
                 {"start_y", num("on its first page, the text starts this "
                                 "far down (below a title); else at the "
                                 "area's top")},
                 {"x", num("the text area on each page: its left edge; "
                           "by default the page less 8% margins")},
                 {"y", num("its top edge")},
                 {"width", num("its width")},
                 {"height", num("its height (leave room for a footer)")},
                 {"columns", integer("1-4; by default 2 on a landscape "
                                     "page, else 1")},
                 {"gutter", num("between columns, pixels")},
                 {"font", str("the body's font; \"Georgia\" by default")},
                 {"size", num("the body's size, pixels; by default about "
                              "65 characters a line")},
                 {"color", str("the body's colour; \"#222222\" by "
                               "default -- light on a dark page")},
                 {"line_spacing", num("lines apart, 1-3; 1.45 by "
                                      "default")},
                 {"align", one_of({"left", "justified", "center",
                                   "right"}, "left by default")},
                 {"heading_font", str("headings' font; \"Avenir Next\" "
                                      "bold by default")},
                 {"heading_size", num("a heading's size; a title (\"# "
                                      "\") half again as big")},
                 {"heading_color", str("headings' colour; the body's by "
                                       "default")},
                 {"accent", str("a quotation's bar; the headings' colour "
                                "by default")},
                 {"keep_line_breaks", boolean("each line kept as written "
                                              "(a poem, lyrics)")},
                 {"page_per_heading", boolean("each heading starts a new "
                                              "page (a chapter a page)")},
                 {"find_headings", boolean("true (by default): short "
                                           "lines standing alone are "
                                           "headings when no line is "
                                           "marked \"# \"; false: only "
                                           "\"# \" lines")}}),
         &H::t_flow_text});
    t.push_back(
        {"add_page_progress", "edit", "Add page progress",
         "A PAGE PROGRESS BAR on each page from `from_page` to `to_page`, "
         "filled as far as the page is through them -- a bar, segments or "
         "dots -- and, with `numbers`, \"3 / 12\" above its right end. "
         "Its layers are in a folder; made again, it replaces the one "
         "before: make it last, once the pages are all there.",
         schema({{"composition", str(kCompDoc)},
                 {"from_page", integer("the first page it counts (2 to "
                                       "leave a cover out); 1 by "
                                       "default")},
                 {"to_page", integer("the last; the last page by "
                                     "default")},
                 {"style", one_of({"bar", "segments", "dots"},
                                  "bar by default")},
                 {"x", num("the bar's left edge; by default the 8% "
                           "margin")},
                 {"y", num("its top edge; by default near the page's "
                           "foot")},
                 {"width", num("its width")},
                 {"height", num("its thickness, pixels (a dot's size)")},
                 {"track", str("the part not reached, \"#rrggbb[aa]\"")},
                 {"fill", str("the part reached")},
                 {"numbers", boolean("\"3 / 12\" above its right end")},
                 {"number_color", str("the numbers' colour; the fill's by "
                                      "default")},
                 {"font", str("the numbers' font")},
                 {"size", num("the numbers' size, pixels")}}),
         &H::t_add_page_progress});
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
    // ---- scenes: shots, each a clip with its sound
    const Json picture_ids = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description", "pictures' ids it draws on -- a character, a "
                        "place, kept the same across cuts -- named "
                        "<Picture 1>, <Picture 2> in this order"}};
    t.push_back(
        {"generate_video", "generate", "Generate a shot",
         "A SHOT of a scene: a clip WITH its own sound -- voices, sounds, "
         "music -- made by the video model from its prompt's parts, in "
         "the background (minutes). It LANDS on the timeline from `start` "
         "(omitted: right after the shots and clips before it), filling "
         "the frame. `continues: true` carries it on from the END of the "
         "shot before it -- its last seconds, picture and sound -- so a "
         "long scene is shot after shot without a jump; it may be asked "
         "for while that one is still being made. Without it, a new shot "
         "(a cut): from the words alone, opening on a picture (`first`), "
         "or drawing on pictures (`references`) to keep a character or a "
         "place the same.",
         schema({{"description",
                  str("what is seen and heard: the style and framing in a "
                      "sentence, then \"[Shot 1] ...\" -- who is where, "
                      "what they do, the camera; \"[Shot 2] At "
                      "00:03.000, ...\" for a cut inside it. A speaker "
                      "named once with an ID: \"the old woman (S1) says: "
                      "<d>[English] the words</d>\". References by the "
                      "model's names: <Picture 1>, the shot continued "
                      "<Video 1>, a subject <Subject 1>")},
                 {"soundscape", str("the ambience and the sounds of what "
                                    "happens, 1-4 sentences; no dialogue, "
                                    "no music")},
                 {"music", str("the score, 1-3 sentences; \"N/A\" (by "
                               "default) for none")},
                 {"subjects", str("with `continues` or `references`: one "
                                  "line per subject -- \"<Subject 1> is "
                                  "the old clockmaker, silver bun, green "
                                  "cardigan, in <Video 1>.\" -- in the "
                                  "SAME words in every shot")},
                 {"summary", str("with `continues` or `references`: what "
                                 "happens in this shot, one or two "
                                 "sentences naming the subjects")},
                 {"seconds", num("its length, 2-10; 5 by default -- made "
                                 "as the model makes lengths (5 is 5.17)")},
                 {"continues", boolean("carry on from the end of the shot "
                                       "before it on this timeline: the "
                                       "next moment of the same scene. "
                                       "Without it the people, place and "
                                       "light come out different")},
                 {"continue_from", str("a clip's id to carry on from "
                                       "instead (its last 3.75 s)")},
                 {"first", str("a picture's id the shot opens on, its "
                               "first frame (not with continues or "
                               "references)")},
                 {"references", picture_ids},
                 {"start", num("where it lands, timeline seconds; omitted "
                               "(best): after the shots before it. A shot "
                               "that continues lands after the one before")},
                 {"seed", integer("another number: another take of the "
                                  "same shot")},
                 {"composition", str(kCompDoc)},
                 {"name", str("the asset's name; omitted: \"Shot N\"")}},
                {"description"}),
         &H::t_generate_video});
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
                             const char* key, bool named)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  // Something made on a still of pages says which: the 9B left `page`
  // out and wrote a contents list on the cover.
  if (named && a.cls == AssetClass::Still && a.pages > 1 &&
      !(args.is_object() && args.contains(key))) {
    return make_error(Code::InvalidArgument, std::format(
        "which page? \"{}\" has {} pages: give `{}` (from 1)", a.name,
        a.pages, key));
  }
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
Controller::AgentHost::box_(const Json& args, media::PixelSize frame,
                            bool line)
{
  std::array<double, 4> b = {
      number(args, "x", 0), number(args, "y", 0),
      number(args, "width", frame.width),
      number(args, "height", frame.height)};
  // A line's may be level (no height) or upright (no width).
  if (line && b[2] >= 0 && b[3] >= 0 && b[2] + b[3] > 0) {
    return b;
  }
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
  // The page's own topmost layer -- what runs across pages passed over:
  // drawn on when it is a markup; else a new markup layer above it, the
  // page's alone.
  for (auto it = a.layers.rbegin(); it != a.layers.rend(); ++it) {
    if (!on_page(it->time, page) || !alone_on(it->time, page)) {
      continue;
    }
    if (it->source) {
      auto src = p->asset(*it->source);
      if (src.ok() && src->cls == AssetClass::Markup) {
        return it->id;
      }
    }
    break;
  }
  const std::string top = a.layers.empty() ? "" : a.layers.back().id;
  VALTZ_ASSIGN(std::string blank, _c.add_layer(pid, comp, top, page));
  VALTZ_TRY(page_alone_(pid, comp, blank, page));
  return _c.markup_layer(pid, comp, {blank}, page);
}

Status
Controller::AgentHost::page_alone_(ProjectId pid, AssetId comp,
                                   const std::string& layer,
                                   std::int64_t page)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  auto it = std::ranges::find(a.layers, layer, &project::Layer::id);
  if (a.cls != AssetClass::Still || it == a.layers.end() ||
      alone_on(it->time, page)) {
    return ok_status();
  }
  return _c.set_layer_pages(pid, comp, layer, page, 1);
}

Result<std::string>
Controller::AgentHost::front_markup_(ProjectId pid, AssetId comp)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  if (a.cls != AssetClass::Still) {
    return make_error(Code::InvalidArgument,
                      "every_page is for a still's pages");
  }
  for (auto it = a.layers.rbegin(); it != a.layers.rend(); ++it) {
    if (it->name == kFrontName && runs_across(it->time) && it->source) {
      return it->id;
    }
  }
  const std::string top = a.layers.empty() ? "" : a.layers.back().id;
  VALTZ_ASSIGN(std::string blank, _c.add_layer(pid, comp, top));
  VALTZ_TRY(_c.set_layer_pages(pid, comp, blank, 0, 0));
  VALTZ_ASSIGN(std::string id, _c.markup_layer(pid, comp, {blank}, 0));
  VALTZ_TRY(_c.rename_layer(pid, comp, id, kFrontName));
  return id;
}

Result<std::string>
Controller::AgentHost::back_markup_(ProjectId pid, AssetId comp,
                                    std::optional<std::int64_t> page)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  if (a.cls != AssetClass::Still) {
    return make_error(Code::InvalidArgument,
                      "below_all is for a still's pages");
  }
  // The back markup across every page: the bottom layer, so named.
  std::optional<std::string> back;
  if (!a.layers.empty() && a.layers.front().name == kBackName &&
      runs_across(a.layers.front().time) && a.layers.front().source) {
    back = a.layers.front().id;
  }
  if (!page) {
    if (back) {
      return *back;
    }
    const std::string top = a.layers.empty() ? "" : a.layers.back().id;
    VALTZ_ASSIGN(std::string blank, _c.add_layer(pid, comp, top));
    VALTZ_TRY(_c.set_layer_pages(pid, comp, blank, 0, 0));
    VALTZ_ASSIGN(std::string id, _c.markup_layer(pid, comp, {blank}, 0));
    VALTZ_TRY(_c.rename_layer(pid, comp, id, kBackName));
    VALTZ_TRY(_c.place_layers(pid, comp, {id}, std::nullopt, ""));
    return id;
  }
  // A page's own, so named, right above the back one: made when there
  // is none (a page's words are on another markup, above its pictures).
  for (const auto& l : a.layers) {
    if (l.name == kPageBackName && alone_on(l.time, *page) && l.source) {
      return l.id;
    }
  }
  const std::string top = a.layers.empty() ? "" : a.layers.back().id;
  VALTZ_ASSIGN(std::string blank, _c.add_layer(pid, comp, top, *page));
  VALTZ_TRY(page_alone_(pid, comp, blank, *page));
  VALTZ_ASSIGN(std::string id, _c.markup_layer(pid, comp, {blank}, *page));
  VALTZ_TRY(_c.rename_layer(pid, comp, id, kPageBackName));
  VALTZ_TRY(to_back_(pid, comp, id));
  return id;
}

Json
Controller::AgentHost::crowding_(ProjectId pid, AssetId comp,
                                 std::int64_t page,
                                 const std::array<double, 4>& box,
                                 const std::string& skip)
{
  auto* p = _c.project(pid);
  if (!p) {
    return Json();
  }
  auto a = p->asset(comp);
  if (!a.ok() || a->cls != AssetClass::Still) {
    return Json();
  }
  Json over = Json::array();
  for (const auto& l : a->layers) {
    if (!alone_on(l.time, page) || !l.source || !l.visible) {
      continue;
    }
    auto src = p->asset(*l.source);
    if (!src.ok() || src->cls != AssetClass::Markup || !src->markup) {
      continue;
    }
    for (const auto& o : src->markup->objects) {
      if (jget<std::string>(o, "kind", "") != "text" ||
          jget<std::string>(o, "id", "") == skip) {
        continue;
      }
      // Its words' reach: the box's width, as far down as they go.
      const double x0 = std::min(jget(o, "x0", 0.0), jget(o, "x1", 0.0));
      const double y0 = std::min(jget(o, "y0", 0.0), jget(o, "y1", 0.0));
      const double w = std::abs(jget(o, "x1", 0.0) - jget(o, "x0", 0.0));
      const double h =
          std::min(std::abs(jget(o, "y1", 0.0) - jget(o, "y0", 0.0)),
                   media::text_fit(o).needed_height);
      const double ix = std::min(x0 + w, box[0] + box[2]) -
                        std::max(x0, box[0]);
      const double iy = std::min(y0 + h, box[1] + box[3]) -
                        std::max(y0, box[1]);
      if (ix > 4 && iy > 4 && over.size() < 3) {
        over.push_back(
            {{"layer", model_layer(l.id)},
             {"object", jget<std::string>(o, "id", "")},
             {"text", std::string(utf8_prefix(
                          jget<std::string>(o, "text", ""), 40))}});
      }
    }
  }
  if (over.empty()) {
    return Json();
  }
  return {{"overlaps", std::move(over)},
          {"overlap_note", "It lies over words already on this page: move "
                           "one of them unless that is meant."}};
}

Status
Controller::AgentHost::to_back_(ProjectId pid, AssetId comp,
                                const std::string& layer)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  std::optional<std::string> above;
  if (!a.layers.empty() && a.layers.front().id != layer &&
      a.layers.front().name == kBackName &&
      runs_across(a.layers.front().time)) {
    above = a.layers.front().id;
  }
  return _c.place_layers(pid, comp, {layer}, above, "");
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
    VALTZ_ASSIGN(std::int64_t page, page_(pid, comp, args, "page", true));
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
  // Shots (generate_video): a clip from words or a picture, and one
  // carried on from another.
  bool video = false, carry = false;
  for (const auto& pick : _c.auto_models()) {
    if (pick.modality != "video" || pick.chosen.empty()) {
      continue;
    }
    const auto* m = _c._catalog.find(pick.chosen);
    video = video || (pick.op == "generate" && m &&
                      _c.runs_soon(*m, "video", "generate"));
    carry = carry || (pick.op == "edit" && m &&
                      _c.runs_soon(*m, "video", "edit"));
  }
  std::lock_guard lk(s.mu);
  return Json{{"name", p->name()},
              {"active", s.target ? s.target->str() : std::string()},
              {"compositions", std::move(comps)},
              {"assets", std::move(assets)},
              {"picture_models", std::move(models)},
              {"auto_picture_model", auto_pick},
              {"can_make_music", music},
              {"can_make_voiceover", speech},
              {"can_describe_clips", watch},
              {"can_make_shots", video},
              {"can_continue_shots", carry}};
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
  // A still's page alone, when asked: a long text's pages are many.
  std::optional<std::int64_t> only;
  if (!timeline && args.contains("page")) {
    VALTZ_ASSIGN(only, page_(s.project, comp, args));
  }
  std::string progress;
  for (const auto& f : a.layer_folders) {
    if (f.name == kProgressName) {
      progress = f.id;
    }
  }
  Json layers = Json::array();
  for (const auto& l : a.layers) {
    if (only && !on_page(l.time, *only)) {
      continue;
    }
    Json j = {{"id", model_layer(l.id)}, {"name", l.name},
              {"visible", l.visible}};
    if (!progress.empty() && l.folder == progress) {
      // The page progress bar's: one line each.
      j["pages"] = Json{l.time.offset + 1, l.time.offset + 1};
      j["shows"] = "page progress";
      layers.push_back(std::move(j));
      continue;
    }
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
            // A long text's head only: a flowed page would tell the whole
            // text back.
            const auto words = jget<std::string>(o, "text", "");
            x["text"] = words.size() <= 160
                            ? words
                            : std::string(utf8_prefix(words, 120)) +
                                  std::string(kEllipsis);
            if (auto al = jget<std::string>(o, "align", "left");
                al != "left") {
              x["align"] = al;
            }
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
          }
          if (const Json g = jget(o, "gradient", Json()); g.is_object()) {
            Json cs = Json::array();
            for (const auto& st : jget(g, "stops", Json::array())) {
              cs.push_back(hex(jget(st, "color", Json())));
            }
            x["gradient"] = {{"type", jget<std::string>(g, "kind", "linear")},
                             {"colors", std::move(cs)}};
          }
          if (x["kind"] == "polygon") {
            x["corners"] = jget(o, "points", Json::array()).size();
          }
          // Its effects: each kind, and its colour.
          for (const auto& e : jget(o, "effects", Json::array())) {
            const auto k = jget<std::string>(e, "kind", "");
            if (k == "shadow" || k == "glow") {
              x[k] = hex(jget(e, "color", Json()));
            }
          }
          if (x["kind"] != "text") {
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
  const std::string align = text_arg(args, "align", "left");
  if (align != "left" && align != "center" && align != "right" &&
      align != "justified") {
    return make_error(Code::InvalidArgument,
                      "align is left, center, right or justified");
  }
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  std::pair<std::string, Json> on;
  if (flag(args, "every_page")) {
    VALTZ_ASSIGN(on.first, front_markup_(s.project, comp));
    on.second = {{"every_page", true}};
  } else {
    VALTZ_ASSIGN(on, markup_for_(s.project, comp, args));
  }
  const std::string layer = on.first;
  VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
  const std::string id = object_id();
  const double fsize = std::clamp(number(args, "size", 48), 4.0, 2000.0);
  VALTZ_ASSIGN(Json fx, effects_arg(args, Json::array(), fsize, *c));
  VALTZ_ASSIGN(Json grad, gradient_arg(args, Json()));
  objs.push_back(
      {{"id", id},
       {"kind", "text"},
       {"gradient", std::move(grad)},
       {"effects", std::move(fx)},
       {"align", align},
       {"line_spacing", std::clamp(number(args, "line_spacing", 1), 1.0,
                                   3.0)},
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
  if (on.second.contains("page")) {
    const auto pg = jget<std::int64_t>(on.second, "page", 1) - 1;
    if (Json c = crowding_(s.project, comp, pg, box, id); c.is_object()) {
      out.update(c);
    }
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_add_shape(Session& s, Step& step, const Json& args,
                                   std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
  const std::string shape = text_arg(args, "shape", "rect");
  if (shape != "rect" && shape != "ellipse" && shape != "line" &&
      shape != "polygon") {
    return make_error(Code::InvalidArgument,
                      "a shape is rect, ellipse, polygon or line");
  }
  // A POLYGON's corners (canvas pixels): its box their bounds, its points
  // in the box's units (media/markup.h).
  Json corners = Json::array();
  std::array<double, 4> pbox{};
  if (shape == "polygon") {
    std::vector<std::array<double, 2>> pts;
    for (const auto& p : jget(args, "points", Json::array())) {
      if (p.is_array() && p.size() == 2 && p[0].is_number() &&
          p[1].is_number()) {
        pts.push_back({p[0].get<double>(), p[1].get<double>()});
      }
    }
    if (pts.size() < 3) {
      return make_error(Code::InvalidArgument,
                        "a polygon is three corners or more: points "
                        "[[x, y], ...] in canvas pixels");
    }
    double x0 = pts[0][0], y0 = pts[0][1], x1 = x0, y1 = y0;
    for (const auto& p : pts) {
      x0 = std::min(x0, p[0]);
      x1 = std::max(x1, p[0]);
      y0 = std::min(y0, p[1]);
      y1 = std::max(y1, p[1]);
    }
    for (const auto& p : pts) {
      corners.push_back({x1 > x0 ? (p[0] - x0) / (x1 - x0) : 0.5,
                         y1 > y0 ? (p[1] - y0) / (y1 - y0) : 0.5});
    }
    pbox = {x0, y0, std::max(1.0, x1 - x0), std::max(1.0, y1 - y0)};
  }
  std::array<double, 4> box = pbox;
  if (shape != "polygon") {
    VALTZ_ASSIGN(box, box_(args, frame, shape == "line"));
  }
  auto fill = colour(text_arg(args, "fill", "#00000000"));
  auto stroke = colour(text_arg(args, "stroke", "#ffffffff"));
  if (!fill || !stroke) {
    return make_error(Code::InvalidArgument,
                      "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
  }
  double lw = std::clamp(number(args, "line_width",
                                shape == "line" ? 4.0 : 0.0), 0.0, 500.0);
  // A line far longer than it is "high" is a RULE given its thickness as
  // its height (the 9B's "height": 4 -- or 54, a band at a page's foot):
  // laid level through the box's middle, that thick. And so upright.
  if (shape == "line") {
    const double w = std::abs(box[2]), h = std::abs(box[3]);
    const double thin = std::max(8.0, lw * 2);
    if (h > 0 && ((h <= thin && w > h * 10) || w >= h * 20)) {
      if (!args.contains("line_width")) {
        lw = std::max(1.0, h);
      }
      box[1] += h / 2;
      box[3] = 0;
    } else if (w > 0 && ((w <= thin && h > w * 10) || h >= w * 20)) {
      if (!args.contains("line_width")) {
        lw = std::max(1.0, w);
      }
      box[0] += w / 2;
      box[2] = 0;
    }
  }
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  std::pair<std::string, Json> on;
  const bool every = flag(args, "every_page");
  // An OPAQUE shape covering a still's page is its BACKGROUND, asked so
  // or not: on top it would hide the page's words (the 9B left below_all
  // out of its every-page paper). A translucent one is a scrim, over.
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const bool covers = a.cls == AssetClass::Still && shape != "line" &&
                      (*fill)[3] >= 0.95 &&
                      box[2] * box[3] >= 0.9 * frame.width * frame.height;
  const bool below = flag(args, "below_all") || covers;
  if (every || below) {
    // Across every page (on top, or under all: the background), or under
    // a page's own layers.
    std::optional<std::int64_t> page;
    if (!every) {
      VALTZ_ASSIGN(page, page_(s.project, comp, args, "page", true));
    }
    VALTZ_ASSIGN(on.first, every && !below
                               ? front_markup_(s.project, comp)
                               : back_markup_(s.project, comp, page));
    on.second = every ? Json{{"every_page", true}}
                      : Json{{"page", *page + 1}};
    if (covers && !flag(args, "below_all")) {
      on.second["note"] = "It covers the page: laid under everything, "
                          "as its background.";
    }
  } else {
    VALTZ_ASSIGN(on, markup_for_(s.project, comp, args));
  }
  const std::string layer = on.first;
  VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
  const std::string id = object_id();
  Json made = {{"id", id},
               {"kind", shape},
               {"x0", box[0]},
               {"y0", box[1]},
               {"x1", box[0] + box[2]},
               {"y1", box[1] + box[3]},
               {"stroke", {(*stroke)[0], (*stroke)[1], (*stroke)[2],
                           (*stroke)[3]}},
               {"fill", {(*fill)[0], (*fill)[1], (*fill)[2], (*fill)[3]}},
               {"width", lw}};
  if (shape == "polygon") {
    made["points"] = std::move(corners);
  }
  if (shape != "line") {
    VALTZ_ASSIGN(Json g, gradient_arg(args, Json()));
    if (g.is_object()) {
      made["gradient"] = std::move(g);
    }
  }
  {
    // Its effects: a glow in its fill, or its line's colour.
    const auto& own = shape != "line" && (*fill)[3] > 0 ? *fill : *stroke;
    VALTZ_ASSIGN(Json fx, effects_arg(args, Json::array(),
                                      shape == "line"
                                          ? std::max(16.0, lw * 3)
                                          : shape_base(box[2], box[3]),
                                      own));
    made["effects"] = std::move(fx);
  }
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
  if (args.contains("align")) {
    const std::string al = text_arg(args, "align");
    if (al != "left" && al != "center" && al != "right" &&
        al != "justified") {
      return make_error(Code::InvalidArgument,
                        "align is left, center, right or justified");
    }
    o["align"] = al;
  }
  if (args.contains("line_spacing")) {
    o["line_spacing"] =
        std::clamp(number(args, "line_spacing", 1), 1.0, 3.0);
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
  {
    const bool text = jget<std::string>(o, "kind", "") == "text";
    const Json own = text || jget(o, "fill", Json::array()).size() != 4 ||
                             jget(o, "fill", Json::array())[3] == 0
                         ? jget(o, "stroke", Json())
                         : jget(o, "fill", Json());
    std::array<double, 4> mine{0, 0, 0, 1};
    for (std::size_t i = 0; own.is_array() && i < 4 && i < own.size(); ++i) {
      mine[i] = own[i].is_number() ? own[i].get<double>() : mine[i];
    }
    const double base =
        text ? jget(jget(o, "font", Json::object()), "size", 48.0)
             : shape_base(jget(o, "x1", 0.0) - jget(o, "x0", 0.0),
                          jget(o, "y1", 0.0) - jget(o, "y0", 0.0));
    VALTZ_ASSIGN(Json fx, effects_arg(args, jget(o, "effects", Json()),
                                      base, mine));
    if (fx.empty()) {
      o.erase("effects");
    } else {
      o["effects"] = std::move(fx);
    }
    if (jget<std::string>(o, "kind", "") != "line") {
      VALTZ_ASSIGN(Json g, gradient_arg(args, jget(o, "gradient", Json())));
      if (g.is_object()) {
        o["gradient"] = std::move(g);
      } else {
        o.erase("gradient");
      }
    }
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

// ---- a long text on pages ---------------------------------------------------

Result<std::string>
Controller::AgentHost::flow_source_(const Session& s, const Json& args,
                                    std::string* after, std::string* before)
{
  constexpr std::size_t kMaxText = 256 * 1024;
  // The person's messages, the latest first.
  std::vector<std::string> said;
  {
    std::lock_guard lk(s.mu);
    for (auto it = s.messages.rbegin(); it != s.messages.rend(); ++it) {
      if (it->role == "user" && it->call_id.empty()) {
        said.push_back(it->content);
      }
    }
  }
  // Where the passage lies in message `m`, `a` to `b`: what the message
  // has past it, and before it but its first paragraph (the request
  // itself), when more than a few words.
  auto around = [&](const std::string& m, std::size_t a, std::size_t b) {
    auto trim = [](std::string_view v) {
      while (!v.empty() && std::isspace(static_cast<unsigned char>(
                               v.front()))) {
        v.remove_prefix(1);
      }
      while (!v.empty() && std::isspace(static_cast<unsigned char>(
                               v.back()))) {
        v.remove_suffix(1);
      }
      return v;
    };
    if (after) {
      if (auto more = trim(std::string_view(m).substr(b));
          more.size() > 200) {
        *after = std::string(more);
      }
    }
    if (before) {
      std::string_view head = std::string_view(m).substr(0, a);
      const auto para = head.find("\n\n");
      head = para == std::string_view::npos ? std::string_view()
                                            : head.substr(para + 2);
      if (head = trim(head); head.size() > 200) {
        *before = std::string(head);
      }
    }
  };
  // The 9B fills every field it is given, "" for those it means not to:
  // an empty one is not given. The person's words by their first words
  // first -- exact, and nothing copied through the model's output.
  const std::string from = text_arg(args, "from_words");
  const std::string to = text_arg(args, "to_words");
  if (!from.empty()) {
    for (const auto& m : said) {
      auto a = find_loose(m, from, 0);
      if (!a) {
        continue;
      }
      std::size_t end = m.size();
      if (!to.empty()) {
        auto b = find_loose(m, to, a->first);
        if (!b) {
          return make_error(Code::NotFound, std::format(
              "\"{}\" is not after \"{}\" in the person's message: give "
              "to_words as written there, or none (to its end)", to, from));
        }
        end = b->second;
      }
      around(m, a->first, end);
      return std::string(utf8_prefix(
          std::string_view(m).substr(a->first, end - a->first), kMaxText));
    }
    return make_error(Code::NotFound, std::format(
        "\"{}\" is not in the person's messages: give from_words as "
        "written there (its first words)", from));
  }
  if (const std::string id = text_arg(args, "asset"); !id.empty()) {
    auto* p = _c.project(s.project);
    const auto aid = AssetId::parse(id);
    if (!p || !aid) {
      return make_error(Code::InvalidArgument, "which text asset?");
    }
    VALTZ_ASSIGN(project::Asset a, p->asset(*aid));
    if (a.kind != AssetKind::Text) {
      return make_error(Code::InvalidArgument,
                        std::format("\"{}\" is not a text", a.name));
    }
    VALTZ_ASSIGN(std::string t, p->read_text(*aid));
    return std::string(utf8_prefix(t, kMaxText));
  }
  std::string t = text_arg(args, "text");
  if (t.empty()) {
    return make_error(Code::InvalidArgument,
                      "give the words: from_words (and to_words) from the "
                      "person's message, an asset, or text");
  }
  if (t.size() > kMaxText) {
    return make_error(Code::InvalidArgument, "the text is too long");
  }
  // The person's words RETYPED (the 9B copied a whole article through
  // its output, a section a call): the passage as they wrote it, from
  // its first words to its last -- the same words, exactly.
  if (t.size() > 300) {
    std::vector<std::string> ws;
    std::string w;
    for (char ch : t + " ") {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        if (!w.empty()) {
          ws.push_back(std::move(w));
          w.clear();
        }
      } else {
        w += ch;
      }
    }
    const std::size_t n = std::min<std::size_t>(8, ws.size());
    std::string head, tail;
    for (std::size_t i = 0; i < n; ++i) {
      head += (i ? " " : "") + ws[i];
      tail += (i ? " " : "") + ws[ws.size() - n + i];
    }
    for (const auto& m : said) {
      auto a = find_loose(m, head, 0);
      auto b = a ? find_loose(m, tail, a->first) : std::nullopt;
      if (a && b) {
        around(m, a->first, b->second);
        return std::string(utf8_prefix(
            std::string_view(m).substr(a->first, b->second - a->first),
            kMaxText));
      }
    }
  }
  return t;
}

Result<Json>
Controller::AgentHost::t_flow_text(Session& s, Step& step, const Json& args,
                                   std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  if (a.cls != AssetClass::Still) {
    return make_error(Code::InvalidArgument,
                      "a text is laid out on a still's pages: make one "
                      "with create_composition");
  }
  VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
  std::string after, before;
  VALTZ_ASSIGN(std::string text, flow_source_(s, args, &after, &before));
  const auto first = static_cast<std::int64_t>(number(args, "first_page", 1));
  if (first < 1 || first > a.pages + 1) {
    return make_error(Code::InvalidArgument, std::format(
        "first_page is one of its {} pages, or {} (a new one at the end)",
        a.pages, a.pages + 1));
  }
  const double W = frame.width, H = frame.height;
  // The area: the page within its margins, room for a footer.
  const double mx = std::round(W * 0.08);
  const double top = std::round(H * 0.09);
  std::array<double, 4> area = {
      number(args, "x", mx), number(args, "y", top),
      number(args, "width", W - 2 * mx),
      number(args, "height", H - top - std::round(H * 0.11))};
  // What every page shows on top -- a header, a rule, a band -- kept
  // clear of: the area's top moved below what lies in its upper half,
  // its bottom above what lies in its lower half (the 9B drew a band
  // through the text's first line on every page).
  for (const auto& l : a.layers) {
    if (l.name != kFrontName || !runs_across(l.time) || !l.source) {
      continue;
    }
    VALTZ_ASSIGN(Json objs, objects_(s.project, comp, l.id));
    for (const auto& o : objs) {
      const double x0 = std::min(jget(o, "x0", 0.0), jget(o, "x1", 0.0));
      const double x1 = std::max(jget(o, "x0", 0.0), jget(o, "x1", 0.0));
      const double half = jget(o, "width", 0.0) / 2;
      const double y0 =
          std::min(jget(o, "y0", 0.0), jget(o, "y1", 0.0)) - half;
      const double y1 =
          std::max(jget(o, "y0", 0.0), jget(o, "y1", 0.0)) + half;
      const double top = area[1], bottom = area[1] + area[3];
      if (x1 <= area[0] || x0 >= area[0] + area[2] || y1 <= top ||
          y0 >= bottom) {
        continue;
      }
      const double gap = std::round(H * 0.015);
      if ((y0 + y1) / 2 < (top + bottom) / 2) {
        area[1] = y1 + gap;
        area[3] = bottom - area[1];
      } else {
        area[3] = y0 - gap - top;
      }
    }
  }
  if (area[2] < 40 || area[3] < 40) {
    return make_error(Code::InvalidArgument, "the area is too small");
  }
  media::FlowSpec spec;
  spec.area = area;
  spec.columns = std::clamp(
      static_cast<int>(number(args, "columns", W > H * 1.15 ? 2 : 1)), 1,
      4);
  spec.gutter = std::max(0.0, number(args, "gutter", std::round(W * 0.04)));
  spec.first_top = number(args, "start_y", 0);
  spec.page_per_heading = flag(args, "page_per_heading");
  const double colw =
      (area[2] - spec.gutter * (spec.columns - 1)) / spec.columns;
  // The body: about 65 characters a line, held to what reads on the page.
  const double size = std::clamp(
      number(args, "size",
             std::round(std::clamp(colw / 32, H * 0.016, H * 0.042))),
      6.0, 400.0);
  auto c = [&](const char* key, const char* fallback)
      -> Result<std::array<double, 4>> {
    auto v = colour(text_arg(args, key, fallback));
    if (!v) {
      return make_error(Code::InvalidArgument,
                        "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
    }
    return *v;
  };
  const std::string align = text_arg(args, "align", "left");
  if (align != "left" && align != "center" && align != "right" &&
      align != "justified") {
    return make_error(Code::InvalidArgument,
                      "align is left, center, right or justified");
  }
  spec.body.font = text_arg(args, "font", "Georgia");
  spec.body.size = size;
  VALTZ_ASSIGN(spec.body.color, c("color", "#222222"));
  spec.body.line_spacing =
      std::clamp(number(args, "line_spacing", 1.45), 1.0, 3.0);
  spec.body.align = align;
  const double hsize = std::clamp(
      number(args, "heading_size", std::round(size * 1.45)), 6.0, 600.0);
  spec.heading.font = text_arg(args, "heading_font", "Avenir Next");
  spec.heading.size = hsize;
  spec.heading.bold = true;
  VALTZ_ASSIGN(spec.heading.color,
               c("heading_color", text_arg(args, "color", "#222222")
                                      .c_str()));
  spec.heading.line_spacing = 1.1;
  spec.title = spec.heading;
  spec.title.size = std::round(hsize * 1.45);
  VALTZ_ASSIGN(spec.accent, c("accent", text_arg(args, "heading_color",
                                                 text_arg(args, "color",
                                                          "#222222"))
                                            .c_str()));
  bool found = false;
  spec.blocks = media::read_flow_blocks(
      text, flag(args, "keep_line_breaks"),
      flag(args, "find_headings", true), &found);
  if (spec.blocks.empty()) {
    return make_error(Code::InvalidArgument, "no words to lay out");
  }
  spec.max_pages = static_cast<int>(
      std::min<std::int64_t>(200, Controller::kMaxPages - a.pages));
  // The first page's own words, shapes and pictures in the area: the
  // text starts below them (a text flowed there before, a title) -- or,
  // no room left there, on the page after.
  bool skip_first = false;
  if (first - 1 < a.pages) {
    const std::int64_t pg = first - 1;
    double low = 0;
    auto take = [&](double x0, double y0, double x1, double y1) {
      if (std::max(x0, x1) > area[0] && std::min(x0, x1) < area[0] + area[2] &&
          std::max(y0, y1) > area[1] && std::min(y0, y1) < area[1] + area[3]) {
        low = std::max(low, std::max(y0, y1));
      }
    };
    for (const auto& l : a.layers) {
      if (!alone_on(l.time, pg) || l.name == kPageBackName || !l.source ||
          !l.visible) {
        continue;
      }
      auto src = p->asset(*l.source);
      if (src.ok() && src->cls == AssetClass::Markup) {
        VALTZ_ASSIGN(Json objs, objects_(s.project, comp, l.id));
        for (const auto& o : objs) {
          take(jget(o, "x0", 0.0), jget(o, "y0", 0.0), jget(o, "x1", 0.0),
               jget(o, "y1", 0.0));
        }
      } else if (Json b = layer_box_(s.project, a, l); b.is_object()) {
        const double x = jget(b, "x", 0.0), y = jget(b, "y", 0.0);
        take(x, y, x + jget(b, "width", 0.0), y + jget(b, "height", 0.0));
      }
    }
    if (low > 0) {
      spec.first_top = std::max(spec.first_top, low + size * 1.2);
    }
    skip_first = spec.first_top > area[1] + area[3] - size * 4;
    if (skip_first) {
      spec.first_top = 0;
    }
  }
  const media::FlowResult r = media::flow_text(spec);
  if (r.pages.empty()) {
    return make_error(Code::InvalidArgument,
                      "the words do not fit the area");
  }

  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  // The text's pages: from first_page, then each next page while it
  // holds nothing of its own (only what runs on to the last page), else
  // a new page right after -- so a closing page made before stays last.
  auto free_page = [&](std::int64_t page) -> Result<bool> {
    VALTZ_ASSIGN(project::Asset now, p->asset(comp));
    if (page >= now.pages) {
      return false;
    }
    return std::ranges::none_of(now.layers, [&](const project::Layer& l) {
      return on_page(l.time, page) && l.time.duration > 0;
    });
  };
  std::int64_t at = first - 1;
  int added = 0;
  if (at == a.pages) {
    VALTZ_ASSIGN(at, _c.add_page(s.project, comp));
    ++added;
  }
  std::vector<std::int64_t> used;
  Json pages = Json::array();
  for (std::size_t i = 0; i < r.pages.size(); ++i) {
    if (i > 0 || skip_first) {
      VALTZ_ASSIGN(bool free, free_page(at + 1));
      if (free) {
        ++at;
      } else {
        VALTZ_ASSIGN(at, _c.add_page(s.project, comp, at));
        ++added;
      }
    }
    used.push_back(at);
    VALTZ_ASSIGN(std::string layer, markup_on_(s.project, comp, at));
    VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
    for (Json o : r.pages[i].objects) {
      o["id"] = object_id();
      objs.push_back(std::move(o));
    }
    VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
    pages.push_back({{"page", at + 1},
                     {"layer", model_layer(layer)},
                     {"starts", r.pages[i].starts}});
  }
  Json heads = Json::array();
  for (const auto& h : r.headings) {
    heads.push_back({{"text", std::string(utf8_prefix(h.text, 120))},
                     {"page", used[static_cast<std::size_t>(h.page)] + 1}});
  }
  VALTZ_ASSIGN(project::Asset now, p->asset(comp));
  const auto& last = r.pages.back();
  const double colx = area[0] + last.last_column * (colw + spec.gutter);
  Json out = {
      {"pages", std::move(pages)},
      {"first_page", used.front() + 1},
      {"last_page", used.back() + 1},
      {"pages_added", added},
      {"composition_pages", now.pages},
      {"headings", std::move(heads)},
      // Room left on the last page, under the text in its last column.
      {"free_on_last_page",
       {{"x", std::lround(colx)},
        {"y", std::lround(last.ends_at)},
        {"width", std::lround(area[2] - (colx - area[0]))},
        {"height", std::lround(area[1] + area[3] - last.ends_at)}}},
      {"area", {{"x", std::lround(area[0])}, {"y", std::lround(area[1])},
                {"width", std::lround(area[2])},
                {"height", std::lround(area[3])}}},
      {"columns", spec.columns},
      {"style",
       {{"font", spec.body.font},
        {"size", spec.body.size},
        {"color", text_arg(args, "color", "#222222")},
        {"heading_font", spec.heading.font},
        {"heading_size", spec.heading.size},
        {"title_size", spec.title.size}}}};
  if (found) {
    out["note"] = "Headings were found in the text (short lines standing "
                  "alone); find_headings false takes only # lines.";
  }
  // Pages after the text holding nothing of their own (made ahead of
  // it): the model's to fill or remove.
  Json empty = Json::array();
  for (std::int64_t pg = used.back() + 1; pg < now.pages; ++pg) {
    VALTZ_ASSIGN(bool free, free_page(pg));
    if (free) {
      empty.push_back(pg + 1);
    }
  }
  if (!empty.empty()) {
    out["empty_pages"] = std::move(empty);
    out["empty_note"] = "These pages after the text hold nothing of their "
                        "own: use them or remove_page them (the last "
                        "first).";
  }
  // The passage stopped short of the person's text (the 9B laid an
  // article out a few paragraphs a call): what is left, and how to go
  // on in one call.
  if (!after.empty()) {
    std::string next;
    int words = 0, in = 0;
    for (char ch : after) {
      const bool sp = std::isspace(static_cast<unsigned char>(ch)) != 0;
      if (!sp && !in) {
        ++words;
      }
      in = !sp;
      if (words <= 8 && !(sp && words == 8)) {
        next += sp ? ' ' : ch;
      }
    }
    out["more_in_message"] = {
        {"words", words},
        {"next_words", one_line(next)},
        {"note", std::format(
             "The person's message goes on for {} more words after "
             "to_words. Lay out ALL the rest in ONE call: flow_text with "
             "from_words \"{}\", no to_words, first_page {} (it goes on "
             "below the text there).",
             words, one_line(next), used.back() + 1)}};
  }
  // And words before it (past the request's first paragraph): perhaps
  // the text's own start -- the 9B began at the first heading and left
  // the article's title and opening paragraph out.
  if (!before.empty()) {
    int words = 0, in = 0;
    for (char ch : before) {
      const bool sp = std::isspace(static_cast<unsigned char>(ch)) != 0;
      words += !sp && !in;
      in = !sp;
    }
    std::string first(utf8_prefix(before, 60));
    out["before_in_message"] = {
        {"words", words},
        {"starts", one_line(first)},
        {"note", std::format(
             "The person's message has {} words before from_words, from "
             "\"{}\". If they are the text's start (a title, an opening "
             "paragraph), lay them out too.",
             words, one_line(first))}};
  }
  if (r.left > 0) {
    out["left_out_bytes"] = r.left;
    out["error"] = "the pages ran out: the rest of the text is not laid";
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_add_page_progress(Session& s, Step& step,
                                           const Json& args,
                                           std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  if (a.cls != AssetClass::Still) {
    return make_error(Code::InvalidArgument,
                      "a page progress bar is for a still's pages");
  }
  VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
  const auto from = static_cast<std::int64_t>(number(args, "from_page", 1));
  const auto to =
      static_cast<std::int64_t>(number(args, "to_page", a.pages));
  if (from < 1 || to > a.pages || from > to) {
    return make_error(Code::InvalidArgument, std::format(
        "from_page and to_page are pages 1 to {}, the first not after "
        "the last", a.pages));
  }
  const std::string style = text_arg(args, "style", "bar");
  if (style != "bar" && style != "segments" && style != "dots") {
    return make_error(Code::InvalidArgument,
                      "style is bar, segments or dots");
  }
  const double W = frame.width, H = frame.height;
  const double bh = std::max(2.0, number(args, "height",
                                         std::max(4.0, std::round(H * 0.006))));
  const double bx = number(args, "x", std::round(W * 0.08));
  const double bw = std::max(8.0, number(args, "width", W - 2 * bx));
  const double by =
      number(args, "y", H - std::round(H * 0.045) - bh);
  auto track = colour(text_arg(args, "track", "#8080804d"));
  auto fill = colour(text_arg(args, "fill", "#808080"));
  auto ncol = colour(text_arg(args, "number_color",
                              text_arg(args, "fill", "#808080")));
  if (!track || !fill || !ncol) {
    return make_error(Code::InvalidArgument,
                      "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
  }
  const bool numbers = flag(args, "numbers");
  const double nsize =
      std::clamp(number(args, "size", std::round(H * 0.02)), 6.0, 200.0);
  const std::string font = text_arg(args, "font", "Avenir Next");
  auto rgba = [](const std::array<double, 4>& v) {
    return Json::array({v[0], v[1], v[2], v[3]});
  };
  auto shape = [&](const char* kind, double x0, double y0, double x1,
                   double y1, const std::array<double, 4>& in) {
    return Json{{"id", object_id()}, {"kind", kind},
                {"x0", x0}, {"y0", y0}, {"x1", x1}, {"y1", y1},
                {"stroke", rgba(in)}, {"fill", rgba(in)}, {"width", 0}};
  };
  // A bar's round ends: a line as thick as the bar, round-capped.
  auto bar = [&](double x0, double x1, const std::array<double, 4>& in) {
    const double r = bh / 2;
    Json o = shape("line", x0 + r, by + r, std::max(x0 + r, x1 - r),
                   by + r, in);
    o["width"] = bh;
    return o;
  };
  const std::int64_t n = to - from + 1;
  // Too many for segments or dots to read: a bar.
  const std::string drawn = (style == "segments" && n > 60) ||
                                    (style == "dots" && n > 40)
                                ? "bar"
                                : style;

  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  // A bar made before goes: its folder's layers.
  for (const auto& f : a.layer_folders) {
    if (f.name != kProgressName) {
      continue;
    }
    std::vector<std::string> old;
    for (const auto& l : a.layers) {
      if (l.folder == f.id) {
        old.push_back(l.id);
      }
    }
    VALTZ_TRY(_c.ungroup_layers(s.project, comp, f.id));
    for (const auto& l : old) {
      VALTZ_TRY(_c.remove_layer(s.project, comp, l));
    }
  }
  std::vector<std::string> made;
  for (std::int64_t k = from - 1; k < to; ++k) {
    const std::int64_t i = k - from + 2;  // 1 .. n
    Json objs = Json::array();
    if (drawn == "bar") {
      objs.push_back(bar(bx, bx + bw, *track));
      objs.push_back(bar(bx, bx + bh + (bw - bh) * i / n, *fill));
    } else if (drawn == "segments") {
      const double gap = std::max(2.0, std::round(bw * 0.006));
      const double sw = (bw - gap * (n - 1)) / n;
      for (std::int64_t j = 0; j < n; ++j) {
        const double x0 = bx + j * (sw + gap);
        objs.push_back(shape("rect", x0, by, x0 + sw, by + bh,
                             j < i ? *fill : *track));
      }
    } else {
      // A dot big enough to see, whatever thickness a bar would have.
      const double d = std::max({bh, 6.0, std::round(H * 0.007)});
      const double stepx = std::min(d * 2.4, bw / n);
      const double x0 = bx + (bw - stepx * (n - 1) - d) / 2;
      for (std::int64_t j = 0; j < n; ++j) {
        const double x = x0 + j * stepx;
        objs.push_back(shape("ellipse", x, by, x + d, by + d,
                             j == i - 1 ? *fill : *track));
      }
    }
    if (numbers) {
      objs.push_back(
          {{"id", object_id()},
           {"kind", "text"},
           {"box", true},
           {"x0", bx},
           {"y0", by - nsize * 1.6},
           {"x1", bx + bw},
           {"y1", by - nsize * 0.2},
           {"text", std::format("{} / {}", i, n)},
           {"align", "right"},
           {"stroke", rgba(*ncol)},
           {"fill", Json::array({0, 0, 0, 0})},
           {"width", 0},
           {"font", {{"family", font}, {"size", nsize}, {"bold", false},
                     {"italic", false}, {"underline", false}}}});
    }
    VALTZ_ASSIGN(project::Asset now, p->asset(comp));
    const std::string top = now.layers.empty() ? "" : now.layers.back().id;
    VALTZ_ASSIGN(std::string blank, _c.add_layer(s.project, comp, top, k));
    VALTZ_TRY(page_alone_(s.project, comp, blank, k));
    VALTZ_ASSIGN(std::string layer,
                 _c.markup_layer(s.project, comp, {blank}, k));
    VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
    VALTZ_TRY(_c.rename_layer(s.project, comp, layer,
                              std::format("Progress {}", k + 1)));
    made.push_back(layer);
  }
  VALTZ_ASSIGN(std::string folder,
               _c.group_layers(s.project, comp, made, kProgressName));
  (void)folder;
  return Json{{"pages", Json::array({from, to})},
              {"style", drawn},
              {"layers", made.size()},
              {"box", {{"x", std::lround(bx)}, {"y", std::lround(by)},
                       {"width", std::lround(bw)},
                       {"height", std::lround(bh)}}},
              {"note", "Made again (it replaces this one) after pages are "
                       "added or removed."}};
}

// ---- layers -----------------------------------------------------------------

Result<Json>
Controller::AgentHost::t_place_asset(Session& s, Step& step,
                                     const Json& args,
                                     std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::int64_t page,
               page_(s.project, comp, args, "page", true));
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
  VALTZ_TRY(page_alone_(s.project, comp, layer, page));
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
    VALTZ_TRY(to_back_(s.project, comp, layer));
  }
  Json out = {{"layer", model_layer(layer)}, {"page", page + 1}};
  if (!flag(args, "below_all")) {
    if (auto a = _c.project(s.project)->asset(comp); a.ok()) {
      auto it = std::ranges::find(a->layers, layer, &project::Layer::id);
      Json b = it != a->layers.end() ? layer_box_(s.project, *a, *it)
                                     : Json();
      if (b.is_object()) {
        const std::array<double, 4> bx = {
            jget(b, "x", 0.0), jget(b, "y", 0.0), jget(b, "width", 0.0),
            jget(b, "height", 0.0)};
        if (Json c = crowding_(s.project, comp, page, bx); c.is_object()) {
          out.update(c);
        }
      }
    }
  }
  return out;
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
  // Where it lands: a box on a page of a composition -- none named (no
  // composition, page or box), an asset alone, as its description says
  // (it landed on the open composition's first page: a scene's
  // character picture on the timeline).
  const bool placed = args.contains("composition") ||
                      args.contains("page") || args.contains("x") ||
                      args.contains("width") || flag(args, "below_all");
  std::pair<int, int> size{1024, 1024};
  if (placed) {
    VALTZ_ASSIGN(AssetId comp, composition_(s, args));
    VALTZ_ASSIGN(l.page, page_(s.project, comp, args, "page", true));
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
    s.idle_waits = 0;
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
    if (!l.bottom) {
      if (Json c = crowding_(s.project, *l.composition, l.page, *l.box);
          c.is_object()) {
        r.update(c);
      }
    }
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
  // Nothing being made, nothing new: said plainly -- and a second such
  // wait refused. A picture that landed within its own step is told in
  // that step's result; the 9B, told only "landed": [], waited 164
  // times in a row.
  if (landed.empty() && s.pending.empty() && s.held.empty()) {
    if (++s.idle_waits > 1) {
      return make_error(Code::InvalidArgument,
                        "nothing is being made and nothing will land: do "
                        "not wait again -- go on with the next step, or "
                        "end with a short message");
    }
    return Json{{"landed", std::move(landed)},
                {"note", "Nothing is being made: everything asked for has "
                         "landed (each told in its own step's result). Go "
                         "on."}};
  }
  s.idle_waits = 0;
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
    s.idle_waits = 0;
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
    s.idle_waits = 0;
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

// ---- scenes: shots ----------------------------------------------------------

Result<Json>
Controller::AgentHost::t_generate_video(Session& s, Step& step,
                                        const Json& args,
                                        std::vector<fs::path>&)
{
  assist::ShotText text;
  text.description = text_arg(args, "description");
  if (one_line(text.description).empty()) {
    return make_error(Code::InvalidArgument,
                      "the shot's description is empty");
  }
  text.soundscape = text_arg(args, "soundscape");
  text.music = text_arg(args, "music");
  text.subjects = text_arg(args, "subjects");
  text.summary = text_arg(args, "summary");
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // What it is made from: a picture to open on, pictures to draw on, a
  // clip to carry on from.
  auto asset_arg = [&](const std::string& t,
                       AssetKind kind) -> Result<AssetId> {
    const auto id = AssetId::parse(t);
    if (!id) {
      return make_error(Code::InvalidArgument,
                        std::format("'{}' is not an asset id", t));
    }
    VALTZ_ASSIGN(project::Asset a, p->asset(*id));
    if (a.kind != kind) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is not a {}", a.name,
          kind == AssetKind::Image ? "picture" : "clip"));
    }
    return *id;
  };
  std::optional<AssetId> first;
  if (const auto f = text_arg(args, "first"); !f.empty()) {
    VALTZ_ASSIGN(first, asset_arg(f, AssetKind::Image));
  }
  std::vector<AssetId> pictures;
  {
    Json refs = args.is_object() && args.contains("references")
                    ? args["references"]
                    : Json::array();
    if (refs.is_string()) {
      refs = Json::array({refs});
    }
    for (const auto& r : refs.is_array() ? refs : Json::array()) {
      if (r.is_string()) {
        VALTZ_ASSIGN(AssetId id, asset_arg(r.get<std::string>(),
                                           AssetKind::Image));
        pictures.push_back(id);
      }
    }
  }
  const bool continues = flag(args, "continues");
  const std::string from_text = text_arg(args, "continue_from");
  if (first && (continues || !from_text.empty() || !pictures.empty())) {
    return make_error(Code::InvalidArgument,
                      "a shot opens on a picture (first), or draws on "
                      "references and carries on from a clip -- not both");
  }
  const bool refs = continues || !from_text.empty() || !pictures.empty();
  // The model Auto runs it on: FL2VA from words or a picture, Ref2VA
  // carrying on or drawing on references.
  const models::ModelEntry* m = nullptr;
  if (refs) {
    m = _c.reference_model_("");
  } else {
    for (const auto& a : _c.auto_models()) {
      if (a.modality == "video" && a.op == "generate" && !a.chosen.empty()) {
        m = _c._catalog.find(a.chosen);
      }
    }
  }
  if (!m || !_c.runs_soon(*m, "video", refs ? "edit" : "generate")) {
    return make_error(Code::Unsupported,
                      refs ? "no video model that carries a clip on or "
                             "draws on references runs here"
                           : "no video model runs here");
  }
  // Where it lands: the timeline named, or the one open; else an asset
  // alone.
  std::optional<AssetId> comp;
  if (args.contains("composition")) {
    VALTZ_ASSIGN(AssetId c, timeline_(s, args));
    comp = c;
  } else if (auto c = timeline_(s, args); c.ok()) {
    comp = *c;
  }
  if (comp) {
    VALTZ_ASSIGN(project::Asset tl, p->asset(*comp));
    if (tl.kind == AssetKind::Audio) {
      return make_error(Code::InvalidArgument,
                        "a composition of sound shows no shots");
    }
  }
  // Its length, as the model makes lengths (17n + 5 frames for H3: 5 s
  // is 124 frames, 5.17 s).
  const Json vp = jget(m->engine, "vpipe", Json::object());
  const Json defaults = jget(vp, "defaults", Json::object());
  const double fps = jget(defaults, "fps", 24.0);
  const Json grid = jget(vp, "frame_grid", Json::object());
  const int gstep = std::max(1, jget(grid, "step", 1));
  const int goff = std::max(1, jget(grid, "offset", 1));
  int frames = static_cast<int>(std::lround(
      std::clamp(number(args, "seconds", 5), 1.0, 10.0) * fps));
  frames = std::min(frames, jget(vp, "max_frames", 241));
  frames = frames <= goff
               ? goff
               : goff + (frames - goff + gstep - 1) / gstep * gstep;
  const double length = frames / fps;
  // Its size: the frame's, at the model's default area at most.
  const int align = std::max(16, jget(vp, "align", 16));
  std::pair<int, int> size{jget(defaults, "width", 960),
                           jget(defaults, "height", 544)};
  if (comp) {
    VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, *comp));
    size = shot_size(frame, size.first, size.second, align);
  }
  Landing l;
  l.session = s.id;
  l.step = step.n;
  l.project = s.project;
  l.clip = true;
  l.seconds = length;
  GenerateVideoRequest req;
  req.project = s.project;
  req.model = m->id;
  req.name = one_line(text_arg(args, "name"));
  req.width = size.first;
  req.height = size.second;
  req.frames = frames;
  req.fps = fps;
  if (args.contains("seed") && args["seed"].is_number_integer()) {
    req.seed = std::max<std::int64_t>(0, args["seed"].get<std::int64_t>());
  }
  req.first = first;
  req.references = pictures;
  Json r = {{"queued", true},
            {"seconds", secs(length)},
            {"size", {size.first, size.second}},
            {"model", m->name}};
  double start = 0;
  std::optional<AssetId> from;
  // Where it goes and what it carries on from are decided, and a shot
  // held behind one still being made is booked, in one hold of the
  // session: a shot that lands meanwhile is marked done in it before its
  // held ones are looked for (land_), so none is missed.
  {
    std::unique_lock lk(s.mu);
    const Session::Shots shots = comp && s.shots.contains(*comp)
                                     ? s.shots.at(*comp)
                                     : Session::Shots{};
    if (req.name.empty()) {
      req.name = std::format("Shot {}", shots.count + 1);
    }
    if (comp) {
      // A shot that carries on goes right after the one it carries on
      // from, whatever start is given: a model counts a scene's seconds,
      // not the lengths the model makes.
      const bool placed = args.contains("start") && !continues;
      start = placed ? std::max(0.0, number(args, "start", 0))
                     : std::max(shown_end_(s.project, *comp), shots.end);
      if (args.contains("start") && continues) {
        r["note"] = std::format("A shot that continues lands where the "
                                "one before it ends: {} s.",
                                secs(start));
      }
      l.composition = comp;
      l.start = frames_of(start, rate_(s.project, *comp).to_double());
      r["lands_on"] = {{"composition", comp->str()},
                       {"start", secs(start)},
                       {"end", secs(start + length)}};
    }
    // What it carries on from: the shot before it on this timeline --
    // made, or still being made (it is then held behind it) -- else the
    // clip that ends the timeline; or the clip named, or a shot of its
    // own still being made.
    int after = 0;
    auto of_step = [&](int n) -> Status {
      const Step& prev = s.steps[static_cast<std::size_t>(n - 1)];
      if (prev.state == "failed" || prev.state == "skipped") {
        return make_error(Code::InvalidArgument, std::format(
            "the shot it would carry on from (step {}) was not made: make "
            "that one again first",
            n));
      }
      if (prev.state != "done") {
        after = n;
        return ok_status();
      }
      const auto made =
          AssetId::parse(jget<std::string>(prev.result, "asset", ""));
      auto a = made ? p->asset(*made) : Result<project::Asset>(make_error(
                                            Code::NotFound, "none"));
      if (!a.ok() || a->head == 0) {
        return make_error(Code::InvalidArgument, std::format(
            "step {} made no clip to carry on from", n));
      }
      from = *made;
      return ok_status();
    };
    if (continues) {
      if (!comp) {
        return make_error(Code::InvalidArgument,
                          "`continues` carries on along a timeline: make "
                          "one, or name the clip (continue_from)");
      }
      // Its subjects in the same words as the shot before's.
      if (one_line(text.subjects).empty()) {
        text.subjects = shots.subjects;
      }
      if (shots.last > 0) {
        VALTZ_TRY(of_step(shots.last));
      } else {
        // No shot of its own before it: the clip that ends the timeline.
        VALTZ_ASSIGN(project::Asset tl, p->asset(*comp));
        VALTZ_ASSIGN(auto times, timings_(s.project, *comp));
        double end = -1;
        for (const auto& x : tl.layers) {
          const auto it = times.find(x.id);
          if (layer_kind_(s.project, x) == "clip" && it != times.end() &&
              std::isfinite(it->second.length) && it->second.end() > end) {
            end = it->second.end();
            from = x.source;
          }
        }
        if (!from) {
          return make_error(Code::InvalidArgument,
                            "nothing on the timeline to carry on from: the "
                            "first shot is made without `continues`");
        }
      }
    } else if (!from_text.empty()) {
      const auto id = AssetId::parse(from_text);
      if (!id) {
        return make_error(Code::InvalidArgument,
                          std::format("'{}' is not an asset id", from_text));
      }
      for (const auto& st : s.steps) {
        if (st.n != step.n && st.tool == "generate_video" &&
            jget<std::string>(st.result, "asset", "") == id->str() &&
            st.state != "done") {
          VALTZ_TRY(of_step(st.n));
        }
      }
      if (!after && !from) {
        VALTZ_ASSIGN(from, asset_arg(from_text, AssetKind::Video));
      }
    }
    if (after > 0) {
      // Held until the shot before it lands.
      const std::string subjects = text.subjects;
      {
        std::lock_guard hk(_mu);
        _held.push_back({s.id, step.n, after, step.title, std::move(req),
                         std::move(text), l});
      }
      s.held.insert(step.n);
      if (comp) {
        auto& sh = s.shots[*comp];
        sh.last = step.n;
        sh.end = std::max(sh.end, start + length);
        ++sh.count;
        if (!one_line(subjects).empty()) {
          sh.subjects = subjects;
        }
      }
      r["after_step"] = after;
      r["note"] = std::format("It is asked for once step {}'s shot lands, "
                              "carrying on from its end.",
                              after);
      return r;
    }
  }
  req.continue_from = from;
  const std::string subjects = text.subjects;
  VALTZ_ASSIGN(JobId job, submit_shot_(s, step.title, std::move(req),
                                       std::move(text), l));
  step.job = job;
  {
    std::lock_guard lk(s.mu);
    if (comp) {
      auto& sh = s.shots[*comp];
      sh.last = step.n;
      sh.end = std::max(sh.end, start + length);
      ++sh.count;
      if (!one_line(subjects).empty()) {
        sh.subjects = subjects;
      }
    }
  }
  r["job"] = job.str();
  if (auto rec = _c._jobs.get(job); rec && !rec->asset.is_nil()) {
    r["asset"] = rec->asset.str();
  }
  if (from) {
    r["continues_from"] = from->str();
  }
  return r;
}

Result<JobId>
Controller::AgentHost::submit_shot_(Session& s, const std::string& title,
                                    GenerateVideoRequest req,
                                    assist::ShotText text, const Landing& l)
{
  auto* p = _c.project(s.project);
  const auto* m = _c._catalog.find(req.model);
  if (!p || !m) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // Its references by the names the model reads them by: the pictures,
  // the clip continued and its sound -- or the picture it opens on.
  const Json fmt = jget(m->prompting, "reference_tags", Json::object());
  auto tag = [&](const char* kind, int n) {
    std::string t = jget<std::string>(fmt, kind, "");
    if (const auto at = t.find("{n}"); at != std::string::npos) {
      t.replace(at, 3, std::to_string(n));
    }
    return t;
  };
  if (req.continue_from || !req.references.empty()) {
    VALTZ_ASSIGN(Json tags,
                 _c.video_reference_tags(s.project, req.model,
                                         req.references, req.continue_from));
    text.pictures.clear();
    for (const auto& id : req.references) {
      text.pictures.push_back(jget<std::string>(tags, id.str().c_str(), ""));
    }
    if (req.continue_from) {
      text.continued =
          jget<std::string>(tags, req.continue_from->str().c_str(), "");
      VALTZ_ASSIGN(project::Asset c, p->asset(*req.continue_from));
      if (_c.sounds_(*p, c, 0)) {
        text.continued_sound = tag("audio", 1);
      }
    }
  } else if (req.first) {
    text.first = tag("image", 1);
  }
  req.prompt = assist::shot_prompt(text);
  auto undo = _c.command_(s.project, "agent",
                          l.composition.value_or(AssetId{}), "",
                          {{"tool", "generate_video"}, {"title", title}});
  VALTZ_ASSIGN(JobId job, _c.generate_video(std::move(req)));
  {
    std::lock_guard lk(_mu);
    _landings[job] = l;
  }
  {
    std::lock_guard lk(s.mu);
    s.pending.insert(job);
    s.idle_waits = 0;
  }
  return job;
}

Json
Controller::AgentHost::lay_shot_(const Landing& l, AssetId comp,
                                 const std::string& layer, AssetId asset,
                                 bool first_clip)
{
  Json out = Json::object();
  auto* p = _c.project(l.project);
  if (!p) {
    return out;
  }
  // The first clip goes under what is there (titles, sounds).
  if (first_clip) {
    if (auto tl = p->asset(comp); tl.ok() && tl->layers.size() > 1) {
      (void)_c.place_layers(l.project, comp, {layer}, std::nullopt, "");
    }
  }
  // Filling the frame: made at its shape, any rounding cut at the edges.
  auto frame = _c.own_frame(l.project, comp);
  auto content = _c.shown_size_(*p, asset, 0, 0);
  if (frame.ok() && content.ok() && content->width > 0 &&
      content->height > 0 &&
      (content->width != frame->width ||
       content->height != frame->height)) {
    media::Crop c = box_crop_(
        *content, *frame,
        {0, 0, static_cast<double>(frame->width),
         static_cast<double>(frame->height)});
    c.scale_x = c.scale_y = std::max(
        static_cast<double>(frame->width) / content->width,
        static_cast<double>(frame->height) / content->height);
    (void)_c.set_crop(l.project, comp, c, layer);
  }
  if (auto times = timings_(l.project, comp); times.ok()) {
    if (const auto it = times->find(layer); it != times->end()) {
      out["start"] = secs(it->second.start);
      out["end"] = secs(it->second.end());
    }
  }
  return out;
}

void
Controller::AgentHost::release_held_(Session& s, int after,
                                     std::optional<AssetId> landed,
                                     const std::string& why)
{
  std::vector<Held> go;
  {
    std::lock_guard lk(_mu);
    for (auto it = _held.begin(); it != _held.end();) {
      if (it->session == s.id && it->after == after) {
        go.push_back(std::move(*it));
        it = _held.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& h : go) {
    bool stopped = false;
    {
      std::lock_guard lk(s.mu);
      stopped = s.stop_asked;
    }
    Result<JobId> job = make_error(Code::Cancelled, "stopped");
    if (!stopped && !landed) {
      job = make_error(Code::InvalidArgument, std::format(
          "the shot it carries on from (step {}) was not made{}", after,
          why.empty() ? "" : ": " + why));
    } else if (!stopped) {
      h.req.continue_from = *landed;
      job = submit_shot_(s, h.title, std::move(h.req), std::move(h.text),
                         h.landing);
    }
    {
      std::lock_guard lk(s.mu);
      s.held.erase(h.step);
      if (h.step >= 1 && h.step <= static_cast<int>(s.steps.size())) {
        Step& st = s.steps[static_cast<std::size_t>(h.step - 1)];
        if (job.ok()) {
          st.job = *job;
          st.state = "waiting";
          st.result["job"] = job->str();
          st.result["continues_from"] = landed->str();
          if (auto rec = _c._jobs.get(*job); rec && !rec->asset.is_nil()) {
            st.result["asset"] = rec->asset.str();
          }
        } else {
          st.state = "failed";
          st.error = job.error().message;
          s.landed.push_back({{"step", h.step}, {"error", st.error}});
        }
      }
    }
    s.cv.notify_all();
    post_(s, h.step);
    if (!job.ok()) {
      // What carries on from it is not made either.
      release_held_(s, h.step, std::nullopt, "");
    }
  }
}

}  // namespace valtz
