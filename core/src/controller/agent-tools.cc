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
#include "valtz/media/exif.h"
#include "valtz/media/markup.h"
#include "valtz/media/text-flow.h"
#include "valtz/media/thumbnail.h"
#include "valtz/media/timing.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <optional>
#include <set>

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

Json
ids(const char* doc)
{
  return {{"type", "array"}, {"items", {{"type", "string"}}},
          {"description", doc}};
}

const char* kCompDoc =
    "the composition's id; omitted: the one the session works on";
const char* kPageDoc =
    "the page, from 1 (needed on a still of more than one page)";
const char* kLayerDoc = "the layer's id (\"0\" is the bottom one)";
const char* kModelDoc =
    "the model's id (list_models) when the person names one -- kept for "
    "the rest of their request; omitted: Auto's";
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

// A line's or outline's `dash` and a line's `arrows` from a tool's
// arguments onto `obj` (media/markup.h): "solid" / "none" take them off.
void
stroke_style(Json& obj, const Json& args)
{
  const auto kind = jget<std::string>(obj, "kind", "");
  if (kind != "text" && args.contains("dash")) {
    const auto d = jget<std::string>(args, "dash", "solid");
    if (d == "dashed" || d == "dotted") {
      obj["dash"] = d;
    } else {
      obj.erase("dash");
    }
  }
  if (kind == "line" && args.contains("arrows")) {
    const auto a = jget<std::string>(args, "arrows", "none");
    if (a == "start" || a == "end" || a == "both") {
      obj["arrows"] = a;
    } else {
      obj.erase("arrows");
    }
  }
}

// An object's `rotate` (degrees) and `flip_x` / `flip_y` from a tool's
// arguments onto `obj`, about its middle (media/markup.h): 0 and false
// take them off.
void
turn_style(Json& obj, const Json& args)
{
  if (args.contains("rotate")) {
    const double r = jget(args, "rotate", 0.0);
    if (std::isfinite(r) && r != 0) {
      obj["rotate"] = r;
    } else {
      obj.erase("rotate");
    }
  }
  for (const char* k : {"flip_x", "flip_y"}) {
    if (args.contains(k)) {
      if (jget(args, k, false)) {
        obj[k] = true;
      } else {
        obj.erase(k);
      }
    }
  }
}

// ---- words in shapes, connectors, joined ends (media/markup.h) ------------

// A shape that holds WORDS: a rectangle, an ellipse, a closed polygon.
bool
holds_words(const Json& o)
{
  const auto k = jget<std::string>(o, "kind", "");
  return k == "rect" || k == "ellipse" ||
         (k == "polygon" && !jget(o, "open", false));
}

// Words set on a shape's inside unless told: black on a light one (or
// none), white on a dark one -- its fill, or its gradient's colours
// together.
Json
words_colour_on(const Json& o)
{
  std::array<double, 4> c{0, 0, 0, 0};
  std::size_t n = 0;
  auto add = [&](const Json& v) {
    if (!v.is_array() || v.size() != 4) {
      return;
    }
    for (std::size_t i = 0; i < 4; ++i) {
      c[i] += v[i].is_number() ? v[i].get<double>() : 0;
    }
    ++n;
  };
  if (const Json g = jget(o, "gradient", Json()); g.is_object()) {
    for (const auto& st : jget(g, "stops", Json::array())) {
      add(jget(st, "color", Json()));
    }
  } else {
    add(jget(o, "fill", Json()));
  }
  for (auto& v : c) {
    v /= static_cast<double>(std::max<std::size_t>(n, 1));
  }
  const double luma = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
  return c[3] >= 0.5 && luma < 0.5 ? Json::array({1, 1, 1, 1})
                                   : Json::array({0, 0, 0, 1});
}

// A shape's WORDS from a tool's arguments onto `o` (holds_words): `text`
// ("" takes them off, with their style), `size`, `font`, `bold`,
// `italic`, `align`, `line_spacing`, `text_color`. New words are
// Helvetica Neue, centred, black or white against the inside and -- no
// `size` given -- as big as fits, up to 48. Its outline's width set
// first (the words lie inside it). -> a note when a style was given to a
// shape with no words.
Result<std::string>
set_words(Json& o, const Json& args)
{
  const bool had = !jget<std::string>(o, "text", "").empty();
  if (args.contains("text")) {
    const std::string t = text_arg(args, "text");
    if (t.empty()) {
      for (const char* k :
           {"text", "font", "align", "line_spacing", "text_color"}) {
        o.erase(k);
      }
      return std::string();
    }
    o["text"] = t;
  }
  if (jget<std::string>(o, "text", "").empty()) {
    for (const char* k : {"size", "font", "bold", "italic", "align",
                          "line_spacing", "text_color"}) {
      if (args.contains(k)) {
        return std::string("It holds no words, so the words' style was "
                           "not set: give `text` with it.");
      }
    }
    return std::string();
  }
  Json font = had ? jget(o, "font", Json::object())
                  : Json{{"family", "Helvetica Neue"},
                         {"bold", false},
                         {"italic", false},
                         {"underline", false}};
  const double w = std::abs(jget(o, "x1", 0.0) - jget(o, "x0", 0.0));
  const double h = std::abs(jget(o, "y1", 0.0) - jget(o, "y0", 0.0));
  const bool sized = args.contains("size");
  if (sized) {
    font["size"] = std::clamp(number(args, "size", 48), 4.0, 2000.0);
  } else if (!had) {
    // A third of its shorter side (an ellipse's words lie in the
    // rectangle inside it), then smaller until they fit.
    const double side = std::min(w, h) *
        (jget<std::string>(o, "kind", "") == "ellipse" ? 0.7 : 1.0);
    font["size"] = std::clamp(std::round(side / 3), 12.0, 48.0);
  }
  if (args.contains("font")) {
    font["family"] = text_arg(args, "font");
  }
  for (const char* k : {"bold", "italic"}) {
    if (args.contains(k)) {
      font[k] = flag(args, k);
    }
  }
  o["font"] = std::move(font);
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
  if (args.contains("text_color")) {
    auto c = colour(text_arg(args, "text_color"));
    if (!c) {
      return make_error(Code::InvalidArgument,
                        "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
    }
    o["text_color"] = {(*c)[0], (*c)[1], (*c)[2], (*c)[3]};
  } else if (!had) {
    o["text_color"] = words_colour_on(o);
  }
  if (!sized && !had) {
    for (int i = 0; i < 24 && !media::text_fit(o).fits; ++i) {
      const double now = jget(o["font"], "size", 12.0);
      if (now <= 10) {
        break;
      }
      o["font"]["size"] = std::max(10.0, std::floor(now * 0.9));
    }
  }
  return std::string();
}

// A line's ends, a path's points: canvas pixels, before its turn.
std::vector<std::array<double, 2>>
points_of(const Json& o)
{
  const double x0 = jget(o, "x0", 0.0), y0 = jget(o, "y0", 0.0);
  const double x1 = jget(o, "x1", 0.0), y1 = jget(o, "y1", 0.0);
  if (jget<std::string>(o, "kind", "") == "line") {
    return {{x0, y0}, {x1, y1}};
  }
  std::vector<std::array<double, 2>> out;
  for (const auto& p : jget(o, "points", Json::array())) {
    if (p.is_array() && p.size() == 2 && p[0].is_number() &&
        p[1].is_number()) {
      out.push_back({x0 + p[0].get<double>() * (x1 - x0),
                     y0 + p[1].get<double>() * (y1 - y0)});
    }
  }
  return out;
}

// Connector `want` of `t`: its id, a SIDE -- "top", "right", "bottom",
// "left": the one furthest that way, the nearest the middle across --
// or, none named, the one nearest `toward`.
Result<media::ConnectorPoint>
connector_named(const Json& t, const std::string& want,
                std::array<double, 2> toward)
{
  const auto pts = media::connector_points(t);
  const auto id = jget<std::string>(t, "id", "");
  if (pts.empty()) {
    return make_error(Code::InvalidArgument,
                      std::format("'{}' has no connectors", id));
  }
  if (want.empty() || want == "nearest") {
    return *std::ranges::min_element(pts, {}, [&](const auto& p) {
      return std::hypot(p.x - toward[0], p.y - toward[1]);
    });
  }
  for (const auto& p : pts) {
    if (p.id == want) {
      return p;
    }
  }
  const bool across = want == "top" || want == "bottom";
  if (!across && want != "left" && want != "right") {
    std::string ids;
    for (const auto& p : pts) {
      ids += (ids.empty() ? "" : ", ") + p.id;
    }
    return make_error(Code::InvalidArgument, std::format(
        "'{}' has connectors {} -- or name a side: top, right, bottom, "
        "left",
        id, ids));
  }
  const auto b = media::object_bounds(t);
  const double cx = b[0] + b[2] / 2, cy = b[1] + b[3] / 2;
  auto along = [&](const media::ConnectorPoint& p) {
    return want == "top"    ? -p.y
           : want == "bottom" ? p.y
           : want == "left"   ? -p.x
                              : p.x;
  };
  const double most = along(std::ranges::max(pts, {}, along));
  const media::ConnectorPoint* best = nullptr;
  for (const auto& p : pts) {
    if (along(p) < most - 1) {
      continue;
    }
    const double off = across ? std::abs(p.x - cx) : std::abs(p.y - cy);
    if (!best || off < (across ? std::abs(best->x - cx)
                               : std::abs(best->y - cy))) {
      best = &p;
    }
  }
  return *best;
}

// A line's or an open path's ENDS JOINED to shapes of its layer (its
// "anchors") as a tool's arguments ask: `from_object` / `to_object` the
// shape's id ("none" lets that end go), `from_connector` /
// `to_connector` which of its connectors (connector_named: by default
// the one nearest the other end -- both ends left so, the nearest pair).
// A shape with no connectors is given its default ones. `objs` the
// layer's objects (`line` among them, or about to be). What was joined,
// and the shapes given connectors, told in `out`.
Status
join_ends(Json& line, Json& objs, const Json& args, Json& out)
{
  if (!args.contains("from_object") && !args.contains("to_object") &&
      !args.contains("from_connector") && !args.contains("to_connector")) {
    return ok_status();
  }
  const auto kind = jget<std::string>(line, "kind", "");
  if (kind != "line" && !(kind == "polygon" && jget(line, "open", false))) {
    return make_error(Code::InvalidArgument,
                      "only a line or an open polygon (a path) joins "
                      "shapes");
  }
  const auto pts = points_of(line);
  if (pts.size() < 2) {
    return make_error(Code::InvalidArgument, "it has no ends to join");
  }
  const std::string self = jget<std::string>(line, "id", "");
  Json anchors = jget(line, "anchors", Json::array());
  struct End {
    const char*                          name = "";
    int                                  point = 0;
    bool                                 asked = false;
    std::string                          to;
    std::string                          want;
    Json*                                shape = nullptr;
    std::optional<media::ConnectorPoint> at;
  };
  std::array<End, 2> ends;
  ends[0].name = "from";
  ends[1].name = "to";
  ends[1].point = static_cast<int>(pts.size()) - 1;
  Json given = Json::array();
  for (auto& e : ends) {
    const std::string obj_key = std::string(e.name) + "_object";
    const std::string conn_key = std::string(e.name) + "_connector";
    e.want = text_arg(args, conn_key.c_str());
    if (args.contains(obj_key)) {
      e.asked = true;
      e.to = text_arg(args, obj_key.c_str());
      if (e.to == "none") {
        e.to.clear();
      }
    } else if (args.contains(conn_key)) {
      // Another connector of the shape it is joined to.
      for (const auto& a : anchors) {
        if (jget(a, "point", -1) == e.point) {
          e.asked = true;
          e.to = jget<std::string>(a, "to", "");
        }
      }
      if (!e.asked) {
        return make_error(Code::InvalidArgument, std::format(
            "{} names no shape: give {} too", conn_key, obj_key));
      }
    }
    if (!e.asked || e.to.empty()) {
      continue;
    }
    if (e.to == self) {
      return make_error(Code::InvalidArgument,
                        "a line joins other shapes, not itself");
    }
    auto it = std::ranges::find_if(objs, [&](const Json& o) {
      return jget<std::string>(o, "id", "") == e.to;
    });
    if (it == objs.end()) {
      return make_error(Code::NotFound, std::format(
          "its layer has no object '{}': a line joins shapes of its own "
          "layer",
          e.to));
    }
    const auto tk = jget<std::string>(*it, "kind", "");
    if (!media::takes_connectors(tk)) {
      return make_error(Code::InvalidArgument, std::format(
          "'{}' is a {}: join the shape it lies on (or give the words a "
          "shape: add_shape's text)",
          e.to, tk));
    }
    if (jget(*it, "connectors", Json::array()).empty()) {
      *it = media::with_default_connectors(*it);
      given.push_back(e.to);
    }
    e.shape = &*it;
  }
  // Those named first; then, both left to choose, the nearest pair (two
  // of one shape apart); then each nearest the other end.
  for (auto& e : ends) {
    if (e.shape && !e.want.empty() && e.want != "nearest") {
      VALTZ_ASSIGN(e.at, connector_named(*e.shape, e.want, {}));
    }
  }
  if (ends[0].shape && ends[1].shape && !ends[0].at && !ends[1].at) {
    const auto a = media::connector_points(*ends[0].shape);
    const auto b = media::connector_points(*ends[1].shape);
    const bool same = ends[0].to == ends[1].to;
    double best = INFINITY;
    for (const auto& p : a) {
      for (const auto& q : b) {
        const double d = std::hypot(p.x - q.x, p.y - q.y);
        if ((!same || p.id != q.id) && d < best) {
          best = d;
          ends[0].at = p;
          ends[1].at = q;
        }
      }
    }
  }
  for (std::size_t i = 0; i < 2; ++i) {
    auto& e = ends[i];
    if (!e.shape || e.at) {
      continue;
    }
    const auto& o = ends[1 - i];
    std::array<double, 2> toward = pts[static_cast<std::size_t>(o.point)];
    if (o.at) {
      toward = {o.at->x, o.at->y};
    } else if (o.shape) {
      const auto b = media::object_bounds(*o.shape);
      toward = {b[0] + b[2] / 2, b[1] + b[3] / 2};
    }
    VALTZ_ASSIGN(e.at, connector_named(*e.shape, "", toward));
  }
  Json kept = Json::array();
  for (const auto& a : anchors) {
    const int k = jget(a, "point", -1);
    if (std::ranges::none_of(ends, [&](const End& e) {
          return e.asked && e.point == k;
        })) {
      kept.push_back(a);
    }
  }
  Json joined = Json::object();
  for (const auto& e : ends) {
    if (!e.asked) {
      continue;
    }
    if (e.to.empty()) {
      joined[e.name] = "none";
      continue;
    }
    kept.push_back(
        {{"point", e.point}, {"to", e.to}, {"connector", e.at->id}});
    joined[e.name] = {{"object", e.to},
                      {"connector", e.at->id},
                      {"at", {std::lround(e.at->x), std::lround(e.at->y)}}};
  }
  if (kept.empty()) {
    line.erase("anchors");
  } else {
    line["anchors"] = std::move(kept);
  }
  out["joined"] = std::move(joined);
  if (!given.empty()) {
    out["connectors_added"] = std::move(given);
  }
  return ok_status();
}

// The anchors of `o`'s points that lay elsewhere in `before` let go: a
// line or a path moved or reshaped is free of the shapes it was joined
// to, as one dragged in the app is (an end left where it was stays
// joined). The ids it was joined to that it let go of.
std::vector<std::string>
let_moved_points_go(Json& o, const Json& before)
{
  std::vector<std::string> gone;
  if (!o.contains("anchors")) {
    return gone;
  }
  const auto now = points_of(o), was = points_of(before);
  Json kept = Json::array();
  for (const auto& a : o["anchors"]) {
    const int k = jget(a, "point", -1);
    const auto i = static_cast<std::size_t>(k);
    if (k >= 0 && i < now.size() && i < was.size() &&
        std::hypot(now[i][0] - was[i][0], now[i][1] - was[i][1]) < 0.5) {
      kept.push_back(a);
    } else {
      gone.push_back(jget<std::string>(a, "to", ""));
    }
  }
  if (kept.empty()) {
    o.erase("anchors");
  } else {
    o["anchors"] = std::move(kept);
  }
  return gone;
}

// Each object's box as the agent is told it: [x, y, width, height].
Json
box_told(const Json& o)
{
  const double x0 = jget(o, "x0", 0.0), y0 = jget(o, "y0", 0.0);
  const double x1 = jget(o, "x1", 0.0), y1 = jget(o, "y1", 0.0);
  return Json::array({std::lround(std::min(x0, x1)),
                      std::lround(std::min(y0, y1)),
                      std::lround(std::abs(x1 - x0)),
                      std::lround(std::abs(y1 - y0))});
}

// The lines and paths that FOLLOWED what was changed: the objects of
// `after` not in `skip` whose box `before` had otherwise -- joined to a
// shape that moved -- by id, with their boxes now.
Json
followed(const Json& before, const Json& after,
         const std::set<std::string>& skip)
{
  std::map<std::string, Json> was;
  for (const auto& o : before) {
    if (o.contains("anchors")) {
      was[jget<std::string>(o, "id", "")] = box_told(o);
    }
  }
  Json out = Json::object();
  for (const auto& o : after) {
    const auto id = jget<std::string>(o, "id", "");
    const auto it = was.find(id);
    if (it != was.end() && !skip.count(id) && it->second != box_told(o)) {
      out[id] = box_told(o);
    }
  }
  return out;
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
// What a drawing tool's `layer` says.
const char* kDrawOnDoc =
    "a markup layer to draw on -- a MASK's (add_mask's mask_layer) -- "
    "by its id; none: the page's own";
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

// A frame's own size on the model's grid -- up to it -- held to `most`
// on its long side (its shape kept): a high-res shot's.
std::pair<int, int>
frame_on_grid(media::PixelSize frame, int grid, int most)
{
  double w = std::max(1, frame.width), h = std::max(1, frame.height);
  if (std::max(w, h) > most) {
    const double k = most / std::max(w, h);
    w *= k;
    h *= k;
  }
  const auto up = [&](double v) {
    return static_cast<int>(std::ceil(v / grid - 1e-9)) * grid;
  };
  return {up(w), up(h)};
}

// A picture at the box's shape with its longer side `side` pixels, on a
// 32-pixel grid, held to `most` (the model's max_side).
std::pair<int, int>
picture_size_long(double w, double h, double side, int most)
{
  const double aspect = std::clamp(w / std::max(1.0, h), 0.25, 4.0);
  const double l = std::clamp(side, 256.0, static_cast<double>(most));
  const double pw = aspect >= 1 ? l : l * aspect;
  const double ph = aspect >= 1 ? l / aspect : l;
  auto down = [most](double v) {
    return static_cast<int>(std::clamp(
        std::floor(std::max(32.0, v) / 32) * 32, 32.0,
        static_cast<double>(most)));
  };
  return {down(pw), down(ph)};
}

// A shape asked by its ratio ("16:9", "3:2", "1:1"): its two sides, or
// none.
std::optional<std::pair<double, double>>
aspect_of(const std::string& s)
{
  const auto colon = s.find(':');
  if (colon == std::string::npos) {
    return std::nullopt;
  }
  double w = 0;
  double h = 0;
  try {
    w = std::stod(s.substr(0, colon));
    h = std::stod(s.substr(colon + 1));
  } catch (...) {
    return std::nullopt;
  }
  if (!(w > 0 && h > 0 && w / h < 8 && h / w < 8)) {
    return std::nullopt;
  }
  return std::pair{w, h};
}

// A shot's length as the model makes lengths (17n + 5 frames for H3: 5 s
// is 124 frames, 5.17 s): its frames and their rate.
std::pair<int, double>
shot_frames(const models::ModelEntry& m, double seconds)
{
  const Json vp = jget(m.engine, "vpipe", Json::object());
  const double fps = jget(jget(vp, "defaults", Json::object()), "fps", 24.0);
  const Json grid = jget(vp, "frame_grid", Json::object());
  const int gstep = std::max(1, jget(grid, "step", 1));
  const int goff = std::max(1, jget(grid, "offset", 1));
  int frames = static_cast<int>(
      std::lround(std::clamp(seconds, 1.0, 30.0) * fps));
  if (const int most = jget(vp, "max_frames", 0); most > 0) {
    frames = std::min(frames, most);
  }
  frames = frames <= goff
               ? goff
               : goff + (frames - goff + gstep - 1) / gstep * gstep;
  return {frames, fps};
}

// A model that continues a clip GUIDED through its end (catalog
// `continuation.guide`: FL2VA) -- the tail's latents pinned over the new
// clip's first frames, which redo them (DESIGN §4e) -- and how many.
int
guide_frames(const models::ModelEntry& m)
{
  const Json g = jget(jget(jget(m.engine, "vpipe", Json::object()),
                           "continuation", Json::object()),
                      "guide", Json());
  return g.is_object() ? std::max(1, jget(g, "frames", 22)) : 0;
}

// A guided shot's OWN frames: with the tail it redoes, on the model's
// grid -- the length nearest the one asked.
int
guided_frames(const models::ModelEntry& m, int frames)
{
  const Json grid = jget(jget(m.engine, "vpipe", Json::object()),
                         "frame_grid", Json::object());
  const int gstep = std::max(1, jget(grid, "step", 1));
  const int goff = std::max(1, jget(grid, "offset", 1));
  const int overlap = guide_frames(m);
  const int all = frames + overlap;
  const int up = all <= goff ? goff
                             : goff + (all - goff + gstep - 1) / gstep * gstep;
  int best = up - overlap;
  if (const int down = up - gstep - overlap;
      down >= gstep && std::abs(down - frames) < std::abs(best - frames)) {
    best = down;
  }
  return std::max(1, best);
}

// A latent-guided shot's words: FL2VA reads no reference, so each
// <Subject N> is the words its definition gives ("<Subject 1> is the old
// clockmaker, silver bun, in <Video 1>." -- "the old clockmaker, silver
// bun"), the clip it carries on "the shot before".
void
guided_words(assist::ShotText& t)
{
  std::vector<std::pair<std::string, std::string>> names;
  std::size_t at = 0;
  const std::string& defs = t.subjects;
  while (at < defs.size()) {
    auto end = defs.find('\n', at);
    end = end == std::string::npos ? defs.size() : end;
    const std::string line = one_line(defs.substr(at, end - at));
    at = end + 1;
    const auto close = line.find('>');
    const auto is = line.find(" is ");
    if (!line.starts_with("<Subject ") || close == std::string::npos ||
        is == std::string::npos || is < close) {
      continue;
    }
    std::string phrase = line.substr(is + 4);
    if (const auto in = phrase.find(" in <"); in != std::string::npos) {
      phrase.erase(in);
    }
    while (!phrase.empty() && (phrase.back() == '.' ||
                               phrase.back() == ',' || phrase.back() == ' ')) {
      phrase.pop_back();
    }
    if (!phrase.empty()) {
      names.emplace_back(line.substr(0, close + 1), phrase);
    }
  }
  for (const char* v : {"<Video 1>", "<Video 2>"}) {
    names.emplace_back(v, "the shot before");
  }
  names.emplace_back("<Audio 1>", "its sound");
  t.description = assist::retag(t.description, names);
  t.soundscape = assist::retag(t.soundscape, names);
  t.subjects.clear();
  t.summary.clear();
  t.continued.clear();
  t.continued_sound.clear();
  t.pictures.clear();
}

// A refusal of the latents a clip kept -- none, none that cover its end,
// no port for them: the continuation made without them instead.
bool
latents_refused(const Error& e)
{
  return e.key == msg::kNoKeptLatents.key ||
         e.key == msg::kLatentsDoNotFit.key ||
         e.key == msg::kLatentsNoPort.key;
}

// "{n}" in a model's name form ("<image{n}>", "Picture {n}") made `n`.
std::string
numbered(std::string form, int n)
{
  if (const auto at = form.find("{n}"); at != std::string::npos) {
    form.replace(at, 3, std::to_string(n));
  }
  return form;
}

// The TASKS a model is chosen for (list_models, each tool's `model`):
// the tool that does it, and what it makes, as the agent is told.
struct ModelTask {
  const char* name;
  const char* tool;
  const char* what;
};
constexpr ModelTask kModelTasks[] = {
    {"picture", "generate_image", "pictures"},
    {"edit", "generate_image with base or references", "picture edits"},
    {"shot", "generate_video", "shots from words, or opening or ending on "
                               "a picture"},
    {"shot_references", "generate_video with references or continues",
     "shots drawing on references or carrying one on"},
    {"music", "generate_music", "music"},
    {"voiceover", "generate_voiceover", "voice-overs"},
    {"transcribe", "transcribe", "transcripts"},
    {"separate", "separate_sound", "a song's voice and accompaniment"},
    {"upscale", "upscale_layer", "upscales"},
};

const ModelTask*
model_task(std::string_view name)
{
  for (const auto& t : kModelTasks) {
    if (name == t.name) {
      return &t;
    }
  }
  return nullptr;
}

// The modality and op a generating task asks Controller::runs about
// ("" for an asset operation's).
std::pair<std::string_view, std::string_view>
task_runs(std::string_view task)
{
  if (task == "picture") return {"image", "generate"};
  if (task == "edit") return {"image", "edit"};
  if (task == "shot") return {"video", "generate"};
  if (task == "shot_references") return {"video", "edit"};
  if (task == "music" || task == "voiceover") return {"audio", "generate"};
  return {};
}

// The capability an asset operation's model has for `m` (an upscaler
// for clips or pictures).
models::Capability
task_capability(std::string_view task, const models::ModelEntry& m)
{
  using C = models::Capability;
  if (task == "transcribe") return C::Transcribe;
  if (task == "separate") return C::AudioSeparate;
  return m.has(C::UpscaleVideo) ? C::UpscaleVideo : C::UpscaleImage;
}

// Does `m` do the task at all -- here, or once downloaded? A quantized
// variant is its source's to pick (Favor), never named apart.
bool
does_task(const models::ModelEntry& m, std::string_view task)
{
  using C = models::Capability;
  if (!m.quantize_from.empty()) {
    return false;
  }
  const Json vp = jget(m.engine, "vpipe", Json::object());
  if (task == "picture") return m.has(C::TextToImage);
  if (task == "edit") return m.has(C::ImageEdit);
  // From words: a text-to-video model's, or Ref2VA's prompt-only request.
  if (task == "shot") {
    return m.has(C::TextToVideo) ||
           (m.has(C::ReferenceToVideo) &&
            jget(jget(vp, "references", Json::object()), "prompt_only",
                 false));
  }
  if (task == "shot_references") return m.has(C::ReferenceToVideo);
  if (task == "music") return m.has(C::TextToAudio);
  if (task == "voiceover") return m.has(C::TextToSpeech);
  if (task == "transcribe") return m.has(C::Transcribe);
  if (task == "separate") return m.has(C::AudioSeparate);
  if (task == "upscale") {
    return m.has(C::UpscaleVideo) || m.has(C::UpscaleImage);
  }
  return false;
}

// A name as matched: lower case, letters and digits alone -- "FLUX.3
// Image Klein", "flux3-klein" and "flux 3" meet in "flux3".
std::string
squeezed(std::string_view s)
{
  std::string out;
  for (const char ch : s) {
    const auto u = static_cast<unsigned char>(ch);
    if (std::isalnum(u)) {
      out.push_back(static_cast<char>(std::tolower(u)));
    }
  }
  return out;
}

// A catalog note's first sentence, short: what the model is.
std::string
about(std::string_view notes)
{
  std::string_view s = notes;
  if (const auto dot = s.find(". "); dot != std::string_view::npos) {
    s = s.substr(0, dot + 1);
  }
  if (s.size() <= 200) {
    return std::string(s);
  }
  return std::string(utf8_prefix(s, 200)) + "...";
}

// A POLYGON's points (canvas pixels) as media/markup.h keeps them: its box
// their bounds, each point in the box's units. `least`: three closed, two
// open.
struct PolygonPoints {
  std::array<double, 4> box{};
  Json                  units = Json::array();
  std::size_t           n = 0;
};

Result<PolygonPoints>
polygon_points(const Json& given, std::size_t least)
{
  std::vector<std::array<double, 2>> pts;
  for (const auto& p : given.is_array() ? given : Json::array()) {
    if (p.is_array() && p.size() == 2 && p[0].is_number() &&
        p[1].is_number()) {
      pts.push_back({p[0].get<double>(), p[1].get<double>()});
    }
  }
  if (pts.size() < least) {
    return make_error(Code::InvalidArgument, std::format(
        "a polygon is {} points or more: points [[x, y], ...] in canvas "
        "pixels",
        least == 2 ? "two" : "three"));
  }
  double x0 = pts[0][0], y0 = pts[0][1], x1 = x0, y1 = y0;
  for (const auto& p : pts) {
    x0 = std::min(x0, p[0]);
    x1 = std::max(x1, p[0]);
    y0 = std::min(y0, p[1]);
    y1 = std::max(y1, p[1]);
  }
  PolygonPoints out;
  for (const auto& p : pts) {
    out.units.push_back({x1 > x0 ? (p[0] - x0) / (x1 - x0) : 0.5,
                         y1 > y0 ? (p[1] - y0) / (y1 - y0) : 0.5});
  }
  out.box = {x0, y0, std::max(1.0, x1 - x0), std::max(1.0, y1 - y0)};
  out.n = pts.size();
  return out;
}

// A path's SMOOTH points (media/markup.h's "smooth"): every one with
// `smooth`, but the `corners` named (from 1) -- or none; null when the
// arguments say neither.
Json
smooth_arg(const Json& args, std::size_t n)
{
  if (!args.is_object() ||
      (!args.contains("smooth") && !args.contains("corners"))) {
    return Json();
  }
  const bool on = flag(args, "smooth", true);
  Json out = Json::array();
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(on);
  }
  for (const auto& c : jget(args, "corners", Json::array())) {
    if (c.is_number_integer() && c.get<std::int64_t>() >= 1 &&
        c.get<std::size_t>() <= n) {
      out[c.get<std::size_t>() - 1] = false;
    }
  }
  return out;
}

// What `text` puts in double quotes -- straight or curly -- with a letter
// in it: words an enhancer would have WRITTEN in a picture ("a sign that
// reads \"OPEN\"").
std::vector<std::string>
quoted_spans(std::string_view text)
{
  std::vector<std::string> out;
  std::size_t i = 0;
  while (i < text.size()) {
    std::string_view close;
    std::size_t from = 0;
    if (text[i] == '"') {
      close = "\"";
      from = i + 1;
    } else if (text.substr(i, 3) == "\u201c") {
      close = "\u201d";
      from = i + 3;
    } else {
      ++i;
      continue;
    }
    const auto end = text.find(close, from);
    if (end == std::string_view::npos) {
      break;
    }
    const std::string_view in = text.substr(from, end - from);
    if (!in.empty() && in.size() <= 120 &&
        std::ranges::any_of(in, [](char c) {
          return std::isalpha(static_cast<unsigned char>(c)) != 0 ||
                 static_cast<unsigned char>(c) >= 0x80;
        })) {
      out.emplace_back(in);
    }
    i = end + close.size();
  }
  return out;
}

// The words of `text` that carry its meaning (four letters or more), in
// lower case: whether a style given is still in what was written.
std::vector<std::string>
key_words(std::string_view text)
{
  std::vector<std::string> out;
  std::string w;
  static const std::set<std::string> filler = {
      "with", "from", "into", "over", "that", "this", "very", "some",
      "style", "look"};
  auto flush = [&] {
    if (w.size() >= 4 && !filler.contains(w) &&
        std::ranges::find(out, w) == out.end()) {
      out.push_back(w);
    }
    w.clear();
  };
  for (const char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      w += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    } else {
      flush();
    }
  }
  flush();
  return out;
}

// An asset of the project by the id an argument gives, of `kind` (a
// picture by default).
Result<AssetId>
asset_of(project::Project& p, const std::string& t, const char* key,
         AssetKind kind = AssetKind::Image)
{
  const auto id = AssetId::parse(t);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      std::format("{}: '{}' is not an asset id", key, t));
  }
  VALTZ_ASSIGN(project::Asset a, p.asset(*id));
  if (a.kind != kind) {
    return make_error(Code::InvalidArgument, std::format(
        "{}: \"{}\" is not a {}", key, a.name,
        kind == AssetKind::Image ? "picture" : "clip"));
  }
  return *id;
}

// The one an argument names, if it names one.
Result<std::optional<AssetId>>
maybe_asset(project::Project& p, const Json& args, const char* key,
            AssetKind kind = AssetKind::Image)
{
  const std::string t = text_arg(args, key);
  if (t.empty()) {
    return std::optional<AssetId>();
  }
  VALTZ_ASSIGN(AssetId id, asset_of(p, t, key, kind));
  return std::optional<AssetId>(id);
}

// The pictures a list argument names (a lone id too), in order.
Result<std::vector<AssetId>>
pictures_of(project::Project& p, const Json& args, const char* key)
{
  Json given = jget(args, key, Json::array());
  if (given.is_string()) {
    given = Json::array({given});
  }
  std::vector<AssetId> out;
  for (const auto& g : given.is_array() ? given : Json::array()) {
    if (g.is_string()) {
      VALTZ_ASSIGN(AssetId id, asset_of(p, g.get<std::string>(), key));
      out.push_back(id);
    }
  }
  return out;
}

// The agent's names for an edit's pictures -- <Picture 1> the first of
// `n` -- as the row's tags a request binds (assist::ref_tag).
std::vector<std::pair<std::string, std::string>>
picture_tags(std::size_t n)
{
  std::vector<std::pair<std::string, std::string>> out;
  for (std::size_t i = 0; i < n; ++i) {
    out.emplace_back(std::format("<Picture {}>", i + 1),
                     assist::ref_tag(assist::RefKind::Image,
                                     static_cast<int>(i)));
  }
  return out;
}

// A text box's words as they lie in it -- or a shape's: whether all
// show, the height they need, the widest line (to centre a box on its
// words).
void
tell_fit(Json& out, const Json& object)
{
  const media::TextFit f = media::text_fit(object);
  const bool shape = jget<std::string>(object, "kind", "") != "text";
  out["lines"] = f.lines;
  out["text_width"] = std::lround(f.widest);
  if (shape) {
    out["size"] = jget(jget(object, "font", Json::object()), "size", 0.0);
  }
  if (!f.fits) {
    out["fits"] = false;
    if (f.needed_height > 0) {
      out["needs_height"] = std::lround(f.needed_height);
    }
    out["note"] = shape ? "Not every word shows: make the shape bigger, "
                          "or the words smaller (size)."
                        : "Not every word shows: make the box taller (or "
                          "wider), or the text smaller.";
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

// A sound's SHAPE: its level, faded in and out; its pitch and its
// equalizer, the same throughout; and the spans it is held LOWER -- the
// music under a voice-over (set_layer_sound's `duck_under`) -- each a
// share of its level, eased down and back up over kDuckRamp. Laid as
// keys at the layer's own frames (shape_keys) and read back from them
// (shape_of), so a trim lays them again over its new length.
struct SoundDip {
  double from = 0;   // its own seconds
  double to = 0;
  double level = 1;  // a share of its volume
};
struct SoundShape {
  double volume = 1;
  double fade_in = 0;
  double fade_out = 0;
  double pitch = 0;
  std::array<double, media::kEqBands> eq{};
  std::vector<SoundDip> dips;  // by time, apart
};
constexpr double kDuckRamp = 0.4;

media::KeyedSound
shape_keys(const SoundShape& sh, double seconds, Rational rate)
{
  const double fps = rate.to_double();
  const double v = std::clamp(sh.volume, 0.0, 2.0);
  const bool timed = std::isfinite(seconds) && seconds > 0;
  const double half = timed ? seconds / 2 : 0;
  const double fi = timed ? std::clamp(sh.fade_in, 0.0, half) : 0;
  const double fo = timed ? std::clamp(sh.fade_out, 0.0, half) : 0;
  media::KeyedSound k;
  k.rate = rate;
  auto key = [&](double at, double gain) {
    const auto f = static_cast<std::int64_t>(std::llround(at * fps));
    if (!k.keys.empty() && k.keys.back().frame >= f) {
      k.keys.back().value.volume = gain;
      return;
    }
    k.keys.push_back({f, media::Sound{gain, sh.pitch, sh.eq}});
  };
  key(0, fi > 0 ? 0.0 : v);
  if (fi > 0) {
    key(fi, v);
  }
  const double last = timed ? seconds - fo : media::kForever;
  for (const SoundDip& d : sh.dips) {
    const double from = std::max(d.from, fi);
    const double to = std::min(d.to, last);
    if (to - from < 0.1 || v <= 0) {
      continue;
    }
    const double r = std::min(kDuckRamp, (to - from) / 3);
    const double low = v * std::clamp(d.level, 0.0, 1.0);
    key(from, v);
    key(from + r, low);
    key(to - r, low);
    key(to, v);
  }
  if (fo > 0) {
    key(seconds - fo, v);
    key(seconds, 0);
  }
  return k;
}

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
  s.pitch = k.keys.front().value.pitch;
  s.eq = k.keys.front().value.eq;
  const auto& ks = k.keys;
  const auto at = [&](std::size_t i) {
    return static_cast<double>(ks[i].frame) / fps;
  };
  if (ks.size() >= 2 && ks.front().value.volume == 0 && top > 0) {
    s.fade_in = at(1) - at(0);
  }
  if (ks.size() >= 2 && ks.back().value.volume == 0 && top > 0) {
    s.fade_out = at(ks.size() - 1) - at(ks.size() - 2);
  }
  // A run of keys below the top between the fades: a dip, from the key
  // before it to the key after.
  const std::size_t lo = s.fade_in > 0 ? 1 : 0;
  const std::size_t hi = s.fade_out > 0 ? ks.size() - 2 : ks.size() - 1;
  const auto under = [&](std::size_t i) {
    return ks[i].value.volume < top - 1e-6;
  };
  for (std::size_t i = lo + 1; i < hi; ++i) {
    if (!under(i)) {
      continue;
    }
    std::size_t j = i;
    double low = ks[i].value.volume;
    while (j + 1 < hi && under(j + 1)) {
      low = std::min(low, ks[++j].value.volume);
    }
    s.dips.push_back({at(i - 1), at(j + 1), top > 0 ? low / top : 0});
    i = j;
  }
  return s;
}

// Keys a shape lays exactly -- set_layer_sound's, a landing's -- not ones
// keyed by hand (key_layer), which a trim leaves as they are.
bool
is_shape(const media::KeyedSound& k, double seconds, Rational rate)
{
  const media::KeyedSound again =
      shape_keys(shape_of(k, rate.to_double()), seconds, rate);
  if (again.keys.size() != k.keys.size()) {
    return false;
  }
  for (std::size_t i = 0; i < k.keys.size(); ++i) {
    const auto& a = again.keys[i];
    const auto& b = k.keys[i];
    if (std::abs(a.frame - b.frame) > 1 ||
        std::abs(a.value.volume - b.value.volume) > 1e-3 ||
        a.value.pitch != b.value.pitch || a.value.eq != b.value.eq) {
      return false;
    }
  }
  return true;
}

// ---- a layer's look, by name ------------------------------------------------

// Adjust's sliders as the tools name them, and their ranges.
constexpr std::pair<const char*, double> kLookSliders[] = {
    {"exposure", 3},   {"contrast", 1},   {"highlights", 1},
    {"shadows", 1},    {"vibrance", 1},   {"saturation", 1},
    {"temperature", 1}, {"tint", 1}};

// Anything of a look a call or a key gives.
bool
says_look(const Json& o)
{
  if (!o.is_object()) {
    return false;
  }
  for (const auto& [k, most] : kLookSliders) {
    if (o.contains(k)) {
      return true;
    }
  }
  return o.contains("opacity") || o.contains("mix") || o.contains("grade");
}

// A hue by its name -- the mixer's seven, and a few more a person says --
// or in degrees.
std::optional<double>
hue_named(const Json& v)
{
  if (v.is_number()) {
    return std::fmod(std::fmod(v.get<double>(), 360.0) + 360.0, 360.0);
  }
  if (!v.is_string()) {
    return std::nullopt;
  }
  std::string n = v.get<std::string>();
  std::ranges::transform(n, n.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  for (const auto& b : media::kMixHueBands) {
    if (n == b.key) {
      return b.degrees;
    }
  }
  static const std::pair<const char*, double> more[] = {
      {"cyan", 180}, {"teal", 180}, {"sky", 210}, {"purple", 280},
      {"violet", 270}, {"pink", 330}, {"skin", 25}, {"orange-red", 15}};
  for (const auto& [k, deg] : more) {
    if (n == k) {
      return deg;
    }
  }
  try {
    return std::fmod(std::fmod(std::stod(n), 360.0) + 360.0, 360.0);
  } catch (...) {
    return std::nullopt;
  }
}

// A curve of the colour mixer from {"<hue>": value, ...}: a point at each
// hue named, and -- so only those move -- a point at 0 on each of the
// seven hues that is not within 25 degrees of one ("all": one value
// everywhere; "none": the curve at rest).
Result<media::MixCurve>
mix_curve(const Json& v)
{
  media::MixCurve c;
  if (v.is_string() && (v == "none" || v == "reset")) {
    return c;
  }
  if (!v.is_object()) {
    return make_error(Code::InvalidArgument,
                      "a mixer curve is {\"<hue>\": -1..1, ...}");
  }
  if (v.contains("all")) {
    c.points.push_back({0, std::clamp(number(v, "all", 0), -1.0, 1.0)});
    return c;
  }
  for (const auto& [k, val] : v.items()) {
    const auto h = hue_named(Json(k));
    if (!h || !val.is_number()) {
      return make_error(Code::InvalidArgument, std::format(
          "'{}' is not a hue: red, orange, yellow, green, aqua, blue, "
          "magenta, or degrees",
          k));
    }
    c.points.push_back({*h, std::clamp(val.get<double>(), -1.0, 1.0)});
  }
  for (const auto& b : media::kMixHueBands) {
    const bool near = std::ranges::any_of(c.points, [&](const auto& p) {
      const double d = std::abs(p.hue - b.degrees);
      return std::min(d, 360 - d) < 25;
    });
    if (!near) {
      c.points.push_back({b.degrees, 0});
    }
  }
  std::ranges::sort(c.points, {}, &media::MixPoint::hue);
  return c;
}

// A tint of the grade: {"hue": <hue>, "saturation": 0..1}, or a hue alone
// (a quarter's strength); "none" none.
Result<media::GradeTint>
grade_tint(const Json& v)
{
  media::GradeTint t;
  if (v.is_string() && (v == "none" || v == "reset")) {
    return t;
  }
  const Json hue = v.is_object() ? jget(v, "hue", Json()) : v;
  const auto h = hue_named(hue);
  if (!h) {
    return make_error(Code::InvalidArgument,
                      "a tint is {\"hue\": a hue's name or degrees, "
                      "\"saturation\": 0..1}");
  }
  t.hue = *h;
  t.saturation =
      std::clamp(v.is_object() ? number(v, "saturation", 0.25) : 0.25, 0.0,
                 1.0);
  return t;
}

// What `o` says of a look laid over `a`: Adjust's sliders, the opacity,
// the colour mixer's curves, the colour grade -- what it leaves out kept.
Result<media::Adjustments>
look_over(const Json& o, media::Adjustments a)
{
  double* slot[] = {&a.exposure,  &a.contrast,   &a.highlights,
                    &a.shadows,   &a.vibrance,   &a.saturation,
                    &a.temperature, &a.tint};
  for (std::size_t i = 0; i < std::size(kLookSliders); ++i) {
    const auto& [k, most] = kLookSliders[i];
    if (o.contains(k)) {
      *slot[i] = std::clamp(number(o, k, 0), -most, most);
    }
  }
  if (o.contains("opacity")) {
    a.alpha = std::clamp(number(o, "opacity", 1), 0.0, 1.0);
  }
  if (const Json mix = jget(o, "mix", Json()); !mix.is_null()) {
    if (mix.is_string() && (mix == "none" || mix == "reset")) {
      a.mix = {};
    } else if (!mix.is_object()) {
      return make_error(Code::InvalidArgument,
                        "mix is {\"hue\" | \"saturation\" | \"luminance\": "
                        "{\"<hue>\": -1..1}}");
    } else {
      for (const auto& [k, v] : mix.items()) {
        media::MixCurve* c = k == "hue"          ? &a.mix.hue
                             : k == "saturation" ? &a.mix.saturation
                             : k == "luminance"  ? &a.mix.luminance
                                                 : nullptr;
        if (!c) {
          return make_error(Code::InvalidArgument, std::format(
              "the mixer has hue, saturation and luminance, not '{}'", k));
        }
        VALTZ_ASSIGN(*c, mix_curve(v));
      }
    }
  }
  if (const Json g = jget(o, "grade", Json()); !g.is_null()) {
    if (g.is_string() && (g == "none" || g == "reset")) {
      a.grade = {};
    } else if (!g.is_object()) {
      return make_error(Code::InvalidArgument,
                        "grade is {\"shadows\" | \"midtones\" | "
                        "\"highlights\": a tint, \"balance\", \"width\", "
                        "\"blend\"}");
    } else {
      for (const auto& [k, t] :
           {std::pair{"shadows", &a.grade.shadows},
            std::pair{"midtones", &a.grade.midtones},
            std::pair{"highlights", &a.grade.highlights}}) {
        if (g.contains(k)) {
          VALTZ_ASSIGN(*t, grade_tint(g[k]));
        }
      }
      if (g.contains("balance")) {
        a.grade.balance = std::clamp(number(g, "balance", 0), -1.0, 1.0);
      }
      if (g.contains("width")) {
        a.grade.width = std::clamp(number(g, "width", 0.5), 0.0, 1.0);
      }
      if (g.contains("blend")) {
        a.grade.blend = std::clamp(number(g, "blend", 0.5), 0.0, 1.0);
      }
    }
  }
  return a;
}

// A look told back in the tools' words: the sliders not at rest, the
// opacity, the mixer's points by hue, the grade's tints.
Json
look_said(const media::Adjustments& a)
{
  Json out = Json::object();
  const double val[] = {a.exposure,  a.contrast,   a.highlights,
                        a.shadows,   a.vibrance,   a.saturation,
                        a.temperature, a.tint};
  for (std::size_t i = 0; i < std::size(kLookSliders); ++i) {
    if (val[i] != 0) {
      out[kLookSliders[i].first] = std::round(val[i] * 100) / 100;
    }
  }
  if (a.alpha != 1) {
    out["opacity"] = std::round(a.alpha * 100) / 100;
  }
  for (const auto& [k, c] : {std::pair{"hue", &a.mix.hue},
                             std::pair{"saturation", &a.mix.saturation},
                             std::pair{"luminance", &a.mix.luminance}}) {
    Json pts = Json::object();
    for (const auto& p : c->points) {
      if (p.value != 0) {
        pts[std::format("{:g}", std::round(p.hue))] =
            std::round(p.value * 100) / 100;
      }
    }
    if (!pts.empty()) {
      out["mix"][k] = std::move(pts);
    }
  }
  for (const auto& [k, t] : {std::pair{"shadows", &a.grade.shadows},
                             std::pair{"midtones", &a.grade.midtones},
                             std::pair{"highlights", &a.grade.highlights}}) {
    if (t->saturation > 0) {
      out["grade"][k] = {{"hue", std::round(t->hue)},
                         {"saturation",
                          std::round(t->saturation * 100) / 100}};
    }
  }
  return out;
}

// The equalizer from {"<frequency>": dB, ...} -- "1000", "1k", "4 kHz", or
// "bass" (the three lowest bands), "mid", "treble" (the three highest) --
// laid over `eq`; "flat" all at 0.
Result<std::array<double, media::kEqBands>>
eq_over(const Json& v, std::array<double, media::kEqBands> eq)
{
  if (v.is_string() && (v == "flat" || v == "none")) {
    return std::array<double, media::kEqBands>{};
  }
  if (v.is_array() && v.size() == media::kEqBands) {
    for (std::size_t i = 0; i < media::kEqBands; ++i) {
      eq[i] = std::clamp(v[i].is_number() ? v[i].get<double>() : 0.0,
                         -media::kMaxEqGain, media::kMaxEqGain);
    }
    return eq;
  }
  if (!v.is_object()) {
    return make_error(Code::InvalidArgument,
                      "eq is {\"<frequency>\": dB, ...}: \"125\", \"1k\", "
                      "\"8k\", or \"bass\", \"mid\", \"treble\"; -12..12");
  }
  for (const auto& [k, val] : v.items()) {
    const double db = std::clamp(val.is_number() ? val.get<double>() : 0.0,
                                 -media::kMaxEqGain, media::kMaxEqGain);
    std::string n = k;
    std::ranges::transform(n, n.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    std::vector<int> bands;
    if (n == "bass" || n == "low" || n == "lows") {
      bands = {0, 1, 2};
    } else if (n == "mid" || n == "mids" || n == "middle") {
      bands = {4, 5, 6};
    } else if (n == "treble" || n == "high" || n == "highs") {
      bands = {7, 8, 9};
    } else {
      double hz = 0;
      try {
        std::size_t used = 0;
        hz = std::stod(n, &used);
        if (n.find('k', used) != std::string::npos) {
          hz *= 1000;
        }
      } catch (...) {
        hz = 0;
      }
      if (hz <= 0) {
        return make_error(Code::InvalidArgument, std::format(
            "'{}' is not a frequency: \"125\", \"1k\", \"bass\", "
            "\"treble\"",
            k));
      }
      int best = 0;
      for (int i = 1; i < media::kEqBands; ++i) {
        if (std::abs(std::log2(hz / media::kEqFrequencies[i])) <
            std::abs(std::log2(hz / media::kEqFrequencies[best]))) {
          best = i;
        }
      }
      bands = {best};
    }
    for (const int b : bands) {
      eq[static_cast<std::size_t>(b)] = db;
    }
  }
  return eq;
}

// The equalizer told back: each band not flat, by its frequency.
Json
eq_said(const std::array<double, media::kEqBands>& eq)
{
  Json out = Json::object();
  for (int i = 0; i < media::kEqBands; ++i) {
    if (eq[static_cast<std::size_t>(i)] != 0) {
      const double f = media::kEqFrequencies[static_cast<std::size_t>(i)];
      out[f >= 1000 ? std::format("{:g}k", f / 1000)
                    : std::format("{:g}", f)] =
          std::round(eq[static_cast<std::size_t>(i)] * 10) / 10;
    }
  }
  return out;
}

// Layer ids from an argument: a list, or one; "0" the bottom layer.
std::vector<std::string>
layer_ids(const Json& args, const char* key)
{
  std::vector<std::string> out;
  Json v = args.is_object() && args.contains(key) ? args[key] : Json();
  if (v.is_string() || v.is_number_integer()) {
    v = Json::array({v});
  }
  for (const auto& x : v.is_array() ? v : Json::array()) {
    std::string id = x.is_string() ? x.get<std::string>()
                     : x.is_number_integer()
                         ? std::to_string(x.get<std::int64_t>())
                         : std::string();
    out.push_back(id == "0" ? std::string() : id);
  }
  return out;
}

const project::LayerFolder*
folder_of(const project::Asset& a, const std::string& id)
{
  for (const auto& f : a.layer_folders) {
    if (f.id == id) {
      return &f;
    }
  }
  return nullptr;
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

namespace {

// A markup listed in full up to this many objects (get_composition);
// past it, the ones on top this many, and find_objects for the rest.
constexpr std::size_t kListedObjects = 80;
constexpr std::size_t kListedTop = 40;
// A path's points told one by one up to so many, else counted.
constexpr std::size_t kListedPoints = 64;
constexpr std::size_t kFoundMost = 200;

// How many objects of each kind.
Json
kinds_of(const Json& objects)
{
  std::map<std::string, std::size_t> n;
  for (const auto& o : objects) {
    ++n[jget<std::string>(o, "kind", "")];
  }
  Json out = Json::object();
  for (const auto& [k, c] : n) {
    out[k] = c;
  }
  return out;
}

// A markup object as the agent is told it (get_composition,
// find_objects): its id, kind and box; a line's ends; a text's words (a
// long one's head), font and colour -- a shape's words too; a shape's
// fill and outline; a gradient, a path's points, its effects, its turn;
// its connectors, and the shapes a line's or a path's ends are joined
// to.
Json
described(const Json& o)
{
  Json x = {{"id", jget<std::string>(o, "id", "")},
            {"kind", jget<std::string>(o, "kind", "")}};
  const double x0 = jget(o, "x0", 0.0), y0 = jget(o, "y0", 0.0);
  const double x1 = jget(o, "x1", 0.0), y1 = jget(o, "y1", 0.0);
  x["x"] = std::lround(std::min(x0, x1));
  x["y"] = std::lround(std::min(y0, y1));
  x["width"] = std::lround(std::abs(x1 - x0));
  x["height"] = std::lround(std::abs(y1 - y0));
  // A line's ends: its box says nothing of which way it runs.
  if (x["kind"] == "line") {
    x["from"] = {std::lround(x0), std::lround(y0)};
    x["to"] = {std::lround(x1), std::lround(y1)};
  }
  const bool words = x["kind"] == "text" ||
                     (holds_words(o) && o.contains("text"));
  if (words) {
    // A long text's head only: a flowed page would tell the whole
    // text back.
    const auto said = jget<std::string>(o, "text", "");
    x["text"] = said.size() <= 160
                    ? said
                    : std::string(utf8_prefix(said, 120)) +
                          std::string(kEllipsis);
    const char* plain = x["kind"] == "text" ? "left" : "center";
    if (auto al = jget<std::string>(o, "align", plain); al != plain) {
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
    if (x["kind"] == "text") {
      x["color"] = hex(jget(o, "stroke", Json()));
    } else {
      x["text_color"] = o.contains("text_color")
                            ? hex(o["text_color"])
                            : std::string("#000000");
    }
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
    // Its points where they lie (before its turn), as add_shape and
    // update_object take them: one changed, the rest sent back as told.
    const auto pts = points_of(o);
    if (pts.size() <= kListedPoints) {
      Json at = Json::array();
      for (const auto& p : pts) {
        at.push_back({std::lround(p[0]), std::lround(p[1])});
      }
      x["points"] = std::move(at);
    } else {
      x["point_count"] = pts.size();
    }
    // Smooth, with the points kept corners (from 1) where some are.
    const Json sm = jget(o, "smooth", Json::array());
    Json corners = Json::array();
    bool any = false;
    for (std::size_t i = 0; i < pts.size(); ++i) {
      const bool on = i < sm.size() && sm[i].is_boolean() &&
                      sm[i].get<bool>();
      any = any || on;
      if (!on) {
        corners.push_back(i + 1);
      }
    }
    if (any) {
      x["smooth"] = true;
      if (!corners.empty()) {
        x["corners"] = std::move(corners);
      }
    }
    if (jget(o, "open", false)) {
      x["open"] = true;
    }
  }
  // Its connectors where they are, and what its ends are joined to.
  if (const auto cs = media::connector_points(o); !cs.empty()) {
    Json at = Json::object();
    for (const auto& c : cs) {
      at[c.id] = {std::lround(c.x), std::lround(c.y)};
    }
    x["connectors"] = std::move(at);
  }
  if (o.contains("anchors")) {
    const auto n = static_cast<int>(points_of(o).size());
    const bool ends = x["kind"] == "line" || jget(o, "open", false);
    Json others = Json::array();
    for (const auto& a : o["anchors"]) {
      const int k = jget(a, "point", -1);
      const auto to = jget<std::string>(a, "to", "");
      const auto c = jget<std::string>(a, "connector", "");
      const char* end = !ends ? nullptr
                        : k == 0 ? "from"
                        : k == n - 1 ? "to"
                                     : nullptr;
      if (end) {
        x[std::string(end) + "_object"] = to;
        x[std::string(end) + "_connector"] = c;
      } else {
        others.push_back({{"point", k + 1}, {"object", to},
                          {"connector", c}});
      }
    }
    if (!others.empty()) {
      x["joined_points"] = std::move(others);
    }
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
    for (const char* k : {"dash", "arrows"}) {
      if (o.contains(k)) {
        x[k] = o[k];
      }
    }
  }
  // Its turn and flips, where it has them.
  for (const char* k : {"rotate", "flip_x", "flip_y"}) {
    if (o.contains(k)) {
      x[k] = o[k];
    }
  }
  return x;
}

}  // namespace

// ---- Favor and its options, by name (DESIGN §9b) ---------------------------

namespace {

// What Favor's Custom options are called: Custom's label, the words a
// person uses for each ('|' between them), and what it does in a line.
// A switch that brings an ADAPTER -- Pika H3 Flex, HyperFlow, TaoMate --
// is asked for as "the Pika LoRA" as often as by its name, so those
// words are the switch's: in Valtz an adapter runs WITH its own steps and
// schedule, never laid on as a plain LoRA (Custom's LoRA list would run
// it on the wrong grid).
struct OptionWords {
  const char* key;
  const char* label;
  const char* words;
  const char* what;
};
constexpr OptionWords kOptionWords[] = {
    {"steps", "Steps", "steps|step count|denoising steps|sampling steps",
     "denoising steps: more is slower and finer"},
    {"pika_flex", "Pika H3 Flex",
     "pika|pika flex|pika h3|pika h3 flex|pika labs|pika lora|pika "
     "adapter|pika flex lora|pika distill|pika distillation",
     "Pika Labs' six-step post-training of MiniMax H3: its adapter AND its "
     "own sampling (6 steps, shifts 12 / 3) together -- the Turbo LoRA, "
     "HyperFlow, TaoMate and MotionCache off"},
    {"hyperflow", "HyperFlow",
     "hyperflow|hyper flow|hyperflow lora|hyperflow adapter|video rebirth|"
     "flow map|flowmap",
     "Video Rebirth's 8-step flow-map adapter, on its own grid of steps -- "
     "the Turbo LoRA off; each step ~9% slower"},
    {"taomate", "TaoMate",
     "taomate|tao mate|taomate lora|taomate adapter|taolive|streaming",
     "TaoLive's 3-step streaming method: the base model writes the "
     "soundtrack, its adapter the video in chunks (words to a clip only)"},
    {"taomate_lora", "TaoMate as a LoRA only",
     "taomate as lora|taomate lora only|taomate as plain lora|"
     "lora only",
     "TaoMate's adapter run as a plain 3-step LoRA: no streaming, and a "
     "clip may open on a picture"},
    {"vdn", "VDN branch",
     "vdn|vdn branch|linear attention|linear branch|vdn attention",
     "the VDN linear-attention branch, for long and large clips (in "
     "Sol's place)"},
    {"vdn_range", "VDN softmax range", "vdn range|vdn reach|softmax range",
     "with VDN: latent chunks either side kept exact"},
    {"sol_attn", "Sol-Attn",
     "sol|sol attn|sol attention|sparse attention|block sparse attention",
     "attends only the key blocks that matter: faster, a little less "
     "exact"},
    {"sol_threshold", "Sol flavour", "sol flavour|sol flavor|sol mode",
     "Sol's flavour: diag (the default) or exact"},
    {"sol_tau", "Sol threshold", "sol tau|sol threshold|tau",
     "Sol's threshold: higher keeps fewer blocks"},
    {"sage_attn", "SageAttention",
     "sage|sage attention|sageattention|sage attn|sageattn",
     "int8 QK in attention (an M5's matrix cores)"},
    {"sage_pv8", "int8 P·V",
     "int8 pv|pv8|sage pv8|sage pv|int8 p v|sage int8 pv",
     "SageAttention's P·V in int8 too"},
    {"sage_pv8_all_rows", "int8 P·V on every row",
     "pv8 all rows|all rows|prompt and sound rows",
     "int8 P·V on the prompt's and soundtrack's rows as well"},
    {"i8_gemm", "int8 GEMM", "int8|int8 gemm|i8 gemm|i8|int8 matmul",
     "int8 block matrix products, about 2x (an M5's matrix cores)"},
    {"i8_gemm_tier", "int8 tier", "int8 tier|i8 tier|i8 gemm tier",
     "which int8: accurate, fast, token (per token), token_plain"},
    {"vae_i8_gemm", "VAE int8", "vae int8|int8 vae|vae i8",
     "int8 convolutions in the VAE"},
    {"vae_i8_gemm_tier", "VAE int8 tier", "vae int8 tier",
     "which VAE int8: accurate or fast"},
    {"ane_ffn", "Neural Engine",
     "ane|ane ffn|neural engine|npu|ane feed forward",
     "the feed-forward on the Neural Engine beside the GPU"},
    {"ane_qkv", "Neural Engine q|k|v", "ane qkv|neural engine qkv",
     "the q|k|v projection on the Neural Engine too"},
    {"ane_int8", "Neural Engine int8", "ane int8|neural engine int8",
     "the Neural Engine's modules in int8: faster, lossy"},
    {"ane_int8_tier", "Neural Engine int8 tier", "ane int8 tier",
     "which ANE int8: token or token_plain"},
    {"motion_cache", "MotionCache",
     "motion cache|motioncache|motion caching",
     "reuses the steps it can predict (16 steps or more)"},
    {"video_shift", "Video shift", "video shift|shift|sigma shift",
     "the video schedule's sigma shift"},
    {"audio_shift", "Audio shift", "audio shift|sound shift",
     "the audio schedule's sigma shift"},
    {"shift", "Shift", "shift|time shift|sigma shift",
     "the noise schedule's time shift"},
    {"guidance_scale", "Guidance", "guidance|cfg|guidance scale|cfg scale",
     "how closely it follows the prompt"},
};

// The words every FEW-STEP LoRA of a model goes by -- the Turbo LoRAs,
// lightx2v's: one of them, the fastest here unless its steps are said.
constexpr const char* kFastWords =
    "turbo|turbo lora|lightx2v|lightx2v lora|distilled|distill lora|"
    "distillation lora|fast lora|few step|few step lora|acceleration lora|"
    "speed lora|lora";

const OptionWords*
option_words(std::string_view key)
{
  for (const auto& w : kOptionWords) {
    if (key == w.key) {
      return &w;
    }
  }
  return nullptr;
}

// A name as it is matched: lower case, letters and digits, single
// spaces, the filler words out -- "the 4-step LoRA" and "4 step lora"
// meet.
std::string
option_norm(std::string_view s)
{
  std::string raw;
  bool space = true;
  for (const char ch : s) {
    const auto u = static_cast<unsigned char>(ch);
    if (u < 0x80 && std::isalnum(u)) {
      raw.push_back(static_cast<char>(std::tolower(u)));
      space = false;
    } else if (!space) {
      raw.push_back(' ');
      space = true;
    }
  }
  static const std::set<std::string_view> filler = {
      "the", "a", "an", "use", "using", "with", "please", "my", "its"};
  std::string out;
  std::size_t at = 0;
  while (at < raw.size()) {
    std::size_t end = raw.find(' ', at);
    end = end == std::string::npos ? raw.size() : end;
    const std::string_view w(raw.data() + at, end - at);
    if (!w.empty() && !filler.contains(w)) {
      out += out.empty() ? "" : " ";
      out += w;
    }
    at = end + 1;
  }
  return out;
}

// `word` inside `name` as whole words: its length, else 0.
std::size_t
whole_words_in(std::string_view name, std::string_view word)
{
  if (word.empty()) {
    return 0;
  }
  for (auto at = name.find(word); at != std::string_view::npos;
       at = name.find(word, at + 1)) {
    const bool left = at == 0 || name[at - 1] == ' ';
    const auto end = at + word.size();
    const bool right = end == name.size() || name[end] == ' ';
    if (left && right) {
      return word.size();
    }
  }
  return 0;
}

// How well `name` (normalized) is said by `words` ('|' between them):
// one said whole beats any it holds; the longest held counts.
std::size_t
name_score(std::string_view name, std::string_view words)
{
  std::size_t best = 0;
  std::size_t at = 0;
  while (at <= words.size()) {
    auto end = words.find('|', at);
    end = end == std::string_view::npos ? words.size() : end;
    const std::string w = option_norm(words.substr(at, end - at));
    if (!w.empty()) {
      if (w == name) {
        best = std::max(best, 10000 + w.size());
      } else {
        best = std::max(best, whole_words_in(name, w));
        // A short name inside a longer word ("flex": "pika flex").
        if (name.size() >= 4 && whole_words_in(w, name)) {
          best = std::max(best, name.size() / 2);
        }
      }
    }
    at = end + 1;
  }
  return best;
}

// The options named in an `options` argument, in order: [name, value]
// -- an object's entries, a list's names (and objects), or one string
// of names split at commas. A bare name's value is null.
std::vector<std::pair<std::string, Json>>
named_options(const Json& v)
{
  std::vector<std::pair<std::string, Json>> out;
  auto names = [&](const std::string& t) {
    std::size_t at = 0;
    while (at <= t.size()) {
      auto end = t.find_first_of(",;", at);
      end = end == std::string::npos ? t.size() : end;
      if (auto n = one_line(t.substr(at, end - at)); !n.empty()) {
        out.emplace_back(std::move(n), Json());
      }
      at = end + 1;
    }
  };
  if (v.is_string()) {
    names(v.get<std::string>());
  } else if (v.is_object()) {
    for (auto it = v.begin(); it != v.end(); ++it) {
      out.emplace_back(it.key(), it.value());
    }
  } else if (v.is_array()) {
    for (const auto& x : v) {
      if (x.is_string()) {
        names(x.get<std::string>());
      } else if (x.is_object()) {
        for (auto it = x.begin(); it != x.end(); ++it) {
          out.emplace_back(it.key(), it.value());
        }
      }
    }
  }
  return out;
}

// A value as on or off: a bool, "on" / "off", "yes" / "no", 1 / 0;
// null (a bare name) on.
std::optional<bool>
on_off(const Json& v)
{
  if (v.is_null()) {
    return true;
  }
  if (v.is_boolean()) {
    return v.get<bool>();
  }
  if (v.is_number()) {
    return v.get<double>() != 0;
  }
  if (v.is_string()) {
    const std::string t = option_norm(v.get<std::string>());
    if (t == "on" || t == "true" || t == "yes" || t == "enabled" ||
        t == "1") {
      return true;
    }
    if (t == "off" || t == "false" || t == "no" || t == "disabled" ||
        t == "0" || t == "none") {
      return false;
    }
  }
  return std::nullopt;
}

// A choice's value as it may be said ("per token" for "token").
std::string
choice_said(const Json& choices, const Json& v)
{
  if (!v.is_string()) {
    return {};
  }
  const std::string t = option_norm(v.get<std::string>());
  static const std::pair<const char*, const char*> kSaid[] = {
      {"token", "per token|per token rotated|token rotated"},
      {"token_plain", "token plain|per token unrotated|unrotated|plain "
                      "token|per token plain"},
      {"accurate", "accurate|groups of 64|default"},
      {"fast", "fast|groups of 512"},
      {"diag", "diag|diagonal"},
      {"exact", "exact|full covariance"},
  };
  std::string best;
  std::size_t score = 0;
  for (const auto& c : choices) {
    if (!c.is_string()) {
      continue;
    }
    const std::string id = c.get<std::string>();
    std::string words = id;
    for (const auto& [k, w] : kSaid) {
      if (id == k) {
        words += std::string("|") + w;
      }
    }
    if (const auto n = name_score(t, words); n >= 10000 && n > score) {
      best = id;
      score = n;
    }
  }
  return best;
}

// The generating tools' FAVOR and OPTIONS arguments.
Json
favor_prop()
{
  return one_of({"fast", "med", "fine"},
                "FAVOR, only when the person asks for speed or quality: "
                "fast (fewest steps, quickest), fine (the most steps, "
                "slowest); left out, med");
}

// A shot's FAVOR or OPTIONS: left out on a timeline that holds clips,
// the clip before's (t_generate_video's made_as_before).
Json
shot_prop(Json p)
{
  p["description"] = jget<std::string>(p, "description", "") +
                     ". Left out on a timeline that holds clips: the clip "
                     "before's, to keep its look";
  return p;
}

Json
options_prop()
{
  return {{"type", "object"},
          {"description",
           "Favor's Custom OPTIONS by name, each to its value -- "
           "{\"Pika H3 Flex\": true}, {\"HyperFlow\": true}, "
           "{\"TaoMate\": true}, {\"4-step LoRA\": true}, {\"steps\": 12}, "
           "{\"SageAttention\": false} -- named loosely, as the person "
           "said them: \"the Pika LoRA\" is the Pika H3 Flex switch (its "
           "adapter WITH its own steps and schedule), \"turbo\" a "
           "few-step LoRA of the model's. model_options lists a model's. "
           "Only what the person asks for: the rest stays Favor's"}};
}

const char*
favor_label(std::string_view preference)
{
  return preference == "speed"     ? "Fast"
         : preference == "quality" ? "Fine"
                                   : "Med";
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
    // A shape's words, its connectors and a line's ends joined to shapes
    // (media/markup.h): add_shape's and update_object's alike.
    const Json words_props = {
        {"text", str("WORDS IN the rectangle, ellipse or closed polygon: "
                     "centred in it, wrapped (\\n a new line); \"\" "
                     "takes them off")},
        {"size", num("the words' size, pixels; by default as big as "
                     "fits, up to 48")},
        {"font", str("their font family; \"Helvetica Neue\" by default")},
        {"bold", boolean("bold")},
        {"italic", boolean("italic")},
        {"text_color", str("their colour, \"#rrggbb[aa]\"; black, or "
                           "white on a dark fill, by default")},
        {"align", one_of({"left", "center", "right", "justified"},
                         "their lines; center by default")},
        {"line_spacing", num("lines apart, 1-3")}};
    const Json join_props = {
        {"connectors", boolean("its CONNECTORS: true adds them -- the "
                               "middle of each side, four round an "
                               "ellipse -- for lines to join; false takes "
                               "them off")},
        {"from_object", str("a line's (an open path's) START JOINED to "
                            "this shape of its layer -- its id: it stays "
                            "on it wherever the shape moves; \"none\" "
                            "lets it go")},
        {"from_connector", str("where on it: \"top\", \"right\", "
                               "\"bottom\", \"left\" or a connector's id "
                               "(\"c2\"); by default the one nearest the "
                               "other end")},
        {"to_object", str("its END joined to this shape (an arrow's "
                          "head, with arrows \"end\")")},
        {"to_connector", str("where on it, as from_connector")}};
    std::vector<Tool> t;
    t.push_back({"get_project", "read", "Look at the project",
                 "The project: its compositions (size, pages), its other "
                 "assets, which composition is active, and the picture "
                 "models that run here (list_models: every task's).",
                 schema(Json::object()), &H::t_get_project});
    {
      std::vector<std::string> names;
      for (const auto& mt : kModelTasks) {
        names.emplace_back(mt.name);
      }
      t.push_back(
          {"list_models", "read", "Look at the models",
           "The MODELS that run here for a task -- each one's id, name, "
           "what it does, Auto's pick marked -- and those not downloaded. "
           "When the person names a model (\"use FLUX.3 for the "
           "pictures\", \"which music models do I have?\"), look here and "
           "give its id as the tool's `model`.",
           schema({{"task", one_of(names,
                                   "picture, edit (a picture changed), "
                                   "shot (generate_video from words or a "
                                   "picture), shot_references (drawing on "
                                   "references, continuing), music, "
                                   "voiceover, transcribe, separate, "
                                   "upscale; omitted: every task")}}),
           &H::t_list_models});
      t.push_back(
          {"model_options", "read", "Look at a model's options",
           "A model's FAVOR -- what Fast, Med and Fine run: steps, "
           "adapters, switches -- and its Custom OPTIONS: Pika H3 Flex, "
           "HyperFlow, TaoMate, VDN, MotionCache, Sol, SageAttention, int8, "
           "steps, its few-step LoRAs -- with what each does and what is "
           "not on this Mac. When the person asks for one by name (\"use "
           "the Pika LoRA\", \"with HyperFlow\", \"30 steps\") and you "
           "are not sure what it is, look here; then give it in the tool's "
           "`options`.",
           schema({{"task", one_of(names, "the task, for Auto's model")},
                   {"model", str("the model's id or name (list_models); "
                                 "omitted: Auto's for the task")}}),
           &H::t_model_options});
    }
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
    t.push_back({"find_objects", "read", "Find objects",
                 "A markup's objects get_composition did not list -- a "
                 "page of many lists the ones on top: those lying in a "
                 "region, of a kind, whose words hold a text, or by id, "
                 "200 at most, bottom first.",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str("a markup layer's id, as "
                                       "get_composition gives it (none: "
                                       "every markup layer)")},
                         {"region", Json{{"type", "object"},
                                         {"description",
                                          "only those whose box meets "
                                          "this one"},
                                         {"properties", box_props}}},
                         {"kind", one_of({"text", "rect", "ellipse",
                                          "polygon", "line", "drawing"},
                                         "only these")},
                         {"text", str("only texts and shapes whose words "
                                      "hold this (any case)")},
                         {"ids", ids("only these objects")},
                         {"limit", integer("at most this many (200)")}}),
                 &H::t_find_objects});
    t.push_back({"view_page", "read", "Look at a page",
                 "A page drawn as it is now, shown to you as a picture "
                 "(when you can see pictures; else told in words).",
                 schema({{"composition", str(kCompDoc)},
                         {"page", integer(kPageDoc)}}),
                 &H::t_view_page});
    t.push_back({"view_asset", "read", "Look at an asset",
                 "An asset of the project as it looks -- a picture "
                 "imported or made, a markup, a composition -- shown to "
                 "you as a picture (when you can see pictures). How to "
                 "see a picture in Assets before you use it.",
                 schema({{"asset", str("the asset's id")}}, {"asset"}),
                 &H::t_view_asset});
    t.push_back({"get_exif", "read", "Read a photo's EXIF",
                 "What a picture's file says of itself, as the camera or "
                 "the editor that saved it wrote it: its EXIF -- camera "
                 "and lens, shutter, aperture, ISO, focal length, when it "
                 "was taken -- TIFF, GPS (where), IPTC, the maker's notes "
                 "and its own fields (size, orientation, colour profile). "
                 "Of an imported picture (JPEG, HEIC, TIFF, PNG, a camera "
                 "RAW) or one made from it; of a composition -- an edited "
                 "copy too -- each layer's picture's.",
                 schema({{"asset", str("the asset's id")}}, {"asset"}),
                 &H::t_get_exif});
    t.push_back({"get_recipe", "read", "Read what made an asset",
                 "What MADE an asset: its model, prompt, seed, size, "
                 "length, Favor and options, what it was made from -- to "
                 "make it again (its seed: the same take) or another take "
                 "of it (another seed, a word changed).",
                 schema({{"asset", str("the asset's id")}}, {"asset"}),
                 &H::t_get_recipe});
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
    t.push_back(
        {"update_composition", "edit", "Change a composition",
         "A composition's CANVAS SIZE (Information's Canvas Size): "
         "`width` x `height` pixels, what it shows kept where it lies "
         "against `anchor` (the centre by default) -- room made for "
         "pictures to be moved into with set_layer_box; or its `name`. "
         "Says where each picture layer lies after.",
         schema({{"composition", str(kCompDoc)},
                 {"width", integer("its new width, pixels")},
                 {"height", integer("its new height, pixels")},
                 {"anchor", one_of({"center", "top-left", "top",
                                    "top-right", "left", "right",
                                    "bottom-left", "bottom",
                                    "bottom-right"},
                                   "what stays put; center by default")},
                 {"name", str("its new name")}}),
         &H::t_update_composition});
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
                      {"layer", str(kDrawOnDoc)},
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
                      {"rotate", num("degrees, + clockwise, about its "
                                     "middle; 0 by default")},
                      {"flip_x", boolean("mirrored left to right")},
                      {"flip_y", boolean("mirrored top to bottom")},
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
         "badge, a divider; filled flat or with a `gradient`. A polygon "
         "with `smooth` is a CURVE through its points (a wave, a cloud, a "
         "leaf, a hill), with `open` a curved or bent LINE (a swoosh, a "
         "path, an outline stroke). A line runs from "
         "(x, y) to (x + width, y + height): a level rule has height 0, "
         "its thickness `line_width`. A DIAGRAM's step is a shape with "
         "its words (`text`), an arrow between two a line JOINED to them "
         "(`from_object`, `to_object`, arrows \"end\"): its ends stay on "
         "them as they move -- its box then any.",
         schema(with(with(with({{"composition", str(kCompDoc)},
                      {"page", integer(kPageDoc)},
                      {"start", num(kStartDoc)},
                      {"duration", num(kDurationDoc)},
                      {"layer", str(kDrawOnDoc)},
                      {"shape", one_of({"rect", "ellipse", "polygon", "line"},
                                       "which shape")},
                      {"points", {{"type", "array"},
                                  {"items", {{"type", "array"},
                                             {"items", {{"type",
                                                         "number"}}}}},
                                  {"description",
                                   "a polygon's corners, [[x, y], ...] in "
                                   "canvas pixels, three or more (two for "
                                   "an open one) -- a star, a badge, an "
                                   "arrow; its box is made from them"}}},
                      {"smooth", boolean("a curve through the points")},
                      {"corners", {{"type", "array"},
                                   {"items", {{"type", "integer"}}},
                                   {"description",
                                    "with smooth: the points (from 1) kept "
                                    "sharp"}}},
                      {"open", boolean("not closed: a line through the "
                                       "points, its colour stroke")},
                      {"fill", str("its inside, \"#rrggbb[aa]\"; none by "
                                   "default")},
                      {"stroke", str("its outline or line colour")},
                      {"line_width", num("the outline's width, pixels; 0 "
                                         "none")},
                      {"dash", one_of({"solid", "dashed", "dotted"},
                                      "its line or outline; solid by "
                                      "default")},
                      {"arrows", one_of({"none", "start", "end", "both"},
                                        "a line's arrowheads: at its end "
                                        "(x1, y1), start, both; none by "
                                        "default")},
                      {"rotate", num("degrees, + clockwise, about its "
                                     "middle; 0 by default")},
                      {"flip_x", boolean("mirrored left to right")},
                      {"flip_y", boolean("mirrored top to bottom")},
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
                     box_props), words_props), join_props),
                {"shape", "x", "y", "width", "height"}),
         &H::t_add_shape});
    t.push_back(
        {"update_object", "edit", "Change a text or shape",
         "A text box or shape changed: only the fields given -- x, y its "
         "place (moved, the rest kept), width, height its size. To move "
         "it, or several together, or onto another page: move_objects. "
         "A shape's words, its connectors, a line's ends joined to "
         "shapes as add_shape's; a joined line given a new place or new "
         "points lets go of the shapes where its ends moved.",
         schema(with(with({{"composition", str(kCompDoc)},
                      {"layer", str(kLayerDoc)},
                      {"object", str("the object's id")},
                      {"text", str("new words: a text box's, or a "
                                   "shape's -- a rectangle's, an "
                                   "ellipse's, a closed polygon's (\"\" "
                                   "takes them off)")},
                      {"points", {{"type", "array"},
                                  {"items", {{"type", "array"},
                                             {"items", {{"type",
                                                         "number"}}}}},
                                  {"description",
                                   "a polygon's new points, [[x, y], ...] "
                                   "canvas pixels: its box made from them"}}},
                      {"smooth", boolean("a polygon's points smooth (a "
                                         "curve), or not (corners)")},
                      {"corners", {{"type", "array"},
                                   {"items", {{"type", "integer"}}},
                                   {"description",
                                    "with smooth: the points kept corners, "
                                    "from 1"}}},
                      {"open", boolean("a polygon open (a line through its "
                                       "points) or closed")},
                      {"size", num("font size, pixels")},
                      {"font", str("font family")},
                      {"bold", boolean("bold")},
                      {"italic", boolean("italic")},
                      {"color", str("text colour (a shape's words' "
                                    "too)")},
                      {"text_color", str("a shape's words' colour")},
                      {"align", one_of({"left", "center", "right",
                                        "justified"}, "its lines")},
                      {"line_spacing", num("lines apart, 1-3")},
                      {"fill", str("a shape's inside")},
                      {"stroke", str("a shape's outline")},
                      {"line_width", num("outline width")},
                      {"dash", one_of({"solid", "dashed", "dotted"},
                                      "a line's or outline's")},
                      {"arrows", one_of({"none", "start", "end", "both"},
                                        "a line's arrowheads")},
                      {"gradient", {{"type", "array"},
                                    {"items", {{"type", "string"}}},
                                    {"description",
                                     "a gradient in place of the fill (a "
                                     "text's colour): its colours, two or "
                                     "more, \"#rrggbb[aa]\"; [] takes it "
                                     "off"}}},
                      {"gradient_type", one_of({"linear", "radial"},
                                               "linear by default")},
                      {"rotate", num("degrees, + clockwise, about its "
                                     "middle; 0 by default")},
                      {"flip_x", boolean("mirrored left to right")},
                      {"flip_y", boolean("mirrored top to bottom")},
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
                     box_props), join_props),
                {"layer", "object"}),
         &H::t_update_object});
    t.push_back(
        {"move_objects", "edit", "Move texts and shapes",
         "Texts and shapes MOVED -- never removed and added again: each "
         "keeps its id, size, words and look. By `dx`, `dy`, or to `x`, "
         "`y` (where the top left of them all goes); several move as one "
         "(a title and the shape behind it). With `page`: onto that page "
         "of a still, where they lie unless moved too. Lines joined to a "
         "shape moved follow it; a joined line moved without its shapes "
         "lets go of them. Strokes from "
         "draw_strokes are pixels of the layer it named: move that layer "
         "with set_layer_box.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str(kLayerDoc)},
                 {"objects", ids("the objects' ids, as get_composition "
                                 "lists them")},
                 {"dx", num("pixels right (negative: left)")},
                 {"dy", num("pixels down (negative: up)")},
                 {"x", num("else where their left edge goes, canvas "
                           "pixels")},
                 {"y", num("where their top edge goes")},
                 {"page", integer("a still's page to move them onto, "
                                  "from 1")}},
                {"layer", "objects"}),
         &H::t_move_objects});
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
        {"arrange_objects", "edit", "Arrange texts and shapes",
         "Texts and shapes of one markup layer moved up or down its order "
         "(drawn bottom first), lined up by their edges or middles, or "
         "three or more spaced evenly; one alone aligns to the canvas.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str(kLayerDoc)},
                 {"objects", ids("the objects' ids")},
                 {"how", one_of({"to-top", "up", "down", "to-bottom",
                                 "left", "center", "right", "top",
                                 "middle", "bottom", "distribute-h",
                                 "distribute-v"},
                                "their order; lined up; spaced across / "
                                "down")}},
                {"layer", "objects", "how"}),
         &H::t_arrange_objects});
    t.push_back(
        {"draw_strokes", "edit", "Draw with the brush",
         "BRUSH STROKES on a page -- a sketch's lines, hatching, a "
         "hand-drawn mark -- round, `size` wide, soft by `softness`; "
         "`erase` takes paint away. Over what was drawn before them; "
         "pixels, not objects (never changed after). On a timeline: from "
         "`start` for `duration` seconds.",
         schema({{"composition", str(kCompDoc)},
                 {"page", integer(kPageDoc)},
                 {"start", num(kStartDoc)},
                 {"duration", num(kDurationDoc)},
                 {"layer", str(kDrawOnDoc)},
                 {"strokes",
                  {{"type", "array"},
                   {"description",
                    "at most 64, each {\"points\": [[x, y], ...], "
                    "\"size\" pixels (8), \"softness\" 0-1, \"color\", "
                    "\"erase\"}"},
                   {"items", {{"type", "object"}}}}}},
                {"strokes"}),
         &H::t_draw_strokes});
    t.push_back(
        {"place_asset", "edit", "Place an asset",
         "An asset of the project (a picture, a clip's frame, another "
         "composition) on a page as a NEW layer, fitted in a box (whole, "
         "centred); omitted, at its own size in the middle. One a layer "
         "shows already is MOVED there with set_layer_box, never placed "
         "again.",
         schema(with({{"composition", str(kCompDoc)},
                      {"asset", str("the asset's id")},
                      {"page", integer(kPageDoc)},
                      {"below_all", boolean("under every layer of the "
                                            "page (a background)")}},
                     box_props),
                {"asset"}),
         &H::t_place_asset});
    t.push_back({"set_layer_box", "edit", "Move a layer",
                 "A layer showing a picture (a clip, a composition) MOVED "
                 "and SIZED to a box on the canvas, whole, centred in it "
                 "-- the Crop panel's placement: how pictures are put in "
                 "a grid, a row, a corner. Turned by `rotate`, mirrored "
                 "by `flip_x` / `flip_y`; else as the person set them. "
                 "(move_objects is a markup's texts and shapes; key_layer "
                 "a box that MOVES over time.)",
                 schema(with({{"composition", str(kCompDoc)},
                              {"layer", str(kLayerDoc)},
                              {"rotate", num("degrees, clockwise, about "
                                             "its middle")},
                              {"flip_x", boolean("mirrored left to right")},
                              {"flip_y", boolean("upside down")}},
                             box_props),
                        {"layer", "x", "y", "width", "height"}),
                 &H::t_set_layer_box});
    t.push_back({"upscale_layer", "generate", "Upscale a layer",
                 "A picture or clip shown LARGER than it was made (its box "
                 "bigger than its pixels) rendered sharp at that size, in "
                 "the background (a picture seconds, a clip minutes).",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"model", str(kModelDoc)}},
                        {"layer"}),
                 &H::t_upscale_layer});
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
                 "A layer's name in the Layers list -- or a layer "
                 "folder's (its id, as group_layers gave it).",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"name", str("its new name")}},
                        {"layer", "name"}),
                 &H::t_rename_layer});
    t.push_back({"set_layer_visible", "edit", "Show or hide a layer",
                 "A layer shown or hidden -- or a folder's layers (its "
                 "id).",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"visible", boolean("shown")}},
                        {"layer", "visible"}),
                 &H::t_set_layer_visible});
    t.push_back(
        {"add_mask", "edit", "Mask a layer",
         "A MASK for a layer: a new markup layer right above it, made "
         "its mask -- the layer then shows only where the mask is drawn "
         "(white and opaque: all of it; clear: none; grey or half clear: "
         "partly), and the mask itself is not seen. It starts empty, so "
         "the layer is hidden until its shape is drawn: draw it with "
         "add_shape, draw_strokes or add_text naming its `mask_layer` as "
         "their `layer`, in white. Rounded corners, a circle, a star, a "
         "soft vignette, a picture seen through words.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str("the layer to mask (a picture's, a "
                               "clip's)")}},
                {"layer"}),
         &H::t_add_mask});
    t.push_back({"set_layer_mask", "edit", "Use a layer as a mask",
                 "A layer made the MASK of the layer right beneath it, or "
                 "not (mask false: drawn as itself again).",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"mask", boolean("a mask of the layer beneath")}},
                        {"layer", "mask"}),
                 &H::t_set_layer_mask});
    t.push_back({"group_layers", "edit", "Group layers",
                 "Layers gathered in a FOLDER of the Layers list (they "
                 "lie together in the stack) -- the parts of a title, a "
                 "page's pictures: shown, hidden or renamed at once by its "
                 "id.",
                 schema({{"composition", str(kCompDoc)},
                         {"layers", ids("the layers' ids")},
                         {"name", str("the folder's name")}},
                        {"layers"}),
                 &H::t_group_layers});
    t.push_back({"ungroup_layers", "edit", "Ungroup layers",
                 "A layer folder taken away, its layers left where they "
                 "are.",
                 schema({{"composition", str(kCompDoc)},
                         {"folder", str("the folder's id")}},
                        {"folder"}),
                 &H::t_ungroup_layers});
    t.push_back({"duplicate_layer", "edit", "Duplicate a layer",
                 "A layer COPIED right above it -- its look, box, keys and "
                 "time; a markup's texts and shapes its own -- to change "
                 "one of the two (a second title in another colour).",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"name", str("the copy's name")}},
                        {"layer"}),
                 &H::t_duplicate_layer});
    t.push_back({"merge_layers", "edit", "Merge two layers",
                 "Two layers of a still made ONE flat picture, as they "
                 "show together (looks and a mask between them), on the "
                 "lower one -- not on a timeline.",
                 schema({{"composition", str(kCompDoc)},
                         {"layers", ids("the two layers' ids")}},
                        {"layers"}),
                 &H::t_merge_layers});
    t.push_back({"set_layer_source", "edit", "Change what a layer shows",
                 "What a layer SHOWS swapped for another asset -- the "
                 "picture on page 2 replaced by another -- its box, look "
                 "and time kept (a clip's part goes).",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"asset", str("the asset's id")}},
                        {"layer", "asset"}),
                 &H::t_set_layer_source});
    t.push_back({"lock_layer", "edit", "Lock a layer",
                 "A layer LOCKED -- nothing of it changes until unlocked -- "
                 "or unlocked. Unlock the person's own only when they ask.",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)},
                         {"locked", boolean("locked; true by default")}},
                        {"layer"}),
                 &H::t_lock_layer});
    t.push_back({"flatten_layer", "edit", "Flatten a layer",
                 "A layer showing a COMPOSITION made to show a flat copy "
                 "of it in its place, its look and box kept -- what "
                 "upscale_layer takes.",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)}},
                        {"layer"}),
                 &H::t_flatten_layer});
    t.push_back({"decompose_layer", "edit", "Decompose a layer",
                 "A layer showing a COMPOSITION replaced by that "
                 "composition's own layers, each placed and timed as they "
                 "showed -- to change one of them here.",
                 schema({{"composition", str(kCompDoc)},
                         {"layer", str(kLayerDoc)}},
                        {"layer"}),
                 &H::t_decompose_layer});
    t.push_back(
        {"adjust_layer", "edit", "Adjust a layer",
         "A layer's LOOK: the Adjust panel's sliders (each 0 is as it "
         "is; exposure in stops, -3..3, the others -1..1), its opacity, "
         "the COLOUR MIXER (one hue's colour moved: the sky bluer, the "
         "greens muted) and the COLOUR GRADE (a tint for the shadows, "
         "midtones, highlights: teal and orange, a warm film look). Only "
         "what is given changes; `reset` starts from none. A look keyed "
         "over time takes the change on every key.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str(kLayerDoc)},
                 {"exposure", num("stops")},
                 {"contrast", num("-1..1")},
                 {"highlights", num("-1..1")},
                 {"shadows", num("-1..1")},
                 {"saturation", num("-1..1")},
                 {"vibrance", num("-1..1")},
                 {"temperature", num("-1..1, + warmer")},
                 {"tint", num("-1..1, + magenta")},
                 {"opacity", num("0 (clear) .. 1 (as it is)")},
                 {"mix", {{"type", "object"},
                          {"description",
                           "the colour mixer: {\"hue\" | \"saturation\" "
                           "| \"luminance\": {\"<hue>\": -1..1}}, hues "
                           "red, orange, yellow, green, aqua, blue, "
                           "magenta (or degrees) -- only those named move: "
                           "{\"saturation\": {\"green\": -0.5}, "
                           "\"luminance\": {\"blue\": -0.3}}; hue +1 "
                           "shifts 30 degrees on. \"none\" clears it"}}},
                 {"grade", {{"type", "object"},
                            {"description",
                             "the colour grade: {\"shadows\" | "
                             "\"midtones\" | \"highlights\": {\"hue\": "
                             "a hue, \"saturation\": 0..1}, \"balance\": "
                             "-1..1 (where the midtones lie), "
                             "\"width\", \"blend\": 0..1} -- teal "
                             "shadows, orange highlights: {\"shadows\": "
                             "{\"hue\": \"aqua\", \"saturation\": "
                             "0.3}, \"highlights\": {\"hue\": "
                             "\"orange\", \"saturation\": 0.25}}. "
                             "\"none\" clears it"}}},
                 {"reset", boolean("its look cleared first")}},
                {"layer"}),
         &H::t_adjust_layer});
    t.push_back(
        {"key_layer", "edit", "Key a layer over time",
         "KEYFRAMES: a layer CHANGING over its time -- on a timeline (`at`: "
         "seconds from the layer's start, or \"end\") or across a still's "
         "pages (`page`, from 1). A slow zoom into a photo (Ken Burns), a "
         "pan across it, a title fading in, a look warming, a speed ramp, "
         "music swelling. Each key gives what changes there; what it "
         "leaves out is as it was. A track a key names -- look, box, "
         "rotate, speed, sound -- is set from these keys (one key: a "
         "steady value); the others stay. Between keys the values go "
         "on by `ease`.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str(kLayerDoc)},
                 {"keys",
                  {{"type", "array"},
                   {"items", {{"type", "object"}}},
                   {"description",
                    "at most 64, each {\"at\": seconds | \"end\" (a "
                    "still: \"page\"), \"ease\": how it moves INTO "
                    "this key, \"linear\" | \"smooth\" | \"hold\", "
                    "and what it is there -- "
                    "LOOK: adjust_layer's own (exposure, opacity, mix, "
                    "grade, ...); BOX: \"zoom\" (its size times this: "
                    "1.2 a fifth larger), \"dx\", \"dy\" (moved, "
                    "pixels), \"focus_x\", \"focus_y\" (the canvas "
                    "point a zoom keeps still; its middle), or \"x\", "
                    "\"y\", \"width\", \"height\" (a box, as "
                    "set_layer_box); \"rotate\" degrees; \"speed\" "
                    "0.1-10 (a clip, a sound); SOUND: \"volume\" 0-2, "
                    "\"pitch\", \"eq\" (set_layer_sound's). Zoom and "
                    "pan are from its box as it is now"}}}},
                {"layer", "keys"}),
         &H::t_key_layer});
    t.push_back({"rename_asset", "edit", "Rename an asset",
                 "An asset's name in Assets -- a picture, a clip, a "
                 "composition.",
                 schema({{"asset", str("the asset's id")},
                         {"name", str("its new name")}},
                        {"asset", "name"}),
                 &H::t_rename_asset});
    t.push_back({"file_assets", "edit", "File assets",
                 "Assets put in a FOLDER of the Assets list -- by its "
                 "name, made when there is none; \"\" the top of the list.",
                 schema({{"assets", ids("the assets' ids")},
                         {"folder", str("the folder's name")}},
                        {"assets", "folder"}),
                 &H::t_file_assets});
    t.push_back({"remove_asset", "edit", "Remove an asset",
                 "An asset REMOVED from the project -- refused while a "
                 "layer shows it or another is made from it. Only what "
                 "you made and is not wanted, or what the person asks to "
                 "go.",
                 schema({{"asset", str("the asset's id")}}, {"asset"}),
                 &H::t_remove_asset});
    t.push_back({"save_text", "edit", "Keep a text",
                 "Words KEPT in the project as a text asset -- notes, a "
                 "script, a shot list, a caption's words -- for the person "
                 "to open and use.",
                 schema({{"text", str("the words; Markdown reads as such")},
                         {"name", str("its name; omitted: its first "
                                      "words")}},
                        {"text"}),
                 &H::t_save_text});
    t.push_back({"create_prompt", "edit", "Save a prompt",
                 "Words kept in the project as a PROMPT asset the person "
                 "can use again (a style, a scene).",
                 schema({{"text", str("the prompt")},
                         {"name", str("its name; omitted: its words")}},
                        {"text"}),
                 &H::t_create_prompt});
    t.push_back(
        {"enhance_prompt", "read", "Enhance a prompt",
         "The PROMPT ENHANCER: your idea written out as the model it is "
         "for reads best (its makers' guide), in the fields of the tool "
         "that makes it -- generate_image's `prompt`, generate_video's "
         "`description` / `soundscape` / `music` (`subjects`, `summary`), "
         "generate_music's `style` / `lyrics`, generate_voiceover's "
         "`text` / `voice`. It does not check: read and correct it first "
         "(`notes`: what to look at).",
         schema({{"for", one_of({"picture", "shot", "music", "voiceover"},
                                "what it is a prompt for; picture by "
                                "default")},
                 {"idea", str("what you want, in your words; pictures "
                              "named <Picture 1>, ... as the tool does")},
                 {"style", str("the look it must keep (the deck's)")},
                 {"model", str("the model the tool that makes it is "
                               "given (list_models); omitted: the one it "
                               "would run")},
                 {"base", str("a picture: the one an edit changes")},
                 {"references", ids("pictures it draws on")},
                 {"first", str("a shot: the picture it opens on")},
                 {"last", str("a shot: the picture it ends on")},
                 {"continue_from", str("a shot: the clip it carries on")},
                 {"seconds", num("a shot's, song's or voice-over's length")},
                 {"width", num("a picture's box width")},
                 {"height", num("its height")}},
                {"idea"}),
         &H::t_enhance_prompt});
    t.push_back(
        {"generate_image", "generate", "Generate a picture",
         "A picture made from a prompt by a picture model, in the "
         "background: it LANDS on the page, fitted to the box, as a new "
         "layer, when it is done. Made at the box's shape. Without a "
         "composition, page or box: only an asset of the project. With "
         "`base` it EDITS that picture, with `references` draws on "
         "pictures: <Picture 1>, ... in the prompt, the base first.",
         schema(with({{"prompt", str("what the picture shows: subject, "
                                     "setting, light, style; no words. An "
                                     "edit: the change (\"the same street "
                                     "at night, wet with rain\")")},
                      {"composition", str(kCompDoc)},
                      {"page", integer(kPageDoc)},
                      {"below_all", boolean("under every layer of the "
                                            "page (a background)")},
                      {"model", str(kModelDoc)},
                      {"base", str("an EDIT: the picture to change")},
                      {"references", ids("pictures to draw on")},
                      {"long_side", integer("its longer side, pixels, up to "
                                            "the model's max_side; about a "
                                            "megapixel by default")},
                      {"seed", integer("the same number, model and words: "
                                       "the same picture again "
                                       "(get_recipe tells one's); another: "
                                       "another take")},
                      {"aspect", str("an asset alone (no box): its shape, "
                                     "\"16:9\", \"3:2\", \"1:1\", "
                                     "\"2:3\", \"9:16\"; square by "
                                     "default")},
                      {"favor", favor_prop()},
                      {"options", options_prop()},
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
        {"transcribe", "read", "Listen to a sound",
         "What is SAID in a sound or a clip, line by line with each line's "
         "span in its own seconds, and the sounds heard -- kept in the "
         "project as its transcript.",
         schema({{"asset", str("the sound's or the clip's id")},
                 {"model", str(kModelDoc)}},
                {"asset"}),
         &H::t_transcribe});
    t.push_back(
        {"separate_sound", "generate", "Separate a song",
         "A song split, in the background, into its VOICE and its "
         "ACCOMPANIMENT, each a new sound of the project.",
         schema({{"asset", str("the song's id (a sound)")},
                 {"model", str(kModelDoc)}},
                {"asset"}),
         &H::t_separate_sound});
    t.push_back({"grab_frame", "edit", "Grab a frame",
                 "A FRAME of a clip or a timeline, at `at` seconds of its "
                 "own (\"end\" its last), made a picture in Assets (a "
                 "still composition): a shot's `first` or `last`, a "
                 "reference, a cover for place_asset.",
                 schema({{"asset", str("the clip's or timeline's id")},
                         {"at", num("seconds into it; \"end\": its last "
                                    "frame")},
                         {"name", str("the picture's name")}},
                        {"asset", "at"}),
                 &H::t_grab_frame});
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
         "a dissolve or a dip it overlaps the clip before by "
         "`transition_seconds`.",
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
                 {"transition", one_of({"dissolve", "dip-black",
                                        "dip-white", "cut"},
                                       "how it follows the clip before")},
                 {"transition_seconds", num("a dissolve's or a dip's "
                                            "length; 0.5 by default")}},
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
        {"set_layer_sound", "edit", "Set a layer's sound",
         "A clip's or a sound's SOUND over its time: a level, faded in "
         "from silence and out to silence; its pitch and equalizer; and "
         "DUCKED -- lower wherever other layers sound (music under a "
         "voice-over or someone speaking), eased down and back up. Only "
         "what is given changes.",
         schema({{"composition", str(kCompDoc)},
                 {"layer", str(kLayerDoc)},
                 {"volume", num("0 (mute) to 1 (as it is); up to 2 "
                                "louder")},
                 {"fade_in", num("seconds from silence at its start")},
                 {"fade_out", num("seconds to silence at its end")},
                 {"pitch", num("semitones, -24..24 (12: an octave up)")},
                 {"eq", {{"type", "object"},
                         {"description",
                          "the equalizer, dB -12..12 by frequency: "
                          "{\"bass\": 3, \"4k\": -2} -- \"125\", "
                          "\"1k\", \"8k\", \"bass\", \"mid\", "
                          "\"treble\"; \"flat\" to clear"}}},
                 {"follow_speed", boolean("its pitch follows its speed, as "
                                          "a tape's (off: held)")},
                 {"duck_under", ids("layers it goes lower under, wherever "
                                    "they sound -- a voice-over's layer "
                                    "(or its asset's id), a clip's where "
                                    "someone speaks; [] to stop")},
                 {"duck_volume", num("its volume while ducked; a quarter "
                                     "of its level by default")}},
                {"layer"}),
         &H::t_set_layer_sound});
    t.push_back(
        {"set_transition", "edit", "Set a transition",
         "How one clip goes to the next: dissolve (picture and sound "
         "cross-fade over `seconds`), dip-black or dip-white (the first "
         "fades to black -- or white -- and its sound out by the middle "
         "of `seconds`, the next fades in from it), cut, or none. For a "
         "dissolve or a dip where the two only meet, the later one and "
         "all after it move earlier to overlap.",
         schema({{"composition", str(kCompDoc)},
                 {"from", str("the earlier layer's id")},
                 {"to", str("the later layer's id")},
                 {"kind", one_of({"dissolve", "dip-black", "dip-white",
                                  "cut", "none"}, "which")},
                 {"seconds", num("a dissolve's or a dip's length; 0.5 by "
                                 "default")}},
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
                 {"model", str(kModelDoc)},
                 {"favor", favor_prop()},
                 {"options", options_prop()},
                 {"name", str("the asset's name")}},
                {"style", "seconds"}),
         &H::t_generate_music});
    t.push_back(
        {"generate_voiceover", "generate", "Generate a voice-over",
         "Words spoken by a speech model, in the background: they LAND "
         "on the timeline from `start` as a new sound layer -- in a voice "
         "described, or cloned from a sound (`voice_from`); a scene of "
         "`speakers`, each in their own voice.",
         schema({{"text", str("the words; [pause 1s] pauses. A scene of "
                              "`speakers`: a line each, \"Name: "
                              "[direction] words\"")},
                 {"voice", str("how it speaks, e.g. \"a warm, unhurried "
                               "female narrator\"")},
                 {"voice_from", str("a sound's id (or a clip's) whose "
                                    "VOICE it speaks in -- cloned: the "
                                    "person's own, a character's")},
                 {"speakers",
                  {{"type", "array"},
                   {"items", {{"type", "object"}}},
                   {"description",
                    "a SCENE of several voices (a model that performs "
                    "scripts: Drama 3): each {\"name\", \"voice_from\": "
                    "a sound's id, \"words\": what is said in it, if "
                    "known} -- the names as the lines begin; every one "
                    "with a voice_from, or none"}}},
                 {"quality", str("the recording's quality (enhance_prompt "
                                 "writes it)")},
                 {"sound_event", str("a sound in it")},
                 {"ambient_sound", str("the room or the place around it")},
                 {"language", str("English, Chinese, ...; omitted: the "
                                  "words'")},
                 {"start", num("timeline seconds; 0 by default")},
                 {"volume", num("0 to 1; 1 by default")},
                 {"composition", str(kCompDoc)},
                 {"model", str(kModelDoc)},
                 {"favor", favor_prop()},
                 {"options", options_prop()},
                 {"name", str("the asset's name")}},
                {"text"}),
         &H::t_generate_voiceover});
    // ---- scenes: shots, each a clip with its sound
    const Json picture_ids = {
        {"type", "array"},
        {"items", {{"type", "string"}}},
        {"description", "ids it draws on -- pictures (a character, a "
                        "place, kept the same across cuts) named "
                        "<Picture 1>, <Picture 2>; clips <Video 1>; "
                        "sounds <Audio 1> (a voice to speak in, a song "
                        "to sing), each numbered in its kind in this "
                        "order; RefMods. A sound needs a picture or a "
                        "clip beside it"}};
    t.push_back(
        {"generate_video", "generate", "Generate a shot",
         "A SHOT of a scene: a clip WITH its own sound -- voices, sounds, "
         "music -- made by the video model from its prompt's parts, in "
         "the background (minutes). It LANDS on the timeline from `start` "
         "(omitted: right after the shots and clips before it), filling "
         "the frame. `continues: true` carries it on from the END of the "
         "shot before it so a long scene is shot after shot without a "
         "jump -- LATENT-GUIDED by default: the shot before's last frames, "
         "from the latents it kept, redone as this shot's first frames, "
         "which then go on (seamless). It may be asked for while that one "
         "is still being made. Without it, a new shot "
         "(a cut): from the words alone, opening on a picture (`first`), "
         "or drawing on `references` -- pictures to keep a character or "
         "a place the same, clips (a motion, a camera move), sounds (a "
         "voice, a song sung), RefMods. A `soundtrack` is a song used as "
         "the shot's own sound, as it is (a music video). `high_res`: at "
         "the timeline's own size, past 768P drafted and regenerated.",
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
                 {"seconds", num("its length, 2-30; 5 by default -- made "
                                 "as the model makes lengths (5 is 5.17). "
                                 "Past 15 s it is past what the model was "
                                 "trained on, slower, and may not fit")},
                 {"soundtrack", str("a sound's id: the shot's own sound, "
                                    "as it is -- the song of a music "
                                    "video (<Audio 1>)")},
                 {"whole", {{"type", "array"},
                            {"items", {{"type", "string"}}},
                            {"description",
                             "clips and sounds of `references` read WHOLE "
                             "(true: all); else each is cut to the shot's "
                             "length"}}},
                 {"latents_only", {{"type", "array"},
                                   {"items", {{"type", "string"}}},
                                   {"description",
                                    "RefMods of `references` given to the "
                                    "video model ALONE (ComfyUI's Apply H3 "
                                    "RefMod): its look and voice drawn on, "
                                    "the words never naming it -- no tag, "
                                    "say who it is in words. Else (the "
                                    "default) shown to the prompt encoder "
                                    "too and named <Picture n> / <Video n>"
                                    }}},
                 {"high_res", boolean("made at its timeline's own size "
                                      "(2560 at most) -- past 768P "
                                      "drafted at 768 and regenerated by "
                                      "Open-H3-Regenerate-2K, several "
                                      "times as long: only when a large "
                                      "size is asked for")},
                 {"continues", boolean("carry on from the end of the shot "
                                       "before it on this timeline: the "
                                       "next moment of the same scene. "
                                       "Without it the people, place and "
                                       "light come out different")},
                 {"continue_from", str("a clip's id to carry on from "
                                       "instead")},
                 {"continue_by",
                  one_of({"latents", "reference"},
                         "how it carries on: latents (the default) -- "
                         "LATENT-GUIDED, the clip's end redone from its "
                         "kept latents (else its encoded tail) and gone "
                         "on from: the seam seamless, the scene carried "
                         "by your words alone (describe who is there); "
                         "with `references`, its latents carried beside "
                         "them -- or reference: its last 3.75 s read "
                         "again as a picture and sound (<Video 1>, "
                         "`subjects`), only when the person asks for it")},
                 {"keep_latents", boolean("keep its latents with it, for "
                                          "a shot carrying on from it to "
                                          "be guided by them; on by "
                                          "default on a timeline")},
                 {"first", str("a picture's id the shot opens on, its "
                               "first frame (not with continues or "
                               "references)")},
                 {"last", str("a picture's id the shot ENDS on, its "
                              "last frame: after first, or alone -- the "
                              "shot leads up to it (not with continues "
                              "or references)")},
                 {"references", picture_ids},
                 {"start", num("where it lands, timeline seconds; omitted "
                               "(best): after the shots before it. A shot "
                               "that continues lands after the one before")},
                 {"seed", integer("another number: another take of the "
                                  "same shot")},
                 {"composition", str(kCompDoc)},
                 {"model", str(kModelDoc)},
                 {"favor", shot_prop(favor_prop())},
                 {"options", shot_prop(options_prop())},
                 {"name", str("the asset's name; omitted: \"Shot N\"")}},
                {"description"}),
         &H::t_generate_video});
    t.push_back(
        {"make_refmod", "generate", "Make a RefMod",
         "A REFMOD: a CHARACTER -- a person, a creature, a mascot, an "
         "object -- made once from its pictures, a clip and a voice of it, "
         "for shots to draw on and keep it the same across them: how it "
         "looks from its pictures, how it moves from a clip, its voice "
         "from a sound. "
         "Made in the background as an asset (seconds to a minute); once "
         "landed, its id goes in generate_video's `references`. Favor "
         "fine (or {\"Full reference\": true}) keeps more detail, at more "
         "tokens a shot.",
         schema({{"from", ids("its pictures (a face, a full figure), a "
                              "CLIP of it moving, a recording of its "
                              "voice")},
                 {"description", str("who or what it is, a sentence: "
                                     "\"a ginger woman in her thirties, "
                                     "green coat\"")},
                 {"name", str("its name; omitted: the description's")},
                 {"favor", favor_prop()},
                 {"options", options_prop()}},
                {"from"}),
         &H::t_make_refmod});
    t.push_back({"ask_user", "read", "Ask the person",
                 "A question for the person, waiting for their answer. "
                 "Only when you cannot go on without it.",
                 schema({{"question", str("the question")}}, {"question"}),
                 &H::t_ask_user});
    return t;
  }();
  return table;
}

const std::vector<Controller::AgentHost::Tool>&
Controller::AgentHost::chat_tools()
{
  using H = AgentHost;
  static const std::vector<Tool> table = [] {
    // The full table's that look -- or add, and only add: a clip's
    // summary, a transcript, a prompt kept -- as they are.
    static constexpr std::string_view kKept[] = {
        "get_project",  "list_models",   "model_options",
        "get_composition",
        "view_page",    "view_asset",    "get_exif",     "get_recipe",
        "grab_frame",   "make_refmod",
        "view_frames",  "describe_clip", "transcribe",
        "enhance_prompt", "create_prompt", "wait_for_generations"};
    std::vector<Tool> t;
    for (const auto name : kKept) {
      for (const auto& x : tools()) {
        if (x.name == name) {
          t.push_back(x);
        }
      }
    }
    // A picture made as an asset alone: generate_image placed nowhere.
    t.push_back(
        {"generate_image", "generate", "Generate a picture",
         "A picture made from a prompt by a picture model, in the "
         "background: an asset of the project when it is done (you are "
         "told; view_asset shows it). With `base` it EDITS that picture, "
         "with `references` draws on pictures: <Picture 1>, ... in the "
         "prompt, the base first.",
         schema({{"prompt", str("what the picture shows: subject, "
                                "setting, light, style; no words. An "
                                "edit: the change")},
                 {"aspect", str("its shape: \"16:9\", \"3:2\", "
                                "\"1:1\", \"2:3\", \"9:16\"; square by "
                                "default; an edit, its base's")},
                 {"model", str(kModelDoc)},
                 {"base", str("an EDIT: the picture to change")},
                 {"references", ids("pictures to draw on")},
                 {"long_side", integer("its longer side, pixels, up to "
                                       "the model's max_side; about a "
                                       "megapixel by default")},
                 {"seed", integer("the same number, model and words: the "
                                  "same picture again; another: another "
                                  "take")},
                 {"favor", favor_prop()},
                 {"options", options_prop()},
                 {"name", str("the asset's name")}},
                {"prompt"}),
         &H::t_chat_picture});
    t.push_back({"save_text", "edit", "Keep a text",
                 "Words KEPT in the project as a text asset -- notes, a "
                 "story, a script, a list -- for the person to open and "
                 "use.",
                 schema({{"text", str("the words; Markdown reads as such")},
                         {"name", str("its name; omitted: its first "
                                      "words")}},
                        {"text"}),
                 &H::t_save_text});
    return t;
  }();
  return table;
}

const std::vector<Controller::AgentHost::Tool>&
Controller::AgentHost::tools_for(std::string_view skills)
{
  static const std::vector<Tool> none;
  return skills == "chat" ? chat_tools() : skills == "none" ? none
                                                            : tools();
}

std::vector<agent::ToolSpec>
Controller::AgentHost::tool_specs(std::string_view skills)
{
  std::vector<agent::ToolSpec> out;
  for (const auto& t : tools_for(skills)) {
    out.push_back({t.name, t.description, t.parameters});
  }
  return out;
}

Json
Controller::AgentHost::tools_json() const
{
  auto listed = [](const std::vector<Tool>& table) {
    Json list = Json::array();
    for (const auto& t : table) {
      list.push_back({{"name", t.name}, {"kind", t.kind},
                      {"title", t.title}, {"description", t.description},
                      {"parameters", t.parameters}});
    }
    return list;
  };
  return {{"tools", listed(tools())},
          {"skill", _c.skill_text("valtz-agent")},
          // A chat's (AgentRequest::skills "chat").
          {"chat", {{"tools", listed(chat_tools())},
                    {"skill", _c.skill_text("valtz-chat")}}}};
}

// ---- helpers ----------------------------------------------------------------

std::string
Controller::AgentHost::model_state_(const models::ModelEntry& m,
                                    std::string_view task) const
{
  if (!does_task(m, task)) {
    return {};
  }
  if (const auto [modality, op] = task_runs(task); !modality.empty()) {
    return _c.runs(m, modality, op)        ? "here"
           : _c.runs_soon(m, modality, op) ? "downloading"
                                           : "";
  }
  // An asset operation: the engine has its graph, and the model is here
  // with what it needs beside it -- or on its way.
  if (!_c.engine_runs_(task_capability(task, m))) {
    return {};
  }
  return _c.ready_(m) ? "here" : _c.coming_(m) ? "downloading" : "";
}

const models::ModelEntry*
Controller::AgentHost::voice_model_() const
{
  const models::ModelEntry* m = nullptr;
  for (const auto& e : _c._catalog.models()) {
    if (does_task(e, "voiceover") && _c.runs(e, "audio", "generate") &&
        (!m || e.rank > m->rank)) {
      m = &e;
    }
  }
  return m;
}

// The model that performs a SCRIPT of several speakers (a speech model
// of catalog `speech.format` "drama3"), the best that runs here.
const models::ModelEntry*
Controller::AgentHost::script_model_() const
{
  const models::ModelEntry* m = nullptr;
  for (const auto& e : _c._catalog.models()) {
    const auto fmt = jget<std::string>(
        jget(jget(e.engine, "vpipe", Json::object()), "speech",
             Json::object()),
        "format", "");
    if (fmt == "drama3" && does_task(e, "voiceover") &&
        _c.runs(e, "audio", "generate") && (!m || e.rank > m->rank)) {
      m = &e;
    }
  }
  return m;
}

std::vector<const models::ModelEntry*>
Controller::AgentHost::auto_for_(std::string_view task) const
{
  using C = models::Capability;
  std::vector<const models::ModelEntry*> out;
  auto add = [&](const models::ModelEntry* m) {
    if (m && std::ranges::find(out, m) == out.end()) {
      out.push_back(m);
    }
  };
  if (const auto [modality, op] = task_runs(task);
      !modality.empty() && task != "voiceover") {
    for (const auto& a : _c.auto_models()) {
      if (a.modality == modality && a.op == op && !a.chosen.empty()) {
        add(_c._catalog.find(a.chosen));
      }
    }
  } else if (task == "voiceover") {
    add(voice_model_());
  } else if (task == "transcribe") {
    add(_c.operation_model_(C::Transcribe, "auto"));
  } else if (task == "separate") {
    add(_c.operation_model_(C::AudioSeparate, "auto"));
  } else if (task == "upscale") {
    for (const auto c : {C::UpscaleVideo, C::UpscaleImage}) {
      add(_c.operation_model_(c, "auto", _c.chosen_upscaler_(c)));
    }
  }
  return out;
}

Result<const models::ModelEntry*>
Controller::AgentHost::model_for_(std::string_view task,
                                  const std::string& named) const
{
  const std::string q = squeezed(named);
  if (q.empty() || q == "auto" || q == "default" || q == "autos") {
    return static_cast<const models::ModelEntry*>(nullptr);
  }
  const ModelTask* t = model_task(task);
  const auto& all = _c._catalog.models();
  // Named exactly -- its id, its name -- else the one of the task whose
  // id or name holds the words, those here first; of several, the one
  // whose name every other's begins with (Qwen-Image 2.1, not its Turbo).
  const models::ModelEntry* m = nullptr;
  for (const auto& e : all) {
    if (!m && e.quantize_from.empty() &&
        (squeezed(e.id) == q || squeezed(e.name) == q)) {
      m = &e;
    }
  }
  if (!m) {
    std::vector<const models::ModelEntry*> loose, others;
    for (const auto& e : all) {
      if (squeezed(e.id).find(q) != std::string::npos ||
          squeezed(e.name).find(q) != std::string::npos) {
        (does_task(e, task) ? loose : others).push_back(&e);
      }
    }
    const auto here = std::ranges::count_if(loose, [&](const auto* e) {
      return !model_state_(*e, task).empty();
    });
    if (here > 0) {
      std::erase_if(loose, [&](const auto* e) {
        return model_state_(*e, task).empty();
      });
    }
    for (const auto* e : loose) {
      const std::string mine = squeezed(e->name);
      if (std::ranges::all_of(loose, [&](const auto* o) {
            return squeezed(o->name).starts_with(mine);
          })) {
        m = e;
      }
    }
    if (!m && loose.size() > 1) {
      std::string names;
      for (const auto* e : loose) {
        names += std::format("{}{} ({})", names.empty() ? "" : ", ",
                             e->name, e->id);
      }
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" could be {}: give one's id", named, names));
    }
    if (!m && !loose.empty()) {
      m = loose.front();
    }
    if (!m && !others.empty()) {
      m = others.front();
    }
  }
  // Those here that do it, Auto's marked: what the person may choose.
  const auto autos = auto_for_(task);
  std::string offer;
  for (const auto& e : all) {
    if (!model_state_(e, task).empty()) {
      offer += std::format("{}{} ({}{})", offer.empty() ? "" : ", ", e.name,
                           e.id,
                           std::ranges::find(autos, &e) != autos.end()
                               ? ", Auto's"
                               : "");
    }
  }
  if (offer.empty()) {
    offer = "none";
  }
  if (!m) {
    return make_error(Code::NotFound, std::format(
        "no model is named \"{}\". Here for {}: {} (list_models)", named,
        t->what, offer));
  }
  if (!does_task(*m, task)) {
    return make_error(Code::InvalidArgument, std::format(
        "{} does not make {}. Here for {}: {}; or leave `model` out for "
        "Auto's", m->name, t->what, t->what, offer));
  }
  if (model_state_(*m, task).empty()) {
    return make_error(Code::Unsupported, std::format(
        "{} is not on this Mac (or not with what it needs): the person "
        "can download it in Settings > Capabilities. Here for {}: {}",
        m->name, t->what, offer));
  }
  return m;
}

Result<Json>
Controller::AgentHost::t_list_models(Session&, Step&, const Json& args,
                                     std::vector<fs::path>&)
{
  const std::string only = text_arg(args, "task");
  if (!only.empty() && !model_task(only)) {
    std::string names;
    for (const auto& t : kModelTasks) {
      names += std::format("{}{}", names.empty() ? "" : ", ", t.name);
    }
    return make_error(Code::InvalidArgument,
                      std::format("the task is one of: {}", names));
  }
  Json tasks = Json::array();
  for (const auto& t : kModelTasks) {
    if (!only.empty() && only != t.name) {
      continue;
    }
    const auto autos = auto_for_(t.name);
    Json here = Json::array(), absent = Json::array();
    for (const auto& m : _c._catalog.models()) {
      if (!does_task(m, t.name)) {
        continue;
      }
      const std::string state = model_state_(m, t.name);
      if (state.empty()) {
        absent.push_back(m.name);
        continue;
      }
      Json j = {{"id", m.id}, {"name", m.name}};
      if (std::ranges::find(autos, &m) != autos.end()) {
        j["auto"] = true;
      }
      if (state == "downloading") {
        j["downloading"] = true;
      }
      if (m.by_name) {
        j["by_name_only"] = true;
      }
      const Json vp = jget(m.engine, "vpipe", Json::object());
      const std::string_view task = t.name;
      if (task == "picture" || task == "edit") {
        j["max_side"] = models::max_edge(m) > 0 ? models::max_edge(m) : 2048;
      }
      if (task == "picture") {
        j["edits"] = _c.runs_soon(m, "image", "edit");
      }
      if (task == "edit") {
        j["max_pictures"] = std::max(
            1, jget(jget(vp, "edit", Json::object()), "max_references", 1));
      }
      if (task == "shot") {
        j["opens_on_picture"] = m.has(models::Capability::ImageToVideo);
        j["ends_on_picture"] = jget(vp, "last_frame", false);
      }
      if (task == "music") {
        j["needs_lyrics"] = jget(jget(vp, "song", Json::object()),
                                 "lyrics_required", false);
      }
      if (task == "upscale") {
        j["for"] = m.has(models::Capability::UpscaleVideo) ? "clips"
                                                           : "pictures";
      }
      if (!m.license.empty()) {
        j["license"] = m.license;
      }
      if (!m.notes.empty()) {
        j["about"] = about(m.notes);
      }
      here.push_back(std::move(j));
    }
    tasks.push_back({{"task", t.name}, {"tool", t.tool},
                     {"models", std::move(here)},
                     {"not_downloaded", std::move(absent)}});
  }
  const char* note =
      "Give a model's id as `model` to its tool when the person names "
      "one; else leave it out (Auto's). A model not downloaded is the "
      "person's to download, in Settings > Capabilities.";
  if (!only.empty()) {
    Json r = tasks.front();
    r["note"] = note;
    return r;
  }
  return Json{{"tasks", std::move(tasks)}, {"note", note}};
}

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

media::Crop
Controller::AgentHost::kept_turn_(const media::Crop& was, media::Crop to)
{
  to.rotate = was.rotate;
  if (was.scale_x < 0) {
    to.scale_x = -to.scale_x;
  }
  if (was.scale_y < 0) {
    to.scale_y = -to.scale_y;
  }
  to.pad = was.pad;
  return to;
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
  const double w = content->width * (c.identity() ? 1.0
                                                  : std::abs(c.scale_x));
  const double h = content->height * (c.identity() ? 1.0
                                                   : std::abs(c.scale_y));
  const double cx = frame->width / 2.0 + c.offset_x * frame->width;
  const double cy = frame->height / 2.0 + c.offset_y * frame->height;
  Json box = {{"x", std::lround(cx - w / 2)}, {"y", std::lround(cy - h / 2)},
              {"width", std::lround(w)}, {"height", std::lround(h)}};
  // Its turn and flips, about the box's middle, where it has them.
  if (c.rotate != 0) {
    box["rotate"] = c.rotate;
  }
  if (c.scale_x < 0) {
    box["flip_x"] = true;
  }
  if (c.scale_y < 0) {
    box["flip_y"] = true;
  }
  return box;
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
    // A MASK (add_mask) is drawn on only when named: the page's words go
    // over it, on a markup of their own.
    if (it->mask) {
      break;
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
                                   const Json& args, bool named)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  // A layer NAMED: drawn on as it is -- a mask's (add_mask), a markup of
  // the person's -- a blank one made a markup first.
  if (named && args.contains("layer")) {
    VALTZ_ASSIGN(std::string id, layer_(args));
    for (const auto& l : a.layers) {
      if (l.id != id) {
        continue;
      }
      if (l.source) {
        auto src = p->asset(*l.source);
        if (src.ok() && src->cls == AssetClass::Markup) {
          return std::pair{id, Json::object()};
        }
        return make_error(Code::InvalidArgument, std::format(
            "layer {} shows a picture, not a markup: name a markup "
            "layer -- add_mask makes a mask's", model_layer(id)));
      }
      VALTZ_ASSIGN(std::string made,
                   _c.markup_layer(pid, comp, {id}, l.time.offset));
      return std::pair{made, Json::object()};
    }
    return make_error(Code::NotFound,
                      std::format("no layer {}", model_layer(id)));
  }
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
  SoundShape sh;
  sh.volume = volume;
  sh.fade_in = fade_in;
  sh.fade_out = fade_out;
  return _c.set_sound_keys(pid, comp,
                           shape_keys(sh, seconds, rate_(pid, comp)), layer);
}

// Its shape laid again over its new length `seconds`, where its keys are
// a shape's (`was`: its length before).
Status
Controller::AgentHost::reshape_sound_(ProjectId pid, AssetId comp,
                                      const std::string& layer, double was,
                                      double seconds)
{
  auto* p = _c.project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const media::KeyedSound keys = Controller::sound_keys_of(a, layer);
  const Rational rate = rate_(pid, comp);
  if (keys.identity() || !is_shape(keys, was, rate)) {
    return ok_status();
  }
  return _c.set_sound_keys(
      pid, comp, shape_keys(shape_of(keys, rate.to_double()), seconds, rate),
      layer);
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
  VALTZ_ASSIGN(times, timings_(pid, comp));
  return reshape_sound_(pid, comp, layer, was.length, times[layer].length);
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

void
Controller::AgentHost::book_(Session& s, Step& step, JobId job)
{
  Landing l;
  l.session = s.id;
  l.step = step.n;
  l.project = s.project;
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
}

Json
Controller::AgentHost::take_reply_(JobId job)
{
  std::lock_guard lk(_mu);
  for (auto it = _replies.begin(); it != _replies.end(); ++it) {
    if (it->first == job) {
      Json r = std::move(it->second);
      _replies.erase(it);
      return r;
    }
  }
  return Json();
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
  // The picture models: which edit, and how large they make.
  Json models = Json::array();
  for (const auto& m : _c._catalog.models()) {
    if (m.has(models::Capability::TextToImage) &&
        _c.runs(m, "image", "generate")) {
      Json j = {{"id", m.id}, {"name", m.name},
                {"max_side", models::max_edge(m) > 0 ? models::max_edge(m)
                                                     : 2048}};
      if (m.has(models::Capability::ImageEdit) &&
          _c.runs(m, "image", "edit")) {
        j["edits"] = true;
      }
      models.push_back(std::move(j));
    }
  }
  std::string auto_pick, auto_edit;
  for (const auto& pick : _c.auto_models()) {
    if (pick.modality == "image" && pick.op == "generate") {
      auto_pick = pick.chosen;
    }
    if (pick.modality == "image" && pick.op == "edit") {
      auto_edit = pick.chosen;
    }
  }
  // What the asset operations can do here: listen, separate, upscale.
  std::map<std::string, bool> ready;
  for (const auto& op : _c.asset_operations_json()) {
    for (const auto& tk : jget(op, "takes", Json::array())) {
      if (jget(tk, "ready", false)) {
        ready[jget<std::string>(op, "name", "") + "/" +
              jget<std::string>(tk, "kind", "")] = true;
      }
    }
  }
  // The prompt enhancer: this Mac's helper, or a fleet member's.
  const bool enhance =
      (_c.assistant_model() != nullptr && _c._engine &&
       _c._engine->supports(engine::kOpChat)) ||
      !_c.fleet_candidates_(std::string(engine::kOpChat), {}, false)
           .empty();
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
  bool video = false, carry = false, ends = false;
  for (const auto& pick : _c.auto_models()) {
    if (pick.modality != "video" || pick.chosen.empty()) {
      continue;
    }
    const auto* m = _c._catalog.find(pick.chosen);
    const bool makes = pick.op == "generate" && m &&
                       _c.runs_soon(*m, "video", "generate");
    video = video || makes;
    ends = ends || (makes && jget(jget(m->engine, "vpipe", Json::object()),
                                  "last_frame", false));
    carry = carry || (pick.op == "edit" && m &&
                      _c.runs_soon(*m, "video", "edit"));
  }
  // A shot past 768P (its refiner here); a scene of speakers (Drama 3).
  bool high = false;
  for (const auto& pick : _c.auto_models()) {
    if (pick.modality == "video" && !pick.chosen.empty()) {
      const auto* m = _c._catalog.find(pick.chosen);
      high = high || (m && _c.refiner_of_(*m) != nullptr);
    }
  }
  const bool scripts = script_model_() != nullptr;
  bool refmods = false;
  for (const auto& e : _c._catalog.models()) {
    refmods = refmods ||
              (jget<std::string>(jget(e.engine, "vpipe", Json::object()),
                                 "shape", "") == "refmod" &&
               _c.runs_soon(e, "video", "refmod"));
  }
  std::lock_guard lk(s.mu);
  return Json{{"name", p->name()},
              {"active", s.target ? s.target->str() : std::string()},
              {"compositions", std::move(comps)},
              {"assets", std::move(assets)},
              {"picture_models", std::move(models)},
              {"auto_picture_model", auto_pick},
              {"auto_edit_model", auto_edit},
              {"can_enhance_prompts", enhance},
              {"can_make_music", music},
              {"can_make_voiceover", speech},
              {"can_describe_clips", watch},
              {"can_transcribe", ready["transcribe/audio"] ||
                                     ready["transcribe/video"]},
              {"can_separate_songs", ready["separate/audio"]},
              {"can_upscale", ready["upscale/image"] ||
                                  ready["upscale/video"]},
              {"can_make_shots", video},
              {"can_end_shots_on_pictures", ends},
              {"can_continue_shots", carry},
              {"can_make_high_res_shots", high},
              {"can_perform_scripts", scripts},
              {"can_make_refmods", refmods}};
}

Result<Json>
Controller::AgentHost::t_find_objects(Session& s, Step&, const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const std::string only = text_arg(args, "layer");
  const std::string want = jget<std::string>(args, "kind", "");
  std::string words = text_arg(args, "text");
  std::ranges::transform(words, words.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  std::set<std::string> ids;
  for (const auto& i : jget(args, "ids", Json::array())) {
    if (i.is_string()) {
      ids.insert(i.get<std::string>());
    }
  }
  const Json region = jget(args, "region", Json());
  const bool in_region = region.is_object();
  const double rx = jget(region, "x", 0.0), ry = jget(region, "y", 0.0);
  const double rw = jget(region, "width", 0.0);
  const double rh = jget(region, "height", 0.0);
  const std::size_t limit = std::clamp<std::size_t>(
      jget<std::size_t>(args, "limit", kFoundMost), 1, kFoundMost);
  Json found = Json::array();
  std::size_t matches = 0;
  for (const auto& l : a.layers) {
    if ((!only.empty() && model_layer(l.id) != only) || !l.source) {
      continue;
    }
    auto src = p->asset(*l.source);
    if (!src.ok() || src->cls != AssetClass::Markup || !src->markup) {
      continue;
    }
    for (const auto& o : src->markup->objects) {
      if (!want.empty() && jget<std::string>(o, "kind", "") != want) {
        continue;
      }
      if (!ids.empty() && !ids.count(jget<std::string>(o, "id", ""))) {
        continue;
      }
      if (!words.empty()) {
        std::string t = jget<std::string>(o, "text", "");
        std::ranges::transform(t, t.begin(), [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
        });
        if (t.find(words) == std::string::npos) {
          continue;
        }
      }
      if (in_region) {
        const auto b = media::object_bounds(o);
        if (b[0] > rx + rw || b[0] + b[2] < rx || b[1] > ry + rh ||
            b[1] + b[3] < ry) {
          continue;
        }
      }
      if (++matches <= limit) {
        Json x = described(o);
        x["layer"] = model_layer(l.id);
        found.push_back(std::move(x));
      }
    }
  }
  Json out = {{"objects", std::move(found)}, {"matches", matches}};
  if (matches > limit) {
    out["note"] = std::format("{} match; the first {} are listed: narrow "
                              "the region, kind or words for the rest.",
                              matches, limit);
  }
  return out;
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
    // The person's lock: nothing of it changes until they unlock it.
    if (l.locked) {
      j["locked"] = true;
    }
    // A MASK: not drawn; the layer under it shows only where it is.
    if (l.mask) {
      j["mask_of_layer_below"] = true;
    }
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
        // Brush strokes on it: pixels, which no object lists.
        if (src->markup && !src->markup->raster.empty()) {
          j["painted"] = true;
        }
        // Many objects -- a page of thousands -- told in sum, the ones on
        // top listed: they were all listed, megabytes past any context.
        Json objs = Json::array();
        const Json raw = src->markup ? src->markup->objects : Json();
        const Json all = raw.is_array() ? raw : Json::array();
        const std::size_t from =
            all.size() > kListedObjects ? all.size() - kListedTop : 0;
        for (std::size_t i = from; i < all.size(); ++i) {
          objs.push_back(described(all[i]));
        }
        if (from > 0) {
          j["objects_total"] = all.size();
          j["objects_by_kind"] = kinds_of(all);
          j["objects_note"] = std::format(
              "{} objects: the {} on top are listed. find_objects lists "
              "the others -- in a region, of a kind, with words, by id.",
              all.size(), kListedTop);
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
      j["adjusted"] = look_said(Controller::adjustments_of(a, l.id));
    }
    if (!l.folder.empty()) {
      j["folder"] = l.folder;
    }
    // What changes over its time (key_layer): each track's key times --
    // seconds from its start, or pages.
    Json keyed = Json::object();
    const auto times_of = [&](const auto& track) {
      Json t = Json::array();
      for (const auto& k : track.keys) {
        t.push_back(timeline ? Json(secs(static_cast<double>(k.frame) / fps))
                             : Json(l.time.offset + k.frame + 1));
      }
      return t;
    };
    if (const auto k = Controller::adjustment_keys_of(a, l.id);
        k.keys.size() > 1) {
      keyed["look"] = times_of(k);
    }
    if (const auto k = Controller::crop_keys_of(a, l.id);
        k.place.keys.size() > 1 || k.turn.keys.size() > 1) {
      if (k.place.keys.size() > 1) {
        keyed["box"] = times_of(k.place);
      }
      if (k.turn.keys.size() > 1) {
        keyed["turn"] = times_of(k.turn);
      }
    }
    if (const auto k = Controller::speed_keys_of(a, l.id);
        k.keys.size() > 1) {
      keyed["speed"] = times_of(k);
    }
    if (!keyed.empty()) {
      j["keyed"] = std::move(keyed);
    }
    if (const auto k = Controller::sound_keys_of(a, l.id); !k.empty()) {
      if (k.keys.front().value.pitch != 0) {
        j["pitch"] = k.keys.front().value.pitch;
      }
      if (!k.keys.front().value.flat()) {
        j["eq"] = eq_said(k.keys.front().value.eq);
      }
    }
    layers.push_back(std::move(j));
  }
  Json out = {{"id", comp.str()},
              {"name", a.name},
              {"layers", std::move(layers)}};
  if (!a.layer_folders.empty()) {
    Json folders = Json::array();
    for (const auto& f : a.layer_folders) {
      if (f.name != kProgressName) {
        folders.push_back({{"id", f.id}, {"name", f.name}});
      }
    }
    if (!folders.empty()) {
      out["folders"] = std::move(folders);
    }
  }
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
  // A picture named for the page -- an import is where a model looks
  // first -- is shown as view_asset shows it, rather than refused.
  if (const auto id = AssetId::parse(text_arg(args, "composition"))) {
    if (auto* p = _c.project(s.project)) {
      if (auto a = p->asset(*id);
          a.ok() && a->cls != AssetClass::Still &&
          a->cls != AssetClass::Composition) {
        return t_view_asset(s, step, {{"asset", id->str()}}, shown);
      }
    }
  }
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

// An asset as it looks. The tools that showed anything showed a page or
// a clip's frames, so a picture in Assets -- an import, the one thing a
// request like "draw the picture I imported" is about -- could be seen
// only by placing it on a page first: view_frames refused it as no
// clip, view_page as no composition, and a model that took either for
// an answer drew from its name and size alone.
Result<Json>
Controller::AgentHost::t_view_asset(Session& s, Step& step, const Json& args,
                                    std::vector<fs::path>& shown)
{
  const auto t = text_arg(args, "asset");
  const auto id = AssetId::parse(t);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      t.empty() ? std::string("which asset?")
                                : std::format("'{}' is not an asset id", t));
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(*id));
  // A still is its pages, a clip or a timeline its frames: the tools
  // that take a page or a time, at their first.
  if (a.cls == AssetClass::Still) {
    return t_view_page(s, step, {{"composition", id->str()}}, shown);
  }
  if (a.kind == AssetKind::Video) {
    return t_view_frames(s, step, {{"asset", id->str()}, {"count", 1}},
                         shown);
  }
  if (a.kind == AssetKind::Audio) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is a sound: it has no picture", a.name));
  }
  if (a.kind == AssetKind::Text) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is a text: it has no picture", a.name));
  }
  if (a.head == 0) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is still being made: wait_for_generations first", a.name));
  }
  Json told = {{"asset", id->str()}, {"name", a.name}};
  if (auto v = p->version(*id); v.ok() && v->info.frame.width > 0) {
    told["width"] = v->info.frame.width;
    told["height"] = v->info.frame.height;
  }
  if (!s.backend->sees()) {
    told["shown"] = false;
    told["note"] = "You cannot see pictures: only its name and size.";
    return told;
  }
  VALTZ_ASSIGN(fs::path drawn, _c.rendered_frame_(s.project, *id, 0, 0, 0));
  const fs::path small = s.dir / std::format("asset-{}.jpg", step.n);
  VALTZ_TRY(media::write_thumbnail(drawn, media::MediaType::Image, 1024,
                                   small));
  shown.push_back(small);
  told["shown"] = true;
  told["note"] = "The asset follows, as a picture.";
  return told;
}

Result<Json>
Controller::AgentHost::t_get_exif(Session& s, Step&, const Json& args,
                                  std::vector<fs::path>&)
{
  const auto t = text_arg(args, "asset");
  const auto id = AssetId::parse(t);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      t.empty() ? std::string("which asset?")
                                : std::format("'{}' is not an asset id", t));
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(*id));
  // A picture's file, read whole (media::exif_all).
  auto of = [&](const project::Asset& x) -> Result<Json> {
    VALTZ_ASSIGN(fs::path f, p->media_path(x.id));
    return Json{{"asset", x.id.str()},
                {"name", x.name},
                {"file", f.filename().string()},
                {"exif", media::exif_all(f)}};
  };
  const bool flat = a.cls == AssetClass::Flat ||
                    a.cls == AssetClass::Generated;
  if (flat) {
    if (a.kind == AssetKind::Video || a.kind == AssetKind::Audio ||
        a.kind == AssetKind::Text) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is no picture: EXIF is a photo's", a.name));
    }
    if (a.head == 0) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is still being made: wait_for_generations first",
          a.name));
    }
    VALTZ_ASSIGN(Json told, of(a));
    if (told["exif"].empty()) {
      told["note"] = "Its file carries no metadata ImageIO reads.";
    }
    return told;
  }
  // A composition -- a picture's edited copy, a still, a timeline: each
  // layer's picture's (the photo carries it; its look does not).
  Json layers = Json::array();
  for (const auto& l : a.layers) {
    if (!l.source) {
      continue;
    }
    auto src = p->asset(*l.source);
    if (!src.ok() || src->kind != AssetKind::Image || src->head == 0 ||
        (src->cls != AssetClass::Flat && src->cls != AssetClass::Generated)) {
      continue;
    }
    auto j = of(*src);
    if (!j.ok() || (*j)["exif"].empty()) {
      continue;
    }
    (*j)["layer"] = l.id.empty() ? "0" : l.id;
    layers.push_back(std::move(*j));
    if (layers.size() == 8) {
      break;
    }
  }
  Json told = {{"asset", id->str()}, {"name", a.name},
               {"layers", std::move(layers)}};
  if (told["layers"].empty()) {
    told["note"] = "None of its layers shows a picture with metadata.";
  }
  return told;
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
    VALTZ_ASSIGN(on, markup_for_(s.project, comp, args, true));
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
  turn_style(objs.back(), args);
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
  // A POLYGON's points (canvas pixels): its box their bounds, its points
  // in the box's units (media/markup.h) -- a PATH, smooth through them,
  // or open: a line through them.
  const bool open = shape == "polygon" && flag(args, "open");
  PolygonPoints poly;
  if (shape == "polygon") {
    VALTZ_ASSIGN(poly, polygon_points(jget(args, "points", Json()),
                                      open ? 2 : 3));
  }
  std::array<double, 4> box = poly.box;
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
                                shape == "line" || open ? 4.0 : 0.0),
                         0.0, 500.0);
  // Joined to shapes (from_object, to_object): its ends where their
  // connectors are, whatever its box.
  std::vector<std::string> joins;
  for (const char* k : {"from_object", "to_object"}) {
    if (auto to = text_arg(args, k); !to.empty() && to != "none") {
      joins.push_back(std::move(to));
    }
  }
  // A line far longer than it is "high" is a RULE given its thickness as
  // its height (the 9B's "height": 4 -- or 54, a band at a page's foot):
  // laid level through the box's middle, that thick. And so upright.
  if (shape == "line" && joins.empty()) {
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
  // A layer named (a mask's) takes it as it is.
  const bool named = args.contains("layer");
  const bool covers = !named && a.cls == AssetClass::Still &&
                      shape != "line" && (*fill)[3] >= 0.95 &&
                      box[2] * box[3] >= 0.9 * frame.width * frame.height;
  const bool below = !named && (flag(args, "below_all") || covers);
  if (!joins.empty()) {
    // On the layer of the shapes it joins: its anchors are to objects of
    // its own markup.
    std::optional<std::string> there;
    for (const auto& to : joins) {
      std::optional<std::string> holds;
      for (const auto& l : a.layers) {
        if (!l.source) {
          continue;
        }
        auto src = p->asset(*l.source);
        if (src.ok() && src->cls == AssetClass::Markup && src->markup &&
            std::ranges::any_of(src->markup->objects, [&](const Json& o) {
              return jget<std::string>(o, "id", "") == to;
            })) {
          holds = l.id;
          break;
        }
      }
      if (!holds) {
        return make_error(Code::NotFound, std::format(
            "no object '{}' in \"{}\": the shapes' ids, as "
            "get_composition lists them",
            to, a.name));
      }
      if (there && *there != *holds) {
        return make_error(Code::InvalidArgument, std::format(
            "'{}' and '{}' are on different layers ({} and {}): a line "
            "joins shapes of one layer",
            joins[0], to, model_layer(*there), model_layer(*holds)));
      }
      there = holds;
    }
    on = {*there, Json::object()};
  } else if (every || below) {
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
    VALTZ_ASSIGN(on, markup_for_(s.project, comp, args, true));
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
    made["points"] = std::move(poly.units);
    if (Json sm = smooth_arg(args, poly.n); !sm.is_null()) {
      made["smooth"] = std::move(sm);
    }
    if (open) {
      made["open"] = true;
    }
  }
  stroke_style(made, args);
  turn_style(made, args);
  if (shape != "line" && !open) {
    VALTZ_ASSIGN(Json g, gradient_arg(args, Json()));
    if (g.is_object()) {
      made["gradient"] = std::move(g);
    }
  }
  {
    // Its effects: a glow in its fill, or its line's colour.
    const bool line = shape == "line" || open;
    const auto& own = !line && (*fill)[3] > 0 ? *fill : *stroke;
    VALTZ_ASSIGN(Json fx, effects_arg(args, Json::array(),
                                      line ? std::max(16.0, lw * 3)
                                           : shape_base(box[2], box[3]),
                                      own));
    made["effects"] = std::move(fx);
  }
  // Its words, in it.
  Json out = {{"layer", model_layer(layer)}, {"object", id}};
  const bool worded = !text_arg(args, "text").empty();
  if (worded) {
    if (!holds_words(made)) {
      return make_error(Code::InvalidArgument,
                        "a line or an open path holds no words: put a "
                        "text box by it (add_text)");
    }
    VALTZ_TRY(set_words(made, args));
  }
  if (flag(args, "connectors")) {
    made = media::with_default_connectors(made);
  }
  VALTZ_TRY(join_ends(made, objs, args, out));
  // Under the layer's words, whenever it is added: a panel is behind
  // the text it was made for (objects are drawn in order).
  const auto words = std::ranges::find_if(objs, [](const Json& o) {
    return jget<std::string>(o, "kind", "") == "text";
  });
  objs.insert(words, std::move(made));
  VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
  out.update(on.second);
  // As kept: a joined line's ends where its shapes' connectors are, the
  // connectors it has, how its words lie.
  VALTZ_ASSIGN(Json now, objects_(s.project, comp, layer));
  for (const auto& o : now) {
    if (jget<std::string>(o, "id", "") != id) {
      continue;
    }
    const Json told = described(o);
    for (const char* k : {"from", "to", "connectors"}) {
      if (told.contains(k)) {
        out[k] = told[k];
      }
    }
    if (!joins.empty() && shape != "line") {
      out["points"] = jget(told, "points", Json());
    }
    if (worded) {
      tell_fit(out, o);
    }
  }
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
  // As they were: what lets go of where it was joined, what followed.
  const Json was = objs;
  const Json before = o;
  const double x0 = std::min(jget(o, "x0", 0.0), jget(o, "x1", 0.0));
  const double y0 = std::min(jget(o, "y0", 0.0), jget(o, "y1", 0.0));
  const double w = std::abs(jget(o, "x1", 0.0) - jget(o, "x0", 0.0));
  const double h = std::abs(jget(o, "y1", 0.0) - jget(o, "y0", 0.0));
  if (args.contains("x") || args.contains("y") || args.contains("width") ||
      args.contains("height")) {
    const double nx = number(args, "x", x0), ny = number(args, "y", y0);
    const double nw = number(args, "width", w), nh = number(args, "height", h);
    // Each end where it lay in the old box: a line from bottom left to
    // top right stays so (made x0 < x1, y0 < y1, it turned over).
    const auto place = [](double v, double lo, double old, double now,
                          double to) {
      return to + (old > 0 ? (v - lo) / old * now : (v > lo ? now : 0));
    };
    o["x0"] = place(jget(o, "x0", 0.0), x0, w, nw, nx);
    o["x1"] = place(jget(o, "x1", 0.0), x0, w, nw, nx);
    o["y0"] = place(jget(o, "y0", 0.0), y0, h, nh, ny);
    o["y1"] = place(jget(o, "y1", 0.0), y0, h, nh, ny);
  }
  // A polygon's points, its curve, open or closed: its box made from new
  // points (the units are the box's).
  if (args.contains("points") || args.contains("smooth") ||
      args.contains("corners") || args.contains("open")) {
    if (jget<std::string>(o, "kind", "") != "polygon") {
      return make_error(Code::InvalidArgument,
                        "only a polygon has points, smooth or open");
    }
    const bool open = flag(args, "open", jget(o, "open", false));
    std::size_t n = jget(o, "points", Json::array()).size();
    if (args.contains("points")) {
      VALTZ_ASSIGN(PolygonPoints pp, polygon_points(args["points"],
                                                    open ? 2 : 3));
      o["x0"] = pp.box[0];
      o["y0"] = pp.box[1];
      o["x1"] = pp.box[0] + pp.box[2];
      o["y1"] = pp.box[1] + pp.box[3];
      o["points"] = std::move(pp.units);
      if (pp.n != n) {
        o.erase("smooth");
      }
      n = pp.n;
    }
    if (Json sm = smooth_arg(args, n); !sm.is_null()) {
      o["smooth"] = std::move(sm);
    }
    if (open) {
      o["open"] = true;
    } else {
      o.erase("open");
    }
  }
  const auto kind = jget<std::string>(o, "kind", "");
  // A shape's words take `color` as theirs (a shape with none, its
  // outline's, as before).
  const bool shape_words =
      holds_words(o) && (o.contains("text") ||
                         !text_arg(args, "text").empty());
  if (kind == "text") {
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
    o["font"] = font;
  } else if (!holds_words(o) && !text_arg(args, "text").empty()) {
    return make_error(Code::InvalidArgument,
                      "a line or an open path holds no words: put a text "
                      "box by it (add_text)");
  }
  for (const char* k : {"color", "stroke", "fill"}) {
    if (!args.contains(k) || (shape_words && std::string(k) == "color")) {
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
  stroke_style(o, args);
  turn_style(o, args);
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
  Json out = {{"layer", model_layer(layer)}, {"object", id}};
  // Its words, once its outline and fill are set (they lie inside it,
  // their colour against it).
  std::string note;
  if (holds_words(o)) {
    Json wargs = args;
    if (shape_words && args.contains("color") &&
        !args.contains("text_color")) {
      wargs["text_color"] = args["color"];
    }
    VALTZ_ASSIGN(note, set_words(o, wargs));
  }
  // Its connectors; its ends let go where they moved, joined as asked.
  if (args.contains("connectors")) {
    if (flag(args, "connectors")) {
      o = media::with_default_connectors(o);
    } else {
      o.erase("connectors");
    }
  }
  const bool reshaped = args.contains("x") || args.contains("y") ||
                        args.contains("width") || args.contains("height") ||
                        args.contains("points");
  const auto let_go = reshaped ? let_moved_points_go(o, before)
                               : std::vector<std::string>();
  VALTZ_TRY(join_ends(o, objs, args, out));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
  // As kept: its words, the lines that followed it, what it let go of.
  VALTZ_ASSIGN(Json now, objects_(s.project, comp, layer));
  for (const auto& n : now) {
    if (jget<std::string>(n, "id", "") != id) {
      continue;
    }
    if (kind == "text" || (holds_words(n) && n.contains("text"))) {
      tell_fit(out, n);
    }
    if (args.contains("connectors") || out.contains("joined")) {
      const Json told = described(n);
      for (const char* k : {"from", "to", "connectors"}) {
        if (told.contains(k)) {
          out[k] = told[k];
        }
      }
    }
  }
  if (Json f = followed(was, now, {id}); !f.empty()) {
    out["followed"] = std::move(f);
  }
  if (!let_go.empty() && !out.contains("joined")) {
    out["let_go_of"] = let_go;
    note = "Its ends moved off the shapes they were joined to and are "
           "free: from_object / to_object join them again.";
  }
  if (!note.empty()) {
    out["note"] = out.contains("note")
                      ? jget<std::string>(out, "note", "") + " " + note
                      : note;
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_move_objects(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  // A layer showing a picture (a clip, a composition) holds no objects:
  // it is moved by its box -- the Crop panel's placement. Asked to lay
  // nine pictures in a grid, a helper moved them here, and failed.
  if (auto* p = _c.project(s.project)) {
    VALTZ_ASSIGN(project::Asset a, p->asset(comp));
    const auto it = std::ranges::find_if(
        a.layers, [&](const project::Layer& l) { return l.id == layer; });
    if (it != a.layers.end() && it->source) {
      auto shown = p->asset(*it->source);
      if (shown.ok() && shown->cls != AssetClass::Markup) {
        return make_error(
            Code::InvalidArgument,
            std::format("layer {} shows a picture, not texts and shapes: "
                        "move and size it with set_layer_box (its box "
                        "now {})",
                        model_layer(layer),
                        to_text(layer_box_(s.project, a, *it))));
      }
    }
  }
  std::vector<std::string> which;
  const Json given = jget(args, "objects", jget(args, "object", Json()));
  for (const auto& o : given.is_string() ? Json::array({given})
                       : given.is_array() ? given
                                          : Json::array()) {
    if (o.is_string() && std::ranges::find(which, o.get<std::string>()) ==
                             which.end()) {
      which.push_back(o.get<std::string>());
    }
  }
  if (which.empty()) {
    return make_error(Code::InvalidArgument,
                      "which objects? their ids, as get_composition "
                      "lists them");
  }
  const bool by_ = args.contains("dx") || args.contains("dy");
  const bool to_ = args.contains("x") || args.contains("y");
  if (!by_ && !to_ && !args.contains("page")) {
    return make_error(Code::InvalidArgument,
                      "where to? dx, dy (by so much), x, y (to there) or "
                      "page");
  }
  if (by_ && to_) {
    return make_error(Code::InvalidArgument,
                      "dx, dy or x, y: one or the other");
  }
  VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
  // Their box together, as their outlines lie.
  double left = 0, top = 0;
  bool first = true;
  for (const auto& id : which) {
    auto it = std::ranges::find_if(objs, [&](const Json& o) {
      return jget<std::string>(o, "id", "") == id;
    });
    if (it == objs.end()) {
      return make_error(Code::NotFound,
                        std::format("layer {} has no object '{}'",
                                    model_layer(layer), id));
    }
    const double ox = std::min(jget(*it, "x0", 0.0), jget(*it, "x1", 0.0));
    const double oy = std::min(jget(*it, "y0", 0.0), jget(*it, "y1", 0.0));
    left = first ? ox : std::min(left, ox);
    top = first ? oy : std::min(top, oy);
    first = false;
  }
  const double dx =
      to_ ? number(args, "x", left) - left : number(args, "dx", 0);
  const double dy = to_ ? number(args, "y", top) - top : number(args, "dy", 0);
  // A polygon's points and a gradient's ends are in its box's units: they
  // go with it.
  const Json was = objs;
  const std::set<std::string> them(which.begin(), which.end());
  for (auto& o : objs) {
    if (!them.count(jget<std::string>(o, "id", ""))) {
      continue;
    }
    o["x0"] = jget(o, "x0", 0.0) + dx;
    o["x1"] = jget(o, "x1", 0.0) + dx;
    o["y0"] = jget(o, "y0", 0.0) + dy;
    o["y1"] = jget(o, "y1", 0.0) + dy;
  }
  // Lines moved without the shapes they join let go of them (as the
  // app's drag); lines joined to what moved follow it.
  std::vector<std::string> let_go;
  for (const auto& o : objs) {
    if (!them.count(jget<std::string>(o, "id", ""))) {
      continue;
    }
    for (const auto& a : jget(o, "anchors", Json::array())) {
      if (!them.count(jget<std::string>(a, "to", ""))) {
        let_go.push_back(jget<std::string>(o, "id", ""));
        break;
      }
    }
  }
  objs = media::detach_moved(objs, them);
  Json moved = Json::array();
  for (const auto& o : objs) {
    if (them.count(jget<std::string>(o, "id", ""))) {
      moved.push_back(o);
    }
  }
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  std::string onto = layer;
  Json where = Json::object();
  if (args.contains("page")) {
    VALTZ_ASSIGN(auto on, markup_for_(s.project, comp, args));
    if (!on.second.contains("page")) {
      return make_error(Code::InvalidArgument,
                        "page is a still's: on a timeline a title moves in "
                        "time with set_layer_time");
    }
    onto = on.first;
    where = on.second;
  }
  if (onto == layer) {
    VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
  } else {
    // Out of one markup, into the other: the ids kept unless the other
    // holds one already.
    VALTZ_ASSIGN(Json there, objects_(s.project, comp, onto));
    objs.erase(std::remove_if(objs.begin(), objs.end(),
                              [&](const Json& o) {
                                return std::ranges::find(
                                           which,
                                           jget<std::string>(o, "id", "")) !=
                                       which.end();
                              }),
               objs.end());
    // Renamed, the lines among them joined to the shapes among them
    // follow the new ids.
    std::map<std::string, std::string> renamed;
    for (auto& o : moved) {
      const auto id = jget<std::string>(o, "id", "");
      if (std::ranges::any_of(there, [&](const Json& t) {
            return jget<std::string>(t, "id", "") == id;
          })) {
        renamed[id] = object_id();
        o["id"] = renamed[id];
      }
    }
    for (auto& o : moved) {
      if (o.contains("anchors")) {
        for (auto& a : o["anchors"]) {
          if (auto r = renamed.find(jget<std::string>(a, "to", ""));
              r != renamed.end()) {
            a["to"] = r->second;
          }
        }
      }
      there.push_back(o);
    }
    for (auto& id : which) {
      if (auto r = renamed.find(id); r != renamed.end()) {
        id = r->second;
      }
    }
    VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
    VALTZ_TRY(_c.set_markup_objects(s.project, comp, onto, there));
  }
  // Where each lies now, as kept; what followed them.
  VALTZ_ASSIGN(Json now, objects_(s.project, comp, onto));
  Json boxes = Json::object();
  for (const auto& o : now) {
    const auto id = jget<std::string>(o, "id", "");
    if (std::ranges::find(which, id) != which.end()) {
      boxes[id] = box_told(o);
    }
  }
  Json out = {{"layer", model_layer(onto)}, {"moved", std::move(boxes)}};
  out.update(where);
  {
    VALTZ_ASSIGN(Json left, objects_(s.project, comp, layer));
    if (Json f = followed(was, left, them); !f.empty()) {
      out["followed"] = std::move(f);
    }
  }
  if (!let_go.empty()) {
    out["let_go"] = let_go;
    out["note"] = "Moved without the shapes they joined, these let go of "
                  "them: from_object / to_object (update_object) join "
                  "them again.";
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
  // The lines joined to it stay where they are, free.
  Json freed = Json::array();
  for (const auto& o : objs) {
    if (std::ranges::any_of(jget(o, "anchors", Json::array()),
                            [&](const Json& a) {
                              return jget<std::string>(a, "to", "") == id;
                            })) {
      freed.push_back(jget<std::string>(o, "id", ""));
    }
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_markup_objects(s.project, comp, layer, objs));
  Json out = {{"removed", id}};
  if (!freed.empty()) {
    out["let_go"] = std::move(freed);
    out["note"] = "The lines joined to it stay where they were, free.";
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_arrange_objects(Session& s, Step& step,
                                         const Json& args,
                                         std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  const std::string how = text_arg(args, "how");
  std::vector<std::string> which;
  const Json given = jget(args, "objects", Json::array());
  for (const auto& o : given.is_string() ? Json::array({given}) : given) {
    if (o.is_string()) {
      which.push_back(o.get<std::string>());
    }
  }
  if (which.empty()) {
    return make_error(Code::InvalidArgument,
                      "which objects? their ids, as get_composition "
                      "lists them");
  }
  VALTZ_ASSIGN(Json objs, objects_(s.project, comp, layer));
  for (const auto& id : which) {
    if (std::ranges::none_of(objs, [&](const Json& o) {
          return jget<std::string>(o, "id", "") == id;
        })) {
      return make_error(Code::NotFound,
                        std::format("layer {} has no object '{}'",
                                    model_layer(layer), id));
    }
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.arrange_markup(s.project, comp, layer, which, how));
  // The order now, bottom first, and where each moved one lies.
  VALTZ_ASSIGN(Json now, objects_(s.project, comp, layer));
  Json order = Json::array();
  Json boxes = Json::object();
  for (const auto& o : now) {
    const auto id = jget<std::string>(o, "id", "");
    order.push_back(id);
    if (std::ranges::find(which, id) != which.end()) {
      const double x0 = jget(o, "x0", 0.0), y0 = jget(o, "y0", 0.0);
      const double x1 = jget(o, "x1", 0.0), y1 = jget(o, "y1", 0.0);
      boxes[id] = {std::lround(std::min(x0, x1)),
                   std::lround(std::min(y0, y1)),
                   std::lround(std::abs(x1 - x0)),
                   std::lround(std::abs(y1 - y0))};
    }
  }
  Json out = {{"layer", model_layer(layer)}, {"how", how},
              {"order", std::move(order)}, {"boxes", std::move(boxes)}};
  if (Json f = followed(objs, now, {which.begin(), which.end()});
      !f.empty()) {
    out["followed"] = std::move(f);
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_draw_strokes(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  Json list = jget(args, "strokes", Json::array());
  if (list.is_object()) {
    list = Json::array({list});
  }
  std::vector<media::Stroke> strokes;
  for (const auto& st : list.is_array() ? list : Json::array()) {
    if (!st.is_object()) {
      continue;
    }
    media::Stroke k;
    for (const auto& pt : jget(st, "points", Json::array())) {
      if (pt.is_array() && pt.size() >= 2 && pt[0].is_number() &&
          pt[1].is_number()) {
        k.points.push_back({pt[0].get<double>(), pt[1].get<double>()});
      }
    }
    if (k.points.empty() || k.points.size() > 4096) {
      return make_error(Code::InvalidArgument, std::format(
          "stroke {} needs points, [[x, y], ...] in canvas pixels (at "
          "most 4096)",
          strokes.size() + 1));
    }
    k.radius = std::clamp(number(st, "size", 8), 1.0, 800.0) / 2;
    k.softness = std::clamp(number(st, "softness", 0), 0.0, 1.0);
    const auto c = colour(text_arg(st, "color", "#000000"));
    if (!c) {
      return make_error(Code::InvalidArgument,
                        "a colour is \"#rrggbb\" or \"#rrggbbaa\"");
    }
    k.color = *c;
    k.erase = flag(st, "erase");
    strokes.push_back(std::move(k));
  }
  if (strokes.empty() || strokes.size() > 64) {
    return make_error(Code::InvalidArgument,
                      "strokes: one to 64, each {\"points\": [[x, y], "
                      "...]}");
  }
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(auto on, markup_for_(s.project, comp, args, true));
  // A markup's strokes lie UNDER its words and shapes (its raster is
  // drawn first): painted on one that holds some they would be hidden --
  // the 27B's waves went under its water. On a markup of their own over
  // it, then -- the page's (the span's) topmost, so the words and shapes
  // added after go on it, over them.
  VALTZ_ASSIGN(Json there, objects_(s.project, comp, on.first));
  bool apart = false;
  // A layer named (a mask's): on it, under its shapes or not.
  if (!there.empty() && !args.contains("layer")) {
    auto* p = _c.project(s.project);
    VALTZ_ASSIGN(project::Asset a, p->asset(comp));
    const std::string top = a.layers.empty() ? "" : a.layers.back().id;
    if (a.cls == AssetClass::Composition) {
      const double fps = rate_(s.project, comp).to_double();
      project::LayerTime t;
      t.offset = std::llround(std::max(0.0, number(args, "start", 0)) * fps);
      t.duration = std::max<std::int64_t>(
          1, std::llround(std::max(0.1, number(args, "duration", 3)) * fps));
      VALTZ_ASSIGN(std::string blank, _c.add_layer(s.project, comp, top));
      VALTZ_ASSIGN(on.first,
                   _c.markup_layer(s.project, comp, {blank}, t.offset));
      VALTZ_TRY(_c.set_layer_time(s.project, comp, on.first, t));
    } else {
      const std::int64_t page = jget(on.second, "page", 1) - 1;
      VALTZ_ASSIGN(std::string blank,
                   _c.add_layer(s.project, comp, top, page));
      VALTZ_TRY(page_alone_(s.project, comp, blank, page));
      VALTZ_ASSIGN(on.first,
                   _c.markup_layer(s.project, comp, {blank}, page));
    }
    apart = true;
  }
  for (const auto& k : strokes) {
    VALTZ_TRY(_c.paint_stroke(s.project, comp, on.first, k));
  }
  Json out = {{"layer", model_layer(on.first)},
              {"strokes", strokes.size()}};
  out.update(on.second);
  if (apart) {
    out["note"] = "On a layer of their own over the shapes and words "
                  "there: what you add next goes over them.";
  }
  return out;
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
Controller::AgentHost::t_update_composition(Session& s, Step& step,
                                            const Json& args,
                                            std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  const bool sized = args.contains("width") || args.contains("height");
  const std::string name = jget<std::string>(args, "name", "");
  if (!sized && name.empty()) {
    return make_error(Code::InvalidArgument,
                      "what to change? width and height (its canvas) or "
                      "name");
  }
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  if (sized) {
    VALTZ_ASSIGN(media::PixelSize now, _c.own_frame(s.project, comp));
    const media::PixelSize size{
        static_cast<int>(number(args, "width", now.width)),
        static_cast<int>(number(args, "height", now.height))};
    // Canvas Size's anchor grid: across and down, 0 .. 1.
    const std::string at = jget<std::string>(args, "anchor", "center");
    const double ax = at.ends_with("left")    ? 0
                      : at.ends_with("right") ? 1
                                              : 0.5;
    const double ay = at.starts_with("top")      ? 0
                      : at.starts_with("bottom") ? 1
                                                 : 0.5;
    VALTZ_TRY(_c.set_canvas(s.project, comp, size, ax, ay));
  }
  if (!name.empty()) {
    VALTZ_TRY(_c.rename_asset(s.project, comp, name));
  }
  VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, comp));
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  Json layers = Json::array();
  for (const auto& l : a.layers) {
    if (Json b = layer_box_(s.project, a, l); b.is_object()) {
      layers.push_back({{"layer", model_layer(l.id)}, {"box", b}});
    }
  }
  return Json{{"composition", comp.str()}, {"name", a.name},
              {"width", frame.width}, {"height", frame.height},
              {"layers", std::move(layers)}};
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
  // Turned and flipped as asked, else as the person left it.
  media::Crop c =
      kept_turn_(crop_of(a, layer), box_crop_(content, frame, box));
  if (args.contains("rotate")) {
    c.rotate = std::clamp(number(args, "rotate", 0), -media::kMaxTurn,
                          media::kMaxTurn);
  }
  if (args.contains("flip_x")) {
    c.scale_x = std::abs(c.scale_x) * (flag(args, "flip_x") ? -1 : 1);
  }
  if (args.contains("flip_y")) {
    c.scale_y = std::abs(c.scale_y) * (flag(args, "flip_y") ? -1 : 1);
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_crop(s.project, comp, c, layer));
  Json placed = layer_box_(s.project, *p->asset(comp), *it);
  Json r = {{"layer", model_layer(layer)}, {"box", placed}};
  // Past the canvas, it is cut there: said, so a grid laid with margins
  // that do not add up is put right (the 9B's ran 64 px off two sides).
  const double x = jget(placed, "x", 0.0), y = jget(placed, "y", 0.0);
  const double w = jget(placed, "width", 0.0);
  const double h = jget(placed, "height", 0.0);
  std::vector<std::string> past;
  const auto off = [&](double by, const char* side) {
    if (by >= 1) {
      past.push_back(std::format("{} px past the {}", std::lround(by),
                                 side));
    }
  };
  off(-x, "left");
  off(-y, "top");
  off(x + w - frame.width, "right");
  off(y + h - frame.height, "bottom");
  if (!past.empty()) {
    std::string said;
    for (const auto& p_ : past) {
      said += (said.empty() ? "" : ", ") + p_;
    }
    r["note"] = std::format("It runs off the {} x {} canvas ({}): what lies "
                            "past the edge is cut.",
                            frame.width, frame.height, said);
  }
  return r;
}

Result<Json>
Controller::AgentHost::t_upscale_layer(Session& s, Step& step,
                                       const Json& args,
                                       std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  VALTZ_ASSIGN(const models::ModelEntry* named,
               model_for_("upscale", text_arg(args, "model")));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  Controller::AssetOpRequest req;
  req.project = s.project;
  req.asset = comp;
  req.operation = "upscale";
  req.layer = layer;
  if (named) {
    req.model = named->id;
  }
  VALTZ_ASSIGN(auto made, _c.run_asset_operation(std::move(req)));
  book_(s, step, made.job);
  Json r = {{"job", made.job.str()}, {"queued", true},
            {"layer", model_layer(layer)}};
  if (!made.made.empty()) {
    r["asset"] = made.made.front().str();
  }
  r["note"] = "Rendered in the background: the layer shows it once it "
              "lands (wait_for_generations).";
  return r;
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
  // A folder's id: the folder renamed.
  if (auto a = _c.project(s.project)->asset(comp);
      a.ok() && folder_of(*a, layer)) {
    VALTZ_TRY(_c.rename_layer_folder(s.project, comp, layer,
                                     one_line(text_arg(args, "name"))));
    return Json{{"folder", layer}};
  }
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
  // A folder's id: every layer in it.
  if (auto a = _c.project(s.project)->asset(comp);
      a.ok() && folder_of(*a, layer)) {
    VALTZ_TRY(_c.set_folder_visible(s.project, comp, layer,
                                    flag(args, "visible", true)));
    return Json{{"folder", layer}, {"visible", flag(args, "visible", true)}};
  }
  VALTZ_TRY(_c.set_layer_visible(s.project, comp, layer,
                                 flag(args, "visible", true)));
  return Json{{"layer", model_layer(layer)},
              {"visible", flag(args, "visible", true)}};
}

Result<Json>
Controller::AgentHost::t_set_layer_mask(Session& s, Step& step,
                                        const Json& args,
                                        std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  const bool on = flag(args, "mask", true);
  VALTZ_TRY(_c.set_layer_mask(s.project, comp, layer, on));
  return Json{{"layer", model_layer(layer)}, {"mask", on}};
}

Result<Json>
Controller::AgentHost::t_add_mask(Session& s, Step& step, const Json& args,
                                  std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const project::Layer* masked = nullptr;
  for (const auto& l : a.layers) {
    if (l.id == layer) {
      masked = &l;
    }
  }
  if (!masked) {
    return make_error(Code::NotFound,
                      std::format("no layer {}", model_layer(layer)));
  }
  const project::LayerTime when = masked->time;
  const bool still = a.cls != AssetClass::Composition;
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  // Right above it, on its pages (its time), a markup -- the mask.
  VALTZ_ASSIGN(std::string blank, _c.add_layer(s.project, comp, layer));
  VALTZ_ASSIGN(std::string mask,
               _c.markup_layer(s.project, comp, {blank}, when.offset));
  if (still) {
    VALTZ_TRY(_c.set_layer_pages(s.project, comp, mask, when.offset,
                                 when.duration));
  } else {
    project::LayerTime t;
    t.offset = when.offset;
    t.duration = when.duration;
    VALTZ_TRY(_c.set_layer_time(s.project, comp, mask, t));
  }
  VALTZ_TRY(_c.rename_layer(s.project, comp, mask, "Mask"));
  VALTZ_TRY(_c.set_layer_mask(s.project, comp, mask, true));
  return Json{{"mask_layer", model_layer(mask)},
              {"masks", model_layer(layer)},
              {"note", std::format(
                  "Layer {} is hidden until its shape is drawn: add_shape "
                  "(or draw_strokes, add_text) with layer \"{}\", fill "
                  "\"#ffffff\" -- where it is white the layer shows.",
                  model_layer(layer), model_layer(mask))}};
}

Result<Json>
Controller::AgentHost::t_adjust_layer(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  if (std::ranges::find(a.layers, layer, &project::Layer::id) ==
      a.layers.end()) {
    return make_error(Code::NotFound,
                      std::format("no layer {}", model_layer(layer)));
  }
  // What is given changes, the rest of its look kept (an exposure, then
  // a warmer tint: both) -- on every key of a look keyed over time.
  const bool reset = flag(args, "reset");
  media::KeyedAdjustments k = Controller::adjustment_keys_of(a, layer);
  if (k.keys.empty()) {
    k.keys.push_back({0, media::Adjustments{}});
    k.rate = a.cls == AssetClass::Composition ? rate_(s.project, comp)
                                              : Rational{1, 1};
  }
  for (auto& key : k.keys) {
    VALTZ_ASSIGN(key.value,
                 look_over(args, reset ? media::Adjustments{} : key.value));
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  if (k.keys.size() == 1) {
    VALTZ_TRY(_c.set_adjustments(s.project, comp, k.keys.front().value,
                                 layer));
  } else {
    VALTZ_TRY(_c.set_adjustment_keys(s.project, comp, k, layer));
  }
  Json r = {{"layer", model_layer(layer)},
            {"look", look_said(k.keys.front().value)}};
  if (k.keys.size() > 1) {
    r["note"] = std::format("Its look is keyed ({} keys): each took the "
                            "change; the first is told.",
                            k.keys.size());
  }
  return r;
}

// ---- keys: a layer changing over its time ----------------------------------

namespace {

// How a value moves INTO a key from the one before -- "linear",
// "smooth" (a spline: eased), "hold" (unchanged until it) -- as a person
// says it of the key they mean ("ease into the end"); none given: none.
std::optional<media::Interp>
ease_of(const Json& key)
{
  if (!key.contains("ease")) {
    return std::nullopt;
  }
  const std::string e = text_arg(key, "ease", "linear");
  if (e == "smooth" || e == "spline" || e == "ease" || e == "eased") {
    return media::Interp::Spline;
  }
  if (e == "hold" || e == "step" || e == "none") {
    return media::Interp::Hold;
  }
  return media::Interp::Linear;
}

bool
says_box(const Json& key)
{
  for (const char* k : {"x", "y", "width", "height", "zoom", "dx", "dy",
                        "focus_x", "focus_y"}) {
    if (key.contains(k)) {
      return true;
    }
  }
  return false;
}

bool
says_sound(const Json& key)
{
  return key.contains("volume") || key.contains("pitch") ||
         key.contains("eq");
}

// Keys by frame, the last given at a frame kept; each span's ease the
// one its later key gives (else its earlier key's), as a Keyframe holds
// it: on the key it goes on from.
template <class T>
void
tidy_keys(media::Keyed<T>& k,
          const std::map<std::int64_t, media::Interp>& eases = {})
{
  std::ranges::stable_sort(k.keys, {}, &media::Keyframe<T>::frame);
  std::vector<media::Keyframe<T>> out;
  for (auto& key : k.keys) {
    if (!out.empty() && out.back().frame == key.frame) {
      out.back() = std::move(key);
    } else {
      out.push_back(std::move(key));
    }
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    out[i].interp = media::Interp::Linear;
    if (i + 1 < out.size()) {
      if (auto e = eases.find(out[i + 1].frame); e != eases.end()) {
        out[i].interp = e->second;
      } else if (auto b = eases.find(out[i].frame); b != eases.end()) {
        out[i].interp = b->second;
      }
    }
  }
  k.keys = std::move(out);
}

// Where a crop puts its picture: its box on the canvas.
Json
crop_box(const media::Crop& c, media::PixelSize content,
         media::PixelSize frame)
{
  const double w = content.width * std::abs(c.scale_x);
  const double h = content.height * std::abs(c.scale_y);
  const double cx = frame.width / 2.0 + c.offset_x * frame.width;
  const double cy = frame.height / 2.0 + c.offset_y * frame.height;
  return {{"x", std::lround(cx - w / 2)}, {"y", std::lround(cy - h / 2)},
          {"width", std::lround(w)}, {"height", std::lround(h)}};
}

}  // namespace

Result<Json>
Controller::AgentHost::t_key_layer(Session& s, Step& step, const Json& args,
                                   std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  const auto it = std::ranges::find(a.layers, layer, &project::Layer::id);
  if (it == a.layers.end()) {
    return make_error(Code::NotFound,
                      std::format("no layer {}", model_layer(layer)));
  }
  const bool timeline = a.cls == AssetClass::Composition;
  if (!timeline && a.pages <= 1) {
    return make_error(Code::InvalidArgument,
                      "keys change a layer over TIME -- on a timeline, or "
                      "across a still's pages; a page alone has one look "
                      "and one box: adjust_layer, set_layer_box");
  }
  // Keys count the composition's frames from the layer's start -- a
  // still's pages from its first.
  const Rational rate = timeline ? rate_(s.project, comp) : Rational{1, 1};
  const double fps = rate.to_double();
  double start = 0;
  double length = media::kForever;
  if (timeline) {
    VALTZ_ASSIGN(auto times, timings_(s.project, comp));
    start = times[layer].start;
    length = times[layer].length;
  } else {
    start = static_cast<double>(it->time.offset);
    length = static_cast<double>(it->time.duration > 0
                                     ? it->time.duration
                                     : a.pages - it->time.offset);
  }
  const std::string kind =
      timeline ? layer_kind_(s.project, *it)
               : (it->source ? std::string("picture") : "nothing");
  const bool timed = kind == "clip" || kind == "sound";
  const bool boxed = it->source && kind != "sound" && kind != "title";
  const Json keys = jget(args, "keys", Json::array());
  if (!keys.is_array() || keys.empty()) {
    return make_error(Code::InvalidArgument,
                      "keys: [{\"at\": 0, ...}, {\"at\": 5, ...}] -- at "
                      "least one");
  }
  if (keys.size() > 64) {
    return make_error(Code::InvalidArgument, "64 keys at most");
  }
  // What each track is now: what a key leaves out is taken from it there.
  const auto adj0 = Controller::adjustment_keys_of(a, layer);
  const auto crop0 = Controller::crop_keys_of(a, layer);
  const auto speed0 = Controller::speed_keys_of(a, layer);
  const auto sound0 = Controller::sound_keys_of(a, layer);
  // Its box as it is: where a zoom or a pan starts from.
  media::PixelSize content, frame;
  media::Crop base = Controller::crop_of(a, layer);
  if (boxed) {
    VALTZ_ASSIGN(frame, _c.own_frame(s.project, comp));
    VALTZ_ASSIGN(content, _c.shown_size_(*p, *it->source,
                                         it->source_version, 0));
    base.content = content;
  }
  media::KeyedAdjustments look;
  media::Keyed<media::Crop> place;
  media::Keyed<media::Rotation> turn;
  media::KeyedSpeed speed;
  media::KeyedSound sound;
  look.rate = place.rate = turn.rate = speed.rate = sound.rate = rate;
  bool any_look = false, any_place = false, any_turn = false;
  bool any_speed = false, any_sound = false;
  std::map<std::int64_t, media::Interp> look_ease, place_ease, turn_ease;
  for (const Json& key : keys) {
    if (!key.is_object()) {
      return make_error(Code::InvalidArgument,
                        "each key is an object: {\"at\": 0, ...}");
    }
    std::int64_t f = 0;
    if (timeline) {
      const Json at = jget(key, "at", Json(0));
      double sec = at.is_string() && at == "end" ? length
                                                 : number(key, "at", 0);
      if (!std::isfinite(sec)) {
        return make_error(Code::InvalidArgument,
                          "it runs to the timeline's end: give `at` in "
                          "seconds");
      }
      if (std::isfinite(length)) {
        sec = std::min(sec, length);
      }
      f = frames_of(sec, fps);
    } else {
      if (!key.contains("page")) {
        return make_error(Code::InvalidArgument,
                          "a still's keys are on its pages: `page` (from "
                          "1) in each");
      }
      f = static_cast<std::int64_t>(number(key, "page", 1)) - 1 -
          static_cast<std::int64_t>(start);
      if (f < 0 || f >= static_cast<std::int64_t>(length)) {
        return make_error(Code::InvalidArgument, std::format(
            "layer {} lies on pages {}-{}", model_layer(layer),
            static_cast<std::int64_t>(start) + 1,
            static_cast<std::int64_t>(start + length)));
      }
    }
    const std::optional<media::Interp> ease = ease_of(key);
    const media::Interp go = ease.value_or(media::Interp::Linear);
    if (says_look(key)) {
      if (kind == "sound") {
        return make_error(Code::InvalidArgument, "a sound has no look");
      }
      VALTZ_ASSIGN(media::Adjustments v,
                   look_over(key, adj0.at(static_cast<double>(f))));
      look.keys.push_back({f, v, go});
      if (ease) {
        look_ease[f] = *ease;
      }
      any_look = true;
    }
    if (says_box(key)) {
      if (!boxed) {
        return make_error(Code::InvalidArgument,
                          "only a picture's or a clip's layer has a box");
      }
      media::Crop c = base;
      if (key.contains("x") || key.contains("y") || key.contains("width") ||
          key.contains("height")) {
        if (!(key.contains("x") && key.contains("y") &&
              key.contains("width") && key.contains("height"))) {
          return make_error(Code::InvalidArgument,
                            "a key's box is all four: x, y, width, "
                            "height");
        }
        VALTZ_ASSIGN(auto b, box_(key, frame));
        c = kept_turn_(base, box_crop_(content, frame, b));
      }
      const double z = number(key, "zoom", 1);
      if (!(z > 0)) {
        return make_error(Code::InvalidArgument,
                          "zoom is a factor above 0 (1: as it is)");
      }
      const double w = frame.width, h = frame.height;
      double cx = w / 2 + c.offset_x * w;
      double cy = h / 2 + c.offset_y * h;
      const double fx = number(key, "focus_x", cx);
      const double fy = number(key, "focus_y", cy);
      c.scale_x = media::held_zoom(c.scale_x * z);
      c.scale_y = media::held_zoom(c.scale_y * z);
      cx = fx + (cx - fx) * z + number(key, "dx", 0);
      cy = fy + (cy - fy) * z + number(key, "dy", 0);
      c.offset_x = (cx - w / 2) / std::max(1.0, w);
      c.offset_y = (cy - h / 2) / std::max(1.0, h);
      c.content = content;
      c.rotate = 0;
      place.keys.push_back({f, c, go});
      if (ease) {
        place_ease[f] = *ease;
      }
      any_place = true;
    }
    if (key.contains("rotate")) {
      if (!boxed) {
        return make_error(Code::InvalidArgument,
                          "only a picture's or a clip's layer turns");
      }
      turn.keys.push_back(
          {f,
           media::Rotation{std::clamp(number(key, "rotate", 0),
                                      -media::kMaxTurn, media::kMaxTurn)},
           go});
      if (ease) {
        turn_ease[f] = *ease;
      }
      any_turn = true;
    }
    if (key.contains("speed")) {
      if (!timed) {
        return make_error(Code::InvalidArgument,
                          "only a clip or a sound has a speed");
      }
      speed.keys.push_back(
          {f, media::Speed{std::clamp(number(key, "speed", 1), 0.1, 10.0)}});
      any_speed = true;
    }
    if (says_sound(key)) {
      if (!timed) {
        return make_error(Code::InvalidArgument,
                          "only a clip or a sound has a sound");
      }
      media::Sound v = sound0.at(static_cast<double>(f));
      v.volume = std::clamp(number(key, "volume", v.volume), 0.0, 2.0);
      v.pitch = std::clamp(number(key, "pitch", v.pitch),
                           -media::kMaxPitch, media::kMaxPitch);
      if (key.contains("eq")) {
        VALTZ_ASSIGN(v.eq, eq_over(key["eq"], v.eq));
      }
      sound.keys.push_back({f, v});
      any_sound = true;
    }
  }
  if (!any_look && !any_place && !any_turn && !any_speed && !any_sound) {
    return make_error(Code::InvalidArgument,
                      "the keys change nothing: give a look (exposure, "
                      "opacity, ...), a box (zoom, dx, x, ...), rotate, "
                      "speed or volume in them");
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  // Each track a key names is set from these keys; the others stay.
  const auto when = [&](std::int64_t f) {
    return timeline ? Json(secs(static_cast<double>(f) / fps))
                    : Json(static_cast<std::int64_t>(start) + f + 1);
  };
  const char* at_key = timeline ? "at" : "page";
  Json told = Json::object();
  if (any_look) {
    tidy_keys(look, look_ease);
    VALTZ_TRY(_c.set_adjustment_keys(s.project, comp, look, layer));
    for (const auto& k : look.keys) {
      Json j = look_said(k.value);
      j[at_key] = when(k.frame);
      told["look"].push_back(std::move(j));
    }
  }
  if (any_place || any_turn) {
    media::KeyedCrop kc = crop0;
    if (any_place) {
      tidy_keys(place, place_ease);
      kc.place = place;
    } else if (kc.place.empty()) {
      base.rotate = 0;
      kc.place.keys.push_back({0, base});
    }
    if (any_turn) {
      tidy_keys(turn, turn_ease);
      kc.turn = turn;
    } else if (kc.turn.empty()) {
      kc.turn.keys.push_back({0, media::Rotation{base.rotate}});
    }
    kc.rate = kc.place.rate = kc.turn.rate = rate;
    VALTZ_TRY(_c.set_crop_keys(s.project, comp, kc, layer));
    if (any_place) {
      for (const auto& k : kc.place.keys) {
        told["box"].push_back({{at_key, when(k.frame)},
                               {"box", crop_box(k.value, content, frame)}});
      }
    }
    if (any_turn) {
      for (const auto& k : kc.turn.keys) {
        told["turn"].push_back(
            {{at_key, when(k.frame)}, {"rotate", k.value.degrees}});
      }
    }
  }
  if (any_speed) {
    tidy_keys(speed);
    VALTZ_TRY(_c.set_speed_keys(s.project, comp, speed, layer));
    for (const auto& k : speed.keys) {
      told["speed"].push_back(
          {{at_key, when(k.frame)}, {"speed", k.value.rate}});
    }
  }
  if (any_sound) {
    tidy_keys(sound);
    VALTZ_TRY(_c.set_sound_keys(s.project, comp, sound, layer));
    for (const auto& k : sound.keys) {
      Json j = {{at_key, when(k.frame)},
                {"volume", std::round(k.value.volume * 100) / 100}};
      if (k.value.pitch != 0) {
        j["pitch"] = k.value.pitch;
      }
      if (!k.value.flat()) {
        j["eq"] = eq_said(k.value.eq);
      }
      told["sound"].push_back(std::move(j));
    }
  }
  Json r = {{"layer", model_layer(layer)}, {"keyed", std::move(told)}};
  if (timeline) {
    r["starts"] = secs(start);
    if (std::isfinite(length)) {
      r["runs"] = secs(length);
    }
  }
  return r;
}

// ---- layers: folders, copies, merges, what they show -----------------------


Result<Json>
Controller::AgentHost::t_group_layers(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  const auto layers = layer_ids(args, "layers");
  if (layers.empty()) {
    return make_error(Code::InvalidArgument, "which layers?");
  }
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(std::string folder,
               _c.group_layers(s.project, comp, layers,
                               one_line(text_arg(args, "name"))));
  Json ids = Json::array();
  for (const auto& l : layers) {
    ids.push_back(model_layer(l));
  }
  return Json{{"folder", folder}, {"layers", std::move(ids)},
              {"note", "A folder's layers lie together in the stack; "
                       "set_layer_visible and rename_layer take its id."}};
}

Result<Json>
Controller::AgentHost::t_ungroup_layers(Session& s, Step& step,
                                        const Json& args,
                                        std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  const std::string folder = text_arg(args, "folder");
  auto undo = _c.command_(s.project, "agent", comp, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.ungroup_layers(s.project, comp, folder));
  return Json{{"ungrouped", folder}};
}

Result<Json>
Controller::AgentHost::t_duplicate_layer(Session& s, Step& step,
                                         const Json& args,
                                         std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(std::string copy,
               _c.duplicate_layer(s.project, comp, layer,
                                  one_line(text_arg(args, "name"))));
  return Json{{"layer", model_layer(copy)}, {"copy_of", model_layer(layer)},
              {"note", "Right above it, the same look, box and time -- "
                       "a markup's objects its own."}};
}

Result<Json>
Controller::AgentHost::t_merge_layers(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  const auto layers = layer_ids(args, "layers");
  if (layers.size() != 2 || layers[0] == layers[1]) {
    return make_error(Code::InvalidArgument,
                      "merge_layers takes two layers");
  }
  auto* p = _c.project(s.project);
  VALTZ_ASSIGN(project::Asset a, p->asset(comp));
  // The lower one shows the two made one.
  auto pos = [&](const std::string& id) {
    return std::ranges::find(a.layers, id, &project::Layer::id) -
           a.layers.begin();
  };
  const std::string lower =
      pos(layers[0]) < pos(layers[1]) ? layers[0] : layers[1];
  auto undo = _c.command_(s.project, "agent", comp, lower,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.merge_layers(s.project, comp, layers[0], layers[1]));
  return Json{{"layer", model_layer(lower)},
              {"note", "The two are one flat picture now, on the lower "
                       "layer; the upper is gone."}};
}

Result<Json>
Controller::AgentHost::t_set_layer_source(Session& s, Step& step,
                                          const Json& args,
                                          std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  const auto t = text_arg(args, "asset");
  const auto id = AssetId::parse(t);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      std::format("'{}' is not an asset id", t));
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_layer_source(s.project, comp, layer, *id));
  Json r = {{"layer", model_layer(layer)}, {"shows", id->str()}};
  auto* p = _c.project(s.project);
  if (auto a = p->asset(comp); a.ok()) {
    if (const auto it =
            std::ranges::find(a->layers, layer, &project::Layer::id);
        it != a->layers.end()) {
      if (Json b = layer_box_(s.project, *a, *it); b.is_object()) {
        r["box"] = std::move(b);
      }
    }
  }
  return r;
}

Result<Json>
Controller::AgentHost::t_lock_layer(Session& s, Step& step, const Json& args,
                                    std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  const bool locked = flag(args, "locked", true);
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_layer_locked(s.project, comp, layer, locked));
  return Json{{"layer", model_layer(layer)}, {"locked", locked}};
}

Result<Json>
Controller::AgentHost::t_flatten_layer(Session& s, Step& step,
                                       const Json& args,
                                       std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(AssetId flat, _c.flatten_layer(s.project, comp, layer));
  return Json{{"layer", model_layer(layer)}, {"shows", flat.str()},
              {"note", "It shows a flat copy of what it showed, its look "
                       "and box kept: what upscale_layer takes."}};
}

Result<Json>
Controller::AgentHost::t_decompose_layer(Session& s, Step& step,
                                         const Json& args,
                                         std::vector<fs::path>&)
{
  VALTZ_ASSIGN(AssetId comp, composition_(s, args));
  VALTZ_ASSIGN(std::string layer, layer_(args));
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.decompose(s.project, comp, layer));
  return Json{{"decomposed", model_layer(layer)},
              {"note", "Its composition's layers are in its place: "
                       "get_composition lists them."}};
}

// ---- assets: names, folders, removal; a frame grabbed; a recipe ------------

Result<Json>
Controller::AgentHost::t_rename_asset(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  const auto t = text_arg(args, "asset");
  const auto id = AssetId::parse(t);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      std::format("'{}' is not an asset id", t));
  }
  const std::string name = one_line(text_arg(args, "name"));
  if (name.empty()) {
    return make_error(Code::InvalidArgument, "its new name?");
  }
  auto undo = _c.command_(s.project, "agent", {}, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.rename_asset(s.project, *id, name));
  return Json{{"asset", id->str()}, {"name", name}};
}

Result<Json>
Controller::AgentHost::t_file_assets(Session& s, Step& step,
                                     const Json& args,
                                     std::vector<fs::path>&)
{
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  std::vector<AssetId> ids;
  for (const auto& x : jget(args, "assets", Json::array())) {
    const auto id = x.is_string() ? AssetId::parse(x.get<std::string>())
                                  : std::nullopt;
    if (!id) {
      return make_error(Code::InvalidArgument,
                        std::format("'{}' is not an asset id", to_text(x)));
    }
    ids.push_back(*id);
  }
  if (ids.empty()) {
    return make_error(Code::InvalidArgument, "which assets?");
  }
  const std::string name = one_line(text_arg(args, "folder"));
  auto undo = _c.command_(s.project, "agent", {}, "",
                          {{"tool", step.tool}, {"title", step.title}});
  // The folder of that name (made when there is none); none: the top.
  std::string folder;
  bool made = false;
  if (!name.empty()) {
    VALTZ_ASSIGN(Json folders, p->folders());
    for (const auto& f : folders) {
      if (jget<std::string>(f, "name", "") == name) {
        folder = jget<std::string>(f, "id", "");
      }
    }
    if (folder.empty()) {
      VALTZ_ASSIGN(folder, _c.create_folder(s.project, name));
      made = true;
    }
  }
  for (const auto& id : ids) {
    VALTZ_TRY(_c.move_asset(s.project, id, folder));
  }
  return Json{{"moved", ids.size()},
              {"folder", name.empty() ? Json("the top of the list")
                                      : Json(name)},
              {"made", made}};
}

Result<Json>
Controller::AgentHost::t_remove_asset(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>&)
{
  const auto t = text_arg(args, "asset");
  const auto id = AssetId::parse(t);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      std::format("'{}' is not an asset id", t));
  }
  auto undo = _c.command_(s.project, "agent", {}, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.remove_asset(s.project, *id));
  return Json{{"removed", id->str()}};
}

// A FRAME of a clip or a timeline as a picture (Grab Frame): a still
// composition showing it from that moment -- a shot's `first`, a
// reference, a cover.
Result<Json>
Controller::AgentHost::t_grab_frame(Session& s, Step& step, const Json& args,
                                    std::vector<fs::path>&)
{
  const auto t = text_arg(args, "asset");
  const auto id = AssetId::parse(t);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      std::format("'{}' is not an asset id", t));
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(*id));
  if (a.kind != AssetKind::Video) {
    return make_error(Code::InvalidArgument,
                      a.kind == AssetKind::Image
                          ? std::format("\"{}\" is a picture already: "
                                        "give its id as it is",
                                        a.name)
                          : std::format("\"{}\" is not a clip or a "
                                        "timeline",
                                        a.name));
  }
  const double whole = source_seconds(_c, *p, a);
  const Json at = jget(args, "at", Json(0));
  double sec = at.is_string() && at == "end" ? whole : number(args, "at", 0);
  sec = std::clamp(sec, 0.0, std::max(0.0, whole));
  const double fps = mark_rate_(s.project, *id, false).to_double();
  std::int64_t f = frames_of(sec, fps);
  if (whole > 0) {
    f = std::min<std::int64_t>(
        f, std::max<std::int64_t>(0, frames_of(whole, fps) - 1));
  }
  auto undo = _c.command_(s.project, "agent", {}, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(AssetId still,
               _c.grab_frame(s.project, *id, f,
                             one_line(text_arg(args, "name"))));
  return Json{{"asset", still.str()},
              {"at", secs(static_cast<double>(f) / fps)},
              {"note", "A picture of that frame, in Assets: a shot's "
                       "`first`, a reference, or place_asset."}};
}

// What MADE an asset: its model, prompt, seed, size, Favor and options,
// what it drew on -- to make it again, or another take of it.
Result<Json>
Controller::AgentHost::t_get_recipe(Session& s, Step&, const Json& args,
                                    std::vector<fs::path>&)
{
  const auto t = text_arg(args, "asset");
  const auto id = AssetId::parse(t);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      std::format("'{}' is not an asset id", t));
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(*id));
  Json out = {{"asset", id->str()}, {"name", a.name}};
  auto r = p->recipe(a.recipe);
  if (!r.ok() || r->op.empty()) {
    out["made_by"] = a.cls == AssetClass::Flat ? "imported or made flat"
                                               : "made by hand";
    return out;
  }
  out["made_by"] = r->op;
  if (!r->model.empty()) {
    const auto* m = _c._catalog.find(r->model);
    out["model"] = {{"id", r->model}, {"name", m ? m->name : r->model}};
  }
  for (const char* k : {"prompt", "seed", "width", "height", "frames",
                        "fps", "steps", "preference", "max_seconds",
                        "lyrics", "reference_sound", "keep_latents"}) {
    if (r->params.contains(k)) {
      out[k] = r->params[k];
    }
  }
  if (r->params.contains("frames") && r->params.contains("fps")) {
    const double fps = jget(r->params, "fps", 24.0);
    if (fps > 0) {
      out["seconds"] = secs(jget(r->params, "frames", 0.0) / fps);
    }
  }
  if (const Json tu = jget(r->params, "tuning", Json());
      tu.is_object() && !tu.empty()) {
    out["options"] = tu;
  }
  Json inputs = Json::array();
  for (const auto& in : r->inputs) {
    Json j = {{"role", in.role}, {"asset", in.asset.str()}};
    if (auto x = p->asset(in.asset); x.ok()) {
      j["name"] = x->name;
    }
    inputs.push_back(std::move(j));
  }
  if (!inputs.empty()) {
    out["made_from"] = std::move(inputs);
  }
  out["note"] = "To make it again, or another take: the same model, "
                "prompt and size to its tool, with this `seed` (the same "
                "result) or another.";
  return out;
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

Result<Json>
Controller::AgentHost::t_save_text(Session& s, Step& step, const Json& args,
                                   std::vector<fs::path>&)
{
  const std::string text = text_arg(args, "text");
  if (one_line(text).empty()) {
    return make_error(Code::InvalidArgument, "the text is empty");
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  std::string name = one_line(text_arg(args, "name"));
  if (name.empty()) {
    name = std::string(utf8_prefix(one_line(text), 48));
  }
  auto undo = _c.command_(s.project, "agent", {}, "",
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_ASSIGN(project::Asset made, p->add_text(name, text, {}));
  _c.post_("assets.changed", JobId{}, {{"project", s.project},
                                       {"asset", made.id},
                                       {"reason", "agent"}});
  return Json{{"asset", made.id.str()}, {"name", made.name}};
}

Result<Json>
Controller::AgentHost::t_chat_picture(Session& s, Step& step,
                                      const Json& args,
                                      std::vector<fs::path>& shown)
{
  // Placed nowhere: a box, a page or a composition it was given anyway
  // (the editor's generate_image takes them) left out.
  Json a = args;
  for (const char* k : {"composition", "page", "below_all", "x", "y",
                        "width", "height"}) {
    a.erase(k);
  }
  return t_generate_image(s, step, a, shown);
}

// ---- Favor and its options -------------------------------------------------

Json
Controller::AgentHost::runs_with_(const Json& values,
                                  std::string_view preference,
                                  const Json& lora_names) const
{
  Json r = {{"favor", favor_label(preference)},
            {"steps", jget(values, "steps", 0)}};
  Json on = Json::array();
  for (const char* k : {"pika_flex", "hyperflow", "taomate", "taomate_lora",
                        "vdn", "motion_cache", "sol_attn", "sage_attn",
                        "sage_pv8", "i8_gemm", "vae_i8_gemm", "ane_ffn"}) {
    if (jget(values, k, false)) {
      on.push_back(option_words(k)->label);
    }
  }
  if (!on.empty()) {
    r["with"] = std::move(on);
  }
  Json loras = Json::array();
  for (const auto& l : jget(values, "loras", Json::array())) {
    if (!jget(l, "on", false)) {
      continue;
    }
    const auto path = jget<std::string>(l, "path", "");
    loras.push_back(jget<std::string>(lora_names, path.c_str(),
                                      fs::path(path).stem().string()));
  }
  if (!loras.empty()) {
    r["loras"] = std::move(loras);
  }
  return r;
}

void
Controller::AgentHost::told_(Json& result, const Tuned& t)
{
  if (!t.given) {
    return;
  }
  result["runs_with"] = t.runs;
  if (!t.read.empty()) {
    result["options_read"] = t.read;
  }
  if (!t.notes.empty()) {
    std::string n;
    for (const auto& x : t.notes) {
      n += (n.empty() ? "" : " ") + x;
    }
    result["options_note"] = n;
  }
}

Result<Controller::AgentHost::Tuned>
Controller::AgentHost::tuned_(const models::ModelEntry& m, const Json& args,
                              bool edit, bool continuation,
                              bool opens_on_picture) const
{
  Tuned t;
  if (const std::string f = option_norm(text_arg(args, "favor"));
      !f.empty()) {
    t.given = true;
    if (f == "fast" || f == "speed" || f == "quick" || f == "draft" ||
        f == "fastest") {
      t.preference = "speed";
    } else if (f == "med" || f == "medium" || f == "balanced" ||
               f == "default" || f == "normal" || f == "custom") {
      t.preference = "balanced";
    } else if (f == "fine" || f == "quality" || f == "best" ||
               f == "slow" || f == "high quality" || f == "finest") {
      t.preference = "quality";
    } else {
      return make_error(Code::InvalidArgument, std::format(
          "favor is fast, med or fine -- not \"{}\"",
          text_arg(args, "favor")));
    }
  }
  const auto asked = named_options(
      args.is_object() && args.contains("options") ? args["options"]
                                                   : Json());
  VALTZ_ASSIGN(Json offered, _c.tuning(m.id, t.preference, edit,
                                       Json::object(), continuation));
  const Json values = jget(offered, "values", Json::object());
  // What it offers, by key; its few-step LoRAs (the loras list's).
  std::map<std::string, Json> by_key;
  std::vector<std::string> order;  // as Custom's panel lists them
  Json lora_opt = Json::object();
  for (const auto& o : jget(offered, "options", Json::array())) {
    const auto key = jget<std::string>(o, "key", "");
    if (key == "loras") {
      lora_opt = o;
    } else if (!key.empty() && key != "dits" && key != "vaes" &&
               !key.starts_with(models::kRegeneratePrefix)) {
      by_key[key] = o;
      order.push_back(key);
    }
  }
  const Json names = jget(lora_opt, "names", Json::object());
  if (asked.empty()) {
    t.runs = runs_with_(values, t.preference, names);
    return t;
  }
  t.given = true;
  auto label_of = [&](const std::string& key) -> std::string {
    if (const auto* w = option_words(key)) {
      return w->label;
    }
    const auto it = by_key.find(key);
    return it == by_key.end()
               ? key
               : jget<std::string>(it->second, "label", key);
  };
  // The words each is said by: its key, Custom's label, the words people
  // use -- and an adapter's own catalog id and name, its switch's.
  struct Said {
    std::string key;   // an option's
    std::string fast;  // a few-step LoRA's id ("*": any of them)
    std::string label;
    std::string words;
  };
  std::vector<Said> said;
  const Json vp = jget(m.engine, "vpipe", Json::object());
  for (const auto& key : order) {
    std::string words = key + "|" + label_of(key);
    if (const auto* w = option_words(key)) {
      words += std::string("|") + w->words;
    }
    if (const Json b = jget(vp, key.c_str(), Json()); b.is_object()) {
      if (const auto id = jget<std::string>(b, "lora", ""); !id.empty()) {
        words += "|" + id;
        if (const auto* e = _c._catalog.find(id)) {
          words += "|" + e->name;
        }
      }
    }
    said.push_back({key, "", label_of(key), std::move(words)});
  }
  const Json fast = jget(lora_opt, "fast", Json::array());
  for (const auto& f : fast) {
    const auto id = jget<std::string>(f, "id", "");
    const auto name = jget<std::string>(f, "name", id);
    std::string words = id + "|" + name;
    if (const int n = jget(f, "steps", 0); n > 0) {
      words += std::format("|{0} step|{0} steps|{0} step lora|{0} step "
                           "turbo|turbo {0} step|{0} step turbo lora|{0} "
                           "step lightx2v|lightx2v {0} step|{0} step "
                           "distill",
                           n);
    }
    said.push_back({"", id, name, std::move(words)});
  }
  if (!fast.empty()) {
    said.push_back({"", "*", "a few-step LoRA", kFastWords});
  }
  // What it offers, for a refusal.
  std::string offers;
  for (const auto& x : said) {
    if (x.fast != "*") {
      offers += (offers.empty() ? "" : ", ") + x.label;
    }
  }
  Json over = Json::object();
  std::set<std::string> asked_keys;
  std::optional<std::string> fast_pick;
  bool fast_none = false;
  for (const auto& [name, value] : asked) {
    std::string n = option_norm(name);
    bool neg = false;
    for (const std::string_view pre :
         {"no ", "without ", "not ", "disable ", "turn off ", "off ",
          "dont use ", "don t use "}) {
      if (n.starts_with(pre)) {
        neg = true;
        n.erase(0, pre.size());
      }
    }
    for (const std::string_view pre : {"enable ", "turn on ", "on "}) {
      if (n.starts_with(pre)) {
        n.erase(0, pre.size());
      }
    }
    for (const std::string_view suf : {" off", " disabled"}) {
      if (n.ends_with(suf)) {
        neg = true;
        n.erase(n.size() - suf.size());
      }
    }
    for (const std::string_view suf : {" on", " enabled"}) {
      if (n.ends_with(suf)) {
        n.erase(n.size() - suf.size());
      }
    }
    if (n.empty()) {
      continue;
    }
    std::size_t best = 0;
    std::vector<const Said*> hits;
    for (const auto& x : said) {
      const auto score = name_score(n, x.words);
      if (score > best) {
        best = score;
        hits = {&x};
      } else if (score == best && score > 0) {
        hits.push_back(&x);
      }
    }
    if (hits.empty()) {
      // An option a model of another family has: whose.
      std::string whose;
      for (const auto& w : kOptionWords) {
        if (name_score(n, std::string(w.key) + "|" + w.label + "|" +
                              w.words) >= 10000) {
          std::string models;
          for (const auto& e : _c._catalog.models()) {
            const Json evp = jget(e.engine, "vpipe", Json::object());
            if (evp.contains(w.key) && e.quantize_from.empty() &&
                e.role != "lora") {
              models += (models.empty() ? "" : ", ") + e.name;
            }
          }
          if (!models.empty()) {
            whose = std::format(" ({} is {}'s)", w.label, models);
          }
        }
      }
      return make_error(Code::InvalidArgument, std::format(
          "{} has no option \"{}\"{}. It offers: {} (model_options)",
          m.name, name, whose, offers));
    }
    if (hits.size() > 1) {
      std::string which;
      for (const auto* h : hits) {
        which += (which.empty() ? "" : " or ") + h->label;
      }
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" could be {}: name one", name, which));
    }
    const Said& h = *hits.front();
    Json read = {{"asked", name}, {"as", h.label}};
    if (!h.fast.empty()) {
      const auto on = neg ? std::optional<bool>(false) : on_off(value);
      if (!on) {
        return make_error(Code::InvalidArgument, std::format(
            "{} is on or off", h.label));
      }
      if (*on) {
        if (fast_pick && *fast_pick != h.fast) {
          return make_error(Code::InvalidArgument,
                            "two few-step LoRAs asked for: name one");
        }
        fast_pick = h.fast;
      } else {
        fast_none = true;
      }
      read["value"] = *on;
      t.read.push_back(std::move(read));
      continue;
    }
    const Json& o = by_key[h.key];
    const auto type = jget<std::string>(o, "type", "bool");
    Json v;
    if (type == "bool") {
      const auto on = neg ? std::optional<bool>(false) : on_off(value);
      if (!on) {
        return make_error(Code::InvalidArgument, std::format(
            "{} is on or off", h.label));
      }
      v = *on;
    } else if (neg || value.is_null()) {
      return make_error(Code::InvalidArgument, std::format(
          "{} takes a value: {{\"{}\": ...}}", h.label, h.key));
    } else if (type == "choice") {
      const auto c = choice_said(jget(o, "choices", Json::array()), value);
      if (c.empty()) {
        std::string cs;
        for (const auto& x : jget(o, "choices", Json::array())) {
          cs += (cs.empty() ? "" : ", ") + x.get<std::string>();
        }
        return make_error(Code::InvalidArgument, std::format(
            "{} is one of: {}", h.label, cs));
      }
      v = c;
    } else if (type == "int" || type == "real") {
      std::optional<double> d;
      if (value.is_number()) {
        d = value.get<double>();
      } else if (value.is_string()) {
        const auto txt = value.get<std::string>();
        if (jget(o, "schedule", false) &&
            txt.find(',') != std::string::npos) {
          v = txt;  // a value a step: held to its range as it settles
        } else {
          char* end = nullptr;
          const double x = std::strtod(txt.c_str(), &end);
          if (end != txt.c_str()) {
            d = x;
          }
        }
      }
      if (d) {
        const double lo = jget(o, "min", *d), hi = jget(o, "max", *d);
        if (*d < lo || *d > hi) {
          return make_error(Code::InvalidArgument, std::format(
              "{} is {} to {} on {}", h.label, lo, hi, m.name));
        }
        v = type == "int" ? Json(std::lround(*d)) : Json(*d);
      } else if (v.is_null()) {
        return make_error(Code::InvalidArgument, std::format(
            "{} takes a number: {{\"{}\": ...}}", h.label, h.key));
      }
    } else {
      return make_error(Code::InvalidArgument, std::format(
          "{} is not set by name", h.label));
    }
    // Not to be had here: said, not dropped.
    const bool turns_on = !(v.is_boolean() && !v.get<bool>());
    if (turns_on && !jget(o, "available", true)) {
      const auto why = jget<std::string>(o, "why", "");
      if (why == "not-installed") {
        std::string what = h.label;
        if (const Json b = jget(vp, h.key.c_str(), Json()); b.is_object()) {
          if (const auto* e = _c._catalog.find(
                  jget<std::string>(b, "lora", jget<std::string>(
                                                   b, "branch", "")))) {
            what = e->name;
          }
        }
        return make_error(Code::Unsupported, std::format(
            "{} is not on this Mac: {} is not downloaded -- the person "
            "can download it in Settings > Capabilities. Tell them, and "
            "make nothing without it unless they say so",
            h.label, what));
      }
      if (why == "needs-matrix-cores") {
        return make_error(Code::Unsupported, std::format(
            "{} needs an M5's matrix cores, which this Mac has not",
            h.label));
      }
      if (why == "own-schedule") {
        t.notes.push_back(std::format(
            "{} is the adapter's own here: left as it is.", h.label));
        read["value"] = v;
        read["kept"] = false;
        t.read.push_back(std::move(read));
        continue;
      }
      return make_error(Code::Unsupported, std::format(
          "{} cannot run here{}", h.label,
          why.empty() ? std::string() : " (" + why + ")"));
    }
    over[h.key] = v;
    asked_keys.insert(h.key);
    read["value"] = v;
    t.read.push_back(std::move(read));
  }
  // Two that do not go together: asked of the person, not chosen.
  std::vector<std::string> on;
  for (const auto& k : asked_keys) {
    if (over[k].is_boolean() && over[k].get<bool>()) {
      on.push_back(k);
    }
  }
  for (const auto& a : on) {
    for (const auto& x : jget(by_key[a], "excludes", Json::array())) {
      if (x.is_string() && std::ranges::find(on, x.get<std::string>()) !=
                               on.end()) {
        return make_error(Code::InvalidArgument, std::format(
            "{} and {} do not go together -- each brings its own steps, "
            "or one turns the other off: ask the person which",
            label_of(a), label_of(x.get<std::string>())));
      }
    }
  }
  if (fast_pick) {
    for (const char* k : {"hyperflow", "pika_flex", "taomate"}) {
      if (std::ranges::find(on, k) != on.end()) {
        return make_error(Code::InvalidArgument, std::format(
            "a few-step LoRA and {} do not go together: {} brings its own "
            "adapter and steps in the LoRA's place -- ask the person which",
            label_of(k), label_of(k)));
      }
    }
  }
  // TaoMate's streaming makes a clip from words alone: opening on a
  // picture, its adapter as a plain LoRA (the request would drop it).
  if (jget(over, "taomate", false) && opens_on_picture &&
      !over.contains("taomate_lora")) {
    const auto it = by_key.find("taomate_lora");
    if (it == by_key.end() || !jget(it->second, "available", false)) {
      return make_error(Code::Unsupported,
                        "TaoMate makes a shot from words alone: not one "
                        "that opens on a picture");
    }
    over["taomate_lora"] = true;
    t.notes.push_back("TaoMate streams a shot made from words alone: this "
                      "one opens on a picture, so its adapter runs as a "
                      "plain 3-step LoRA (TaoMate as a LoRA only).");
  }
  // A few-step LoRA picked -- or none: the others off, the one picked
  // first and on, at the steps and shifts it was distilled for (Custom's
  // Acceleration pick).
  if (fast_pick || fast_none) {
    std::set<std::string> paths;
    for (const auto& f : fast) {
      if (const auto p = jget<std::string>(f, "path", ""); !p.empty()) {
        paths.insert(p);
      }
    }
    if (const auto tp = jget<std::string>(lora_opt, "turbo", "");
        !tp.empty()) {
      paths.insert(tp);
    }
    Json ls = jget(values, "loras", Json::array());
    for (auto& l : ls) {
      if (paths.contains(jget<std::string>(l, "path", ""))) {
        l["on"] = false;
      }
    }
    const Json* chosen = nullptr;
    if (fast_pick) {
      const auto preset = jget<std::string>(
          jget(offered, "turbo", Json::object()), "id", "");
      for (const auto& f : fast) {
        const auto id = jget<std::string>(f, "id", "");
        const bool here = !jget<std::string>(f, "path", "").empty();
        if (*fast_pick == "*" ? id == preset && here : id == *fast_pick) {
          chosen = &f;
        }
      }
      // Any of them: the fastest here (fewest steps first).
      for (const auto& f : fast) {
        if (!chosen && *fast_pick == "*" &&
            !jget<std::string>(f, "path", "").empty()) {
          chosen = &f;
        }
      }
      if (!chosen || jget<std::string>(*chosen, "path", "").empty()) {
        std::string all;
        for (const auto& f : fast) {
          all += (all.empty() ? "" : ", ") +
                 jget<std::string>(f, "name", "");
        }
        return make_error(Code::Unsupported, std::format(
            "{} is not on this Mac: not downloaded ({}'s few-step LoRAs: "
            "{}) -- the person can download it in Settings > "
            "Capabilities",
            chosen ? jget<std::string>(*chosen, "name", "")
                   : std::string("none of them"),
            m.name, all));
      }
      const auto path = jget<std::string>(*chosen, "path", "");
      Json entry = {{"path", path}, {"scale", 1.0}, {"on", true}};
      for (auto it = ls.begin(); it != ls.end(); ++it) {
        if (jget<std::string>(*it, "path", "") == path) {
          entry = *it;
          entry["on"] = true;
          ls.erase(it);
          break;
        }
      }
      ls.insert(ls.begin(), entry);
      if (!over.contains("steps")) {
        over["steps"] = jget(*chosen, "steps", jget(values, "steps", 8));
      }
      for (auto& r : t.read) {
        if (r["as"] == "a few-step LoRA") {
          r["as"] = jget<std::string>(*chosen, "name", "");
        }
      }
    } else if (!over.contains("steps") && lora_opt.contains("steps_off")) {
      over["steps"] = lora_opt["steps_off"];
    }
    const Json shifts = chosen ? jget(*chosen, "config", Json::object())
                               : jget(lora_opt, "shift_off",
                                      Json::object());
    for (const char* k : {"video_shift", "audio_shift", "shift"}) {
      if (values.contains(k) && !over.contains(k) && shifts.contains(k)) {
        over[k] = shifts[k];
      }
    }
    for (const char* k : {"hyperflow", "pika_flex", "taomate",
                          "taomate_lora"}) {
      if (by_key.contains(k) && !over.contains(k)) {
        over[k] = false;
      }
    }
    over["loras"] = std::move(ls);
  }
  // As it settles: what was asked and did not stay so, said.
  VALTZ_ASSIGN(Json settled, _c.tuning(m.id, t.preference, edit, over,
                                       continuation));
  const Json sv = jget(settled, "values", Json::object());
  for (const auto& k : asked_keys) {
    const Json want = over[k];
    const Json got = jget(sv, k.c_str(), Json());
    const bool same =
        want.is_number() && got.is_number()
            ? std::abs(want.get<double>() - got.get<double>()) < 1e-9
            : want == got;
    if (same || want.is_string()) {
      continue;
    }
    // Set by a switch that is on (Pika H3 Flex's own steps, ...).
    std::string by;
    for (const auto& [key, o] : by_key) {
      if (jget(sv, key.c_str(), false) == true &&
          jget(o, "fixes", Json::object()).contains(k)) {
        by = label_of(key);
      }
    }
    t.notes.push_back(std::format(
        "{} runs at {} here, not {}{}.", label_of(k), to_text(got),
        to_text(want), by.empty() ? "" : " (" + by + " sets it)"));
  }
  t.tuning = std::move(over);
  t.runs = runs_with_(sv, t.preference, names);
  return t;
}

Result<Json>
Controller::AgentHost::t_model_options(Session&, Step&, const Json& args,
                                       std::vector<fs::path>&)
{
  std::string task = text_arg(args, "task");
  const std::string named = text_arg(args, "model");
  if (!task.empty() && !model_task(task)) {
    return make_error(Code::InvalidArgument,
                      "the task is picture, edit, shot, shot_references, "
                      "music or voiceover");
  }
  const models::ModelEntry* m = nullptr;
  if (task.empty()) {
    if (named.empty()) {
      return make_error(Code::InvalidArgument,
                        "which model (its id or name), or which task?");
    }
    // The first task the model named makes.
    std::optional<Error> first;
    for (const char* tk : {"shot", "shot_references", "picture", "edit",
                           "music", "voiceover"}) {
      auto r = model_for_(tk, named);
      if (r.ok() && *r) {
        m = *r;
        task = tk;
        break;
      }
      if (!r.ok() && !first) {
        first = r.error();
      }
    }
    if (!m) {
      if (first) {
        return *first;
      }
      return make_error(Code::NotFound, std::format(
          "no model is named \"{}\" (list_models)", named));
    }
  } else {
    VALTZ_ASSIGN(m, model_for_(task, named));
    if (!m) {
      const auto autos = auto_for_(task);
      m = autos.empty() ? nullptr : autos.front();
    }
  }
  if (!m) {
    return make_error(Code::Unsupported,
                      std::format("no model for {} runs here", task));
  }
  const bool edit = task == "edit";
  Json favor = Json::object();
  for (const char* pref : {"speed", "balanced", "quality"}) {
    VALTZ_ASSIGN(Json o, _c.tuning(m->id, pref, edit));
    Json names = Json::object();
    for (const auto& x : jget(o, "options", Json::array())) {
      if (x["key"] == "loras") {
        names = jget(x, "names", Json::object());
      }
    }
    Json f = runs_with_(jget(o, "values", Json::object()), pref, names);
    f.erase("favor");
    Json needs = Json::array();
    for (const auto& x : jget(o, "missing", Json::array())) {
      needs.push_back(jget<std::string>(x, "name", ""));
    }
    if (!needs.empty()) {
      f["needs_download"] = std::move(needs);
    }
    std::string lower = favor_label(pref);
    lower[0] = static_cast<char>(std::tolower(
        static_cast<unsigned char>(lower[0])));
    favor[lower] = std::move(f);
  }
  VALTZ_ASSIGN(Json med, _c.tuning(m->id, "balanced", edit));
  const Json values = jget(med, "values", Json::object());
  Json opts = Json::array(), few = Json::array();
  for (const auto& o : jget(med, "options", Json::array())) {
    const auto key = jget<std::string>(o, "key", "");
    if (key == "loras") {
      for (const auto& f : jget(o, "fast", Json::array())) {
        few.push_back({{"name", jget<std::string>(f, "name", "")},
                       {"id", jget<std::string>(f, "id", "")},
                       {"steps", jget(f, "steps", 0)},
                       {"downloaded",
                        !jget<std::string>(f, "path", "").empty()}});
      }
      continue;
    }
    if (key.empty() || key == "dits" || key == "vaes" ||
        key.starts_with(models::kRegeneratePrefix)) {
      continue;
    }
    const auto* w = option_words(key);
    Json j = {{"name", w ? std::string(w->label)
                         : jget<std::string>(o, "label", key)},
              {"key", key}};
    if (w) {
      j["what"] = w->what;
    } else if (const auto note = jget<std::string>(o, "note", "");
               !note.empty()) {
      j["what"] = note;
    }
    const auto type = jget<std::string>(o, "type", "bool");
    if (type != "bool") {
      j["type"] = type;
    }
    if (type == "int" || type == "real") {
      j["range"] = {jget(o, "min", 0.0), jget(o, "max", 0.0)};
    }
    if (type == "choice") {
      j["choices"] = jget(o, "choices", Json::array());
    }
    if (values.contains(key)) {
      j["med"] = values[key];
    }
    if (!jget(o, "available", true)) {
      const auto why = jget<std::string>(o, "why", "");
      j["not_here"] = why == "not-installed" ? "not downloaded"
                      : why == "needs-matrix-cores"
                          ? "needs an M5's matrix cores"
                      : why == "own-schedule" ? "set by the adapter on"
                                              : why;
    }
    Json off = Json::array();
    for (const auto& x : jget(o, "excludes", Json::array())) {
      if (x.is_string()) {
        const auto* xw = option_words(x.get<std::string>());
        off.push_back(xw ? std::string(xw->label) : x.get<std::string>());
      }
    }
    if (!off.empty()) {
      j["turns_off"] = std::move(off);
    }
    opts.push_back(std::move(j));
  }
  Json r = {{"model", m->id}, {"name", m->name}, {"task", task},
            {"favor", std::move(favor)}, {"options", std::move(opts)}};
  if (!few.empty()) {
    r["few_step_loras"] = std::move(few);
  }
  r["note"] =
      "Ask in the tool's `favor` (fast, med, fine) and `options`, by name: "
      "{\"Pika H3 Flex\": true}, {\"HyperFlow\": true}, {\"steps\": 12}, "
      "{\"4-step LoRA\": true}. Pika H3 Flex, HyperFlow and TaoMate are "
      "SWITCHES that bring their adapter WITH its own steps and schedule: "
      "a person's \"Pika LoRA\" or \"HyperFlow LoRA\" is that switch, "
      "never a LoRA laid on alone. A few-step LoRA is one of "
      "few_step_loras.";
  return r;
}

// ---- generating -------------------------------------------------------------

Result<Json>
Controller::AgentHost::t_enhance_prompt(Session& s, Step&, const Json& args,
                                        std::vector<fs::path>&)
{
  const std::string idea = text_arg(args, "idea");
  if (one_line(idea).empty()) {
    return make_error(Code::InvalidArgument, "the idea is empty");
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  const std::string what = text_arg(args, "for", "picture");
  const std::string style = one_line(text_arg(args, "style"));
  EnhancePromptRequest req;
  req.project = s.project;
  req.style = style;
  // The agent's names for the pictures (<Picture 1>, ...) as the
  // request's row tags -- and back: what the enhancer wrote, in the names
  // the agent's tools take.
  std::vector<std::pair<std::string, std::string>> in, back;
  auto name_back = [&](std::size_t k, const std::string& as) {
    back.emplace_back(
        assist::ref_tag(assist::RefKind::Image, static_cast<int>(k)), as);
  };
  const models::ModelEntry* m = nullptr;
  std::string tool;
  if (what == "picture") {
    tool = "generate_image";
    VALTZ_ASSIGN(auto base, maybe_asset(*p, args, "base"));
    VALTZ_ASSIGN(auto refs, pictures_of(*p, args, "references"));
    std::vector<AssetId> row;
    if (base) {
      row.push_back(*base);
    }
    row.insert(row.end(), refs.begin(), refs.end());
    const bool edit = !row.empty();
    // The model generate_image would run: the one named, else Auto's.
    VALTZ_ASSIGN(const models::ModelEntry* named,
                 model_for_(edit ? "edit" : "picture",
                            text_arg(args, "model")));
    std::string id = named ? named->id : std::string();
    for (const auto& a : _c.auto_models()) {
      if (id.empty() && a.modality == "image" &&
          a.op == (edit ? "edit" : "generate")) {
        id = a.chosen;
      }
    }
    m = _c._catalog.find(id);
    if (!m) {
      return make_error(Code::NotFound,
                        edit ? "no picture model that edits runs here"
                             : "no picture model runs here");
    }
    req.target = "image";
    req.model = m->id;
    req.mode = edit ? "edit" : "generate";
    req.base = base;
    req.references = refs;
    req.row = row;
    in = picture_tags(row.size());
    // What the model calls them, as its rewriter writes them ("<image2>").
    const auto form = jget<std::string>(m->prompting, "reference_tag", "");
    for (std::size_t i = 0; i < row.size(); ++i) {
      const std::string mine = std::format("<Picture {}>", i + 1);
      name_back(i, mine);
      if (!form.empty()) {
        back.emplace_back(numbered(form, static_cast<int>(i) + 1), mine);
      }
    }
    // Its size: the box's shape, as generate_image makes it.
    if (args.contains("width") && args.contains("height")) {
      const auto [w, h] = picture_size(number(args, "width", 1024),
                                       number(args, "height", 1024));
      req.size = std::format("{}x{}", w, h);
    }
  } else if (what == "shot") {
    tool = "generate_video";
    VALTZ_ASSIGN(auto first, maybe_asset(*p, args, "first"));
    VALTZ_ASSIGN(auto last, maybe_asset(*p, args, "last"));
    VALTZ_ASSIGN(auto refs, pictures_of(*p, args, "references"));
    VALTZ_ASSIGN(auto from, maybe_asset(*p, args, "continue_from",
                                        AssetKind::Video));
    if ((first || last) && (from || !refs.empty())) {
      return make_error(Code::InvalidArgument,
                        "a shot opens on a picture (first), or draws on "
                        "references and carries on from a clip -- not both");
    }
    const bool drawn = from || !refs.empty();
    // The one named, else Auto's.
    VALTZ_ASSIGN(m, model_for_(drawn ? "shot_references" : "shot",
                               text_arg(args, "model")));
    if (!m && drawn) {
      m = _c.reference_model_("");
    }
    for (const auto& a : _c.auto_models()) {
      if (!m && !drawn && a.modality == "video" && a.op == "generate" &&
          !a.chosen.empty()) {
        m = _c._catalog.find(a.chosen);
      }
    }
    if (!m) {
      return make_error(Code::Unsupported, "no video model runs here");
    }
    if (first && !m->has(models::Capability::ImageToVideo)) {
      return make_error(Code::Unsupported, std::format(
          "{} does not open a shot on a picture", m->name));
    }
    req.target = "video";
    req.model = m->id;
    req.base = first;
    req.last = last;
    req.references = refs;
    req.continue_from = from;
    const auto [frames, fps] = shot_frames(*m, number(args, "seconds", 5));
    req.frames = frames;
    req.fps = fps;
    // The pictures in the row's order -- the first, the last, the
    // references -- each by the name generate_video's description takes.
    const Json forms = jget(m->prompting, "reference_tags", Json::object());
    const auto image = jget<std::string>(forms, "image", "<Picture {n}>");
    auto bare = jget<std::string>(m->prompting, "first_last_tag", "");
    if (bare.empty()) {
      bare = image;
    }
    std::size_t k = 0;
    if (first) {
      in.emplace_back("<Picture 1>", assist::ref_tag(assist::RefKind::Image,
                                                     0));
      name_back(k++, last ? numbered(bare, 1) : numbered(image, 1));
    }
    // Ending on one: Picture 2 after the first (FL2VA), else <Picture 1>
    // (L2VA).
    if (last) {
      in.emplace_back(first ? "<Picture 2>" : "<Picture 1>",
                      assist::ref_tag(assist::RefKind::Image,
                                      static_cast<int>(k)));
      name_back(k, first ? numbered(bare, 2) : numbered(image, 1));
      ++k;
    }
    if (drawn) {
      VALTZ_ASSIGN(Json tags, _c.video_reference_tags(s.project, m->id, refs,
                                                      from));
      for (const auto& id : refs) {
        const auto tag = jget<std::string>(tags, id.str().c_str(), "");
        if (!tag.empty()) {
          in.emplace_back(tag, assist::ref_tag(assist::RefKind::Image,
                                               static_cast<int>(k)));
          name_back(k, tag);
        }
        ++k;
      }
    }
  } else if (what == "music") {
    tool = "generate_music";
    VALTZ_ASSIGN(m, model_for_("music", text_arg(args, "model")));
    req.target = "audio";
    req.model = m ? m->id : "auto";
    req.seconds = std::max(0.0, number(args, "seconds", 0));
  } else if (what == "voiceover") {
    tool = "generate_voiceover";
    // The speech model generate_voiceover speaks with.
    VALTZ_ASSIGN(m, model_for_("voiceover", text_arg(args, "model")));
    if (!m) {
      m = voice_model_();
    }
    if (!m) {
      return make_error(Code::Unsupported, "no speech model runs here");
    }
    req.target = "audio";
    req.model = m->id;
    req.seconds = std::max(0.0, number(args, "seconds", 0));
  } else {
    return make_error(Code::InvalidArgument,
                      "for is picture, shot, music or voiceover");
  }
  req.prompt = assist::retag(idea, in);
  // On the helper -- between the agent's own turns, its conversation kept
  // apart under its key (vpipe keeps each context by key).
  VALTZ_ASSIGN(JobId job, _c.enhance_prompt(std::move(req)));
  if (!wait_job_(s, job)) {
    return make_error(Code::Cancelled, "stopped");
  }
  const Json reply = take_reply_(job);
  if (!reply.is_object()) {
    const auto rec = _c._jobs.get(job);
    return make_error(Code::Engine,
                      rec && rec->state == JobState::Failed &&
                              !rec->message.empty()
                          ? rec->message
                          : std::string("the enhancer's reply could not be "
                                        "read: write the prompt yourself"));
  }
  const std::string written =
      assist::retag(jget<std::string>(reply, "prompt", ""), back);
  Json out = {{"for", what}};
  if (m) {
    out["written_for"] = m->name;
  }
  // What to look at in it -- found here; the rest is the agent's reading.
  Json notes = Json::array();
  auto lower = [](std::string t) {
    for (auto& ch : t) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return t;
  };
  if (what == "picture") {
    out["prompt"] = written;
    const auto asked = quoted_spans(idea);
    std::string put;
    for (const auto& q : quoted_spans(written)) {
      if (std::ranges::find(asked, q) == asked.end()) {
        put += (put.empty() ? "\"" : ", \"") + q + "\"";
      }
    }
    if (!put.empty()) {
      notes.push_back(std::format(
          "It writes words into the picture: {}. A picture model misspells "
          "them: take them out, and set the words in a text box over the "
          "picture.",
          put));
    }
  } else if (what == "shot") {
    const auto parts = assist::clip_parts(written);
    auto part = [&](const char* k) {
      const auto it = parts.find(k);
      return it == parts.end() ? std::string() : it->second;
    };
    std::string d = part("integrated_multimodal_description");
    if (d.empty()) {
      d = part("detailed_description");
    }
    out["description"] = parts.empty() ? written : d;
    out["soundscape"] = part("overall_soundscape");
    out["music"] = part("non_diegetic_music");
    if (!part("subject_definitions").empty()) {
      out["subjects"] = part("subject_definitions");
    }
    if (!part("summary").empty()) {
      out["summary"] = part("summary");
    }
    // Words spoken that the idea did not give.
    auto spoken = [](std::string_view t) {
      std::vector<std::string> lines;
      for (auto at = t.find("<d>"); at != std::string_view::npos;
           at = t.find("<d>", at + 3)) {
        const auto end = t.find("</d>", at);
        if (end == std::string_view::npos) {
          break;
        }
        lines.emplace_back(t.substr(at + 3, end - at - 3));
      }
      return lines;
    };
    const auto mine = spoken(idea);
    std::string added;
    for (const auto& l : spoken(d)) {
      if (std::ranges::find(mine, l) == mine.end()) {
        added += (added.empty() ? "" : " / ") + l;
      }
    }
    if (!added.empty()) {
      notes.push_back(std::format(
          "It has someone speak: {}. Keep it only if the shot wants "
          "those words.",
          std::string(utf8_prefix(added, 300))));
    }
  } else if (what == "music") {
    const assist::SongText song = assist::split_song(written);
    out["style"] = song.style;
    if (!song.lyrics.empty()) {
      out["lyrics"] = song.lyrics;
      if (lower(idea).find("instrumental") != std::string::npos) {
        notes.push_back("You asked for instrumental music and it wrote "
                        "lyrics: leave `lyrics` out.");
      }
    }
  } else {
    const assist::SpeechText sp = assist::split_speech(written);
    out["text"] = sp.text;
    out["voice"] = sp.instruction;
    for (const auto& [k, v] :
         {std::pair{"quality", &sp.quality},
          std::pair{"sound_event", &sp.sound_event},
          std::pair{"ambient_sound", &sp.ambient_sound},
          std::pair{"language", &sp.language}}) {
      if (!one_line(*v).empty()) {
        out[k] = *v;
      }
    }
    const std::string a = loosen(idea).text, b = loosen(sp.text).text;
    if (b.find(a) == std::string::npos && a.find(b) == std::string::npos) {
      notes.push_back("It wrote words of its own: if yours were the "
                      "script, keep yours as `text`.");
    }
  }
  // The look kept, in its words -- a word or two of it reworded is
  // kept (the 9B, told "soft" was missing of seven, dropped the whole
  // rewrite for its own words).
  if (!style.empty()) {
    const std::string low = lower(written);
    const auto words = key_words(style);
    std::string missing;
    std::size_t gone = 0;
    for (const auto& w : words) {
      if (low.find(w) == std::string::npos) {
        missing += (missing.empty() ? "" : ", ") + w;
        ++gone;
      }
    }
    if (gone * 3 >= words.size() && gone > 0) {
      notes.push_back(std::format(
          "It leaves out the look you keep (\"{}\": {} missing): put it "
          "back in its words.",
          style, missing));
    }
  }
  if (!notes.empty()) {
    out["notes"] = std::move(notes);
  }
  out["review"] = std::format(
      "Read it before you use it: keep what you asked for and what it adds "
      "that fits; take out what does not -- another subject, a look other "
      "than the one you keep, a mood or a detail you did not ask for{}. "
      "Then call {} with it as you corrected it.",
      what == "picture" ? ", words written in the picture" : "", tool);
  return out;
}

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
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // An EDIT: the picture it changes, the pictures it draws on.
  VALTZ_ASSIGN(auto base, maybe_asset(*p, args, "base"));
  VALTZ_ASSIGN(auto refs, pictures_of(*p, args, "references"));
  const bool edit = base || !refs.empty();
  // The model the person named -- one that edits, for an edit -- else
  // Auto's; and, for a size asked, how large it makes (Auto's: its pick).
  GenerateImageRequest req;
  VALTZ_ASSIGN(const models::ModelEntry* named,
               model_for_(edit ? "edit" : "picture",
                          text_arg(args, "model")));
  req.model = named ? named->id : std::string();
  int most = 2048;
  const models::ModelEntry* runs = named;
  for (const auto& a : _c.auto_models()) {
    if (!runs && a.modality == "image" &&
        a.op == (edit ? "edit" : "generate")) {
      runs = _c._catalog.find(a.chosen);
    }
  }
  if (runs && models::max_edge(*runs)) {
    most = models::max_edge(*runs);
  }
  // Favor and its options, as asked -- for the model that runs it (Auto's
  // then named, so the options read are the ones it runs).
  Tuned tuned;
  if (runs) {
    VALTZ_ASSIGN(tuned, tuned_(*runs, args, edit, false, false));
  } else if (args.is_object() &&
             (args.contains("favor") || args.contains("options"))) {
    return make_error(Code::Unsupported, "no picture model runs here");
  }
  const bool sized = args.contains("long_side");
  const double side = number(args, "long_side", 1024);
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
    size = sized ? picture_size_long(box[2], box[3], side, most)
                 : picture_size(box[2], box[3]);
  } else if (base) {
    // The base's own shape: the model's own area, or the side asked.
    size = {0, 0};
    if (auto v = p->version(*base); sized && v.ok()) {
      size = picture_size_long(v->info.frame.width, v->info.frame.height,
                               side, most);
    }
  } else if (const auto shape = aspect_of(text_arg(args, "aspect"))) {
    // An asset alone, at the shape asked.
    size = sized ? picture_size_long(shape->first, shape->second, side,
                                     most)
                 : picture_size(shape->first, shape->second);
  } else if (sized) {
    size = picture_size_long(1, 1, side, most);
  }
  req.project = s.project;
  req.prompt = prompt;
  req.name = one_line(text_arg(args, "name"));
  req.width = size.first;
  req.height = size.second;
  req.mode = edit ? "edit" : "generate";
  req.preference = tuned.preference;
  req.tuning = tuned.tuning;
  // A seed: the same take again with the same model and words, or
  // another -- the model pinned, as Auto's could change.
  const bool seeded =
      args.contains("seed") && args["seed"].is_number_integer();
  if (seeded) {
    req.seed = std::max<std::int64_t>(0, args["seed"].get<std::int64_t>());
  }
  if ((tuned.given || seeded) && runs) {
    req.model = runs->id;
  }
  if (edit) {
    // The pictures in order, the base first: <Picture n> in the prompt
    // is the row's n-th, which the model reads by its own name.
    if (base) {
      req.row.push_back(*base);
    }
    req.row.insert(req.row.end(), refs.begin(), refs.end());
    req.prompt = assist::retag(prompt, picture_tags(req.row.size()));
    req.base = base;
    req.references = refs;
  }
  if (runs) {
    l.model = runs->name;
  }
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
  Json r = {{"job", job.str()}, {"queued", true}};
  if (runs) {
    r["model"] = runs->name;
  }
  if (seeded) {
    r["seed"] = std::max<std::int64_t>(0, args["seed"].get<std::int64_t>());
  }
  if (size.first > 0) {
    r["size"] = {size.first, size.second};
  }
  if (edit) {
    r["edits"] = base ? base->str() : std::string("its references");
  }
  told_(r, tuned);
  if (std::max(size.first, size.second) > 2048) {
    r["note"] = "A picture this large takes long: many minutes -- at "
                "4096 an hour or more. Go on meanwhile.";
  }
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
  // What landed on a composition is on it already: the 9B add_clip'd a
  // landed shot again, the clip twice on its timeline.
  for (const auto& x : landed) {
    if (x.contains("composition")) {
      return Json{{"landed", std::move(landed)},
                  {"note", "Each landed ON its composition already, as "
                           "the layer it names: do not add it again."}};
    }
  }
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
    Controller::AssetOpRequest req;
    req.project = s.project;
    req.asset = *asset;
    req.operation = "summarize";
    VALTZ_ASSIGN(auto made, _c.run_asset_operation(std::move(req)));
    text = made.made.front();
    if (!wait_job_(s, made.job)) {
      return make_error(Code::Cancelled, "stopped");
    }
    const auto rec = _c._jobs.get(made.job);
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
Controller::AgentHost::t_transcribe(Session& s, Step&, const Json& args,
                                    std::vector<fs::path>&)
{
  const auto asset = AssetId::parse(text_arg(args, "asset"));
  if (!asset) {
    return make_error(Code::InvalidArgument, "which sound or clip?");
  }
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset src, p->asset(*asset));
  if (src.kind != AssetKind::Audio && src.kind != AssetKind::Video) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is not a sound or a clip", src.name));
  }
  // The model the person named, else Auto's.
  VALTZ_ASSIGN(const models::ModelEntry* named,
               model_for_("transcribe", text_arg(args, "model")));
  // Heard before -- by the person, or an earlier step -- and still about
  // it as it is (by the model named, if one is): that transcript.
  std::optional<AssetId> text;
  if (auto deps = p->dependents(*asset, false); deps.ok()) {
    for (const auto& d : *deps) {
      auto a = p->asset(d);
      auto r = a.ok() && a->head > 0 ? p->recipe(a->recipe)
                                     : Result<project::Recipe>(make_error(
                                           Code::NotFound, "none"));
      auto st = p->staleness(d);
      if (r.ok() && r->op == engine::kOpTranscribeAudio && st.ok() &&
          st->fresh() && (!named || r->model == named->id)) {
        text = d;
      }
    }
  }
  if (!text) {
    Controller::AssetOpRequest req;
    req.project = s.project;
    req.asset = *asset;
    req.operation = "transcribe";
    if (named) {
      req.model = named->id;
    }
    VALTZ_ASSIGN(auto made, _c.run_asset_operation(std::move(req)));
    text = made.made.front();
    if (!wait_job_(s, made.job)) {
      return make_error(Code::Cancelled, "stopped");
    }
    const auto rec = _c._jobs.get(made.job);
    if (rec && rec->state != JobState::Finished) {
      return make_error(Code::Engine, rec->message.empty()
                                          ? std::string("it could not be "
                                                        "heard")
                                          : rec->message);
    }
  }
  VALTZ_ASSIGN(project::AssetVersion v, p->version(*text));
  const Json t = jget(v.outputs, "transcript", Json::object());
  Json out = {{"asset", asset->str()},
              {"name", src.name},
              {"transcript", text->str()}};
  if (!t.is_object() || !t.contains("lines")) {
    // Its words as they are kept (edited, perhaps).
    auto words = p->read_text(*text);
    out["text"] = words.ok() ? std::string(utf8_prefix(*words, 8192))
                             : std::string();
    return out;
  }
  out["seconds"] = secs(jget(t, "seconds", 0.0));
  // What was said, line by line in its own seconds -- a long one's first
  // 300 lines -- and the sounds heard.
  constexpr std::size_t kMostLines = 300;
  const Json all = jget(t, "lines", Json::array());
  Json lines = Json::array();
  for (const auto& l : all) {
    if (lines.size() == kMostLines) {
      out["more_lines"] = all.size() - kMostLines;
      break;
    }
    lines.push_back(
        {{"start", secs(jget(l, "start", 0.0))},
         {"end", secs(jget(l, "end", 0.0))},
         {"text", std::string(utf8_prefix(jget<std::string>(l, "text", ""),
                                          600))}});
  }
  if (lines.empty()) {
    out["speech"] = false;
  }
  out["lines"] = std::move(lines);
  Json sounds = Json::array();
  for (const auto& e : jget(t, "events", Json::array())) {
    if (sounds.size() == 60) {
      break;
    }
    sounds.push_back({{"start", secs(jget(e, "start", 0.0))},
                      {"end", secs(jget(e, "end", 0.0))},
                      {"label", jget<std::string>(e, "label", "")}});
  }
  if (!sounds.empty()) {
    out["sounds"] = std::move(sounds);
  }
  return out;
}

Result<Json>
Controller::AgentHost::t_separate_sound(Session& s, Step& step,
                                        const Json& args,
                                        std::vector<fs::path>&)
{
  const auto asset = AssetId::parse(text_arg(args, "asset"));
  if (!asset) {
    return make_error(Code::InvalidArgument, "which song?");
  }
  VALTZ_ASSIGN(const models::ModelEntry* named,
               model_for_("separate", text_arg(args, "model")));
  auto undo = _c.command_(s.project, "agent", AssetId{}, "",
                          {{"tool", step.tool}, {"title", step.title}});
  Controller::AssetOpRequest req;
  req.project = s.project;
  req.asset = *asset;
  req.operation = "separate";
  if (named) {
    req.model = named->id;
  }
  VALTZ_ASSIGN(auto made, _c.run_asset_operation(std::move(req)));
  book_(s, step, made.job);
  Json r = {{"job", made.job.str()}, {"queued", true}};
  // In the operation's order: the voice, then the rest.
  if (made.made.size() >= 2) {
    r["vocals"] = made.made[0].str();
    r["accompaniment"] = made.made[1].str();
  }
  r["note"] = "Made in the background: wait_for_generations, then "
              "add_sound the one you want.";
  return r;
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
  // A picture, a still: shown as view_asset shows it (a still's other
  // pages are view_page's).
  if (a.kind == AssetKind::Image) {
    return t_view_asset(s, step, {{"asset", id->str()}}, shown);
  }
  if (a.kind != AssetKind::Video) {
    return make_error(Code::InvalidArgument, std::format(
        "\"{}\" is not a clip or a timeline with a picture", a.name));
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
  if (!transition.empty() && !media::transition_kind(transition)) {
    return make_error(Code::InvalidArgument,
                      "a transition is dissolve, dip-black, dip-white or "
                      "cut");
  }
  // A dissolve or a dip takes an overlap with the clip before; a cut
  // none.
  const bool overlaps = !transition.empty() && transition != "cut";
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
      overlaps
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
  // A dissolve or a dip overlaps the clip before by its length,
  // whatever start was given -- unless that start overlaps it already.
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
  if (overlaps) {
    const auto b = before ? times.find(*before) : times.end();
    if (b != times.end() && mine.start < b->second.end() - 1e-3) {
      VALTZ_TRY(_c.set_transition(s.project, comp, *before, layer,
                                  transition));
      out["transition"] = {{"from", model_layer(*before)},
                           {"kind", transition},
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
  // A clip's or a sound's length is its part: a `duration` given alone is
  // read as its part from its `in`.
  Json given = args;
  if (given.contains("duration") && timed) {
    if (!given.contains("out")) {
      VALTZ_ASSIGN(auto now, timings_(s.project, comp));
      const double in = number(given, "in", now[layer].in);
      given["out"] = in + std::max(0.0, number(given, "duration", 0));
    }
    given.erase("duration");
  }
  const Json& args_ = given;
  if ((args_.contains("in") || args_.contains("out") ||
       args_.contains("speed")) && !timed) {
    return make_error(Code::InvalidArgument,
                      "only a clip or a sound has a part and a speed");
  }
  const Rational rate = rate_(s.project, comp);
  const double fps = rate.to_double();
  VALTZ_ASSIGN(auto times, timings_(s.project, comp));
  const media::LayerTiming was = times[layer];
  project::LayerTime t = it->time;
  if (args_.contains("start")) {
    t.offset = frames_of(number(args_, "start", 0), fps);
  }
  if (args_.contains("duration")) {
    t.duration = std::max<std::int64_t>(
        1, frames_of(number(args_, "duration", 3), fps));
  }
  std::string clamped;
  if (args_.contains("in") || args_.contains("out")) {
    VALTZ_ASSIGN(project::Asset src, p->asset(*it->source));
    const double whole = source_seconds(_c, *p, src);
    if (number(args_, "out", 0) > whole + 0.05) {
      clamped = std::format("\"{}\" is {:.2f} s long: its part ends "
                            "there.", src.name, whole);
    }
    const double in =
        std::clamp(number(args_, "in", was.in), 0.0, whole);
    const double out = std::clamp(
        number(args_, "out", std::isfinite(was.out) ? was.out : whole), in,
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
  if (args_.contains("speed")) {
    const double speed = std::clamp(number(args_, "speed", 1), 0.1, 10.0);
    media::KeyedSpeed k;
    k.rate = rate;
    k.keys.push_back({0, media::Speed{speed}});
    VALTZ_TRY(_c.set_speed_keys(s.project, comp, k, layer));
  }
  VALTZ_ASSIGN(times, timings_(s.project, comp));
  const media::LayerTiming now = times[layer];
  // Its fades kept at its ends, wherever they are now.
  if (timed && std::abs(now.length - was.length) > 1e-3) {
    VALTZ_TRY(reshape_sound_(s.project, comp, layer, was.length,
                             now.length));
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
  const Rational rate = rate_(s.project, comp);
  VALTZ_ASSIGN(auto times, timings_(s.project, comp));
  const media::LayerTiming mine = times[layer];
  const media::KeyedSound keys = Controller::sound_keys_of(a, layer);
  // Its shape as it is -- what is not given stays. Keys made by hand
  // (key_layer) are no shape: they are replaced, and said.
  const bool by_hand =
      !keys.identity() && !is_shape(keys, mine.length, rate);
  SoundShape sh = shape_of(keys, rate.to_double());
  if (args.contains("volume")) {
    sh.volume = std::clamp(number(args, "volume", 1), 0.0, 2.0);
  }
  if (args.contains("fade_in")) {
    sh.fade_in = std::max(0.0, number(args, "fade_in", 0));
  }
  if (args.contains("fade_out")) {
    sh.fade_out = std::max(0.0, number(args, "fade_out", 0));
  }
  if (args.contains("pitch")) {
    sh.pitch = std::clamp(number(args, "pitch", 0), -media::kMaxPitch,
                          media::kMaxPitch);
  }
  if (args.contains("eq")) {
    VALTZ_ASSIGN(sh.eq, eq_over(args["eq"], sh.eq));
  }
  // DUCKED under other layers -- a voice-over, someone speaking: lower
  // wherever they sound, eased down and back up. Where they are now:
  // moved later, it is asked again.
  if (args.contains("duck_under")) {
    Json under = args["duck_under"];
    if (under.is_string()) {
      under = Json::array({under});
    }
    const double level =
        args.contains("duck_volume") && sh.volume > 0
            ? std::clamp(number(args, "duck_volume", 0.25) / sh.volume, 0.0,
                         1.0)
            : 0.25;
    std::vector<SoundDip> dips;
    // The layers it goes under: by their ids -- or an asset's (a
    // voice-over's, as it landed): the layers that show it.
    std::vector<std::string> unders;
    const auto sounds = [&](const project::Layer& l) {
      const std::string k = layer_kind_(s.project, l);
      return l.id != layer && (k == "sound" || k == "clip");
    };
    for (const auto& u : under.is_array() ? under : Json::array()) {
      std::string id = u.is_string() ? u.get<std::string>()
                       : u.is_number_integer()
                           ? std::to_string(u.get<std::int64_t>())
                           : std::string();
      id = id == "0" ? std::string() : id;
      const auto asset = AssetId::parse(id);
      std::size_t found = 0;
      for (const auto& l : a.layers) {
        if ((l.id == id || (asset && l.source == *asset)) && sounds(l)) {
          unders.push_back(l.id);
          ++found;
        }
      }
      if (found == 0) {
        std::string sounding;
        for (const auto& l : a.layers) {
          if (sounds(l)) {
            sounding += std::format("{}{} ({})", sounding.empty() ? "" : ", ",
                                    model_layer(l.id), l.name);
          }
        }
        return make_error(Code::NotFound, std::format(
            "no other layer {} that sounds, to duck under -- the layers "
            "that sound: {}",
            model_layer(id), sounding.empty() ? "none" : sounding));
      }
    }
    for (const auto& id : unders) {
      const auto t = times.find(id);
      if (t == times.end()) {
        continue;
      }
      const double from = std::max(0.0, t->second.start - mine.start);
      double to = t->second.end() - mine.start;
      if (std::isfinite(mine.length)) {
        to = std::min(to, mine.length);
      }
      if (to - from > 0.1 && std::isfinite(to)) {
        dips.push_back({from, to, level});
      }
    }
    std::ranges::sort(dips, {}, &SoundDip::from);
    sh.dips.clear();
    for (const auto& d : dips) {
      // Two a breath apart are one.
      if (!sh.dips.empty() && d.from <= sh.dips.back().to + 2 * kDuckRamp) {
        sh.dips.back().to = std::max(sh.dips.back().to, d.to);
      } else {
        sh.dips.push_back(d);
      }
    }
  } else if (args.contains("duck_volume") && sh.volume > 0) {
    for (auto& d : sh.dips) {
      d.level = std::clamp(number(args, "duck_volume", 0.25) / sh.volume,
                           0.0, 1.0);
    }
  }
  std::optional<bool> follow;
  if (args.contains("follow_speed")) {
    follow = flag(args, "follow_speed");
  }
  auto undo = _c.command_(s.project, "agent", comp, layer,
                          {{"tool", step.tool}, {"title", step.title}});
  VALTZ_TRY(_c.set_sound_keys(s.project, comp,
                              shape_keys(sh, mine.length, rate), layer,
                              follow));
  Json r = {{"layer", model_layer(layer)},
            {"volume", std::clamp(sh.volume, 0.0, 2.0)},
            {"fade_in", sh.fade_in},
            {"fade_out", sh.fade_out}};
  if (sh.pitch != 0) {
    r["pitch"] = sh.pitch;
  }
  if (const Json eq = eq_said(sh.eq); !eq.empty()) {
    r["eq"] = eq;
  }
  if (!sh.dips.empty()) {
    Json d = Json::array();
    for (const auto& x : sh.dips) {
      d.push_back({{"from", secs(mine.start + x.from)},
                   {"to", secs(mine.start + x.to)},
                   {"volume", std::round(sh.volume * x.level * 100) / 100}});
    }
    r["ducked"] = std::move(d);
  }
  if (follow) {
    r["follow_speed"] = *follow;
  }
  if (by_hand) {
    r["note"] = "Its volume was keyed by hand (key_layer): those keys are "
                "replaced by this level and its fades.";
  }
  return r;
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
  if (kind != "none" && !media::transition_kind(kind)) {
    return make_error(Code::InvalidArgument,
                      "a transition is dissolve, dip-black, dip-white, cut "
                      "or none");
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
  if (kind != "cut" && kind != "none") {
    // A dissolve or a dip, too short an overlap (none: they only meet):
    // the later one -- and all that starts with or after it -- moves
    // earlier, so the two overlap by its length.
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
    VALTZ_TRY(_c.set_transition(s.project, comp, from, to, kind));
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
  // The model the person named, else Auto's.
  VALTZ_ASSIGN(const models::ModelEntry* m,
               model_for_("music", text_arg(args, "model")));
  const models::ModelEntry* sings = m;
  for (const auto& a : _c.auto_models()) {
    if (!sings && a.modality == "audio" && a.op == "generate") {
      sings = _c._catalog.find(a.chosen);
    }
  }
  Tuned tuned;
  if (sings) {
    VALTZ_ASSIGN(tuned, tuned_(*sings, args, false, false, false));
  }
  // Instrumental on a model that sings only to lyrics (MiniMax-Music3,
  // catalog song.lyrics_required): its sections without words.
  if (one_line(lyrics).empty() && sings &&
      jget(jget(jget(sings->engine, "vpipe", Json::object()), "song",
                Json::object()),
           "lyrics_required", false)) {
    lyrics = "[Intro]\n[Instrumental]\n[Outro]";
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
  if (sings) {
    l.model = sings->name;
  }
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
  if (m) {
    req.model = m->id;
  }
  req.name = one_line(text_arg(args, "name"));
  req.preference = tuned.preference;
  req.tuning = tuned.tuning;
  if (tuned.given && sings) {
    req.model = sings->id;
  }
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
  if (sings) {
    r["model"] = sings->name;
  }
  told_(r, tuned);
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
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // A VOICE to speak in, cloned from a sound -- or a clip's sound -- of
  // the project.
  auto voice_of = [&](const std::string& t) -> Result<AssetId> {
    const auto id = AssetId::parse(t);
    if (!id) {
      return make_error(Code::InvalidArgument,
                        std::format("'{}' is not an asset id", t));
    }
    VALTZ_ASSIGN(project::Asset v, p->asset(*id));
    if (!_c.voiced_(*p, *id)) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" has no sound to take a voice from", v.name));
    }
    return *id;
  };
  std::optional<AssetId> voice;
  if (const auto t = text_arg(args, "voice_from"); !t.empty()) {
    VALTZ_ASSIGN(voice, voice_of(t));
  }
  // SPEAKERS: a scene of several voices (Drama 3), each line "Name:
  // [direction] words" -- each speaker's voice by the order their names
  // first come in the words.
  struct Speaker {
    std::string name;
    std::optional<AssetId> voice;
    std::string words;
    std::size_t first = std::string::npos;
  };
  std::vector<Speaker> speakers;
  for (const auto& sp : jget(args, "speakers", Json::array())) {
    Speaker x;
    x.name = one_line(jget<std::string>(sp, "name", ""));
    if (x.name.empty()) {
      return make_error(Code::InvalidArgument,
                        "each speaker has a `name`, as the lines say it");
    }
    if (const auto t = jget<std::string>(sp, "voice_from", "");
        !t.empty()) {
      VALTZ_ASSIGN(x.voice, voice_of(t));
    }
    x.words = jget<std::string>(sp, "words", "");
    for (std::size_t at = words.find(x.name + ":");
         at != std::string::npos; at = words.find(x.name + ":", at + 1)) {
      if (at == 0 || words[at - 1] == '\n') {
        x.first = at;
        break;
      }
    }
    if (x.first == std::string::npos) {
      return make_error(Code::InvalidArgument, std::format(
          "no line of the words begins \"{}:\" -- a scene's lines are "
          "\"Name: [direction] words\", one a line",
          x.name));
    }
    speakers.push_back(std::move(x));
  }
  std::ranges::sort(speakers, {}, &Speaker::first);
  const bool voiced_all = std::ranges::all_of(
      speakers, [](const Speaker& x) { return x.voice.has_value(); });
  const bool voiced_none = std::ranges::none_of(
      speakers, [](const Speaker& x) { return x.voice.has_value(); });
  if (!voiced_all && !voiced_none) {
    return make_error(Code::InvalidArgument,
                      "give every speaker a voice_from, or none (each then "
                      "speaks in a voice of the model's own)");
  }
  // The model the person named, else the best that speaks here (a
  // quantized variant is its source's choice, by Favor) -- a scene of
  // speakers, one that performs scripts.
  VALTZ_ASSIGN(const models::ModelEntry* m,
               model_for_("voiceover", text_arg(args, "model")));
  if (!m && !speakers.empty()) {
    m = script_model_();
  }
  if (!m) {
    m = voice_model_();
  }
  if (!m) {
    return make_error(Code::Unsupported, "no speech model runs here");
  }
  const bool scripts =
      jget<std::string>(jget(jget(m->engine, "vpipe", Json::object()),
                             "speech", Json::object()),
                        "format", "") == "drama3";
  if (!speakers.empty() && !scripts) {
    return make_error(Code::Unsupported, std::format(
        "{} speaks one voice at a time: a scene of speakers needs Drama 3 "
        "(list_models), else one generate_voiceover a speaker, each with "
        "its voice_from",
        m->name));
  }
  assist::SpeechText st;
  st.instruction = one_line(text_arg(args, "voice"));
  st.quality = one_line(text_arg(args, "quality"));
  st.sound_event = one_line(text_arg(args, "sound_event"));
  st.ambient_sound = one_line(text_arg(args, "ambient_sound"));
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
  l.model = m->name;
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
  VALTZ_ASSIGN(const Tuned tuned, tuned_(*m, args, false, false, false));
  GenerateAudioRequest req;
  req.project = s.project;
  req.prompt = assist::speech_prompt(st);
  req.model = m->id;
  req.name = one_line(text_arg(args, "name"));
  req.preference = tuned.preference;
  req.tuning = tuned.tuning;
  req.voice = voice;
  if (voiced_all && !speakers.empty()) {
    for (const auto& x : speakers) {
      req.voices.push_back(*x.voice);
      req.voice_texts.push_back(x.words);
    }
  }
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
  told_(r, tuned);
  if (voice) {
    r["voice_from"] = voice->str();
  }
  if (!speakers.empty()) {
    Json who = Json::array();
    for (const auto& x : speakers) {
      who.push_back(x.voice ? Json{{"name", x.name},
                                   {"voice_from", x.voice->str()}}
                            : Json{{"name", x.name}});
    }
    r["speakers"] = std::move(who);
    if (voiced_all) {
      r["note"] = "Each voice is cloned from its sound and the words said "
                  "in it -- a sound without them is transcribed first, "
                  "and the speech waits for it.";
    }
  }
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

std::optional<Controller::AgentHost::MadeAs>
Controller::AgentHost::made_before_(Session& s, std::optional<AssetId> comp,
                                    std::optional<AssetId> from)
{
  static constexpr std::array<const char*, 5> kMethod = {
      "pika_flex", "hyperflow", "taomate", "taomate_lora", "vdn"};
  auto* p = _c.project(s.project);
  if (!p || (!comp && !from)) {
    return std::nullopt;
  }
  // The session's own shot before it on the timeline: its clip -- made
  // or being made -- or, held behind another, its request as it will go.
  if (!from) {
    int last = 0;
    std::string made;
    {
      std::lock_guard lk(s.mu);
      if (const auto it = s.shots.find(*comp); it != s.shots.end()) {
        last = it->second.last;
      }
      if (last >= 1 && last <= static_cast<int>(s.steps.size())) {
        made = jget<std::string>(
            s.steps[static_cast<std::size_t>(last - 1)].result, "asset", "");
      }
    }
    if (!made.empty()) {
      from = AssetId::parse(made);
    } else if (last > 0) {
      std::lock_guard lk(_mu);
      for (const auto& h : _held) {
        if (h.session == s.id && h.step == last) {
          MadeAs out;
          out.clip = h.req.name;
          out.favor = favor_label(h.req.preference);
          for (const char* k : kMethod) {
            if (jget(h.req.tuning, k, false)) {
              out.options[k] = true;
            }
          }
          return out;
        }
      }
    }
  }
  // Else the clip that ends the timeline.
  if (!from) {
    auto tl = p->asset(*comp);
    auto times = timings_(s.project, *comp);
    if (!tl.ok() || !times.ok()) {
      return std::nullopt;
    }
    double end = -1;
    for (const auto& x : tl->layers) {
      const auto it = times->find(x.id);
      if (layer_kind_(s.project, x) == "clip" && it != times->end() &&
          std::isfinite(it->second.length) && it->second.end() > end) {
        end = it->second.end();
        from = x.source;
      }
    }
  }
  // What it shows, made: through a part the scissors cut and an edited
  // copy (each a composition of the clip alone), to the generation.
  std::string name;
  for (int hop = 0; from && hop < 4; ++hop) {
    auto a = p->asset(*from);
    if (!a.ok()) {
      return std::nullopt;
    }
    if (name.empty()) {
      name = a->name;
    }
    auto r = a->recipe.is_nil()
                 ? Result<project::Recipe>(make_error(Code::NotFound, "none"))
                 : p->recipe(a->recipe);
    if (r.ok() && r->op == engine::kOpGenerateVideo) {
      MadeAs out;
      out.clip = name;
      const Json t = jget(r->params, "tuning", Json::object());
      for (const char* k : kMethod) {
        if (jget(t, k, false)) {
          out.options[k] = true;
        }
      }
      // Its Favor as asked: its generation's, as it was recorded.
      if (auto nodes = p->generate_nodes();
          nodes.ok() && nodes->is_array()) {
        for (const auto& n : *nodes) {
          if (jget<std::string>(jget(n, "started", Json::object()), "asset",
                                "") == from->str()) {
            out.favor = favor_label(
                jget<std::string>(n, "preference", "balanced"));
          }
        }
      }
      return out;
    }
    if (a->cls != AssetClass::Composition || a->layers.size() != 1) {
      return std::nullopt;
    }
    from = a->layers.front().source;
  }
  return std::nullopt;
}

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
  std::optional<AssetId> first, last;
  // A RefMod or a clip given to open on is drawn on (references): a shot
  // opens on a picture alone.
  std::optional<AssetId> first_drawn;
  if (const auto f = text_arg(args, "first"); !f.empty()) {
    const auto id = AssetId::parse(f);
    auto fa = id ? p->asset(*id)
                 : Result<project::Asset>(make_error(
                       Code::InvalidArgument,
                       std::format("'{}' is not an asset id", f)));
    if (fa.ok() && (fa->kind == AssetKind::RefMod ||
                    fa->kind == AssetKind::Video)) {
      first_drawn = id;
    } else {
      VALTZ_ASSIGN(first, asset_arg(f, AssetKind::Image));
    }
  }
  // The picture it ENDS on: from the first to it (FL2VA), or leading up
  // to it alone (L2VA).
  if (const auto f = text_arg(args, "last"); !f.empty()) {
    VALTZ_ASSIGN(last, asset_arg(f, AssetKind::Image));
  }
  // REFERENCES of every kind Ref2VA reads: pictures (a face, a place),
  // clips (a motion, a camera move), sounds (a voice, a song), RefMods.
  std::vector<AssetId> pictures;
  bool seen = false;
  std::size_t sounds_in = 0;
  {
    Json refs = args.is_object() && args.contains("references")
                    ? args["references"]
                    : Json::array();
    if (refs.is_string()) {
      refs = Json::array({refs});
    }
    if (first_drawn) {
      Json front = Json::array({first_drawn->str()});
      for (const auto& x : refs.is_array() ? refs : Json::array()) {
        if (x != front[0]) {
          front.push_back(x);
        }
      }
      refs = std::move(front);
    }
    for (const auto& r : refs.is_array() ? refs : Json::array()) {
      if (!r.is_string()) {
        continue;
      }
      const auto id = AssetId::parse(r.get<std::string>());
      if (!id) {
        return make_error(Code::InvalidArgument, std::format(
            "'{}' is not an asset id", r.get<std::string>()));
      }
      VALTZ_ASSIGN(project::Asset ra, p->asset(*id));
      // A RefMod is read from its file: one still being made, not yet.
      if (ra.kind == AssetKind::RefMod && ra.head == 0) {
        return make_error(Code::InvalidArgument, std::format(
            "the RefMod \"{}\" is still being made: wait_for_generations, "
            "then ask again",
            ra.name));
      }
      if (ra.kind == AssetKind::Audio) {
        ++sounds_in;
      } else if (ra.kind == AssetKind::Image ||
                 ra.kind == AssetKind::Video ||
                 ra.kind == AssetKind::RefMod) {
        seen = true;
      } else {
        return make_error(Code::InvalidArgument, std::format(
            "\"{}\" is no picture, clip, sound or RefMod to draw on",
            ra.name));
      }
      pictures.push_back(*id);
    }
  }
  // A SOUNDTRACK: a sound used as the shot's own, as it is -- a music
  // video's song -- the first sound it draws on.
  std::optional<AssetId> soundtrack;
  if (const auto t = text_arg(args, "soundtrack"); !t.empty()) {
    const auto id = AssetId::parse(t);
    if (!id) {
      return make_error(Code::InvalidArgument,
                        std::format("'{}' is not an asset id", t));
    }
    VALTZ_ASSIGN(project::Asset sa, p->asset(*id));
    if (sa.kind != AssetKind::Audio) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is not a sound: a soundtrack is a song or a sound",
          sa.name));
    }
    std::erase(pictures, *id);
    sounds_in += 1;
    // Before any other sound: the first sound reference is the one kept.
    auto at = std::ranges::find_if(pictures, [&](AssetId x) {
      auto xa = p->asset(x);
      return xa.ok() && xa->kind == AssetKind::Audio;
    });
    pictures.insert(at, *id);
    soundtrack = id;
  }
  // A picture to open on beside sounds alone -- "the fox in this picture
  // sings this song" -- is the one they go with: drawn on (Ref2VA opens on
  // no picture), and said.
  std::string moved_first = first_drawn ? first_drawn->str() : "";
  if (sounds_in > 0 && !seen && first && !last) {
    pictures.insert(pictures.begin(), *first);
    moved_first = first->str();
    first.reset();
    seen = true;
  }
  if (sounds_in > 0 && !seen) {
    return make_error(Code::InvalidArgument,
                      "a sound is drawn on beside a picture or a clip of "
                      "who sings or speaks it: give that picture in "
                      "`references` too (not `first`)");
  }
  const bool continues = flag(args, "continues");
  const std::string from_text = text_arg(args, "continue_from");
  if ((first || last) &&
      (continues || !from_text.empty() || !pictures.empty())) {
    return make_error(Code::InvalidArgument,
                      "a shot opens or ends on a picture (first, last), or "
                      "draws on references and carries on from a clip -- "
                      "not both");
  }
  const bool continuing = continues || !from_text.empty();
  // How it carries on (DESIGN §4e): LATENT-GUIDED -- through the clip's
  // end on a model that takes a guide (FL2VA: the tail's latents pinned
  // over its first frames, which redo them), or with pictures beside it
  // its latents carried on Ref2VA -- unless the person asks for the old
  // way, its last seconds read again as a reference.
  const std::string by = option_norm(text_arg(args, "continue_by"));
  if (!by.empty() && by != "latents" && by != "latent" && by != "guide" &&
      by != "guided" && by != "reference") {
    return make_error(Code::InvalidArgument,
                      "continue_by is latents or reference");
  }
  const bool by_reference = by == "reference";
  const std::string want = text_arg(args, "model");
  const models::ModelEntry* m = nullptr;
  bool named = false, guided = false;
  if (continuing && !by_reference && pictures.empty()) {
    if (!want.empty()) {
      // A model named that guides (FL2VA); another (Ref2VA) carries.
      if (auto r = model_for_("shot", want);
          r.ok() && *r && guide_frames(**r) > 0) {
        m = *r;
        named = guided = true;
      }
    } else {
      for (const auto& a : _c.auto_models()) {
        if (!m && a.modality == "video" && a.op == "generate" &&
            !a.chosen.empty()) {
          const auto* g = _c._catalog.find(a.chosen);
          if (g && guide_frames(*g) > 0 &&
              _c.runs_soon(*g, "video", "generate")) {
            m = g;
            guided = true;
          }
        }
      }
    }
  }
  const bool refs = !guided && (continuing || !pictures.empty());
  // The model the person named -- one that draws on references, for a
  // shot that does -- else Auto's: FL2VA from words or a picture, Ref2VA
  // carrying on or drawing on references.
  if (!m) {
    VALTZ_ASSIGN(m, model_for_(refs ? "shot_references" : "shot", want));
    named = m != nullptr;
  }
  if (!m && refs) {
    m = _c.reference_model_("");
  }
  for (const auto& a : _c.auto_models()) {
    if (!m && !refs && a.modality == "video" && a.op == "generate" &&
        !a.chosen.empty()) {
      m = _c._catalog.find(a.chosen);
    }
  }
  if (!m || !_c.runs_soon(*m, "video", refs ? "edit" : "generate")) {
    return make_error(Code::Unsupported,
                      refs ? "no video model that carries a clip on or "
                             "draws on references runs here"
                           : "no video model runs here");
  }
  // Ref2VA from the prompt alone has no keyframe to open on.
  if (named && first && !m->has(models::Capability::ImageToVideo)) {
    return make_error(Code::Unsupported, std::format(
        "{} does not open a shot on a picture: leave `model` out for "
        "this shot (Auto's opens on one)", m->name));
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
  const Json vp = jget(m->engine, "vpipe", Json::object());
  const Json defaults = jget(vp, "defaults", Json::object());
  if (last && !jget(vp, "last_frame", false)) {
    return make_error(Code::Unsupported, std::format(
        "{} does not end a shot on a picture", m->name));
  }
  // MADE AS THE CLIP BEFORE IT: on a timeline that holds clips -- or
  // carrying on from one -- a shot saying no Favor or options of its own
  // is made as the clip it comes after was, its Favor and its method
  // (Pika H3 Flex, HyperFlow, TaoMate, VDN): what keeps a style from
  // shot to shot. Asked otherwise -- the person's word -- the call says
  // its own.
  Json targs = args;
  Json carried;
  std::string favor_as;
  Json options_as = Json::array();
  if (!args.contains("favor") || !args.contains("options")) {
    std::optional<AssetId> named_from;
    if (!from_text.empty()) {
      named_from = AssetId::parse(from_text);
    }
    if (auto before = made_before_(s, comp, named_from)) {
      if (!args.contains("favor") && !before->favor.empty()) {
        targs["favor"] = before->favor;
        favor_as = "Favor " + before->favor;
      }
      if (!args.contains("options") && !before->options.empty()) {
        targs["options"] = before->options;
        for (const auto& [k, v] : before->options.items()) {
          if (const auto* w = option_words(k)) {
            options_as.push_back(w->label);
          }
        }
      }
      if (!favor_as.empty() || !options_as.empty()) {
        carried = {{"clip", before->clip}};
      }
    }
  }
  // Favor and its options, as asked: "the Pika LoRA" its switch. The
  // clip before's way not to be had here (an adapter not downloaded for
  // this model, a method not for this shot): its Favor alone, then the
  // call as it is -- said.
  const bool cont = continues || !from_text.empty();
  auto tr = tuned_(*m, targs, false, cont, first.has_value());
  if (!tr.ok() && !carried.is_null()) {
    const std::string why = tr.error().message;
    Json less = targs;
    less.erase("options");
    if (args.contains("options")) {
      less["options"] = args["options"];
    }
    if (!options_as.empty()) {
      carried["not_carried"] = {{"options", options_as}, {"why", why}};
      options_as = Json::array();
      tr = tuned_(*m, less, false, cont, first.has_value());
    }
    if (!tr.ok()) {
      carried["not_carried"] = {{"favor", favor_as}, {"why", why}};
      favor_as.clear();
      tr = tuned_(*m, args, false, cont, first.has_value());
    }
  }
  if (!carried.is_null()) {
    Json as = Json::array();
    if (!favor_as.empty()) {
      as.push_back(favor_as);
    }
    for (const auto& o : options_as) {
      as.push_back(o);
    }
    carried["as"] = std::move(as);
  }
  VALTZ_ASSIGN(const Tuned tuned, std::move(tr));
  // Its length, as the model makes lengths -- guided, what comes after
  // the tail it redoes (that marked out where it lands).
  auto [frames, fps] = shot_frames(*m, number(args, "seconds", 5));
  if (guided) {
    frames = guided_frames(*m, frames);
  }
  const double length = frames / fps;
  // Its size: the frame's, at the model's default area at most -- or,
  // HIGH-RES, the frame's own (2560 on its long side at most): past the
  // size the model was trained at, drafted there and regenerated by its
  // refiner (DESIGN §4q), which must be here.
  const int align = std::max(16, jget(vp, "align", 16));
  std::pair<int, int> size{jget(defaults, "width", 960),
                           jget(defaults, "height", 544)};
  const bool high = flag(args, "high_res");
  if (high && !comp) {
    return make_error(Code::InvalidArgument,
                      "high_res makes a shot at its timeline's size: name "
                      "the timeline (or work on one)");
  }
  bool regenerated = false;
  if (comp) {
    VALTZ_ASSIGN(media::PixelSize frame, _c.own_frame(s.project, *comp));
    const auto plain = shot_size(frame, size.first, size.second, align);
    size = high ? frame_on_grid(frame, align, 2560) : plain;
    const Json rg = jget(vp, "regenerate", Json());
    const int above = jget(rg, "above", 768);
    if (high && std::min(size.first, size.second) > above) {
      if (!rg.is_object() || !_c.refiner_of_(*m)) {
        return make_error(Code::Unsupported, std::format(
            "a shot past {}P is drafted at {} and regenerated by "
            "Open-H3-Regenerate-2K, which is not on this Mac (the person "
            "downloads it in Settings > Capabilities). Without high_res it "
            "is made at {} x {} and fills the frame.",
            above, above, plain.first, plain.second));
      }
      regenerated = true;
    }
  }
  Landing l;
  l.session = s.id;
  l.step = step.n;
  l.project = s.project;
  l.clip = true;
  l.model = m->name;
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
  req.last = last;
  req.references = pictures;
  if (soundtrack) {
    req.reference_sound = true;
  }
  // Clips and sounds it draws on CUT to its length, unless kept WHOLE.
  {
    Json whole = jget(args, "whole", Json());
    if (whole.is_boolean() && whole.get<bool>()) {
      for (const auto& id : pictures) {
        if (auto x = p->asset(id);
            x.ok() && (x->kind == AssetKind::Video ||
                       x->kind == AssetKind::Audio)) {
          req.whole.push_back(id);
        }
      }
    } else if (whole.is_array()) {
      for (const auto& w : whole) {
        if (const auto id = w.is_string() ? AssetId::parse(w.get<std::string>())
                                          : std::nullopt;
            id && std::ranges::find(pictures, *id) != pictures.end()) {
          req.whole.push_back(*id);
        }
      }
    }
  }
  // RefMods given to the video model alone: no tag for the words.
  for (const auto& w : jget(args, "latents_only", Json::array())) {
    const auto id = w.is_string() ? AssetId::parse(w.get<std::string>())
                                  : std::nullopt;
    if (!id || std::ranges::find(pictures, *id) == pictures.end()) {
      return make_error(Code::InvalidArgument, std::format(
          "latents_only: '{}' is not one of the shot's `references`",
          w.is_string() ? w.get<std::string>() : to_text(w)));
    }
    VALTZ_ASSIGN(project::Asset la, p->asset(*id));
    if (la.kind != AssetKind::RefMod) {
      return make_error(Code::InvalidArgument, std::format(
          "latents_only: \"{}\" is no RefMod (only a RefMod's latents "
          "can be the video model's alone)",
          la.name));
    }
    req.latents_only.push_back(*id);
  }
  if (regenerated) {
    req.regenerate = true;
  }
  req.preference = tuned.preference;
  req.tuning = tuned.tuning;
  // From the latents the clip kept (none, or not covering its end: made
  // without, submit_shot_); its own kept, for the shot after it.
  req.continue_latents = continuing && !by_reference;
  req.keep_latents = flag(args, "keep_latents", comp.has_value());
  Json r = {{"queued", true},
            {"seconds", secs(length)},
            {"size", {size.first, size.second}},
            {"model", m->name}};
  if (regenerated) {
    r["high_res"] = std::format(
        "drafted at {} on its short side and regenerated at {} x {}: "
        "several times as long to make",
        jget(jget(vp, "regenerate", Json::object()), "above", 768),
        size.first, size.second);
  }
  if (soundtrack) {
    r["soundtrack"] = soundtrack->str();
    r["note"] = "Its sound IS the song, as it is: do not add it to the "
                "timeline as well.";
  }
  // How the model reads each reference -- a RefMod by its first picture
  // or clip, where it falls among the rest (a clip carried on from is
  // <Video 1> before them: then said by the prompt as made).
  if (!pictures.empty() && !continuing) {
    if (auto tags = _c.video_reference_tags(s.project, m->id, pictures,
                                            std::nullopt, req.latents_only);
        tags.ok() && tags->is_object() && !tags->empty()) {
      r["named"] = *tags;
      for (const auto& id : req.latents_only) {
        r["named"][id.str()] = "(not named: the video model's alone -- "
                               "say who it is in words)";
      }
    }
  }
  if (!moved_first.empty()) {
    r["first_drawn_on"] = std::format(
        "{} is drawn on (a reference: `named` says its name) -- a shot "
        "opens on a picture alone, and one drawing on references on none",
        moved_first);
  }
  told_(r, tuned);
  if (!carried.is_null()) {
    carried["note"] = "Made as the clip before it was made, to keep its "
                      "look: give favor or options to make it otherwise "
                      "(the person asked so).";
    r["made_as_before"] = std::move(carried);
  }
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
  std::string how;
  VALTZ_ASSIGN(JobId job, submit_shot_(s, step.title, std::move(req),
                                       std::move(text), l, &how));
  if (!how.empty()) {
    r["continued_by"] = how;
  }
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

// A REFMOD made (DESIGN §4s): a character -- its look, its motion, its
// voice -- encoded once by the RefMod maker from the pictures, clips and
// sounds given, an asset of the project for shots to draw on
// (`references`) and keep it the same across them.
Result<Json>
Controller::AgentHost::t_make_refmod(Session& s, Step& step,
                                     const Json& args,
                                     std::vector<fs::path>&)
{
  auto* p = _c.project(s.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  const models::ModelEntry* m = nullptr;
  for (const auto& e : _c._catalog.models()) {
    if (!m &&
        jget<std::string>(jget(e.engine, "vpipe", Json::object()), "shape",
                          "") == "refmod" &&
        _c.runs_soon(e, "video", "refmod")) {
      m = &e;
    }
  }
  if (!m) {
    return make_error(Code::Unsupported,
                      "the RefMod maker (MiniMax H3 RefMod) does not run "
                      "here: it needs MiniMax H3 Ref2VA, which the person "
                      "downloads in Settings > Capabilities");
  }
  Json from = jget(args, "from", Json::array());
  if (from.is_string()) {
    from = Json::array({from});
  }
  std::vector<AssetId> row;
  Json holds = Json::array();
  for (const auto& x : from.is_array() ? from : Json::array()) {
    const std::string t = x.is_string() ? x.get<std::string>() : "";
    const auto id = AssetId::parse(t);
    if (!id) {
      return make_error(Code::InvalidArgument,
                        std::format("'{}' is not an asset id", t));
    }
    VALTZ_ASSIGN(project::Asset a, p->asset(*id));
    if (a.head == 0) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is still being made: wait_for_generations, then ask "
          "again",
          a.name));
    }
    if (a.kind == AssetKind::Image) {
      holds.push_back("picture");
    } else if (a.kind == AssetKind::Video) {
      holds.push_back("clip");
    } else if (a.kind == AssetKind::Audio) {
      holds.push_back("voice");
    } else {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is no picture, clip or sound to make a RefMod of",
          a.name));
    }
    row.push_back(*id);
  }
  if (row.empty()) {
    return make_error(Code::InvalidArgument,
                      "from: the pictures, clips and sounds of it");
  }
  VALTZ_ASSIGN(const Tuned tuned, tuned_(*m, args, false, false, false));
  GenerateVideoRequest req;
  req.project = s.project;
  req.model = m->id;
  req.row = row;
  req.prompt = text_arg(args, "description");
  req.name = one_line(text_arg(args, "name"));
  req.preference = tuned.preference;
  req.tuning = tuned.tuning;
  Landing l;
  l.session = s.id;
  l.step = step.n;
  l.project = s.project;
  l.model = m->name;
  auto undo = _c.command_(s.project, "agent", {}, "",
                          {{"tool", step.tool}, {"title", step.title}});
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
  step.job = job;
  Json r = {{"job", job.str()}, {"queued", true}, {"model", m->name},
            {"holds", std::move(holds)}};
  told_(r, tuned);
  if (auto rec = _c._jobs.get(job); rec && !rec->asset.is_nil()) {
    r["asset"] = rec->asset.str();
  }
  // How a shot will name it: by its first picture or clip -- a picture
  // after those the shot lists itself, a clip first in its references --
  // its voice an <Audio n> after any sound the shot draws on itself.
  const bool voiced =
      std::ranges::find(r["holds"], Json("voice")) != r["holds"].end();
  std::string named = "<Video 1>";
  std::string where = "first in a shot's references";
  for (const auto& h : r["holds"]) {
    if (h == "picture") {
      named = "<Picture 1>";
      where = "where the shot draws on no other picture";
      break;
    }
    if (h == "clip") {
      break;
    }
  }
  r["named"] = std::format("{} ({}){}", named, where,
                           voiced ? "; its voice <Audio 1> where the shot "
                                    "draws on no other sound"
                                  : "");
  r["note"] = std::format(
      "Made in the background (seconds to a minute). Once it has landed "
      "(wait_for_generations), give its id FIRST in generate_video's "
      "`references`, and name it so in `subjects`: \"<Subject 1> is <who> "
      "in {}.\" -- the shot's result tells each reference's name "
      "(`named`). It carries what its pictures and clip show around it "
      "too.", named) +
      (voiced ? " To have it SPEAK in its voice, write the words in the "
                "shot's description -- \"<Subject 1> (S1) says: <d>[English] "
                "Hello, friends!</d>\" -- not a voice-over."
              : "");
  return r;
}

Result<JobId>
Controller::AgentHost::submit_shot_(Session& s, const std::string& title,
                                    GenerateVideoRequest req,
                                    assist::ShotText text, const Landing& l,
                                    std::string* how)
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
  // Guided through the clip's end (FL2VA): no reference, its words alone.
  const bool guided = req.continue_from && req.references.empty() &&
                      guide_frames(*m) > 0;
  if (guided) {
    guided_words(text);
  } else if (req.continue_from || !req.references.empty()) {
    VALTZ_ASSIGN(Json tags,
                 _c.video_reference_tags(s.project, req.model,
                                         req.references, req.continue_from,
                                         req.latents_only));
    text.pictures.clear();
    text.clips.clear();
    text.sounds.clear();
    // Each by its kind -- a RefMod by what it is named by -- the sounds
    // read first, so the clip continued's is numbered after them.
    const std::string sound_mark = tag("audio", 0).substr(0, 3);
    int listed_sounds = 0;
    for (const auto& id : req.references) {
      const std::string t = jget<std::string>(tags, id.str().c_str(), "");
      if (t.empty()) {
        continue;  // the video model's alone: the words never name it
      }
      VALTZ_ASSIGN(project::Asset ra, p->asset(id));
      if (ra.kind == AssetKind::Audio) {
        text.sounds.push_back(t);
        ++listed_sounds;
      } else if (ra.kind == AssetKind::Video) {
        text.clips.push_back(t);
      } else if (ra.kind == AssetKind::RefMod && !sound_mark.empty() &&
                 t.starts_with(sound_mark)) {
        text.sounds.push_back(t);
      } else {
        text.pictures.push_back(t);
      }
    }
    text.sound_reused = req.reference_sound && !text.sounds.empty();
    if (req.continue_from) {
      text.continued =
          jget<std::string>(tags, req.continue_from->str().c_str(), "");
      VALTZ_ASSIGN(project::Asset c, p->asset(*req.continue_from));
      if (_c.sounds_(*p, c, 0)) {
        text.continued_sound = tag("audio", listed_sounds + 1);
      }
    }
  } else if (req.first && req.last) {
    // Opening on one picture and ending on another (FL2VA): the two as
    // that case of its guide names them, bare -- in the description too.
    const auto both = jget<std::string>(m->prompting, "first_last_tag", "");
    text.first = both.empty() ? tag("image", 1) : numbered(both, 1);
    text.last = both.empty() ? tag("image", 2) : numbered(both, 2);
    text.seconds = req.fps > 0 ? req.frames / req.fps : 0;
    text.description = assist::retag(text.description,
                                     {{tag("image", 1), text.first},
                                      {tag("image", 2), text.last}});
  } else if (req.first) {
    text.first = tag("image", 1);
  } else if (req.last) {
    // Ending on one alone (L2VA): <Picture 1>, at the clip's end.
    text.last = tag("image", 1);
    text.seconds = req.fps > 0 ? req.frames / req.fps : 0;
  }
  req.prompt = assist::shot_prompt(text);
  auto undo = _c.command_(s.project, "agent",
                          l.composition.value_or(AssetId{}), "",
                          {{"tool", "generate_video"}, {"title", title}});
  // From the latents it kept -- refused before anything is written where
  // it kept none that cover its end: then without them.
  const bool continuing = req.continue_from.has_value();
  const bool carried = continuing && req.continue_latents;
  GenerateVideoRequest again = carried ? req : GenerateVideoRequest{};
  auto made = _c.generate_video(std::move(req));
  bool kept = carried;
  if (!made.ok() && carried && latents_refused(made.error())) {
    again.continue_latents = false;
    kept = false;
    made = _c.generate_video(std::move(again));
  }
  VALTZ_ASSIGN(JobId job, std::move(made));
  if (how && guided) {
    *how = kept ? std::format("latent-guided: its last {} frames redone "
                              "from the latents it kept, then on",
                              guide_frames(*m))
                : std::format("latent-guided: its last {} frames encoded "
                              "(it kept no latents that cover its end) "
                              "and redone, then on",
                              guide_frames(*m));
  } else if (how && continuing) {
    *how = kept ? "its latents carried beside its last seconds, read as a "
                  "reference"
           : carried ? "its last seconds read as a reference (it kept no "
                       "latents that cover its end)"
                     : "its last seconds read as a reference";
  }
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
    std::string how;
    if (!stopped && !landed) {
      job = make_error(Code::InvalidArgument, std::format(
          "the shot it carries on from (step {}) was not made{}", after,
          why.empty() ? "" : ": " + why));
    } else if (!stopped) {
      h.req.continue_from = *landed;
      job = submit_shot_(s, h.title, std::move(h.req), std::move(h.text),
                         h.landing, &how);
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
          if (!how.empty()) {
            st.result["continued_by"] = how;
          }
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
