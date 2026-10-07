// The Controller's COMPOSITIONS (DESIGN §6a): layers, their looks and
// time, markup, the project's composition, instantiation and decomposing,
// and the RENDERINGS everything else draws from -- an asset as a file,
// made once and cached under its render key.

#include "valtz/controller/controller.h"

#include "valtz/base/log.h"
#include "valtz/base/text.h"
#include "valtz/media/layers.h"
#include "valtz/media/markup.h"
#include "valtz/media/model-input.h"
#include "valtz/media/probe.h"
#include "valtz/media/sound.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <format>
#include <numbers>

namespace valtz {

namespace fs = std::filesystem;
using project::AssetClass;
using project::AssetKind;

namespace {

bool
is_comp(const project::Asset& a)
{
  return a.cls == AssetClass::Still || a.cls == AssetClass::Composition;
}

bool
is_timeline(const project::Asset& a)
{
  return a.cls == AssetClass::Composition;
}

// What it is when shown: a clip (a clip, a composition with a frame), a
// sound (a sound, a composition of sound alone) -- else a picture.
bool
clip_like(const project::Asset& s)
{
  return s.kind == AssetKind::Video;
}

bool
sound_like(const project::Asset& s)
{
  return s.kind == AssetKind::Audio;
}

std::vector<project::Layer>::iterator
find_layer(std::vector<project::Layer>& ls, const std::string& id)
{
  return std::find_if(ls.begin(), ls.end(),
                      [&](const project::Layer& l) { return l.id == id; });
}

// A new layer's id: one past the largest.
std::string
next_layer_id(const std::vector<project::Layer>& ls)
{
  int next = 1;
  for (const auto& l : ls) {
    next = std::max(next, std::atoi(l.id.c_str()) + 1);
  }
  return std::to_string(next);
}

double
seconds_of(const project::AssetVersion& v)
{
  if (v.info.duration.valid() && v.info.duration.seconds() > 0) {
    return v.info.duration.seconds();
  }
  const double fps = v.info.frame_rate.to_double();
  return fps > 0 && v.info.frame_count > 0
             ? static_cast<double>(v.info.frame_count) / fps : 0.0;
}

// The modifiers of one layer, renamed to another.
std::vector<project::Modifier>
layer_mods(const project::Asset& a, const std::string& from,
           const std::string& to)
{
  std::vector<project::Modifier> out;
  for (const auto& m : a.modifiers) {
    if (m.layer == from) {
      project::Modifier mm = m;
      mm.layer = to;
      out.push_back(std::move(mm));
    }
  }
  return out;
}

void
drop_layer_mods(std::vector<project::Modifier>& mods, const std::string& id)
{
  std::erase_if(mods, [&](const project::Modifier& m) {
    return m.layer == id;
  });
}

// ---- pages (a still's; DESIGN §6a) ------------------------------------

// A still with PAGES: drawn a page at a time, its layers' time in pages.
bool
paged(const project::Asset& a)
{
  return a.cls == AssetClass::Still && a.pages > 1;
}

// Is the layer on page `page` of `pages`? It starts at its offset and
// runs its duration, 0 to the last page.
bool
on_page(const project::LayerTime& t, std::int64_t page, std::int64_t pages)
{
  if (page < t.offset || page >= pages) {
    return false;
  }
  return t.duration <= 0 || page < t.offset + t.duration;
}

// A key at `f` with the value the track has there, unless one is.
template <class T>
void
pin_key(media::Keyed<T>& k, std::int64_t f)
{
  if (f < 0 || std::ranges::any_of(k.keys, [&](const auto& x) {
        return x.frame == f;
      })) {
    return;
  }
  k.keys.push_back({f, k.at(static_cast<double>(f))});
  std::ranges::sort(k.keys, {}, &media::Keyframe<T>::frame);
}

// A page made at local frame `at`: every page there and after one later,
// each showing what it showed -- the new one between its neighbours.
template <class T>
void
insert_key_frame(media::Keyed<T>& k, std::int64_t at)
{
  if (k.keys.size() < 2 || at > k.keys.back().frame) {
    return;
  }
  if (at > k.keys.front().frame) {
    pin_key(k, at - 1);
    pin_key(k, at);
  }
  for (auto& x : k.keys) {
    if (x.frame >= at) {
      ++x.frame;
    }
  }
}

// The page at local frame `at` gone: every page after one earlier, each
// showing what it showed.
template <class T>
void
remove_key_frame(media::Keyed<T>& k, std::int64_t at)
{
  if (k.keys.size() < 2 || at > k.keys.back().frame) {
    return;
  }
  if (at >= k.keys.front().frame) {
    pin_key(k, at - 1);
    pin_key(k, at + 1);
    std::erase_if(k.keys, [&](const auto& x) { return x.frame == at; });
  }
  for (auto& x : k.keys) {
    if (x.frame > at) {
      --x.frame;
    }
  }
}

// A layer's look keyed by page, a page made (`by` +1) or gone (-1) at its
// local frame `at`.
void
shift_layer_keys(std::vector<project::Modifier>& mods,
                 const std::string& layer, std::int64_t at, int by)
{
  for (auto& m : mods) {
    if (m.layer != layer) {
      continue;
    }
    // Written back only when it changed: a picture's flat value stays one.
    if (m.kind == "adjust") {
      const auto was = media::keyed_adjustments_from_json(m.params);
      auto k = was;
      by > 0 ? insert_key_frame(k, at) : remove_key_frame(k, at);
      if (!(k == was)) {
        m.params = media::to_json(k);
      }
    } else if (m.kind == "crop") {
      const auto was = media::keyed_crop_from_json(m.params);
      auto k = was;
      if (by > 0) {
        insert_key_frame(k.place, at);
        insert_key_frame(k.turn, at);
      } else {
        remove_key_frame(k.place, at);
        remove_key_frame(k.turn, at);
      }
      if (!(k == was)) {
        m.params = media::to_json(k);
      }
    }
  }
  std::erase_if(mods, [&](const project::Modifier& m) {
    return m.layer == layer && m.params.is_object() && m.params.empty();
  });
}

// ---- placements composed (Decompose) ---------------------------------

// A layer's placement on a frame: content pixels -> frame pixels (Core
// Image's frame, y up), as compose_images draws it -- through its crop,
// or centred at its own size.
media::Affine
placement(const media::Crop& c, media::PixelSize content,
          media::PixelSize frame)
{
  media::Crop k = c;
  if (k.identity()) {
    media::Affine m;
    m.tx = (frame.width - content.width) / 2.0;
    m.ty = (frame.height - content.height) / 2.0;
    return m;
  }
  if (k.canvas.width == 0 || k.canvas.height == 0) {
    k.canvas = frame;
  }
  if (k.content.width == 0) {
    k.content = content;
  }
  return media::crop_placement(k, content.width, content.height).transform;
}

media::Affine
then(const media::Affine& first, const media::Affine& second)
{
  media::Affine o;
  o.a = second.a * first.a + second.c * first.b;
  o.b = second.b * first.a + second.d * first.b;
  o.c = second.a * first.c + second.c * first.d;
  o.d = second.b * first.c + second.d * first.d;
  o.tx = second.a * first.tx + second.c * first.ty + second.tx;
  o.ty = second.b * first.tx + second.d * first.ty + second.ty;
  return o;
}

// The crop that places `content` on `frame` by `m` -- none when `m`
// skews or flips (a stretch under a turn): a crop scales its own axes,
// then turns.
std::optional<media::Crop>
crop_for(const media::Affine& m, media::PixelSize content,
         media::PixelSize frame)
{
  const double sx = std::hypot(m.a, m.b);
  const double sy = std::hypot(m.c, m.d);
  if (sx <= 0 || sy <= 0) {
    return std::nullopt;
  }
  // Columns (a, b) = sx (cos, -sin) and (c, d) = sy (sin, cos).
  const double co = m.a / sx;
  const double si = -m.b / sx;
  if (std::abs(m.c / sy - si) > 1e-6 || std::abs(m.d / sy - co) > 1e-6) {
    return std::nullopt;
  }
  media::Crop c;
  c.content = content;
  c.canvas = frame;
  c.scale_x = sx;
  c.scale_y = sy;
  c.rotate = std::atan2(si, co) * 180 / std::numbers::pi;
  const double cx = m.a * content.width / 2 + m.c * content.height / 2 +
                    m.tx;
  const double cy = m.b * content.width / 2 + m.d * content.height / 2 +
                    m.ty;
  c.offset_x = (cx - frame.width / 2.0) / frame.width;
  c.offset_y = -(cy - frame.height / 2.0) / frame.height;
  c.pad = {0, 0, 0, 0};
  return c;
}

}

// ---- modifiers -----------------------------------------------------------

Status
Controller::set_modifier_(ProjectId pid, AssetId aid, const std::string& kind,
                          const std::string& layer, Json params)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_comp(a)) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", a.name}});
  }
  if (std::ranges::none_of(a.layers, [&](const project::Layer& l) {
        return l.id == layer;
      })) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  std::vector<project::Modifier> mods;
  for (auto& m : a.modifiers) {
    if (!(m.kind == kind && m.layer == layer)) {
      mods.push_back(std::move(m));
    }
  }
  if (params.is_object() && !params.empty()) {
    mods.push_back({kind, layer, std::move(params)});
  }
  VALTZ_TRY(p->set_modifiers(aid, std::move(mods)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "modified"}});
  return ok_status();
}

Json
Controller::modifier_of_(const project::Asset& a, const std::string& kind,
                         const std::string& layer)
{
  for (const auto& m : a.modifiers) {
    if (m.kind == kind && m.layer == layer) {
      return m.params;
    }
  }
  return Json::object();
}

Status
Controller::set_adjustments(ProjectId pid, AssetId aid,
                            const media::Adjustments& adj,
                            const std::string& layer)
{
  auto undo = command_(pid, "adjust", aid, layer);
  return set_modifier_(pid, aid, "adjust", layer, media::to_json(adj));
}

media::Adjustments
Controller::adjustments_of(const project::Asset& a, const std::string& layer)
{
  // A track's first key, for a picture.
  const auto k = adjustment_keys_of(a, layer);
  return k.keys.empty() ? media::Adjustments{} : k.keys.front().value;
}

Status
Controller::set_crop(ProjectId pid, AssetId aid, const media::Crop& c,
                     const std::string& layer)
{
  auto undo = command_(pid, "crop", aid, layer);
  return set_modifier_(pid, aid, "crop", layer, media::to_json(c));
}

media::Crop
Controller::crop_of(const project::Asset& a, const std::string& layer)
{
  return crop_keys_of(a, layer).at(0);
}

Status
Controller::set_adjustment_keys(ProjectId pid, AssetId aid,
                                const media::KeyedAdjustments& k,
                                const std::string& layer)
{
  auto undo = command_(pid, "keys", aid, layer);
  return set_modifier_(pid, aid, "adjust", layer, media::to_json(k));
}

Status
Controller::set_crop_keys(ProjectId pid, AssetId aid,
                          const media::KeyedCrop& k, const std::string& layer)
{
  auto undo = command_(pid, "keys", aid, layer);
  return set_modifier_(pid, aid, "crop", layer, media::to_json(k));
}

Status
Controller::set_speed_keys(ProjectId pid, AssetId aid,
                           const media::KeyedSpeed& k,
                           const std::string& layer)
{
  auto undo = command_(pid, "keys", aid, layer);
  return set_modifier_(pid, aid, "speed", layer, media::to_json(k));
}

Status
Controller::set_sound_keys(ProjectId pid, AssetId aid,
                           const media::KeyedSound& k,
                           const std::string& layer,
                           std::optional<bool> follow_speed)
{
  auto undo = command_(pid, "keys", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  // Beside the keys, a number (the app reads a modifier's numbers): kept
  // while it is on, even with every key the identity.
  Json j = media::to_json(k);
  if (follow_speed.value_or(pitch_follows_speed(a, layer))) {
    j["follow_speed"] = 1;
  }
  return set_modifier_(pid, aid, "audio", layer, std::move(j));
}

media::KeyedAdjustments
Controller::adjustment_keys_of(const project::Asset& a,
                               const std::string& layer)
{
  return media::keyed_adjustments_from_json(
      modifier_of_(a, "adjust", layer));
}

media::KeyedCrop
Controller::crop_keys_of(const project::Asset& a, const std::string& layer)
{
  return media::keyed_crop_from_json(modifier_of_(a, "crop", layer));
}

media::KeyedSpeed
Controller::speed_keys_of(const project::Asset& a, const std::string& layer)
{
  return media::keyed_speed_from_json(modifier_of_(a, "speed", layer));
}

media::KeyedSound
Controller::sound_keys_of(const project::Asset& a, const std::string& layer)
{
  return media::keyed_sound_from_json(modifier_of_(a, "audio", layer));
}

bool
Controller::pitch_follows_speed(const project::Asset& a,
                                const std::string& layer)
{
  const Json m = modifier_of_(a, "audio", layer);
  const auto it = m.find("follow_speed");
  if (it == m.end()) {
    return false;
  }
  return it->is_boolean() ? it->get<bool>()
         : it->is_number() ? it->get<double>() != 0
                           : false;
}

Status
Controller::set_layer_time(ProjectId pid, AssetId aid,
                           const std::string& layer,
                           const project::LayerTime& t)
{
  auto undo = command_(pid, "time", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_comp(a)) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", a.name}});
  }
  auto ls = a.layers;
  auto it = find_layer(ls, layer);
  if (it == ls.end()) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  project::LayerTime n = t;
  n.offset = std::max<std::int64_t>(0, n.offset);
  n.duration = std::max<std::int64_t>(0, n.duration);
  if (n.in >= 0 && n.out >= 0 && n.out < n.in) {
    std::swap(n.in, n.out);
  }
  it->time = n;
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

Status
Controller::set_transition(ProjectId pid, AssetId aid,
                           const std::string& from, const std::string& to,
                           const std::string& kind)
{
  auto undo = command_(pid, "transition", aid, from);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_timeline(a)) {
    return make_error(Code::InvalidArgument, msg::kTimelineNeedsClip);
  }
  auto has = [&](const std::string& id) {
    return std::ranges::any_of(a.layers, [&](const project::Layer& l) {
      return l.id == id;
    });
  };
  if (from == to || !has(from) || !has(to)) {
    return make_error(Code::InvalidArgument, msg::kTransitionLayers);
  }
  if (!kind.empty() && kind != "cut" && kind != "dissolve") {
    return make_error(Code::InvalidArgument, msg::kTransitionLayers);
  }
  VALTZ_TRY(p->update_asset(aid, [&](project::Asset& x) -> Status {
    std::erase_if(x.transitions, [&](const project::Transition& t) {
      return (t.from == from && t.to == to) ||
             (t.from == to && t.to == from);
    });
    if (!kind.empty()) {
      project::Transition t;
      t.from = from;
      t.to = to;
      t.kind = kind;
      x.transitions.push_back(std::move(t));
    }
    return ok_status();
  }));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

// ---- layers ------------------------------------------------------------

Status
Controller::check_source_(project::Project& p, const project::Asset& comp,
                          const project::Asset& src)
{
  if (src.kind == AssetKind::Text || src.kind == AssetKind::Other) {
    return make_error(Code::InvalidArgument, msg::kLayerSourceKind);
  }
  if (src.cls == AssetClass::Flat || src.cls == AssetClass::Generated) {
    if (src.head == 0) {
      return make_error(Code::NotFound, msg::kAssetHasNoContent);
    }
  }
  if (comp.cls == AssetClass::Still) {
    // A picture, or a frame of a clip; no sound.
    if (sound_like(src)) {
      return make_error(Code::InvalidArgument, msg::kLayerSourceKind);
    }
  } else if (comp.kind == AssetKind::Audio && !sound_like(src)) {
    return make_error(Code::InvalidArgument, msg::kSoundOnlyComposition);
  }
  // Never itself, nor anything that shows it.
  if (src.id == comp.id) {
    return make_error(Code::InvalidArgument, msg::kRecursiveAsset,
                      {{"name", src.name}});
  }
  std::set<AssetId> seen;
  std::vector<AssetId> todo{src.id};
  while (!todo.empty()) {
    const AssetId x = todo.back();
    todo.pop_back();
    if (!seen.insert(x).second) {
      continue;
    }
    if (x == comp.id) {
      return make_error(Code::InvalidArgument, msg::kRecursiveAsset,
                        {{"name", src.name}});
    }
    if (auto xa = p.asset(x); xa.ok()) {
      for (const auto& l : xa->layers) {
        if (l.source) {
          todo.push_back(*l.source);
        }
      }
    }
  }
  return ok_status();
}

Result<std::string>
Controller::add_layer(ProjectId pid, AssetId aid, const std::string& above,
                      std::optional<std::int64_t> page)
{
  auto undo = command_(pid, "layer.add", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_comp(a)) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", a.name}});
  }
  auto ls = a.layers;
  project::Layer added;
  added.id = ls.empty() ? std::string() : next_layer_id(ls);
  if (paged(a) && page) {
    // Made on a page: on that page alone.
    added.time.offset = std::clamp<std::int64_t>(*page, 0, a.pages - 1);
    added.time.duration = 1;
  }
  const std::string id = added.id;
  auto at = find_layer(ls, above);
  ls.insert(at == ls.end() ? ls.end() : at + 1, std::move(added));
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return id;
}

Status
Controller::move_layer(ProjectId pid, AssetId aid, const std::string& layer,
                       int by)
{
  auto undo = command_(pid, "layer.move", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  auto ls = a.layers;
  auto it = find_layer(ls, layer);
  // On a frame of its own every layer moves; a stack from before keeps
  // its bottom one -- the frame -- where it is.
  const bool any = a.canvas.framed() || a.kind == AssetKind::Audio;
  if (it == ls.end() || (it == ls.begin() && !any && by < 0) ||
      (layer.empty() && !any)) {
    return make_error(Code::InvalidArgument, "no such layer to move");
  }
  const auto i = std::distance(ls.begin(), it);
  const auto to = std::clamp<std::ptrdiff_t>(
      i + by, any ? 0 : 1, static_cast<std::ptrdiff_t>(ls.size()) - 1);
  if (to != i) {
    project::Layer l = std::move(ls[i]);
    ls.erase(ls.begin() + i);
    ls.insert(ls.begin() + to, std::move(l));
    VALTZ_TRY(p->set_layers(aid, std::move(ls)));
    post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                      {"reason", "layers"}});
  }
  return ok_status();
}

Status
Controller::set_layer_visible(ProjectId pid, AssetId aid,
                              const std::string& layer, bool visible)
{
  auto undo = command_(pid, visible ? "layer.show" : "layer.hide", aid,
                         layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  auto ls = a.layers;
  auto it = find_layer(ls, layer);
  if (it == ls.end()) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  it->visible = visible;
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

Status
Controller::rename_layer(ProjectId pid, AssetId aid,
                         const std::string& layer, std::string name)
{
  auto undo = command_(pid, "layer.rename", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  auto ls = a.layers;
  auto it = find_layer(ls, layer);
  if (it == ls.end()) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  it->name = utf8_prefix(name, 200);
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

Status
Controller::set_layer_source(ProjectId pid, AssetId aid,
                             const std::string& layer, AssetId source,
                             std::optional<JobId> job)
{
  auto undo = job ? join_(pid, *job)
                  : command_(pid, "layer.source", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_comp(a)) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", a.name}});
  }
  VALTZ_ASSIGN(project::Asset src, p->asset(source));
  VALTZ_TRY(check_source_(*p, a, src));
  auto ls = a.layers;
  auto it = find_layer(ls, layer);
  if (it == ls.end()) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  // A clip dropped on a blank layer of a timeline: after what is there.
  const bool append = !job && !it->source && sequenced_(a, src);
  // What it shows now: marks were the old one's; where it starts stays.
  it->source = source;
  it->source_version = 0;
  it->time.in = -1;
  it->time.out = -1;
  it->time.rate = {0, 1};
  if (append) {
    VALTZ_ASSIGN(it->time.offset, content_end_(pid, aid));
    it->time.duration = 0;
  }
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  if (append) {
    VALTZ_TRY(grow_to_content_(pid, *p, aid));
  }
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

Status
Controller::remove_layer(ProjectId pid, AssetId aid,
                         const std::string& layer)
{
  auto undo = command_(pid, "layer.remove", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  auto ls = a.layers;
  auto it = find_layer(ls, layer);
  if (it == ls.end()) {
    return make_error(Code::InvalidArgument, "no such layer to remove");
  }
  if (ls.size() < 2) {
    return make_error(Code::InvalidArgument, msg::kRemoveLastLayer);
  }
  // On a frame of its own any layer goes, and the frame stays; a stack
  // from before has its bottom one set the frame.
  const bool any = a.canvas.framed() || a.kind == AssetKind::Audio;
  if (it == ls.begin() && !any) {
    return remove_bottom_(*p, pid, a);
  }
  ls.erase(it);
  std::vector<project::Modifier> mods = a.modifiers;
  drop_layer_mods(mods, layer);
  VALTZ_TRY(p->set_modifiers(aid, std::move(mods)));
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  VALTZ_TRY(p->update_asset(aid, [&](project::Asset& x) -> Status {
    std::erase_if(x.transitions, [&](const project::Transition& t) {
      return t.from == layer || t.to == layer;
    });
    return ok_status();
  }));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

Status
Controller::remove_bottom_(project::Project& p, ProjectId pid,
                           const project::Asset& a)
{
  if (is_timeline(a)) {
    return make_error(Code::InvalidArgument, msg::kRemoveClipOwn);
  }
  // The canvas as it is: what everything above was placed on.
  VALTZ_ASSIGN(media::PixelSize canvas, own_frame_(p, a, 0));
  auto ls = a.layers;
  const std::string bottom = ls.front().id;
  ls.erase(ls.begin());
  project::Layer& up = ls.front();
  const std::string was = up.id;
  // Nothing beneath it to mask.
  up.mask = false;
  media::PixelSize content;
  if (up.source) {
    VALTZ_ASSIGN(content, shown_size_(p, *up.source, up.source_version, 0));
  }
  // Where it showed, as the bottom's crop: an upper layer with no crop is
  // centred at its own size, with one is placed on the canvas -- both
  // the same as a crop onto a canvas of that size. Clear around it.
  media::Crop c = crop_of(a, was);
  if (c.canvas.width == 0 || c.canvas.height == 0) {
    c.canvas = canvas;
  }
  if ((c.content.width == 0 || c.content.height == 0) &&
      content.width > 0) {
    c.content = content;
  }
  c.pad = {0, 0, 0, 0};
  const bool fits = c.content == canvas && c.canvas == canvas &&
                    c.scale_x == 1 && c.scale_y == 1 && c.offset_x == 0 &&
                    c.offset_y == 0 && c.rotate == 0;
  // Its modifiers become the bottom's; the old bottom's go.
  std::vector<project::Modifier> mods;
  for (auto m : a.modifiers) {
    if (m.layer == bottom) {
      continue;
    }
    if (m.layer == was) {
      if (m.kind == "crop") {
        continue;
      }
      m.layer = bottom;
    }
    mods.push_back(std::move(m));
  }
  if (!fits) {
    mods.push_back({"crop", bottom, media::to_json(c)});
  }
  up.id = bottom;
  VALTZ_TRY(p.set_modifiers(a.id, std::move(mods)));
  VALTZ_TRY(p.set_layers(a.id, std::move(ls)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", a.id},
                                    {"reason", "layers"}});
  return ok_status();
}

Status
Controller::set_layer_mask(ProjectId pid, AssetId aid,
                           const std::string& layer, bool mask)
{
  auto undo = command_(pid, mask ? "layer.mask" : "layer.unmask", aid,
                         layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  auto ls = a.layers;
  auto it = find_layer(ls, layer);
  if (it == ls.end()) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  if (mask && it == ls.begin()) {
    return make_error(Code::InvalidArgument, msg::kMaskNeedsLayer);
  }
  it->mask = mask;
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

Status
Controller::merge_layers(ProjectId pid, AssetId aid, const std::string& a_id,
                         const std::string& b_id)
{
  auto undo = command_(pid, "layer.merge", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  // A timeline's layers change over time: two are not one picture.
  if (is_timeline(a)) {
    return make_error(Code::InvalidArgument, msg::kMergeOnClip);
  }
  if (!is_comp(a)) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", a.name}});
  }
  VALTZ_ASSIGN(media::PixelSize frame, own_frame_(*p, a, 0));
  auto ls = a.layers;
  auto ia = find_layer(ls, a_id);
  auto ib = find_layer(ls, b_id);
  if (ia == ls.end() || ib == ls.end() || ia == ib) {
    return make_error(Code::InvalidArgument, "two layers to merge");
  }
  const std::size_t lo = std::min(ia, ib) - ls.begin();
  const std::size_t hi = std::max(ia, ib) - ls.begin();
  if (paged(a)) {
    // One picture for both: on the same pages, looking the same on each.
    auto keyed = [&](const std::string& id) {
      const auto k = crop_keys_of(a, id);
      return adjustment_keys_of(a, id).keys.size() > 1 ||
             k.place.keys.size() > 1 || k.turn.keys.size() > 1;
    };
    if (ls[lo].time.offset != ls[hi].time.offset ||
        ls[lo].time.duration != ls[hi].time.duration ||
        keyed(ls[lo].id) || keyed(ls[hi].id)) {
      return make_error(Code::InvalidArgument, msg::kMergeAcrossPages);
    }
  }
  // A mask may join them -- the upper one masking the lower, right
  // above it -- but not tie either to a layer outside the two.
  auto masks_below = [&](std::size_t i) {
    return i + 1 < ls.size() && ls[i + 1].mask;
  };
  const bool joined = ls[hi].mask && hi == lo + 1;
  if ((ls[hi].mask && !joined) || ls[lo].mask ||
      (masks_below(lo) && !joined) || masks_below(hi)) {
    return make_error(Code::InvalidArgument, msg::kMergeAcrossMask);
  }
  auto picture = [&](const project::Layer& l) -> Result<media::LayerPicture> {
    media::LayerPicture pic;
    pic.visible = l.visible;
    pic.mask = l.mask;
    pic.adjust = adjustments_of(a, l.id);
    pic.crop = crop_of(a, l.id);
    VALTZ_TRY(layer_picture_(pid, *p, a, l, frame, {}, pic, 0));
    return pic;
  };
  std::vector<media::LayerPicture> pics;
  media::StackCanvas sc;
  if (a.canvas.framed()) {
    // On the frame, as they lie on it.
    sc.frame_w = frame.width;
    sc.frame_h = frame.height;
  } else if (lo > 0) {
    // Over nothing: a clear canvas sets the size.
    media::LayerPicture clear;
    VALTZ_ASSIGN(clear.drawn,
                 media::render_markup_picture({}, frame, Json::array(), {}));
    pics.push_back(std::move(clear));
  }
  VALTZ_ASSIGN(media::LayerPicture lower, picture(ls[lo]));
  VALTZ_ASSIGN(media::LayerPicture upper, picture(ls[hi]));
  pics.push_back(std::move(lower));
  pics.push_back(std::move(upper));
  const fs::path out = p->blobs().make_tmp_path("png");
  VALTZ_TRY(media::flatten_layers(pics, out, sc));
  // The pixels: a flat picture of the project's, which the lower layer
  // shows now -- in its place, at its id.
  project::ImportOptions io;
  io.placement = project::Placement::Copy;
  io.name = std::format("{} (merged)", ls[lo].name.empty()
                                            ? a.name : ls[lo].name);
  VALTZ_ASSIGN(project::Asset merged, p->import_file(out, io));
  std::error_code ec;
  fs::remove(out, ec);
  VALTZ_TRY(p->update_asset(merged.id, [&](project::Asset& x) -> Status {
    x.cls = AssetClass::Flat;
    x.source_path.clear();
    x.folder = a.folder;
    x.from = Json{{"asset", a.id.str()}};
    return ok_status();
  }));
  const std::string lo_id = ls[lo].id, hi_id = ls[hi].id;
  project::Layer& m = ls[lo];
  m.source = merged.id;
  m.source_version = 0;
  m.mask = false;
  m.visible = true;
  ls.erase(ls.begin() + static_cast<std::ptrdiff_t>(hi));
  // Their looks are in the pixels now.
  std::vector<project::Modifier> mods = a.modifiers;
  drop_layer_mods(mods, lo_id);
  drop_layer_mods(mods, hi_id);
  VALTZ_TRY(p->set_modifiers(aid, std::move(mods)));
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

// ---- sizes and rates ----------------------------------------------------

Result<media::PixelSize>
Controller::shown_size_(project::Project& p, AssetId id,
                        std::uint32_t version, int depth)
{
  if (depth > kMaxNesting) {
    return make_error(Code::InvalidArgument, msg::kRecursiveAsset,
                      {{"name", id.str()}});
  }
  VALTZ_ASSIGN(project::Asset a, p.asset(id));
  if (is_comp(a)) {
    if (a.canvas.set()) {
      return media::PixelSize{a.canvas.width, a.canvas.height};
    }
    return own_frame_(p, a, depth + 1);
  }
  if (a.cls == AssetClass::Markup) {
    return media::PixelSize{a.markup ? a.markup->width : 0,
                            a.markup ? a.markup->height : 0};
  }
  VALTZ_ASSIGN(project::AssetVersion v, p.version(id, version));
  return media::PixelSize{v.info.frame.width, v.info.frame.height};
}

Result<media::PixelSize>
Controller::own_frame_(project::Project& p, const project::Asset& a,
                       int depth)
{
  if (a.canvas.framed()) {
    return a.canvas.frame();
  }
  if (!is_comp(a)) {
    return shown_size_(p, a.id, 0, depth);
  }
  // A stack from before: its bottom layer's picture, through its crop.
  const project::Layer* b = a.layers.empty() ? nullptr : &a.layers.front();
  media::PixelSize size;
  if (b && b->source) {
    VALTZ_ASSIGN(size, shown_size_(p, *b->source, b->source_version,
                                   depth + 1));
  }
  const std::string bottom = b ? b->id : std::string();
  const media::Crop c = crop_keys_of(a, bottom).at(0);
  // A bottom with no picture of its own -- empty, or markup alone -- is
  // its crop's canvas.
  if ((size.width <= 0 || size.height <= 0) && c.canvas.width > 0 &&
      c.canvas.height > 0) {
    return c.canvas;
  }
  if (size.width <= 0 || size.height <= 0) {
    return make_error(Code::Corrupt, "the picture has no size");
  }
  if (!c.identity()) {
    const auto pl = media::crop_placement(c, size.width, size.height);
    size = {static_cast<std::int32_t>(std::lround(pl.canvas_w)),
            static_cast<std::int32_t>(std::lround(pl.canvas_h))};
  }
  return size;
}

Rational
Controller::rate_of_(project::Project& p, const project::Asset& a)
{
  if (a.rate.num > 0) {
    return a.rate;
  }
  // From before: its bottom clip's.
  for (const auto& l : a.layers) {
    if (!l.source) {
      continue;
    }
    if (auto s = p.asset(*l.source); s.ok() && s->kind == AssetKind::Video) {
      if (s->cls == AssetClass::Composition) {
        return rate_of_(p, *s);
      }
      if (auto v = p.version(*l.source, l.source_version); v.ok() &&
          v->info.frame_rate.num > 0) {
        return v->info.frame_rate;
      }
    }
  }
  return a.kind == AssetKind::Audio ? Rational{1000, 1} : Rational{24, 1};
}

Result<media::PixelSize>
Controller::canvas_size(ProjectId pid, AssetId aid)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  return own_frame_(*p, a, 0);
}

Result<media::PixelSize>
Controller::own_frame(ProjectId pid, AssetId aid)
{
  return canvas_size(pid, aid);
}

media::StackCanvas
Controller::canvas_setting_(ProjectId pid, AssetId aid)
{
  project::Project* p = project(pid);
  if (!p) {
    return {};
  }
  auto a = p->asset(aid);
  return a.ok() ? a->canvas : media::StackCanvas{};
}

Status
Controller::set_canvas(ProjectId pid, AssetId aid, media::PixelSize size,
                       double anchor_x, double anchor_y)
{
  auto undo = command_(pid, "canvas", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_comp(a) || a.kind == AssetKind::Audio) {
    return make_error(Code::InvalidArgument, msg::kCanvasNeedsPicture);
  }
  if (size.width <= 0 || size.height <= 0 || size.width > 32768 ||
      size.height > 32768) {
    return make_error(Code::InvalidArgument, msg::kCanvasSizeOutOfRange,
                      {{"width", std::to_string(size.width)},
                       {"height", std::to_string(size.height)}});
  }
  VALTZ_ASSIGN(media::PixelSize own, own_frame_(*p, a, 0));
  VALTZ_TRY(p->set_canvas(aid, media::resize_canvas(a.canvas, own, size,
                                                    anchor_x, anchor_y)));
  // A composition's frame is its size: the resize is made its frame.
  VALTZ_TRY(fold_canvas_(pid, *p, aid));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "canvas"}});
  return ok_status();
}

Status
Controller::fold_canvas_(ProjectId pid, project::Project& p, AssetId aid)
{
  VALTZ_ASSIGN(project::Asset a, p.asset(aid));
  const media::StackCanvas c = a.canvas;
  if (!c.framed() || !c.set() ||
      (c.width == c.frame_w && c.height == c.frame_h && c.x == 0 &&
       c.y == 0)) {
    return ok_status();
  }
  // The old frame, and where it lies on the canvas (its top left, y
  // down); the new frame is the canvas.
  const double fw = c.frame_w, fh = c.frame_h;
  const double cw = c.width, ch = c.height;
  std::vector<project::Modifier> mods;
  for (const auto& m : a.modifiers) {
    if (m.kind != "crop") {
      mods.push_back(m);
    }
  }
  for (const auto& l : a.layers) {
    media::PixelSize shown;
    if (l.source) {
      if (auto s = shown_size_(p, *l.source, l.source_version, 0); s.ok()) {
        shown = *s;
      }
    }
    media::KeyedCrop k = crop_keys_of(a, l.id);
    // Where a key puts the layer's centre on the canvas, as a crop on
    // the new frame (media/crop.h crop_placement): its old canvas -- its
    // own, else the old frame -- scaled with a decode at another size.
    auto moved = [&](media::Crop key) {
      const double s = key.content.width > 0 && shown.width > 0
          ? double(shown.width) / key.content.width : 1.0;
      const double kw = (key.canvas.width > 0 ? key.canvas.width : fw) * s;
      const double kh = (key.canvas.height > 0 ? key.canvas.height : fh) *
                        s;
      // Core Image's y is up, from the old frame's bottom.
      const double x = c.x + kw / 2 + key.offset_x * kw;
      const double y = c.y + fh - (kh / 2 - key.offset_y * kh);
      key.canvas = {};
      if (shown.width > 0) {
        key.content = shown;
      }
      key.offset_x = (x - cw / 2) / cw;
      key.offset_y = (y - ch / 2) / ch;
      return key;
    };
    if (k.place.keys.empty()) {
      // Centred at its own size on the old frame: placed there now.
      const double ox = (c.x + fw / 2 - cw / 2) / cw;
      const double oy = (c.y + fh / 2 - ch / 2) / ch;
      if (std::abs(ox) < 1e-9 && std::abs(oy) < 1e-9) {
        // As it was: its modifier kept whole.
        for (const auto& m : a.modifiers) {
          if (m.kind == "crop" && m.layer == l.id) {
            mods.push_back(m);
          }
        }
        continue;
      }
      media::Crop at;
      at.content = shown;
      at.offset_x = ox;
      at.offset_y = oy;
      k.place.keys.push_back({0, at});
      if (k.rate.num <= 0) {
        k.rate = rate_of_(p, a);
      }
      k.place.rate = k.turn.rate = k.rate;
    } else {
      for (auto& key : k.place.keys) {
        key.value = moved(key.value);
      }
    }
    // A still's crop is one value, as set_crop writes it; a timeline's
    // or a paged still's, its tracks.
    const bool flat = a.cls == AssetClass::Still && a.pages <= 1 &&
                      k.place.keys.size() <= 1 && k.turn.keys.size() <= 1;
    const Json j = flat ? media::to_json(k.at(0)) : media::to_json(k);
    if (j.is_object() && !j.empty()) {
      mods.push_back({"crop", l.id, j});
    }
  }
  VALTZ_TRY(p.set_modifiers(aid, std::move(mods)));
  // The frame the canvas, which stays SET -- the same size, at no
  // offset: it draws the same, and it says the composition was resized,
  // so a new take lies on it rather than sizing it again
  // (place_in_project).
  media::StackCanvas framed{c.width, c.height, 0, 0};
  framed.frame_w = c.width;
  framed.frame_h = c.height;
  VALTZ_TRY(p.set_canvas(aid, framed));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "canvas"}});
  return ok_status();
}

Status
Controller::reset_canvas(ProjectId pid, AssetId aid)
{
  auto undo = command_(pid, "canvas", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // Back to the own frame -- which a composition keeps.
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  media::StackCanvas own;
  own.frame_w = a.canvas.frame_w;
  own.frame_h = a.canvas.frame_h;
  VALTZ_TRY(p->set_canvas(aid, own));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "canvas"}});
  return ok_status();
}

Status
Controller::set_timeline(ProjectId pid, AssetId aid, std::int64_t frames)
{
  auto undo = command_(pid, "timeline", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_timeline(a)) {
    return make_error(Code::InvalidArgument, msg::kTimelineNeedsClip);
  }
  if (frames < 0 || frames > 1000 * 60 * 60 * 24) {
    return make_error(Code::InvalidArgument, msg::kTimelineOutOfRange);
  }
  VALTZ_TRY(p->set_timeline(aid, frames));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "timeline"}});
  return ok_status();
}

// ---- pages ---------------------------------------------------------------

Result<std::int64_t>
Controller::add_page(ProjectId pid, AssetId aid,
                     std::optional<std::int64_t> after)
{
  auto undo = command_(pid, "page.add", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (a.cls != AssetClass::Still) {
    return make_error(Code::InvalidArgument, msg::kPagesNeedStill);
  }
  if (a.pages >= kMaxPages) {
    return make_error(Code::InvalidArgument, msg::kTooManyPages,
                      {{"pages", std::to_string(kMaxPages)}});
  }
  // The new page's index: after `after`, else after the last.
  const std::int64_t at =
      std::clamp<std::int64_t>(after ? *after + 1 : a.pages, 0, a.pages);
  // Pages share one frame: a stack from before has its bottom layer set
  // it, which a page's look could move -- framed where it is now.
  media::StackCanvas canvas = a.canvas;
  if (!canvas.framed()) {
    VALTZ_ASSIGN(media::PixelSize own, own_frame_(*p, a, 0));
    canvas.frame_w = own.width;
    canvas.frame_h = own.height;
  }
  auto ls = a.layers;
  auto mods = a.modifiers;
  for (auto& l : ls) {
    project::LayerTime& t = l.time;
    if (t.offset >= at) {
      ++t.offset;  // later: it moves with its pages
    } else if (t.duration <= 0 || t.offset + t.duration > at) {
      // Across it: one page longer, its keys where its pages went.
      if (t.duration > 0) {
        ++t.duration;
      }
      shift_layer_keys(mods, l.id, at - t.offset, +1);
    }
  }
  VALTZ_TRY(p->update_asset(aid, [&](project::Asset& x) -> Status {
    x.layers = std::move(ls);
    x.modifiers = std::move(mods);
    x.canvas = canvas;
    x.pages = a.pages + 1;
    return ok_status();
  }));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "pages"}});
  return at;
}

Status
Controller::remove_page(ProjectId pid, AssetId aid, std::int64_t page)
{
  auto undo = command_(pid, "page.remove", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (a.cls != AssetClass::Still) {
    return make_error(Code::InvalidArgument, msg::kPagesNeedStill);
  }
  if (page < 0 || page >= a.pages) {
    return make_error(Code::InvalidArgument, msg::kNoSuchPage,
                      {{"page", std::to_string(page + 1)}});
  }
  if (a.pages < 2) {
    return make_error(Code::InvalidArgument, msg::kRemoveLastPage);
  }
  std::vector<project::Layer> ls;
  auto mods = a.modifiers;
  std::set<std::string> gone;
  for (auto l : a.layers) {
    project::LayerTime& t = l.time;
    if (t.offset > page) {
      --t.offset;  // later: it moves with its pages
    } else if (on_page(t, page, a.pages)) {
      // On it: one page shorter -- gone with it, when it was its only.
      const std::int64_t end = t.duration > 0
          ? std::min(t.offset + t.duration, a.pages) : a.pages;
      if (end - t.offset <= 1) {
        gone.insert(l.id);
        continue;
      }
      if (t.duration > 0) {
        --t.duration;
      }
      shift_layer_keys(mods, l.id, page - t.offset, -1);
    }
    ls.push_back(std::move(l));
  }
  for (const auto& id : gone) {
    drop_layer_mods(mods, id);
  }
  VALTZ_TRY(p->update_asset(aid, [&](project::Asset& x) -> Status {
    x.layers = std::move(ls);
    x.modifiers = std::move(mods);
    std::erase_if(x.transitions, [&](const project::Transition& t) {
      return gone.count(t.from) || gone.count(t.to);
    });
    x.pages = a.pages - 1;
    return ok_status();
  }));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "pages"}});
  return ok_status();
}

Status
Controller::set_layer_pages(ProjectId pid, AssetId aid,
                            const std::string& layer, std::int64_t first,
                            std::int64_t count)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (a.cls != AssetClass::Still) {
    return make_error(Code::InvalidArgument, msg::kPagesNeedStill);
  }
  if (first < 0 || first >= a.pages) {
    return make_error(Code::InvalidArgument, msg::kNoSuchPage,
                      {{"page", std::to_string(first + 1)}});
  }
  auto it = std::ranges::find(a.layers, layer, &project::Layer::id);
  if (it == a.layers.end()) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  project::LayerTime t = it->time;
  t.offset = first;
  t.duration = std::max<std::int64_t>(0, count);  // 0: to the last page
  return set_layer_time(pid, aid, layer, t);
}

// ---- markup ---------------------------------------------------------------

Result<std::string>
Controller::markup_layer(ProjectId pid, AssetId aid,
                         const std::vector<std::string>& selected,
                         std::optional<std::int64_t> page)
{
  auto undo = command_(pid, "markup.layer", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_comp(a) || a.kind == AssetKind::Audio) {
    return make_error(Code::InvalidArgument, msg::kMarkupNeedsPicture);
  }
  VALTZ_ASSIGN(media::PixelSize frame, own_frame_(*p, a, 0));
  auto ls = a.layers;
  // A still with pages draws on the page shown: a layer elsewhere is not
  // drawn on, and a new one is that page's alone.
  const bool pages = paged(a) && page.has_value();
  const std::int64_t at =
      pages ? std::clamp<std::int64_t>(*page, 0, a.pages - 1) : 0;
  auto here = [&](const project::Layer& l) {
    return !pages || on_page(l.time, at, a.pages);
  };
  auto shows_markup = [&](const project::Layer& l) {
    if (!l.source || !here(l)) {
      return false;
    }
    auto s = p->asset(*l.source);
    return s.ok() && s->cls == AssetClass::Markup;
  };
  // A markup of the composition's frame, new.
  auto new_markup = [&]() -> Result<AssetId> {
    project::Asset m;
    m.name = std::format("{} markup", a.name);
    m.kind = AssetKind::Image;
    m.cls = AssetClass::Markup;
    m.folder = a.folder;
    project::Markup mk;
    mk.width = frame.width;
    mk.height = frame.height;
    m.markup = std::move(mk);
    VALTZ_ASSIGN(project::Asset made, p->add_asset(std::move(m), 64));
    post_("assets.changed", JobId{}, {{"project", pid},
                                      {"asset", made.id},
                                      {"reason", "defined"}});
    return made.id;
  };
  // The top one of the selection: empty, it takes a markup; showing one,
  // it is drawn on.
  std::ptrdiff_t top = -1;
  for (std::size_t i = 0; i < ls.size(); ++i) {
    if (std::find(selected.begin(), selected.end(), ls[i].id) !=
        selected.end()) {
      top = static_cast<std::ptrdiff_t>(i);
    }
  }
  std::string id;
  bool changed = false;
  if (top >= 0 && ls[top].empty() && here(ls[top])) {
    VALTZ_ASSIGN(AssetId m, new_markup());
    ls[top].source = m;
    id = ls[top].id;
    changed = true;
  } else if (top >= 0 && shows_markup(ls[top])) {
    id = ls[top].id;
  } else if (!ls.empty() && shows_markup(ls.back())) {
    id = ls.back().id;
  } else {
    VALTZ_ASSIGN(AssetId m, new_markup());
    project::Layer l;
    l.id = ls.empty() ? std::string() : next_layer_id(ls);
    l.source = m;
    if (pages) {
      l.time.offset = at;
      l.time.duration = 1;
    }
    id = l.id;
    ls.push_back(std::move(l));
    changed = true;
  }
  if (changed) {
    VALTZ_TRY(p->set_layers(aid, std::move(ls)));
    post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                      {"reason", "layers"}});
  }
  return id;
}

Result<std::pair<AssetId, project::Markup>>
Controller::markup_at_(project::Project& p, const project::Asset& comp,
                       const std::string& layer)
{
  for (const auto& l : comp.layers) {
    if (l.id != layer || !l.source) {
      continue;
    }
    VALTZ_ASSIGN(project::Asset m, p.asset(*l.source));
    if (m.cls != AssetClass::Markup) {
      break;
    }
    project::Markup mk = m.markup.value_or(project::Markup{});
    if (mk.width <= 0 || mk.height <= 0) {
      // A size it never had: the frame it is drawn on.
      VALTZ_ASSIGN(media::PixelSize f, own_frame_(p, comp, 0));
      mk.width = f.width;
      mk.height = f.height;
    }
    return std::pair{m.id, std::move(mk)};
  }
  return make_error(Code::InvalidArgument, "not a markup layer");
}

Status
Controller::put_markup_(ProjectId pid, project::Project& p, AssetId comp,
                        AssetId markup, project::Markup mk)
{
  VALTZ_TRY(p.update_asset(markup, [&](project::Asset& x) -> Status {
    x.markup = std::move(mk);
    return ok_status();
  }));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", markup},
                                    {"reason", "modified"}});
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", comp},
                                    {"reason", "layers"}});
  return ok_status();
}

Status
Controller::paint_stroke(ProjectId pid, AssetId aid,
                         const std::string& layer, const media::Stroke& s)
{
  auto undo = command_(pid, "markup.paint", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  VALTZ_ASSIGN(auto at, markup_at_(*p, a, layer));
  auto& [mid, mk] = at;
  const fs::path raster = mk.raster.empty()
                              ? fs::path() : p->blobs().path_of(mk.raster);
  const fs::path out = p->blobs().make_tmp_path("png");
  VALTZ_TRY(media::paint_stroke(raster, {mk.width, mk.height}, s, out));
  VALTZ_ASSIGN(mk.raster, p->blobs().adopt_file(out, "png"));
  return put_markup_(pid, *p, aid, mid, std::move(mk));
}

Status
Controller::set_markup_objects(ProjectId pid, AssetId aid,
                               const std::string& layer, const Json& objects)
{
  auto undo = command_(pid, "markup.objects", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  VALTZ_ASSIGN(auto at, markup_at_(*p, a, layer));
  auto& [mid, mk] = at;
  mk.objects = media::normalize_objects(objects);
  return put_markup_(pid, *p, aid, mid, std::move(mk));
}

Status
Controller::materialize_markup(ProjectId pid, AssetId aid,
                               const std::string& layer,
                               const std::vector<std::string>& objects)
{
  auto undo = command_(pid, "markup.pixels", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  VALTZ_ASSIGN(auto at, markup_at_(*p, a, layer));
  auto& [mid, mk] = at;
  // Drawn: the ones named; kept: the rest.
  const std::set<std::string> named(objects.begin(), objects.end());
  std::set<std::string> kept;
  Json rest = Json::array();
  for (const auto& o : mk.objects) {
    const auto id = jget<std::string>(o, "id", "");
    if (!named.count(id)) {
      kept.insert(id);
      rest.push_back(o);
    }
  }
  if (rest.size() == mk.objects.size()) {
    return ok_status();
  }
  const fs::path raster = mk.raster.empty()
                              ? fs::path() : p->blobs().path_of(mk.raster);
  const fs::path out = p->blobs().make_tmp_path("png");
  VALTZ_TRY(media::render_markup(raster, {mk.width, mk.height}, mk.objects,
                                 kept, out));
  VALTZ_ASSIGN(mk.raster, p->blobs().adopt_file(out, "png"));
  mk.objects = std::move(rest);
  return put_markup_(pid, *p, aid, mid, std::move(mk));
}

// ---- renderings -----------------------------------------------------------

Result<ContentHash>
Controller::render_key_(project::Project& p, AssetId id,
                        std::uint32_t version, int depth)
{
  if (depth > kMaxNesting) {
    return make_error(Code::InvalidArgument, msg::kRecursiveAsset,
                      {{"name", id.str()}});
  }
  return p.content_key(id, version);
}

Result<fs::path>
Controller::cached_(ProjectId pid, const std::string& key,
                    const std::function<Status(const fs::path&)>& make)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // One maker at a time: two asking for one rendering get one.
  std::lock_guard<std::recursive_mutex> lk(_render_mu);
  if (is_ephemeral_(pid)) {
    // Kept in the package: nothing of the session in the shared cache.
    const fs::path dir = p->blobs().tmp_dir() / "renders";
    const fs::path out = dir / key;
    std::error_code ec;
    if (fs::exists(out, ec)) {
      return out;
    }
    fs::create_directories(dir, ec);
    const fs::path part = dir / (".part-" + key);
    if (auto st = make(part); !st.ok()) {
      fs::remove(part, ec);
      return st.error();
    }
    fs::rename(part, out, ec);
    if (ec) {
      return make_error(Code::Io, std::format("cannot keep {}: {}",
                                              out.string(), ec.message()));
    }
    return out;
  }
  if (auto hit = _cache->find(key)) {
    return *hit;
  }
  // Made under its own extension (a writer picks its container by it),
  // then staged.
  const fs::path staged = _cache->staging_path(key);
  const fs::path part = staged.parent_path() / (".part-" + key);
  std::error_code ec;
  if (auto st = make(part); !st.ok()) {
    fs::remove(part, ec);
    return st.error();
  }
  fs::rename(part, staged, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot stage {}: {}",
                                            key, ec.message()));
  }
  return _cache->commit(key);
}

Result<fs::path>
Controller::rendered_(ProjectId pid, AssetId id, std::uint32_t version,
                      int depth)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(id));
  if (a.cls == AssetClass::Flat || a.cls == AssetClass::Generated) {
    return p->media_path(id, version);
  }
  VALTZ_ASSIGN(ContentHash key, render_key_(*p, id, version, depth));
  if (a.cls == AssetClass::Markup) {
    const project::Markup mk = a.markup.value_or(project::Markup{});
    return cached_(pid, cache::CacheStore::key_for(key, "markup.png"),
                   [&](const fs::path& out) -> Status {
      const fs::path raster = mk.raster.empty()
                                  ? fs::path() : p->blobs().path_of(mk.raster);
      return media::render_markup(raster, {mk.width, mk.height}, mk.objects,
                                  {}, out);
    });
  }
  if (a.cls == AssetClass::Still) {
    return rendered_page_(pid, id, 0, depth);  // its first page
  }
  // A composition: its sound alone, or a movie with its sound.
  if (a.kind == AssetKind::Audio) {
    VALTZ_ASSIGN(fs::path mix, mixed_(pid, id, depth));
    if (mix.empty()) {
      return make_error(Code::NotFound, msg::kAssetHasNoContent);
    }
    return mix;
  }
  return cached_(pid, cache::CacheStore::key_for(key, "movie.mov"),
                 [&](const fs::path& out) -> Status {
    VALTZ_ASSIGN(media::MovieStack stack,
                 movie_stack_(pid, id, true, std::nullopt, depth + 1));
    VALTZ_ASSIGN(fs::path sound, mixed_(pid, id, depth));
    VALTZ_ASSIGN(media::PixelSize size, shown_size_(*p, id, 0, depth));
    return media::encode_movie_stack(stack, rate_of_(*p, a), size, sound,
                                     out);
  });
}

Result<fs::path>
Controller::rendered_page_(ProjectId pid, AssetId id, std::int64_t page,
                           int depth)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(id));
  if (a.cls != AssetClass::Still) {
    return rendered_(pid, id, 0, depth);
  }
  VALTZ_ASSIGN(ContentHash key, render_key_(*p, id, 0, depth));
  const std::int64_t at = std::clamp<std::int64_t>(page, 0, a.pages - 1);
  const std::string variant = a.pages > 1 ? std::format("page-{}.png", at)
                                          : std::string("still.png");
  return cached_(pid, cache::CacheStore::key_for(key, variant),
                 [&](const fs::path& out) -> Status {
    VALTZ_ASSIGN(auto pics, stack_pictures_(pid, id, std::nullopt,
                                            std::nullopt, {}, depth + 1,
                                            at));
    return media::flatten_layers(pics, out, a.canvas);
  });
}

Result<fs::path>
Controller::mixed_(ProjectId pid, AssetId id, int depth)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(ContentHash key, render_key_(*p, id, 0, depth));
  VALTZ_ASSIGN(media::SoundPlan plan, sound_plan_(pid, id, depth + 1));
  bool any = false;
  for (const auto& l : plan.layers) {
    any = any || !l.file.empty();
  }
  if (!any || plan.seconds <= 0) {
    return fs::path();
  }
  bool silent = false;
  VALTZ_ASSIGN(fs::path out,
               cached_(pid, cache::CacheStore::key_for(key, "mix.wav"),
                       [&](const fs::path& out) -> Status {
    VALTZ_ASSIGN(bool made, media::mix_sound(plan, out));
    if (!made) {
      // Nothing sounded: an empty file says so, cached as the answer.
      silent = true;
      std::FILE* f = std::fopen(out.c_str(), "wb");
      if (f) {
        std::fclose(f);
      }
    }
    return ok_status();
  }));
  std::error_code ec;
  if (silent || fs::file_size(out, ec) == 0) {
    return fs::path();
  }
  return out;
}

Result<fs::path>
Controller::rendered_frame_(ProjectId pid, AssetId id, std::uint32_t version,
                            std::int64_t frame, int depth)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(id));
  if (!clip_like(a)) {
    return rendered_(pid, id, version, depth);
  }
  frame = std::max<std::int64_t>(0, frame);
  VALTZ_ASSIGN(ContentHash key, render_key_(*p, id, version, depth));
  const std::string variant = std::format("frame-{}.png", frame);
  if (a.cls == AssetClass::Composition) {
    return cached_(pid, cache::CacheStore::key_for(key, variant),
                   [&](const fs::path& out) -> Status {
      VALTZ_ASSIGN(media::MovieStack stack,
                   movie_stack_(pid, id, true, std::nullopt, depth + 1));
      return media::write_stack_frame(stack, rate_of_(*p, a), frame, out);
    });
  }
  VALTZ_ASSIGN(project::AssetVersion v, p->version(id, version));
  VALTZ_ASSIGN(fs::path src, p->media_path(id, version));
  const double fps = v.info.frame_rate.num > 0
                         ? v.info.frame_rate.to_double() : 24.0;
  return cached_(pid, cache::CacheStore::key_for(key, variant),
                 [&](const fs::path& out) -> Status {
    return media::write_movie_frame(src, (frame + 0.5) / fps, out);
  });
}

Result<fs::path>
Controller::rendered(ProjectId pid, AssetId id, std::uint32_t version)
{
  return rendered_(pid, id, version, 0);
}

Status
Controller::layer_picture_(ProjectId pid, project::Project& p,
                           const project::Asset& comp,
                           const project::Layer& l, media::PixelSize frame,
                           const std::set<std::string>& hidden,
                           media::LayerPicture& pic, int depth)
{
  if (!l.source) {
    return ok_status();
  }
  VALTZ_ASSIGN(project::Asset s, p.asset(*l.source));
  if (s.cls == AssetClass::Markup) {
    // Drawn in memory with the objects the app is not drawing itself.
    const project::Markup mk = s.markup.value_or(project::Markup{});
    const media::PixelSize size =
        mk.width > 0 && mk.height > 0 ? media::PixelSize{mk.width, mk.height}
                                      : frame;
    const fs::path raster = mk.raster.empty()
                                ? fs::path() : p.blobs().path_of(mk.raster);
    const bool drawn = std::any_of(
        mk.objects.begin(), mk.objects.end(), [&](const Json& o) {
          return !hidden.count(jget<std::string>(o, "id", ""));
        });
    if (!drawn && !raster.empty()) {
      pic.file = raster;
      return ok_status();
    }
    VALTZ_ASSIGN(pic.drawn, media::render_markup_picture(
                                raster, size, mk.objects, hidden));
    return ok_status();
  }
  if (sound_like(s)) {
    return ok_status();  // a sound has no picture
  }
  if (clip_like(s)) {
    // A still shows the frame at its mark-in.
    VALTZ_ASSIGN(pic.file, rendered_frame_(pid, s.id, l.source_version,
                                           std::max<std::int64_t>(
                                               0, l.time.in),
                                           depth + 1));
    return ok_status();
  }
  (void)comp;
  if (paged(s)) {
    // A still with pages: the page at its mark-in, as a clip's frame.
    VALTZ_ASSIGN(pic.file, rendered_page_(pid, s.id,
                                          std::max<std::int64_t>(
                                              0, l.time.in),
                                          depth + 1));
    return ok_status();
  }
  VALTZ_ASSIGN(pic.file, rendered_(pid, s.id, l.source_version, depth + 1));
  return ok_status();
}

Result<std::vector<media::LayerPicture>>
Controller::stack_pictures_(ProjectId pid, AssetId aid,
                            const std::optional<LayerLook>& look,
                            const std::optional<std::string>& only,
                            const std::set<std::string>& hidden, int depth,
                            std::int64_t page)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (depth > kMaxNesting) {
    return make_error(Code::InvalidArgument, msg::kRecursiveAsset,
                      {{"name", aid.str()}});
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  std::vector<media::LayerPicture> pics;
  if (!is_comp(a)) {
    // Shown as it is.
    media::LayerPicture pic;
    VALTZ_ASSIGN(pic.file, rendered_frame_(pid, aid, 0, 0, depth));
    pics.push_back(std::move(pic));
    return pics;
  }
  VALTZ_ASSIGN(media::PixelSize frame, own_frame_(*p, a, depth));
  // A still with pages: the layers on that page, each looking as its
  // keys say there -- counted from its own first page.
  const bool pages = paged(a);
  const std::int64_t at =
      pages ? std::clamp<std::int64_t>(page, 0, a.pages - 1) : 0;
  for (const auto& l : a.layers) {
    media::LayerPicture pic;
    // Alone, a mask is shown as what it is, not as a mask.
    pic.visible = only ? l.id == *only : l.visible;
    pic.mask = !only && l.mask;
    if (pages) {
      // Off the page it draws nothing -- a mask masks nothing.
      pic.visible = pic.visible && on_page(l.time, at, a.pages);
      const auto u = static_cast<double>(at - l.time.offset);
      pic.adjust = adjustment_keys_of(a, l.id).at(u);
      pic.crop = crop_keys_of(a, l.id).at(u);
    } else {
      pic.adjust = adjustments_of(a, l.id);
      pic.crop = crop_of(a, l.id);
    }
    if (look && look->layer == l.id) {
      pic.adjust = look->adjust;
      pic.crop = look->crop;
    }
    VALTZ_TRY(layer_picture_(pid, *p, a, l, frame, hidden, pic, depth));
    pics.push_back(std::move(pic));
  }
  if (pics.empty() && !a.canvas.framed()) {
    pics.emplace_back();
  }
  // Without a frame of its own, the bottom one sets the canvas, even
  // with nothing drawn yet.
  if (!a.canvas.framed() && pics.front().file.empty() &&
      !pics.front().drawn) {
    VALTZ_ASSIGN(pics.front().drawn, media::render_markup_picture(
                                         {}, frame, Json::array(), {}));
  }
  return pics;
}

Status
Controller::flatten(ProjectId pid, AssetId aid, const fs::path& out,
                    const std::optional<LayerLook>& look,
                    const std::optional<std::string>& only,
                    const std::set<std::string>& hidden, std::int64_t page)
{
  VALTZ_ASSIGN(auto pics, stack_pictures_(pid, aid, look, only, hidden, 0,
                                          page));
  // On its canvas -- one layer alone too, where it lies on it (moved
  // off the frame, it is still there).
  return media::flatten_layers(pics, out, canvas_setting_(pid, aid));
}

Result<IOSurfaceRef>
Controller::flatten_surface(ProjectId pid, AssetId aid,
                            const std::optional<LayerLook>& look,
                            const std::optional<std::string>& only,
                            const std::set<std::string>& hidden,
                            std::int64_t page)
{
  VALTZ_ASSIGN(auto pics, stack_pictures_(pid, aid, look, only, hidden, 0,
                                          page));
  return media::flatten_layers_surface(pics, canvas_setting_(pid, aid));
}

Result<media::MovieStack>
Controller::movie_stack(ProjectId pid, AssetId aid, bool for_job,
                        const std::optional<LiveTracks>& live)
{
  return movie_stack_(pid, aid, for_job, live, 0);
}

Result<Controller::TimedSource>
Controller::timed_source_(ProjectId pid, project::Project& p,
                          const project::Layer& l, bool for_job,
                          media::PixelSize frame, int depth)
{
  TimedSource t;
  if (!l.source) {
    return t;
  }
  VALTZ_ASSIGN(project::Asset s, p.asset(*l.source));
  if (s.cls == AssetClass::Markup) {
    const project::Markup mk = s.markup.value_or(project::Markup{});
    const media::PixelSize size =
        mk.width > 0 && mk.height > 0 ? media::PixelSize{mk.width, mk.height}
                                      : frame;
    const fs::path raster =
        mk.raster.empty() ? fs::path() : p.blobs().path_of(mk.raster);
    if (mk.objects.empty()) {
      t.file = raster;
    } else if (for_job) {
      // Drawn into a file the job reads.
      VALTZ_ASSIGN(t.file, rendered_(pid, s.id, 0, depth + 1));
    } else {
      VALTZ_ASSIGN(t.drawn, media::render_markup_picture(
                                raster, size, mk.objects, {}));
    }
    return t;
  }
  if (s.cls == AssetClass::Still) {
    // With pages: the page at its mark-in, a picture for its span.
    VALTZ_ASSIGN(t.file, rendered_page_(pid, s.id,
                                        std::max<std::int64_t>(0, l.time.in),
                                        depth + 1));
    return t;
  }
  if (s.cls == AssetClass::Composition) {
    VALTZ_ASSIGN(t.file, rendered_(pid, s.id, 0, depth + 1));
    t.video = s.kind == AssetKind::Video;
    t.audio_only = s.kind == AssetKind::Audio;
    const Rational r = rate_of_(p, s);
    VALTZ_ASSIGN(std::int64_t n, length_of_(pid, s.id, depth + 1));
    t.seconds = static_cast<double>(n) / r.to_double();
    t.mark_rate = r;
    return t;
  }
  VALTZ_ASSIGN(t.file, p.media_path(s.id, l.source_version));
  VALTZ_ASSIGN(project::AssetVersion v, p.version(s.id, l.source_version));
  t.video = s.kind == AssetKind::Video;
  t.audio_only = s.kind == AssetKind::Audio;
  if (t.video || t.audio_only) {
    t.seconds = seconds_of(v);
    t.mark_rate = t.video && v.info.frame_rate.num > 0
                      ? v.info.frame_rate : Rational{1000, 1};
  }
  return t;
}

bool
Controller::sounds_(project::Project& p, const project::Asset& a, int depth)
{
  if (depth > kMaxNesting) {
    return false;
  }
  if (a.kind == AssetKind::Audio) {
    return true;
  }
  if (a.kind != AssetKind::Video) {
    return false;
  }
  if (!is_comp(a)) {
    auto v = p.version(a.id);
    return v.ok() && v->info.has_audio;
  }
  return std::ranges::any_of(a.layers, [&](const project::Layer& l) {
    if (!l.source || !l.visible || l.mask) {
      return false;
    }
    auto s = p.asset(*l.source);
    return s.ok() && sounds_(p, *s, depth + 1);
  });
}

Result<std::int64_t>
Controller::length_of_(ProjectId pid, AssetId aid, int depth)
{
  VALTZ_ASSIGN(media::MovieStack m,
               movie_stack_(pid, aid, true, std::nullopt, depth));
  project::Project* p = project(pid);
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  return media::timeline_frames(m, rate_of_(*p, a));
}

Result<media::MovieStack>
Controller::movie_stack_(ProjectId pid, AssetId aid, bool for_job,
                         const std::optional<LiveTracks>& live, int depth)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (depth > kMaxNesting) {
    return make_error(Code::InvalidArgument, msg::kRecursiveAsset,
                      {{"name", aid.str()}});
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_timeline(a)) {
    return make_error(Code::InvalidArgument, msg::kTimelineNeedsClip);
  }
  const Rational rate = rate_of_(*p, a);
  media::PixelSize frame;
  if (a.kind != AssetKind::Audio) {
    VALTZ_ASSIGN(frame, own_frame_(*p, a, depth));
  }
  media::MovieStack m;
  m.canvas = a.canvas;
  m.frames = a.timeline_frames;
  m.rate = rate;
  std::map<std::string, std::size_t> index;
  for (const auto& l : a.layers) {
    media::MovieLayer ml;
    ml.visible = l.visible;
    ml.mask = l.mask;
    if (live && live->layer == l.id) {
      ml.adjust = live->adjust;
      ml.crop = live->crop;
    } else {
      ml.adjust = adjustment_keys_of(a, l.id);
      ml.crop = crop_keys_of(a, l.id);
    }
    VALTZ_ASSIGN(TimedSource src, timed_source_(pid, *p, l, for_job, frame,
                                                depth));
    ml.file = src.file;
    ml.drawn = src.drawn;
    ml.video = src.video;
    ml.audio_only = src.audio_only;
    const project::LayerTime& t = l.time;
    ml.timing = media::resolve_timing(
        t.offset, t.duration, t.in, t.out,
        t.rate.num > 0 ? t.rate : src.mark_rate, rate, src.seconds,
        speed_keys_of(a, l.id));
    index[l.id] = m.layers.size();
    m.layers.push_back(std::move(ml));
  }
  for (const auto& t : a.transitions) {
    auto f = index.find(t.from);
    auto g = index.find(t.to);
    if (f != index.end() && g != index.end()) {
      m.transitions.push_back({f->second, g->second, t.kind == "dissolve"});
    }
  }
  return m;
}

Result<media::SoundPlan>
Controller::sound_plan(ProjectId pid, AssetId aid)
{
  return sound_plan_(pid, aid, 0);
}

Result<fs::path>
Controller::sound_mix(ProjectId pid, AssetId aid)
{
  return mixed_(pid, aid, 0);
}

Result<std::int64_t>
Controller::composition_length(ProjectId pid, AssetId aid)
{
  return length_of_(pid, aid, 0);
}

Result<media::SoundPlan>
Controller::sound_plan_(ProjectId pid, AssetId aid, int depth)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(media::MovieStack m,
               movie_stack_(pid, aid, true, std::nullopt, depth));
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  const Rational rate = rate_of_(*p, a);
  VALTZ_ASSIGN(std::int64_t n, media::timeline_frames(m, rate));
  media::SoundPlan plan;
  plan.seconds = static_cast<double>(n) / rate.to_double();
  plan.transitions = m.transitions;
  for (std::size_t i = 0; i < m.layers.size(); ++i) {
    const media::MovieLayer& ml = m.layers[i];
    media::SoundLayer sl;
    sl.timing = ml.timing;
    // A hidden layer gives nothing, picture or sound; a picture, no
    // sound.
    if (ml.visible && !ml.mask && (ml.video || ml.audio_only) &&
        media::has_sound(ml.file)) {
      sl.file = ml.file;
    }
    sl.sound = sound_keys_of(a, a.layers[i].id);
    sl.follow_speed = pitch_follows_speed(a, a.layers[i].id);
    plan.layers.push_back(std::move(sl));
  }
  return plan;
}

// ---- compositions and the project's ------------------------------------

Result<AssetId>
Controller::new_composition_(ProjectId pid, project::Project& p,
                             AssetClass cls, media::PixelSize size,
                             Rational rate, std::string name,
                             std::string folder)
{
  const bool sound = cls == AssetClass::Composition &&
                     (size.width <= 0 || size.height <= 0);
  if (!sound && (size.width <= 0 || size.height <= 0 ||
                 size.width > 32768 || size.height > 32768)) {
    return make_error(Code::InvalidArgument, msg::kCanvasSizeOutOfRange,
                      {{"width", std::to_string(size.width)},
                       {"height", std::to_string(size.height)}});
  }
  project::Asset c;
  c.name = name.empty() ? p.name() : std::move(name);
  c.cls = cls;
  c.kind = cls == AssetClass::Still ? AssetKind::Image
           : sound                  ? AssetKind::Audio
                                    : AssetKind::Video;
  c.folder = std::move(folder);
  if (!sound) {
    c.canvas.frame_w = size.width;
    c.canvas.frame_h = size.height;
  }
  if (cls == AssetClass::Composition) {
    c.rate = rate.num > 0 ? rate
             : sound      ? Rational{1000, 1}
                          : Rational{24, 1};
  }
  VALTZ_ASSIGN(project::Asset made, p.add_asset(std::move(c), 64));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", made.id},
                                    {"reason", "defined"}});
  return made.id;
}

Result<project::OutputSettings>
Controller::project_output(ProjectId pid)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  return p->output();
}

Status
Controller::set_project_output(ProjectId pid,
                               const project::OutputSettings& o)
{
  auto undo = command_(pid, "project.output");
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (!media::output_color(o.color)) {
    return make_error(Code::InvalidArgument, msg::kOutputColorUnknown,
                      {{"color", o.color}});
  }
  VALTZ_ASSIGN(project::OutputSettings was, p->output());
  if (!(o.fps == was.fps)) {
    // The project timeline's rate: changed only while it is empty.
    if (const auto view = project_composition(pid)) {
      VALTZ_ASSIGN(project::Asset c, p->asset(*view));
      if (is_timeline(c) && c.kind == AssetKind::Video) {
        if (std::ranges::any_of(c.layers, [](const project::Layer& l) {
              return l.source.has_value();
            })) {
          return make_error(Code::InvalidArgument, msg::kOutputRateSet);
        }
        VALTZ_TRY(p->update_asset(c.id, [&](project::Asset& x) -> Status {
          x.rate = o.fps;
          return ok_status();
        }));
      }
    }
  }
  VALTZ_TRY(p->set_output(o));
  post_("assets.changed", JobId{}, {{"project", pid},
                                    {"reason", "output"}});
  return ok_status();
}

Result<AssetId>
Controller::set_up_project(ProjectId pid, AssetClass cls,
                           media::PixelSize size,
                           const project::OutputSettings& o)
{
  auto undo = command_(pid, "project.setup");
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (!media::output_color(o.color)) {
    return make_error(Code::InvalidArgument, msg::kOutputColorUnknown,
                      {{"color", o.color}});
  }
  VALTZ_TRY(p->set_output(o));
  const bool sound = size.width <= 0 || size.height <= 0;
  return create_composition(
      pid, cls, size,
      cls == AssetClass::Composition && !sound ? o.fps : Rational{0, 1},
      "", /*as_project=*/true);
}

Result<AssetId>
Controller::create_composition(ProjectId pid, AssetClass cls,
                               media::PixelSize size, Rational rate,
                               std::string name, bool as_project,
                               bool claim)
{
  auto undo = command_(pid, "composition.new");
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (cls != AssetClass::Still && cls != AssetClass::Composition) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", name}});
  }
  VALTZ_ASSIGN(AssetId c, new_composition_(pid, *p, cls, size, rate,
                                           std::move(name), ""));
  VALTZ_ASSIGN(auto now, p->composition());
  if (as_project || (claim && !now)) {
    VALTZ_TRY(p->set_composition(c));
    post_("assets.changed", JobId{}, {{"project", pid}, {"asset", c},
                                      {"reason", "project"}});
  }
  return c;
}

std::optional<AssetId>
Controller::project_composition(ProjectId pid)
{
  project::Project* p = project(pid);
  if (!p) {
    return std::nullopt;
  }
  auto c = p->composition();
  if (!c.ok() || !*c) {
    return std::nullopt;
  }
  // Gone (an undo of what made it): none.
  if (auto a = p->asset(**c); !a.ok() || !is_comp(*a)) {
    return std::nullopt;
  }
  return **c;
}

Status
Controller::set_project_composition(ProjectId pid, AssetId aid)
{
  auto undo = command_(pid, "project.set", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (!is_comp(a)) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", a.name}});
  }
  // Resized before a resize was made its frame: the project's frame is
  // its size.
  VALTZ_TRY(fold_canvas_(pid, *p, aid));
  VALTZ_TRY(p->set_composition(aid));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "project"}});
  return ok_status();
}

Result<std::optional<AssetId>>
Controller::place_in_project(ProjectId pid, AssetId asset,
                             const std::string& layer,
                             std::optional<JobId> job)
{
  auto undo = job ? join_(pid, *job) : command_(pid, "asset.place", asset);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(asset));
  if (a.kind != AssetKind::Image && a.kind != AssetKind::Video &&
      a.kind != AssetKind::Audio) {
    return make_error(Code::InvalidArgument, msg::kLayerSourceKind);
  }
  if ((a.cls == AssetClass::Flat || a.cls == AssetClass::Generated) &&
      a.head == 0) {
    return make_error(Code::NotFound, msg::kAssetHasNoContent);
  }
  media::PixelSize size;
  if (a.kind != AssetKind::Audio) {
    VALTZ_ASSIGN(size, shown_size_(*p, asset, 0, 0));
  }
  const auto view = project_composition(pid);
  if (!view) {
    // The project's composition, made of it: its kind is the project's,
    // its size the frame; a timeline at the project's frame rate (its
    // output), a clip at another resampled to it.
    Rational rate{0, 1};
    if (a.kind == AssetKind::Video) {
      VALTZ_ASSIGN(project::OutputSettings o, p->output());
      rate = o.fps;
    }
    VALTZ_ASSIGN(AssetId c, new_composition_(
        pid, *p,
        a.kind == AssetKind::Image ? AssetClass::Still
                                   : AssetClass::Composition,
        size, rate, p->name(), ""));
    project::Layer l0;
    l0.source = asset;
    VALTZ_TRY(p->set_layers(c, {l0}));
    VALTZ_TRY(p->set_composition(c));
    post_("assets.changed", JobId{}, {{"project", pid}, {"asset", c},
                                      {"reason", "project"}});
    return std::optional<AssetId>(c);
  }
  VALTZ_ASSIGN(project::Asset c, p->asset(*view));
  // Made on a chosen layer: there, when that layer can show it.
  if (!layer.empty()) {
    if (!check_source_(*p, c, a).ok()) {
      return std::optional<AssetId>();
    }
    VALTZ_TRY(set_layer_source(pid, *view, layer, asset));
    return std::optional<AssetId>(*view);
  }
  // Another kind than the project's is not placed: it is an asset, shown
  // as itself.
  if (c.kind != a.kind) {
    return std::optional<AssetId>();
  }
  auto ls = c.layers;
  // Layer 0 -- the take layer -- made again at the bottom if it went.
  auto it = find_layer(ls, "");
  const bool had_take = it != ls.end() && it->source;
  if (it == ls.end()) {
    ls.insert(ls.begin(), project::Layer{});
    it = ls.begin();
  }
  // The project is still just its take in layer 0 -- its canvas never
  // resized, nothing on another layer, no length set: the frame follows
  // the new take. Else the frame (and a timeline's length) stays, and the
  // take lies on it, centred at its own size, as on any layer. One set up
  // at a size of its own keeps it from its first take on.
  const bool alone =
      had_take && !c.canvas.set() && c.timeline_frames == 0 &&
      c.pages <= 1 &&
      std::ranges::none_of(ls, [](const project::Layer& l) {
        return !l.id.empty() && !l.empty();
      });
  media::StackCanvas canvas = c.canvas;
  std::int64_t timeline = c.timeline_frames;
  if (alone && a.kind != AssetKind::Audio) {
    canvas.frame_w = size.width;
    canvas.frame_h = size.height;
  } else if (a.kind != AssetKind::Audio) {
    if (!canvas.framed()) {
      // A composition from before frames: the one it had, kept.
      VALTZ_ASSIGN(media::PixelSize was, own_frame_(*p, c, 0));
      canvas.frame_w = was.width;
      canvas.frame_h = was.height;
    }
  }
  if (!alone && is_timeline(c) && timeline == 0) {
    // Its length as it was, pinned.
    auto n = length_of_(pid, c.id, 0);
    if (n.ok()) {
      timeline = *n;
    }
  }
  // Layer 0: a new take, raw -- the look the last one had there goes
  // with it, and its marks; where it starts stays.
  it->source = asset;
  it->source_version = 0;
  it->time.in = -1;
  it->time.out = -1;
  it->time.rate = {0, 1};
  if (!paged(c)) {
    it->time.duration = 0;  // a still's pages stay its pages
  }
  std::vector<project::Modifier> mods;
  for (const auto& m : c.modifiers) {
    if (!m.layer.empty() || m.kind == "audio" || m.kind == "speed") {
      mods.push_back(m);
    }
  }
  VALTZ_TRY(p->set_modifiers(c.id, std::move(mods)));
  VALTZ_TRY(p->set_layers(c.id, std::move(ls)));
  VALTZ_TRY(p->set_canvas(c.id, canvas));
  if (timeline != c.timeline_frames) {
    VALTZ_TRY(p->set_timeline(c.id, timeline));
  }
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", c.id},
                                    {"reason", "layers"}});
  return std::optional<AssetId>(c.id);
}

Result<std::pair<AssetId, std::string>>
Controller::place_result(ProjectId pid, AssetId asset,
                         std::optional<AssetId> onto,
                         std::optional<std::string> at, std::int64_t offset,
                         std::optional<JobId> job)
{
  auto undo = job ? join_(pid, *job) : command_(pid, "asset.place", asset);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(asset));
  if (a.kind != AssetKind::Image && a.kind != AssetKind::Video &&
      a.kind != AssetKind::Audio) {
    return make_error(Code::InvalidArgument, msg::kLayerSourceKind);
  }
  if ((a.cls == AssetClass::Flat || a.cls == AssetClass::Generated) &&
      a.head == 0) {
    return make_error(Code::NotFound, msg::kAssetHasNoContent);
  }
  if (onto) {
    auto t = p->asset(*onto);
    if (t.ok() && is_comp(*t) && check_source_(*p, *t, a).ok()) {
      return instantiate(pid, asset, *onto, at, offset);
    }
  }
  // Its own: the size it is, a clip's rate.
  media::PixelSize size;
  if (a.kind != AssetKind::Audio) {
    VALTZ_ASSIGN(size, shown_size_(*p, asset, 0, 0));
  }
  // The project's, named by the project, when it has none -- at the
  // project's frame rate (its output); else a clip's own.
  const bool claims = !project_composition(pid);
  Rational rate{0, 1};
  if (a.kind == AssetKind::Video && claims) {
    VALTZ_ASSIGN(project::OutputSettings o, p->output());
    rate = o.fps;
  } else if (a.kind == AssetKind::Video && !is_comp(a)) {
    VALTZ_ASSIGN(project::AssetVersion v, p->version(asset, a.head));
    rate = v.info.frame_rate;
  } else if (a.kind == AssetKind::Video) {
    rate = rate_of_(*p, a);
  }
  VALTZ_ASSIGN(AssetId c, new_composition_(
      pid, *p,
      a.kind == AssetKind::Image ? AssetClass::Still
                                 : AssetClass::Composition,
      size, rate, claims ? p->name() : a.name, a.folder));
  project::Layer l0;
  l0.source = asset;
  VALTZ_TRY(p->set_layers(c, {l0}));
  if (claims) {
    VALTZ_TRY(p->set_composition(c));
  }
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", c},
                                    {"reason", "layers"}, {"layer", ""}});
  return std::pair{c, std::string()};
}

Result<std::pair<AssetId, std::string>>
Controller::instantiate(ProjectId pid, AssetId asset,
                        std::optional<AssetId> onto,
                        std::optional<std::string> at, std::int64_t offset)
{
  auto undo = command_(pid, "asset.instantiate", asset);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset s, p->asset(asset));
  if (s.kind == AssetKind::Text || s.kind == AssetKind::Other) {
    return make_error(Code::InvalidArgument, msg::kLayerSourceKind);
  }
  if (!onto) {
    onto = project_composition(pid);
  }
  if (!onto) {
    // No project yet: it is made of this, in its layer 0.
    VALTZ_ASSIGN(auto made, place_in_project(pid, asset));
    if (!made) {
      return make_error(Code::InvalidArgument, msg::kLayerSourceKind);
    }
    return std::pair{*made, std::string()};
  }
  VALTZ_ASSIGN(project::Asset t, p->asset(*onto));
  if (!is_comp(t)) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", t.name}});
  }
  VALTZ_TRY(check_source_(*p, t, s));
  auto ls = t.layers;
  // A NEW layer showing it: in a blank `at`, else right above it; with
  // none, on top.
  std::size_t pos = ls.size();
  std::string id;
  bool fill = false;
  if (at) {
    auto it = find_layer(ls, *at);
    if (it == ls.end()) {
      return make_error(Code::InvalidArgument, "no such layer");
    }
    if (it->empty()) {
      fill = true;
      id = it->id;
      pos = static_cast<std::size_t>(it - ls.begin());
    } else {
      pos = static_cast<std::size_t>(it - ls.begin()) + 1;
    }
  }
  project::Layer n;
  n.id = fill ? id : (ls.empty() ? std::string() : next_layer_id(ls));
  n.name = fill && !ls[pos].name.empty()
               ? ls[pos].name
               : std::string(utf8_prefix(one_line(s.name), 64));
  n.source = asset;
  // A clip in a blank layer of a timeline: after what is there.
  const bool append = fill && sequenced_(t, s);
  if (append) {
    VALTZ_ASSIGN(n.time.offset, content_end_(pid, t.id));
  } else if (is_timeline(t)) {
    n.time.offset = std::max<std::int64_t>(0, offset);
  } else if (paged(t)) {
    // On the page it was put on, alone.
    n.time.offset = std::clamp<std::int64_t>(offset, 0, t.pages - 1);
    n.time.duration = 1;
  }
  if (fill) {
    n.mask = ls[pos].mask;
    n.visible = ls[pos].visible;
    if (paged(t)) {
      n.time = ls[pos].time;  // its pages are the blank layer's
    }
    ls[pos] = n;
  } else {
    ls.insert(ls.begin() + static_cast<std::ptrdiff_t>(pos), n);
  }
  VALTZ_TRY(p->set_layers(t.id, std::move(ls)));
  if (append) {
    VALTZ_TRY(grow_to_content_(pid, *p, t.id));
  }
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", t.id},
                                    {"reason", "layers"}, {"layer", n.id}});
  return std::pair{t.id, n.id};
}

bool
Controller::sequenced_(const project::Asset& comp, const project::Asset& src)
{
  return is_timeline(comp) && src.kind == AssetKind::Video;
}

Result<std::int64_t>
Controller::content_end_(ProjectId pid, AssetId comp)
{
  VALTZ_ASSIGN(media::MovieStack m,
               movie_stack_(pid, comp, true, std::nullopt, 0));
  std::vector<media::LayerTiming> ts;
  for (const auto& l : m.layers) {
    ts.push_back(l.timing);
  }
  const double end = media::timeline_end(ts);
  return static_cast<std::int64_t>(
      std::ceil(end * m.rate.to_double() - 1e-6));
}

Status
Controller::grow_to_content_(ProjectId pid, project::Project& p,
                             AssetId comp)
{
  VALTZ_ASSIGN(project::Asset c, p.asset(comp));
  // Unset (0), its length follows its content already.
  if (c.timeline_frames <= 0) {
    return ok_status();
  }
  VALTZ_ASSIGN(std::int64_t end, content_end_(pid, comp));
  if (end > c.timeline_frames) {
    VALTZ_TRY(p.set_timeline(comp, end));
  }
  return ok_status();
}

Result<std::pair<std::int64_t, Rational>>
Controller::clip_frames_(project::Project& p, const project::Asset& a)
{
  if (is_comp(a)) {
    VALTZ_ASSIGN(std::int64_t n, length_of_(p.id(), a.id, 0));
    return std::pair{n, rate_of_(p, a)};
  }
  VALTZ_ASSIGN(project::AssetVersion v, p.version(a.id));
  return std::pair{v.info.frame_count, v.info.frame_rate.num > 0
                                           ? v.info.frame_rate
                                           : Rational{24, 1}};
}

Result<Json>
Controller::continuation_guide(ProjectId pid, AssetId clip, double seconds,
                               const std::string& model, std::string name)
{
  auto undo = command_(pid, "continue.guide", clip);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // Its grid and rate are all a guide needs of the model: without the
  // engine (valtzctl), Auto's first choice for clips from references.
  const models::ModelEntry* m = reference_model_(model);
  if (!m && (model.empty() || model == "auto")) {
    if (const auto& order = _catalog.auto_order("video", "edit");
        !order.empty()) {
      m = _catalog.find(order.front());
    }
  }
  if (!m) {
    return make_error(Code::NotFound, msg::kNoReferenceModel);
  }
  VALTZ_ASSIGN(project::Asset c, p->asset(clip));
  if (c.kind != AssetKind::Video) {
    return make_error(Code::InvalidArgument, msg::kContinueNeedsClip);
  }
  VALTZ_ASSIGN(auto have, clip_frames_(*p, c));
  const double rate =
      static_cast<double>(have.second.num) /
      static_cast<double>(std::max<std::int64_t>(1, have.second.den));
  if (have.first <= 0 || rate <= 0) {
    return make_error(Code::InvalidArgument, msg::kContinueNeedsClip);
  }
  // The guide's length in the MODEL's frames: what was asked rounded up
  // to the grid, down again to what the clip holds.
  const Json vp = jget(m->engine, "vpipe", Json::object());
  const double fps =
      jget(jget(vp, "defaults", Json::object()), "fps", 24.0);
  const Json grid = jget(vp, "frame_grid", Json::object());
  const std::int64_t step = std::max(1, jget(grid, "step", 1));
  const std::int64_t off = std::max(1, jget(grid, "offset", 1));
  const std::int64_t fit = static_cast<std::int64_t>(std::floor(
      static_cast<double>(have.first) * fps / rate + 1e-6));
  std::int64_t frames =
      seconds > 0
          ? static_cast<std::int64_t>(std::ceil(seconds * fps - 1e-6))
          : jget<std::int64_t>(jget(vp, "references", Json::object()),
                               "tail_frames", 90);
  frames = frames <= off ? off : off + (frames - off + step - 1) / step * step;
  if (frames > fit) {
    frames = fit < off + step ? fit : off + (fit - off) / step * step;
  }
  if (frames <= 0) {
    return make_error(Code::InvalidArgument, msg::kContinueNeedsClip);
  }
  const double guide_s = static_cast<double>(frames) / fps;
  // The clip's last guide_s seconds, in its own frames.
  const std::int64_t span = std::min<std::int64_t>(
      have.first, std::llround(guide_s * rate));
  VALTZ_ASSIGN(media::PixelSize size, own_frame_(*p, c, 0));
  const Rational model_rate =
      std::abs(fps - std::round(fps)) < 1e-6
          ? Rational{static_cast<std::int64_t>(std::lround(fps)), 1}
          : Rational{static_cast<std::int64_t>(std::lround(fps * 1000)),
                     1000};
  VALTZ_ASSIGN(AssetId g,
               new_composition_(pid, *p, AssetClass::Composition, size,
                                model_rate, name.empty() ? c.name : name,
                                c.folder));
  VALTZ_ASSIGN(auto placed, instantiate(pid, clip, g));
  project::LayerTime t;
  t.in = have.first - span;
  t.rate = have.second;
  VALTZ_TRY(set_layer_time(pid, g, placed.second, t));
  VALTZ_TRY(set_timeline(pid, g, frames));
  return Json{{"asset", g.str()},
              {"frames", frames},
              {"seconds", guide_s},
              {"fps", fps}};
}

Result<AssetId>
Controller::grab_frame(ProjectId pid, AssetId asset, std::int64_t frame,
                       std::string name)
{
  auto undo = command_(pid, "frame.grab", asset);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(asset));
  if (a.kind != AssetKind::Video) {
    return make_error(Code::InvalidArgument, msg::kGrabNeedsClip);
  }
  VALTZ_ASSIGN(auto have, clip_frames_(*p, a));
  VALTZ_ASSIGN(media::PixelSize size, own_frame_(*p, a, 0));
  VALTZ_ASSIGN(AssetId still,
               new_composition_(pid, *p, AssetClass::Still, size, {0, 1},
                                name.empty() ? a.name : name, a.folder));
  VALTZ_ASSIGN(auto placed, instantiate(pid, asset, still));
  project::LayerTime t;
  t.in = std::clamp<std::int64_t>(frame, 0,
                                  std::max<std::int64_t>(0, have.first - 1));
  t.rate = have.second;
  VALTZ_TRY(set_layer_time(pid, still, placed.second, t));
  return still;
}

Status
Controller::decompose(ProjectId pid, AssetId aid, const std::string& layer)
{
  auto undo = command_(pid, "layer.decompose", aid, layer);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset c, p->asset(aid));
  if (!is_comp(c)) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", c.name}});
  }
  auto ls = c.layers;
  auto it = find_layer(ls, layer);
  if (it == ls.end() || !it->source) {
    return make_error(Code::InvalidArgument, msg::kDecomposeNeedsComposition);
  }
  const project::Layer inst = *it;
  const std::size_t at = static_cast<std::size_t>(it - ls.begin());
  VALTZ_ASSIGN(project::Asset src, p->asset(*inst.source));
  if (!is_comp(src)) {
    return make_error(Code::InvalidArgument, msg::kDecomposeNeedsComposition);
  }
  // What cannot be carried over exactly is refused, by name.
  if (inst.mask || (at + 1 < ls.size() && ls[at + 1].mask)) {
    return make_error(Code::InvalidArgument, msg::kDecomposeMask);
  }
  const auto adj = adjustment_keys_of(c, inst.id);
  const auto crop = crop_keys_of(c, inst.id);
  if (!adj.identity() || !speed_keys_of(c, inst.id).identity() ||
      !sound_keys_of(c, inst.id).identity() || crop.place.keys.size() > 1 ||
      crop.turn.keys.size() > 1) {
    return make_error(Code::InvalidArgument, msg::kDecomposeLook);
  }
  if (is_timeline(src) && !is_timeline(c)) {
    return make_error(Code::InvalidArgument, msg::kDecomposeTimeline);
  }
  if (paged(src)) {
    return make_error(Code::InvalidArgument, msg::kDecomposePages);
  }
  if (is_timeline(src) && is_timeline(c) &&
      !(rate_of_(*p, src) == rate_of_(*p, c))) {
    return make_error(Code::InvalidArgument, msg::kDecomposeRate);
  }
  const media::Crop outer = crop.at(0);
  media::PixelSize in_frame, out_frame;
  if (src.kind != AssetKind::Audio) {
    VALTZ_ASSIGN(in_frame, shown_size_(*p, src.id, 0, 0));
    VALTZ_ASSIGN(out_frame, own_frame_(*p, c, 0));
  }
  // The inner composition's own frame, and where it lies on its canvas.
  media::PixelSize inner_own = in_frame;
  if (src.kind != AssetKind::Audio) {
    VALTZ_ASSIGN(inner_own, own_frame_(*p, src, 0));
  }
  const media::Affine outer_m = placement(outer, in_frame, out_frame);
  media::Affine shift;  // own frame -> canvas (Canvas Size)
  if (src.canvas.set()) {
    shift.tx = src.canvas.x;
    shift.ty = src.canvas.height - inner_own.height - src.canvas.y;
  }
  const Rational rate = is_timeline(c) ? rate_of_(*p, c) : Rational{24, 1};
  // The inner timeline's window the layer shows, in its frames.
  const std::int64_t win_in = std::max<std::int64_t>(0, inst.time.in);
  std::int64_t win_len = -1;
  if (is_timeline(src)) {
    if (inst.time.duration > 0) {
      win_len = inst.time.duration;
    } else if (inst.time.out >= 0) {
      win_len = inst.time.out + 1 - win_in;
    }
  }
  std::vector<project::Layer> kids;
  std::vector<project::Modifier> kid_mods;
  std::map<std::string, std::string> renamed;
  auto used = ls;
  for (const auto& k : src.layers) {
    project::Layer n = k;
    // Layer 0 keeps its id: new takes still go there.
    n.id = kids.empty() && inst.id.empty() ? std::string()
                                           : next_layer_id(used);
    used.push_back(n);
    renamed[k.id] = n.id;
    if (n.name.empty()) {
      n.name = k.id.empty() ? "Layer 0" : "Layer " + k.id;
    }
    if (kids.empty()) {
      n.mask = false;
    }
    // Time: where the window lies, from the instance's start (a still's
    // pages: the instance's).
    if (is_timeline(c) || paged(c)) {
      if (is_timeline(src)) {
        std::int64_t start = k.time.offset - win_in;
        if (start < 0) {
          // It began before the window: its marks move on by as much.
          if (!speed_keys_of(src, k.id).identity()) {
            return make_error(Code::InvalidArgument, msg::kDecomposeLook);
          }
          VALTZ_ASSIGN(TimedSource ts, timed_source_(pid, *p, k, true,
                                                     inner_own, 0));
          const double secs = static_cast<double>(-start) /
                              rate.to_double();
          const Rational mr = k.time.rate.num > 0 ? k.time.rate
                                                  : ts.mark_rate;
          if (ts.seconds > 0 || k.time.in >= 0) {
            n.time.in = std::max<std::int64_t>(0, k.time.in) +
                        std::llround(secs * mr.to_double());
            n.time.rate = mr;
          }
          if (n.time.duration > 0) {
            n.time.duration = std::max<std::int64_t>(1, n.time.duration +
                                                            start);
          }
          start = 0;
        }
        if (win_len >= 0 && start >= win_len) {
          continue;  // after the window: not shown
        }
        n.time.offset = inst.time.offset + start;
        if (win_len >= 0) {
          // Cut where the window ends.
          const std::int64_t room = win_len - start;
          if (n.time.duration == 0 || n.time.duration > room) {
            VALTZ_ASSIGN(TimedSource ts, timed_source_(pid, *p, k, true,
                                                       inner_own, 0));
            const auto lt = media::resolve_timing(
                0, n.time.duration, n.time.in, n.time.out,
                n.time.rate.num > 0 ? n.time.rate : ts.mark_rate, rate,
                ts.seconds, speed_keys_of(src, k.id));
            const double len = lt.length * rate.to_double();
            if (!std::isfinite(len) ||
                len > static_cast<double>(room)) {
              n.time.duration = room;
            }
          }
        }
      } else {
        // A still's layers: on for as long as the layer was.
        n.time = {};
        n.time.in = k.time.in;
        n.time.rate = k.time.rate;
        n.time.offset = inst.time.offset;
        n.time.duration = inst.time.duration;
      }
    }
    // Placement: the inner one, then the layer's own, as one crop.
    for (const auto& m : layer_mods(src, k.id, n.id)) {
      if (m.kind != "crop") {
        kid_mods.push_back(m);
      }
    }
    if (src.kind != AssetKind::Audio && k.source) {
      VALTZ_ASSIGN(media::PixelSize content,
                   shown_size_(*p, *k.source, k.source_version, 0));
      const auto kc = crop_keys_of(src, k.id);
      std::set<std::int64_t> frames = {0};
      for (const auto& key : kc.place.keys) {
        frames.insert(key.frame);
      }
      for (const auto& key : kc.turn.keys) {
        frames.insert(key.frame);
      }
      media::KeyedCrop out;
      out.rate = kc.rate.num > 0 ? kc.rate : rate;
      out.place.rate = out.rate;
      out.turn.rate = out.rate;
      for (const std::int64_t f : frames) {
        const media::Crop inner = kc.at(static_cast<double>(f));
        const media::Affine m = then(
            then(placement(inner, content, inner_own), shift), outer_m);
        auto cr = crop_for(m, content, out_frame);
        if (!cr) {
          return make_error(Code::InvalidArgument, msg::kDecomposeSkew);
        }
        media::Crop place = *cr;
        place.rotate = 0;
        out.place.keys.push_back({f, place});
        out.turn.keys.push_back({f, {cr->rotate}});
      }
      out.pad = {0, 0, 0, 0};
      kid_mods.push_back({"crop", n.id, media::to_json(out)});
    }
    kids.push_back(std::move(n));
  }
  // The instance gives way to its layers, in its place.
  ls.erase(ls.begin() + static_cast<std::ptrdiff_t>(at));
  ls.insert(ls.begin() + static_cast<std::ptrdiff_t>(at), kids.begin(),
            kids.end());
  std::vector<project::Modifier> mods = c.modifiers;
  drop_layer_mods(mods, inst.id);
  mods.insert(mods.end(), kid_mods.begin(), kid_mods.end());
  VALTZ_TRY(p->set_modifiers(aid, std::move(mods)));
  VALTZ_TRY(p->set_layers(aid, std::move(ls)));
  VALTZ_TRY(p->update_asset(aid, [&](project::Asset& x) -> Status {
    std::erase_if(x.transitions, [&](const project::Transition& t) {
      return t.from == inst.id || t.to == inst.id;
    });
    for (const auto& t : src.transitions) {
      auto f = renamed.find(t.from);
      auto g = renamed.find(t.to);
      if (f != renamed.end() && g != renamed.end()) {
        project::Transition n = t;
        n.from = f->second;
        n.to = g->second;
        x.transitions.push_back(std::move(n));
      }
    }
    return ok_status();
  }));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "layers"}});
  return ok_status();
}

Result<AssetId>
Controller::derive_modified(ProjectId pid, AssetId source, std::string name)
{
  auto undo = command_(pid, "asset.modify", source);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset s, p->asset(source));
  if (s.kind != AssetKind::Image && s.kind != AssetKind::Video &&
      s.kind != AssetKind::Audio) {
    return make_error(Code::InvalidArgument, msg::kModifyNeedsMedia);
  }
  if (is_comp(s) || s.cls == AssetClass::Markup) {
    // A copy of it, to change instead.
    project::Asset c = s;
    c.name = name.empty() ? s.name : std::move(name);
    c.head = 0;
    c.recipe = {};
    c.origin = project::Origin::Source;
    c.tags.clear();
    VALTZ_ASSIGN(project::Asset made, p->add_asset(std::move(c), 48));
    post_("assets.changed", JobId{}, {{"project", pid}, {"asset", made.id},
                                      {"reason", "defined"}});
    return made.id;
  }
  if (s.head == 0) {
    return make_error(Code::NotFound, msg::kAssetHasNoContent);
  }
  // A flat or generated asset has no look of its own: a composition of
  // one layer showing it takes one.
  media::PixelSize size;
  Rational rate{0, 1};
  if (s.kind != AssetKind::Audio) {
    VALTZ_ASSIGN(size, shown_size_(*p, source, 0, 0));
  }
  if (s.kind == AssetKind::Video) {
    VALTZ_ASSIGN(project::AssetVersion v, p->version(source, s.head));
    rate = v.info.frame_rate;
  }
  VALTZ_ASSIGN(AssetId c, new_composition_(
      pid, *p,
      s.kind == AssetKind::Image ? AssetClass::Still
                                 : AssetClass::Composition,
      size, rate, name.empty() ? s.name : std::move(name), s.folder));
  project::Layer l0;
  l0.source = source;
  VALTZ_TRY(p->set_layers(c, {l0}));
  return c;
}

Result<AssetId>
Controller::capture(ProjectId pid, AssetId source,
                    const std::vector<std::string>& layers, std::string name)
{
  auto undo = command_(pid, "asset.capture", source);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset s, p->asset(source));
  if (!is_comp(s)) {
    // As it is, pinned: a composition of one layer showing this version.
    VALTZ_ASSIGN(AssetId c, derive_modified(pid, source, std::move(name)));
    VALTZ_ASSIGN(project::Asset ca, p->asset(c));
    auto ls = ca.layers;
    for (auto& l : ls) {
      l.source_version = s.head;
    }
    VALTZ_TRY(p->set_layers(c, std::move(ls)));
    return c;
  }
  auto chosen = [&](const std::string& id) {
    return layers.empty() || std::ranges::find(layers, id) != layers.end();
  };
  std::vector<project::Layer> keep;
  for (auto l : s.layers) {
    if (!chosen(l.id)) {
      continue;
    }
    // What it shows now, pinned.
    if (l.source && l.source_version == 0) {
      VALTZ_ASSIGN(project::Asset sa, p->asset(*l.source));
      l.source_version = sa.head;
    }
    keep.push_back(std::move(l));
  }
  if (keep.empty()) {
    return make_error(Code::InvalidArgument, "no such layer to capture");
  }
  std::vector<project::Modifier> mods;
  for (const auto& m : s.modifiers) {
    if (std::ranges::any_of(keep, [&](const project::Layer& l) {
          return l.id == m.layer;
        })) {
      mods.push_back(m);
    }
  }
  project::Asset c;
  c.name = name.empty() ? s.name : std::move(name);
  c.kind = s.kind;
  c.cls = s.cls;
  c.canvas = s.canvas;
  c.timeline_frames = s.timeline_frames;
  c.pages = s.pages;
  c.rate = s.rate.num > 0 ? s.rate : rate_of_(*p, s);
  c.folder = s.folder;
  if (!keep.front().id.empty() && !s.canvas.framed() &&
      s.kind != AssetKind::Audio) {
    // From before frames, without its bottom: on a frame of the old
    // canvas's size, each layer where it was.
    VALTZ_ASSIGN(media::PixelSize own, own_frame_(*p, s, 0));
    c.canvas.frame_w = own.width;
    c.canvas.frame_h = own.height;
  }
  for (const auto& t : s.transitions) {
    if (chosen(t.from) && chosen(t.to)) {
      c.transitions.push_back(t);
    }
  }
  keep.front().mask = false;
  c.layers = std::move(keep);
  c.modifiers = std::move(mods);
  VALTZ_ASSIGN(project::Asset made, p->add_asset(std::move(c), 48));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", made.id},
                                    {"reason", "defined"}});
  return made.id;
}

Result<AssetId>
Controller::flatten_asset(ProjectId pid, AssetId aid, std::string name)
{
  auto undo = command_(pid, "asset.flatten", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (a.kind == AssetKind::Text || a.kind == AssetKind::Other) {
    return make_error(Code::InvalidArgument, msg::kModifyNeedsMedia);
  }
  if (name.empty()) {
    name = a.name;
  }
  const Json from = {{"asset", aid.str()}, {"version", a.head}};
  if (a.cls == AssetClass::Flat || a.cls == AssetClass::Generated) {
    if (a.head == 0) {
      return make_error(Code::NotFound, msg::kAssetHasNoContent);
    }
    // Its take, as a flat copy: the same file.
    project::Asset f;
    f.name = std::move(name);
    f.kind = a.kind;
    f.cls = AssetClass::Flat;
    f.folder = a.folder;
    f.from = from;
    VALTZ_ASSIGN(project::Asset made, p->add_asset(std::move(f), 64));
    project::Recipe r;
    r.op = "flatten";
    VALTZ_ASSIGN(auto v, p->share_version(made.id, aid, a.head, r, {}));
    (void)v;
    post_("assets.changed", JobId{}, {{"project", pid}, {"asset", made.id},
                                      {"reason", "defined"}});
    return made.id;
  }
  if (paged(a)) {
    // A picture a page, each named by its page; the first is returned.
    std::optional<AssetId> first;
    for (std::int64_t i = 0; i < a.pages; ++i) {
      VALTZ_ASSIGN(AssetId made, flat_page_(
          pid, a, i, std::format("{}, page {}", name, i + 1)));
      if (!first) {
        first = made;
      }
    }
    return *first;
  }
  return flat_page_(pid, a, 0, std::move(name));
}

Result<AssetId>
Controller::flat_page_(ProjectId pid, const project::Asset& a,
                       std::int64_t page, std::string name)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(fs::path file, a.cls == AssetClass::Still
                                  ? rendered_page_(pid, a.id, page, 0)
                                  : rendered_(pid, a.id, 0, 0));
  project::ImportOptions io;
  io.placement = project::Placement::Copy;
  io.name = std::move(name);
  VALTZ_ASSIGN(project::Asset made, p->import_file(file, io));
  Json from = {{"asset", a.id.str()}, {"version", a.head}};
  if (paged(a)) {
    from["page"] = page;
  }
  VALTZ_TRY(p->update_asset(made.id, [&](project::Asset& x) -> Status {
    x.cls = AssetClass::Flat;
    x.source_path.clear();
    x.folder = a.folder;
    x.from = from;
    return ok_status();
  }));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", made.id},
                                    {"reason", "defined"}});
  return made.id;
}

}

namespace valtz {

// ---- capture --------------------------------------------------------------

namespace {

// "23:10": the local time, as a recording is named.
std::string
local_time_hm()
{
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  return std::format("{:02}:{:02}", tm.tm_hour, tm.tm_min);
}

}

Json
Controller::capture_sources() const
{
  Json out = Json::array();
  for (const auto& s : media::capture_sources()) {
    out.push_back({{"id", s.id}, {"name", s.name}, {"kind", s.kind},
                   {"preferred", s.preferred}});
  }
  return {{"sources", std::move(out)},
          {"microphone", media::microphone_permission()}};
}

Status
Controller::start_capture(ProjectId pid, std::optional<AssetId> into,
                          const std::string& source)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  const AssetId aid = into.value_or(AssetId{});
  if (into) {
    VALTZ_ASSIGN(project::Asset a, p->asset(*into));
    // Sound goes into a composition of sound alone.
    if (a.cls != AssetClass::Composition || a.kind != AssetKind::Audio) {
      return make_error(Code::InvalidArgument, msg::kCaptureNeedsSound);
    }
  }
  std::lock_guard lk(_capture_mu);
  if (_capture) {
    return make_error(Code::Busy, msg::kCaptureRunning);
  }
  std::string name = source;
  for (const auto& s : media::capture_sources()) {
    if (s.id == source) {
      name = s.name;
    }
  }
  const fs::path out = p->blobs().tmp_dir() /
                       std::format("capture-{}.wav", JobId::make().str());
  VALTZ_ASSIGN(_capture, media::SoundCapture::start(source, out));
  _capture_project = pid;
  _capture_asset = aid;
  _capture_name = name;
  post_("capture.started", JobId{}, {{"project", pid}, {"asset", aid},
                                     {"source", source}});
  return ok_status();
}

Json
Controller::capture_state() const
{
  std::lock_guard lk(_capture_mu);
  if (!_capture) {
    return {{"recording", false}};
  }
  return {{"recording", true},
          {"project", _capture_project},
          {"asset", _capture_asset.is_nil() ? std::string()
                                            : _capture_asset.str()},
          {"source", _capture->source()},
          {"name", _capture_name},
          {"seconds", _capture->seconds()},
          {"level", _capture->level()}};
}

Result<AssetId>
Controller::stop_capture()
{
  std::unique_ptr<media::SoundCapture> c;
  ProjectId pid;
  AssetId comp;
  std::string name;
  {
    std::lock_guard lk(_capture_mu);
    if (!_capture) {
      return make_error(Code::InvalidArgument, msg::kNoCapture);
    }
    c = std::move(_capture);
    pid = _capture_project;
    comp = _capture_asset;
    name = _capture_name;
  }
  const fs::path file = c->file();
  const Status st = c->stop();
  c.reset();
  std::error_code ec;
  post_("capture.stopped", JobId{}, {{"project", pid}, {"asset", comp}});
  if (!st.ok()) {
    fs::remove(file, ec);
    return st.error();
  }
  // The recording: a flat sound of the project's, and on the
  // composition when it went into one -- one command.
  auto undo = command_(pid, "capture.record", comp);
  project::Project* p = project(pid);
  if (!p) {
    fs::remove(file, ec);
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (comp.is_nil()) {
    return import_taken_(pid, file, std::format("{} {}", name,
                                                local_time_hm()), "");
  }
  VALTZ_ASSIGN(project::Asset c_asset, p->asset(comp));
  VALTZ_ASSIGN(AssetId made,
               import_taken_(pid, file,
                             std::format("{} {}", name, local_time_hm()),
                             c_asset.folder));
  // In its blank layer, else a new one on top.
  std::optional<std::string> at;
  for (const auto& l : c_asset.layers) {
    if (l.empty()) {
      at = l.id;
    }
  }
  VALTZ_ASSIGN(auto placed, instantiate(pid, made, comp, at, 0));
  (void)placed;
  return made;
}

Result<AssetId>
Controller::import_taken_(ProjectId pid, const fs::path& file,
                          std::string name, std::string folder)
{
  project::Project* p = project(pid);
  std::error_code ec;
  if (!p) {
    fs::remove(file, ec);
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  project::ImportOptions io;
  io.placement = project::Placement::Copy;
  io.name = std::move(name);
  auto made = p->import_file(file, io);
  fs::remove(file, ec);
  if (!made.ok()) {
    return made.error();
  }
  VALTZ_TRY(p->update_asset(made->id, [&](project::Asset& x) -> Status {
    x.cls = AssetClass::Flat;
    x.source_path.clear();
    x.folder = folder;
    return ok_status();
  }));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", made->id},
                                    {"reason", "imported"}});
  return made->id;
}

// ---- the camera --------------------------------------------------------

Json
Controller::camera_sources() const
{
  Json out = Json::array();
  for (const auto& c : media::camera_sources()) {
    out.push_back({{"id", c.id}, {"name", c.name},
                   {"preferred", c.preferred}});
  }
  return {{"cameras", std::move(out)},
          {"camera", media::camera_permission()},
          {"microphone", media::microphone_permission()}};
}

Status
Controller::start_camera(ProjectId pid, const std::string& camera,
                         bool sound)
{
  if (!project(pid)) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  std::lock_guard lk(_camera_mu);
  if (_camera) {
    if (_camera_project == pid && (camera.empty() || camera == _camera_id)) {
      return ok_status();
    }
    _camera.reset();
  }
  VALTZ_ASSIGN(_camera, media::CameraCapture::start(camera, sound));
  _camera_project = pid;
  _camera_id = camera;
  post_("camera.started", JobId{}, {{"project", pid},
                                    {"name", _camera->name()}});
  return ok_status();
}

Json
Controller::camera_state() const
{
  std::lock_guard lk(_camera_mu);
  if (!_camera) {
    return {{"on", false}};
  }
  const media::CameraCapture::Frame f = _camera->newest();
  if (f.surface) {
    CFRelease(f.surface);
  }
  return {{"on", true},
          {"project", _camera_project},
          {"camera", _camera_id},
          {"name", _camera->name()},
          {"recording", _camera->recording()},
          {"seconds", _camera->seconds()},
          {"width", f.width},
          {"height", f.height},
          {"frames", f.count}};
}

media::CameraCapture::Frame
Controller::camera_frame() const
{
  std::lock_guard lk(_camera_mu);
  return _camera ? _camera->newest() : media::CameraCapture::Frame{};
}

Result<AssetId>
Controller::camera_snap()
{
  ProjectId pid;
  std::string name;
  fs::path file;
  {
    std::lock_guard lk(_camera_mu);
    if (!_camera) {
      return make_error(Code::InvalidArgument, msg::kNoCamera);
    }
    project::Project* p = project(_camera_project);
    if (!p) {
      return make_error(Code::NotFound, msg::kProjectNotOpen);
    }
    pid = _camera_project;
    name = _camera->name();
    file = p->blobs().tmp_dir() /
           std::format("camera-{}.jpg", JobId::make().str());
    VALTZ_TRY(_camera->snap(file));
  }
  auto undo = command_(pid, "capture.camera");
  return import_taken_(pid, file, std::format("{} {}", name,
                                              local_time_hm()), "");
}

Status
Controller::camera_record()
{
  std::lock_guard lk(_camera_mu);
  if (!_camera) {
    return make_error(Code::InvalidArgument, msg::kNoCamera);
  }
  project::Project* p = project(_camera_project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  return _camera->record(p->blobs().tmp_dir() /
                         std::format("camera-{}.mov", JobId::make().str()));
}

Result<AssetId>
Controller::camera_stop_recording()
{
  ProjectId pid;
  std::string name;
  fs::path file;
  {
    std::lock_guard lk(_camera_mu);
    if (!_camera || !_camera->recording()) {
      return make_error(Code::InvalidArgument, msg::kNoCamera);
    }
    pid = _camera_project;
    name = _camera->name();
    file = _camera->file();
    VALTZ_TRY(_camera->stop_recording());
  }
  auto undo = command_(pid, "capture.camera");
  return import_taken_(pid, file, std::format("{} {}", name,
                                              local_time_hm()), "");
}

Status
Controller::stop_camera()
{
  std::unique_ptr<media::CameraCapture> c;
  {
    std::lock_guard lk(_camera_mu);
    c = std::move(_camera);
  }
  if (c) {
    VALTZ_LOG_INFO("capture", "camera {} off", c->name());
  }
  return ok_status();
}

Status
Controller::cancel_capture()
{
  std::unique_ptr<media::SoundCapture> c;
  ProjectId pid;
  AssetId comp;
  {
    std::lock_guard lk(_capture_mu);
    if (!_capture) {
      return make_error(Code::InvalidArgument, msg::kNoCapture);
    }
    c = std::move(_capture);
    pid = _capture_project;
    comp = _capture_asset;
  }
  const fs::path file = c->file();
  (void)c->stop();
  c.reset();
  std::error_code ec;
  fs::remove(file, ec);
  post_("capture.stopped", JobId{}, {{"project", pid}, {"asset", comp},
                                     {"cancelled", true}});
  return ok_status();
}

}
