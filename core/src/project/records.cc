#include "valtz/project/records.h"

#include <algorithm>
#include <array>
#include <chrono>

namespace valtz::project {

namespace {

constexpr std::array<const char*, 8> kKindNames = {
  "text", "image", "video", "audio", "contact-sheet", "mask", "latent",
  "other",
};

constexpr std::array<const char*, 5> kClassNames = {
  "flat", "markup", "generated", "still", "composition",
};

Json
to_json_markup(const Markup& m)
{
  Json j = {{"objects", m.objects}};
  if (!m.raster.empty()) {
    j["raster"] = m.raster;
  }
  if (m.width > 0 && m.height > 0) {
    j["w"] = m.width;
    j["h"] = m.height;
  }
  put_rest(j, m.rest);
  return j;
}

Markup
markup_from_json(const Json& m)
{
  Markup mk;
  mk.raster = jget(m, "raster", BlobRef{});
  mk.objects = jget(m, "objects", Json::array());
  if (!mk.objects.is_array()) {
    mk.objects = Json::array();
  }
  mk.width = std::max(0, jget<std::int32_t>(m, "w", 0));
  mk.height = std::max(0, jget<std::int32_t>(m, "h", 0));
  mk.rest = rest_of(m, {"raster", "objects", "w", "h"});
  return mk;
}

Json
to_json_time(const LayerTime& t)
{
  Json j = Json::object();
  if (t.in >= 0) { j["in"] = t.in; }
  if (t.out >= 0) { j["out"] = t.out; }
  if (t.rate.num > 0) {
    j["rate_num"] = t.rate.num;
    j["rate_den"] = t.rate.den;
  }
  if (t.offset != 0) { j["offset"] = t.offset; }
  if (t.duration > 0) { j["duration"] = t.duration; }
  return j;
}

LayerTime
time_from_json(const Json& j)
{
  LayerTime t;
  t.in = std::max<std::int64_t>(-1, jget<std::int64_t>(j, "in", -1));
  t.out = std::max<std::int64_t>(-1, jget<std::int64_t>(j, "out", -1));
  const auto num = jget<std::int64_t>(j, "rate_num", 0);
  const auto den = jget<std::int64_t>(j, "rate_den", 1);
  if (num > 0 && den > 0) { t.rate = Rational(num, den); }
  t.offset = std::max<std::int64_t>(0, jget<std::int64_t>(j, "offset", 0));
  t.duration = std::max<std::int64_t>(0, jget<std::int64_t>(j, "duration", 0));
  return t;
}

}

const char*
to_str(AssetKind k)
{
  auto i = static_cast<std::size_t>(k);
  return i < kKindNames.size() ? kKindNames[i] : "other";
}

const char*
to_str(Origin o)
{
  return o == Origin::Source ? "source" : "derived";
}

const char*
to_str(AssetClass c)
{
  auto i = static_cast<std::size_t>(c);
  return i < kClassNames.size() ? kClassNames[i] : "flat";
}

AssetClass
asset_class_from_str(std::string_view s)
{
  for (std::size_t i = 0; i < kClassNames.size(); ++i) {
    if (s == kClassNames[i]) {
      return static_cast<AssetClass>(i);
    }
  }
  return AssetClass::Flat;
}

AssetKind
asset_kind_from_str(std::string_view s)
{
  for (std::size_t i = 0; i < kKindNames.size(); ++i) {
    if (s == kKindNames[i]) {
      return static_cast<AssetKind>(i);
    }
  }
  return AssetKind::Other;
}

AssetKind
asset_kind_for(media::MediaType t)
{
  switch (t) {
  case media::MediaType::Image: return AssetKind::Image;
  case media::MediaType::Video: return AssetKind::Video;
  case media::MediaType::Audio: return AssetKind::Audio;
  case media::MediaType::Text:  return AssetKind::Text;
  case media::MediaType::Unknown: break;
  }
  return AssetKind::Other;
}

std::int64_t
now_ms()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

ContentHash
fingerprint(const Recipe& r, const std::vector<ResolvedInput>& inputs,
            std::string_view model_digest)
{
  // nlohmann objects are std::map-backed, so dump() emits sorted keys
  // with no whitespace: a canonical form for equal documents.
  Json ins = Json::array();
  for (const auto& in : inputs) {
    ins.push_back({{"role", in.role}, {"hash", in.hash}});
  }
  Json doc = {
    {"op", r.op},
    {"op_version", r.op_version},
    {"model", r.model},
    {"model_digest", std::string(model_digest)},
    {"params", r.params},
    {"inputs", ins},
  };
  return ContentHash::of(doc.dump());
}

// ---- codecs -----------------------------------------------------------

void
to_json(Json& j, const OutputSettings& o)
{
  j = {{"color", o.color},
       {"fps", o.fps},
       {"channels", o.channels},
       {"sample_rate", o.sample_rate}};
}

void
from_json(const Json& j, OutputSettings& o)
{
  const OutputSettings d;
  o.color = jget<std::string>(j, "color", d.color);
  o.fps = jget(j, "fps", d.fps);
  if (o.fps.num <= 0 || o.fps.den <= 0) {
    o.fps = d.fps;
  }
  o.channels = std::clamp(jget(j, "channels", d.channels), 1, 2);
  o.sample_rate = jget(j, "sample_rate", d.sample_rate);
  if (o.sample_rate < 8000 || o.sample_rate > 192000) {
    o.sample_rate = d.sample_rate;
  }
}

void
to_json(Json& j, const BlobRef& b)
{
  j = {{"hash", b.hash}, {"size", b.size}, {"ext", b.ext}};
}

void
from_json(const Json& j, BlobRef& b)
{
  b.hash = jget(j, "hash", ContentHash{});
  b.size = jget<std::uint64_t>(j, "size", 0);
  b.ext = jget<std::string>(j, "ext", "");
}

Json
rest_of(const Json& j, std::initializer_list<std::string_view> known)
{
  Json out = Json::object();
  if (!j.is_object()) {
    return out;
  }
  for (const auto& [k, v] : j.items()) {
    if (std::ranges::find(known, std::string_view(k)) == known.end()) {
      out[k] = v;
    }
  }
  return out;
}

void
put_rest(Json& j, const Json& rest)
{
  if (!rest.is_object() || rest.empty()) {
    return;
  }
  for (const auto& [k, v] : rest.items()) {
    if (!j.contains(k)) {
      j[k] = v;
    }
  }
}

void
to_json(Json& j, const Asset& a)
{
  j = {
    {"id", a.id},
    {"name", a.name},
    {"kind", a.kind == AssetKind::Other && !a.kind_name.empty()
                 ? a.kind_name : std::string(to_str(a.kind))},
    {"origin", to_str(a.origin)},
    {"class", to_str(a.cls)},
    {"head", a.head},
    {"recipe", a.recipe},
    {"tags", a.tags},
    {"created", a.created_ms},
    {"modified", a.modified_ms},
    {"source_path", a.source_path},
    {"linked", a.linked},
    {"extra", a.extra},
  };
  if (!a.modifiers.empty()) {
    Json mods = Json::array();
    for (const auto& m : a.modifiers) {
      Json mj = {{"kind", m.kind}, {"layer", m.layer},
                 {"params", m.params}};
      put_rest(mj, m.rest);
      mods.push_back(std::move(mj));
    }
    j["modifiers"] = std::move(mods);
  }
  if (a.linked) {
    j["link"] = {{"bookmark", a.link.bookmark}, {"size", a.link.size},
                 {"mtime_ns", a.link.mtime_ns}};
  }
  if (!a.layers.empty()) {
    Json ls = Json::array();
    for (const auto& l : a.layers) {
      Json lj = {{"id", l.id}, {"name", l.name}, {"visible", l.visible}};
      if (l.source) {
        lj["source"] = *l.source;
        if (l.source_version > 0) {
          lj["source_version"] = l.source_version;
        }
      }
      if (l.mask) {
        lj["mask"] = true;
      }
      if (!l.time.identity() || l.time.rate.num > 0) {
        lj["time"] = to_json_time(l.time);
      }
      // From before 2026-10-04: kept until the migration takes them.
      if (l.own) {
        lj["own"] = true;
      }
      if (l.markup) {
        lj["markup"] = to_json_markup(*l.markup);
      }
      put_rest(lj, l.rest);
      ls.push_back(std::move(lj));
    }
    j["layers"] = std::move(ls);
  }
  if (a.canvas.set() || a.canvas.framed()) {
    j["canvas"] = media::to_json(a.canvas);
  }
  if (a.timeline_frames > 0) {
    j["timeline"] = a.timeline_frames;
  }
  if (a.rate.num > 0) {
    j["rate_num"] = a.rate.num;
    j["rate_den"] = a.rate.den;
  }
  if (a.pages > 1) {
    j["pages"] = a.pages;
  }
  if (!a.transitions.empty()) {
    Json ts = Json::array();
    for (const auto& t : a.transitions) {
      Json tj = {{"from", t.from}, {"to", t.to}, {"kind", t.kind}};
      put_rest(tj, t.rest);
      ts.push_back(std::move(tj));
    }
    j["transitions"] = std::move(ts);
  }
  if (a.markup) {
    j["markup"] = to_json_markup(*a.markup);
  }
  if (!a.from.is_null()) {
    j["from"] = a.from;
  }
  if (!a.folder.empty()) {
    j["folder"] = a.folder;
  }
  put_rest(j, a.rest);
}

void
from_json(const Json& j, Asset& a)
{
  a.id = jget(j, "id", AssetId{});
  a.name = jget<std::string>(j, "name", "");
  const auto kind = jget<std::string>(j, "kind", "other");
  a.kind = asset_kind_from_str(kind);
  a.kind_name = a.kind == AssetKind::Other && kind != "other" ? kind : "";
  a.origin = jget<std::string>(j, "origin", "source") == "derived"
                 ? Origin::Derived
                 : Origin::Source;
  a.cls = asset_class_from_str(jget<std::string>(j, "class", "flat"));
  a.head = jget<std::uint32_t>(j, "head", 0);
  a.recipe = jget(j, "recipe", RecipeId{});
  a.tags = jget(j, "tags", std::vector<std::string>{});
  a.created_ms = jget<std::int64_t>(j, "created", 0);
  a.modified_ms = jget<std::int64_t>(j, "modified", 0);
  a.source_path = jget<std::string>(j, "source_path", "");
  a.linked = jget(j, "linked", false);
  Json link = jget(j, "link", Json::object());
  a.link.bookmark = jget<std::string>(link, "bookmark", "");
  a.link.size = jget<std::uint64_t>(link, "size", 0);
  a.link.mtime_ns = jget<std::int64_t>(link, "mtime_ns", 0);
  a.extra = jget(j, "extra", Json::object());
  a.modifiers.clear();
  for (const auto& m : jget(j, "modifiers", Json::array())) {
    Modifier mod;
    mod.kind = jget<std::string>(m, "kind", "");
    mod.layer = jget<std::string>(m, "layer", "");
    mod.params = jget(m, "params", Json::object());
    mod.rest = rest_of(m, {"kind", "layer", "params"});
    if (!mod.kind.empty()) { a.modifiers.push_back(std::move(mod)); }
  }
  a.canvas = media::stack_canvas_from_json(jget(j, "canvas", Json::object()));
  a.timeline_frames = std::max<std::int64_t>(0, jget<std::int64_t>(j, "timeline", 0));
  {
    const auto num = jget<std::int64_t>(j, "rate_num", 0);
    const auto den = jget<std::int64_t>(j, "rate_den", 1);
    a.rate = num > 0 && den > 0 ? Rational(num, den) : Rational(0, 1);
  }
  a.pages = std::max<std::int64_t>(1, jget<std::int64_t>(j, "pages", 1));
  a.transitions.clear();
  for (const auto& t : jget(j, "transitions", Json::array())) {
    Transition tr;
    tr.from = jget<std::string>(t, "from", "");
    tr.to = jget<std::string>(t, "to", "");
    tr.kind = jget<std::string>(t, "kind", "cut");
    tr.rest = rest_of(t, {"from", "to", "kind"});
    a.transitions.push_back(std::move(tr));
  }
  if (const Json m = jget(j, "markup", Json()); m.is_object()) {
    a.markup = markup_from_json(m);
  } else {
    a.markup.reset();
  }
  a.from = jget(j, "from", Json());
  a.folder = jget<std::string>(j, "folder", "");
  a.layers.clear();
  for (const auto& l : jget(j, "layers", Json::array())) {
    Layer layer;
    layer.id = jget<std::string>(l, "id", "");
    layer.name = jget<std::string>(l, "name", "");
    layer.visible = jget(l, "visible", true);
    layer.own = jget(l, "own", false);
    if (l.contains("source")) {
      layer.source = jget(l, "source", AssetId{});
      if (layer.source->is_nil()) {
        layer.source.reset();
      }
      layer.source_version = jget<std::uint32_t>(l, "source_version", 0);
    }
    if (const Json m = jget(l, "markup", Json()); m.is_object()) {
      layer.markup = markup_from_json(m);
    }
    layer.mask = jget(l, "mask", false);
    layer.time = time_from_json(jget(l, "time", Json::object()));
    layer.rest = rest_of(l, {"id", "name", "visible", "own", "source",
                             "source_version", "markup", "mask", "time"});
    a.layers.push_back(std::move(layer));
  }
  a.rest = rest_of(j, {"id", "name", "kind", "origin", "class", "head",
                       "recipe", "tags", "created", "modified",
                       "source_path", "linked", "link", "extra",
                       "modifiers", "canvas", "timeline", "rate_num",
                       "rate_den", "pages", "transitions", "markup",
                       "from", "folder", "layers"});
}

void
to_json(Json& j, const RecipeInput& in)
{
  j = {{"role", in.role}, {"asset", in.asset}, {"version", in.version}};
}

void
from_json(const Json& j, RecipeInput& in)
{
  in.role = jget<std::string>(j, "role", "");
  in.asset = jget(j, "asset", AssetId{});
  in.version = jget<std::uint32_t>(j, "version", 0);
}

void
to_json(Json& j, const Recipe& r)
{
  j = {
    {"id", r.id},
    {"op", r.op},
    {"op_version", r.op_version},
    {"model", r.model},
    {"params", r.params},
    {"inputs", r.inputs},
    {"deterministic", r.deterministic},
    {"created", r.created_ms},
  };
  put_rest(j, r.rest);
}

void
from_json(const Json& j, Recipe& r)
{
  r.id = jget(j, "id", RecipeId{});
  r.op = jget<std::string>(j, "op", "");
  r.op_version = jget<std::uint32_t>(j, "op_version", 1);
  r.model = jget<std::string>(j, "model", "");
  r.params = jget(j, "params", Json::object());
  r.inputs = jget(j, "inputs", std::vector<RecipeInput>{});
  r.deterministic = jget(j, "deterministic", false);
  r.created_ms = jget<std::int64_t>(j, "created", 0);
  r.rest = rest_of(j, {"id", "op", "op_version", "model", "params",
                       "inputs", "deterministic", "created"});
}

void
to_json(Json& j, const ResolvedInput& in)
{
  j = {{"role", in.role}, {"asset", in.asset}, {"version", in.version},
       {"hash", in.hash}};
}

void
from_json(const Json& j, ResolvedInput& in)
{
  in.role = jget<std::string>(j, "role", "");
  in.asset = jget(j, "asset", AssetId{});
  in.version = jget<std::uint32_t>(j, "version", 0);
  in.hash = jget(j, "hash", ContentHash{});
}

void
to_json(Json& j, const AssetVersion& v)
{
  j = {
    {"asset", v.asset},
    {"number", v.number},
    {"blob", v.blob},
    {"info", v.info},
    {"created", v.created_ms},
    {"recipe", v.recipe},
    {"executed", v.executed},
    {"inputs", v.inputs},
    {"fingerprint", v.fingerprint},
    {"engine", v.engine},
    {"host", v.host},
    {"content", v.content},
    {"timing", v.timing},
  };
  if (!v.outputs.empty()) {
    j["outputs"] = v.outputs;
  }
  put_rest(j, v.rest);
}

void
from_json(const Json& j, AssetVersion& v)
{
  v.asset = jget(j, "asset", AssetId{});
  v.number = jget<std::uint32_t>(j, "number", 0);
  v.blob = jget(j, "blob", BlobRef{});
  v.info = jget(j, "info", media::MediaInfo{});
  v.created_ms = jget<std::int64_t>(j, "created", 0);
  v.recipe = jget(j, "recipe", RecipeId{});
  v.executed = jget(j, "executed", Json::object());
  v.inputs = jget(j, "inputs", std::vector<ResolvedInput>{});
  v.fingerprint = jget(j, "fingerprint", ContentHash{});
  v.engine = jget<std::string>(j, "engine", "");
  v.host = jget<std::string>(j, "host", "");
  v.content = jget(j, "content", ContentHash{});
  v.timing = jget(j, "timing", Json::object());
  v.outputs = jget(j, "outputs", Json::object());
  v.rest = rest_of(j, {"asset", "number", "blob", "info", "created",
                       "recipe", "executed", "inputs", "fingerprint",
                       "engine", "host", "content", "timing", "outputs"});
}

void
to_json(Json& j, const SubjectEntry& e)
{
  j = {{"role", e.role}, {"asset", e.asset}, {"version", e.version},
       {"note", e.note}};
}

void
from_json(const Json& j, SubjectEntry& e)
{
  e.role = jget<std::string>(j, "role", "");
  e.asset = jget(j, "asset", AssetId{});
  e.version = jget<std::uint32_t>(j, "version", 0);
  e.note = jget<std::string>(j, "note", "");
}

void
to_json(Json& j, const Subject& s)
{
  j = {
    {"id", s.id},
    {"name", s.name},
    {"kind", s.kind},
    {"entries", s.entries},
    {"tags", s.tags},
    {"created", s.created_ms},
    {"modified", s.modified_ms},
  };
  put_rest(j, s.rest);
}

void
from_json(const Json& j, Subject& s)
{
  s.id = jget(j, "id", SubjectId{});
  s.name = jget<std::string>(j, "name", "");
  s.kind = jget<std::string>(j, "kind", "");
  s.entries = jget(j, "entries", std::vector<SubjectEntry>{});
  s.tags = jget(j, "tags", std::vector<std::string>{});
  s.created_ms = jget<std::int64_t>(j, "created", 0);
  s.modified_ms = jget<std::int64_t>(j, "modified", 0);
  s.rest = rest_of(j, {"id", "name", "kind", "entries", "tags", "created",
                       "modified"});
}

void
to_json(Json& j, const HistoryEntry& h)
{
  j = {
    {"id", h.id},
    {"created", h.created_ms},
    {"role", h.role},
    {"picture", h.picture},
    {"width", h.width},
    {"height", h.height},
    {"asset", h.asset},
    {"version", h.version},
    {"generation", h.generation},
    {"generation_version", h.generation_version},
    {"label", h.label},
    {"rendered", h.rendered},
    {"kind", h.kind},
  };
  if (h.frames > 0 || h.seconds > 0) {
    j["frames"] = h.frames;
    j["seconds"] = h.seconds;
  }
  put_rest(j, h.rest);
}

void
from_json(const Json& j, HistoryEntry& h)
{
  h.id = jget(j, "id", HistoryId{});
  h.created_ms = jget<std::int64_t>(j, "created", 0);
  h.role = jget<std::string>(j, "role", "");
  h.picture = jget(j, "picture", BlobRef{});
  h.width = jget(j, "width", 0);
  h.height = jget(j, "height", 0);
  h.asset = jget(j, "asset", AssetId{});
  h.version = jget<std::uint32_t>(j, "version", 0);
  h.generation = jget(j, "generation", AssetId{});
  h.generation_version = jget<std::uint32_t>(j, "generation_version", 0);
  h.label = jget<std::string>(j, "label", "");
  h.rendered = jget(j, "rendered", false);
  h.frames = jget<std::int64_t>(j, "frames", 0);
  h.seconds = jget(j, "seconds", 0.0);
  h.kind = jget<std::string>(j, "kind", h.frames > 0 ? "video" : "image");
  h.rest = rest_of(j, {"id", "created", "role", "picture", "width",
                       "height", "asset", "version", "generation",
                       "generation_version", "label", "rendered", "kind",
                       "frames", "seconds"});
}

}

namespace valtz::media {

void
to_json(Json& j, const ColorInfo& c)
{
  j = {
    {"primaries", static_cast<int>(c.primaries)},
    {"transfer", static_cast<int>(c.transfer)},
    {"matrix", static_cast<int>(c.matrix)},
    {"range", static_cast<int>(c.range)},
  };
  if (!c.icc_name.empty()) {
    j["icc"] = c.icc_name;
  }
  if (c.mastering) {
    const auto& m = *c.mastering;
    j["mastering"] = {m.red_x,   m.red_y,   m.green_x,       m.green_y,
                      m.blue_x,  m.blue_y,  m.white_x,       m.white_y,
                      m.max_luminance,      m.min_luminance};
  }
  if (c.content_light) {
    j["cll"] = {c.content_light->max_cll, c.content_light->max_fall};
  }
}

void
from_json(const Json& j, ColorInfo& c)
{
  c = ColorInfo{};
  c.primaries = static_cast<Primaries>(jget<int>(j, "primaries", 2));
  c.transfer = static_cast<Transfer>(jget<int>(j, "transfer", 2));
  c.matrix = static_cast<Matrix>(jget<int>(j, "matrix", 2));
  c.range = static_cast<Range>(jget<int>(j, "range", 0));
  c.icc_name = jget<std::string>(j, "icc", "");
  auto m = jget(j, "mastering", std::vector<std::uint32_t>{});
  if (m.size() == 10) {
    MasteringDisplay md;
    md.red_x = static_cast<std::uint16_t>(m[0]);
    md.red_y = static_cast<std::uint16_t>(m[1]);
    md.green_x = static_cast<std::uint16_t>(m[2]);
    md.green_y = static_cast<std::uint16_t>(m[3]);
    md.blue_x = static_cast<std::uint16_t>(m[4]);
    md.blue_y = static_cast<std::uint16_t>(m[5]);
    md.white_x = static_cast<std::uint16_t>(m[6]);
    md.white_y = static_cast<std::uint16_t>(m[7]);
    md.max_luminance = m[8];
    md.min_luminance = m[9];
    c.mastering = md;
  }
  auto cll = jget(j, "cll", std::vector<std::uint16_t>{});
  if (cll.size() == 2) {
    c.content_light = ContentLight{cll[0], cll[1]};
  }
}

void
to_json(Json& j, const FrameDesc& f)
{
  j = {
    {"w", f.width},
    {"h", f.height},
    {"format", info(f.format).name},
    {"color", f.color},
    {"alpha", to_str(f.alpha)},
    {"par", f.pixel_aspect},
    {"bits", f.bits_per_component},
  };
}

void
from_json(const Json& j, FrameDesc& f)
{
  f = FrameDesc{};
  f.width = jget<std::int32_t>(j, "w", 0);
  f.height = jget<std::int32_t>(j, "h", 0);
  auto fmt = jget<std::string>(j, "format", "unknown");
  for (int i = 0; i < 16; ++i) {
    auto pf = static_cast<PixelFormat>(i);
    if (fmt == info(pf).name) {
      f.format = pf;
      break;
    }
  }
  f.color = jget(j, "color", ColorInfo{});
  auto a = jget<std::string>(j, "alpha", "none");
  f.alpha = a == "straight"        ? AlphaMode::Straight
            : a == "premultiplied" ? AlphaMode::Premultiplied
                                   : AlphaMode::None;
  f.pixel_aspect = jget(j, "par", Rational{1, 1});
  f.bits_per_component = jget<std::uint8_t>(j, "bits", 8);
}

void
to_json(Json& j, const MediaInfo& m)
{
  j = {
    {"type", to_str(m.type)},
    {"uti", m.uti},
    {"codec", m.codec},
    {"codec_name", m.codec_name},
    {"frame", m.frame},
  };
  if (m.type == MediaType::Video || m.type == MediaType::Audio) {
    j["fps"] = m.frame_rate;
    j["duration"] = {m.duration.value, m.duration.timescale};
    j["frames"] = m.frame_count;
    j["audio"] = {m.has_audio, m.audio_channels, m.audio_sample_rate};
  }
  if (m.has_hdr_gain_map) {
    j["gain_map"] = true;
  }
  if (m.image_count > 1) {
    j["images"] = m.image_count;
  }
  if (m.exif.is_object() && !m.exif.empty()) {
    j["exif"] = m.exif;
  }
}

void
from_json(const Json& j, MediaInfo& m)
{
  m = MediaInfo{};
  auto t = jget<std::string>(j, "type", "unknown");
  m.type = t == "image"   ? MediaType::Image
           : t == "video" ? MediaType::Video
           : t == "audio" ? MediaType::Audio
           : t == "text"  ? MediaType::Text
                          : MediaType::Unknown;
  m.uti = jget<std::string>(j, "uti", "");
  m.codec = jget<std::string>(j, "codec", "");
  m.codec_name = jget<std::string>(j, "codec_name", "");
  m.frame = jget(j, "frame", FrameDesc{});
  m.frame_rate = jget(j, "fps", Rational{0, 1});
  auto d = jget(j, "duration", std::vector<std::int64_t>{});
  if (d.size() == 2) {
    m.duration = MediaTime{d[0], static_cast<std::int32_t>(d[1])};
  }
  m.frame_count = jget<std::int64_t>(j, "frames", 0);
  if (auto it = j.find("audio"); it != j.end() && it->is_array() &&
                                 it->size() == 3) {
    m.has_audio = (*it)[0].get<bool>();
    m.audio_channels = (*it)[1].get<std::int32_t>();
    m.audio_sample_rate = (*it)[2].get<std::int32_t>();
  }
  m.has_hdr_gain_map = jget(j, "gain_map", false);
  m.image_count = jget<std::int32_t>(j, "images", 1);
  m.exif = jget(j, "exif", Json::object());
  if (!m.exif.is_object()) {
    m.exif = Json::object();
  }
}

}
