// The Atlas's GENERATE NODES (DESIGN §10a Atlas): a generation set up in
// the graph before it runs -- its prompt and reference row linked from
// the assets there, its model and settings chosen -- then started.
//
// Kept as one list in the project's meta, written in commands: a link
// undone is a link gone. A node is data until it starts; started, it
// builds the very request the editor's Start would (GenerateImageRequest
// ...), so what it makes is a generation like any -- its recipe, its
// task, its result -- and the node, frozen, joins that generation's
// command: undone, the task is withdrawn and the node is a draft again.

#include "valtz/controller/controller.h"

#include "valtz/base/log.h"
#include "valtz/base/message.h"
#include "valtz/base/text.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <random>

namespace valtz {

namespace {

using project::AssetKind;

std::int64_t
fresh_seed()
{
  std::random_device rd;
  // In 31 bits, as every generation's (random_seed).
  return static_cast<std::int64_t>(rd() & 0x7fffffff);
}

std::int64_t
now_ms()
{
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

bool
is_target(const std::string& t)
{
  return t == "image" || t == "video" || t == "audio";
}

// What a target reads in its row: a picture's generation pictures; a
// clip's pictures, clips and sounds (Ref2VA's references, FL2VA's
// keyframes); a sound's a voice -- a sound, or a clip's.
bool
reads(const std::string& target, AssetKind k)
{
  if (target == "image") {
    return k == AssetKind::Image;
  }
  if (target == "video") {
    return k == AssetKind::Image || k == AssetKind::Video ||
           k == AssetKind::Audio;
  }
  return k == AssetKind::Audio || k == AssetKind::Video;
}

// The settings a node keeps, and what each is when not given.
Json
defaults()
{
  return {{"model", ""}, {"preference", "balanced"}, {"custom", false},
          {"tuning", Json::object()}, {"width", 0}, {"height", 0},
          {"seconds", 0.0}, {"plan", "full"}, {"seed", -1}};
}

}  // namespace

Result<Json>
Controller::generate_nodes(ProjectId pid)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(Json nodes, p->generate_nodes());
  // What it links that is gone (removed from the project) is not linked.
  for (auto& n : nodes) {
    if (auto pa = AssetId::parse(jget<std::string>(n, "prompt", ""));
        pa && !p->asset(*pa).ok()) {
      n.erase("prompt");
    }
    Json refs = Json::array();
    for (const auto& r : jget(n, "refs", Json::array())) {
      const auto id = AssetId::parse(r.is_string() ? r.get<std::string>()
                                                   : std::string());
      if (id && p->asset(*id).ok()) {
        refs.push_back(r);
      }
    }
    n["refs"] = std::move(refs);
  }
  return nodes;
}

Result<Json>
Controller::generate_node_op(ProjectId pid, const Json& req)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  const std::string action = jget<std::string>(req, "action", "");
  const std::string id = jget<std::string>(req, "node", "");
  auto asset_arg = [&](const Json& v) -> Result<project::Asset> {
    const auto a = AssetId::parse(v.is_string() ? v.get<std::string>()
                                                : std::string());
    if (!a) {
      return make_error(Code::InvalidArgument, "which asset?");
    }
    return p->asset(*a);
  };

  // A node as it is: nothing changed.
  if (action == "get") {
    VALTZ_ASSIGN(Json nodes, generate_nodes(pid));
    for (const auto& n : nodes) {
      if (jget<std::string>(n, "id", "") == id) {
        return Json{{"node", n}};
      }
    }
    return make_error(Code::NotFound, "no such generate node");
  }
  // A STARTED node runs its generation in the generation's own command
  // (it opens it); every other change is a command of its own.
  if (action == "start") {
    VALTZ_ASSIGN(Json nodes, generate_nodes(pid));
    auto it = std::ranges::find_if(nodes, [&](const Json& n) {
      return jget<std::string>(n, "id", "") == id;
    });
    if (it == nodes.end()) {
      return make_error(Code::NotFound, "no such generate node");
    }
    Json node = *it;
    if (node.contains("started")) {
      return make_error(Code::InvalidArgument, msg::kGenerateHasRun);
    }
    const std::string target = jget<std::string>(node, "target", "");
    std::optional<AssetId> prompt_asset =
        AssetId::parse(jget<std::string>(node, "prompt", ""));
    std::string words;
    if (prompt_asset) {
      VALTZ_ASSIGN(words, p->read_text(*prompt_asset));
    }
    std::vector<AssetId> refs;
    std::vector<AssetKind> kinds;
    for (const auto& r : jget(node, "refs", Json::array())) {
      if (auto a = AssetId::parse(r.get<std::string>())) {
        VALTZ_ASSIGN(project::Asset as, p->asset(*a));
        refs.push_back(*a);
        kinds.push_back(as.kind);
      }
    }
    std::string model = jget<std::string>(node, "model", "");
    const std::string pref = jget<std::string>(node, "preference",
                                               "balanced");
    const Json tuning = jget(node, "custom", false)
                            ? jget(node, "tuning", Json::object())
                            : Json::object();
    const bool seed_given = jget<std::int64_t>(node, "seed", -1) >= 0;
    const std::int64_t seed = seed_given
                                  ? jget<std::int64_t>(node, "seed", 0)
                                  : fresh_seed();
    const auto width = jget<std::int32_t>(node, "width", 0);
    const auto height = jget<std::int32_t>(node, "height", 0);
    const double seconds = jget(node, "seconds", 0.0);
    // What a node's own settings do not say -- a recorded generation's
    // (record_generation_): the row the prompt's tags name, which picture
    // an edit changes, a clip continued, latents kept, references whole,
    // lyrics apart, steps.
    const Json x = jget(node, "extra", Json::object());
    auto ids = [](const Json& v) {
      std::vector<AssetId> out;
      for (const auto& e : v.is_array() ? v : Json::array()) {
        if (auto a = AssetId::parse(e.is_string() ? e.get<std::string>()
                                                  : std::string())) {
          out.push_back(*a);
        }
      }
      return out;
    };
    const std::vector<AssetId> row =
        x.contains("row") ? ids(x["row"]) : refs;
    auto asset_of = [&](const char* key) {
      return AssetId::parse(jget<std::string>(x, key, ""));
    };
    const auto steps = jget<std::int32_t>(x, "steps", 0);
    Result<JobId> job = make_error(Code::InvalidArgument, "no target");
    if (target == "image") {
      GenerateImageRequest r;
      r.project = pid;
      r.node = id;
      r.prompt = words;
      r.prompt_asset = prompt_asset;
      r.row = row;
      r.model = model;
      r.preference = pref;
      r.tuning = tuning;
      r.seed = seed;
      r.width = width;
      r.height = height;
      r.steps = steps;
      r.mode = jget<std::string>(x, "mode", "auto");
      r.negative = jget<std::string>(x, "negative", "");
      // The picture edited: the one named, else the row's first; none
      // where it composed from its references alone.
      if (!refs.empty() && !jget(x, "no_base", false)) {
        const auto named = asset_of("base_asset");
        const auto b = named && std::ranges::find(refs, *named) !=
                                    refs.end()
                           ? *named
                           : refs.front();
        r.base = b;
        for (const auto& a : refs) {
          if (a != b) {
            r.references.push_back(a);
          }
        }
      } else {
        r.references = refs;
      }
      job = generate_image(std::move(r));
    } else if (target == "video") {
      GenerateVideoRequest r;
      r.project = pid;
      r.node = id;
      r.prompt = words;
      r.prompt_asset = prompt_asset;
      r.row = row;
      r.preference = pref;
      r.tuning = tuning;
      r.seed = seed;
      r.width = width;
      r.height = height;
      r.steps = steps;
      r.turbo = jget(x, "turbo", true);
      r.fps = jget(x, "fps", 0.0);
      r.continue_from = asset_of("continue_from");
      r.tail_seconds = jget(x, "tail_seconds", 0.0);
      r.continue_latents = jget(x, "continue_latents", false);
      r.keep_latents = jget(x, "keep_latents", false);
      r.reference_sound = jget(x, "reference_sound", false);
      r.whole = ids(jget(x, "whole", Json::array()));
      r.latents_only = ids(jget(x, "latents_only", Json::array()));
      if (x.contains("regenerate") && x["regenerate"].is_boolean()) {
        r.regenerate = x["regenerate"].get<bool>();
      }
      // The row: references the clip draws on (Auto's: a model that
      // takes them) -- or, a model that opens and ends on pictures
      // chosen (FL2VA), its first frame and its last.
      const bool carried = r.continue_from.has_value();
      const models::ModelEntry* m =
          model.empty() ? (refs.empty() && !carried ? nullptr
                                                    : reference_model_(""))
                        : _catalog.find(model);
      if (!model.empty() && !m) {
        return make_error(Code::NotFound,
                          std::format("no model '{}'", model));
      }
      if ((!refs.empty() || carried) && !m) {
        return make_error(Code::Unsupported,
                          msg::kGenerateNoReferenceModel);
      }
      r.model = m ? m->id : std::string();
      if (!refs.empty() &&
          !m->has(models::Capability::ReferenceToVideo)) {
        const Json vp = jget(m->engine, "vpipe", Json::object());
        const std::size_t most = jget(vp, "last_frame", false) ? 2 : 1;
        if (!std::ranges::all_of(kinds, [](AssetKind k) {
              return k == AssetKind::Image;
            }) ||
            refs.size() > most) {
          return make_error(Code::InvalidArgument, msg::kGenerateKeyframes,
                            {{"model", m->name},
                             {"count", std::to_string(most)}});
        }
        // Ending on its one picture (L2VA), as it was asked.
        if (jget<std::string>(x, "keyframes", "") == "last") {
          r.last = refs[0];
        } else {
          r.first = refs[0];
          if (refs.size() == 2) {
            r.last = refs[1];
          }
        }
      } else {
        r.references = refs;
      }
      if (seconds > 0) {
        const double fps =
            r.fps > 0 ? r.fps
            : m       ? jget(jget(jget(m->engine, "vpipe", Json::object()),
                                  "defaults", Json::object()),
                             "fps", 24.0)
                      : 24.0;
        r.frames = static_cast<std::int32_t>(std::lround(seconds * fps));
      }
      job = generate_video(std::move(r));
    } else if (target == "audio") {
      GenerateAudioRequest r;
      r.project = pid;
      r.node = id;
      r.prompt = words;
      r.prompt_asset = prompt_asset;
      r.row = row;
      r.model = model;
      r.preference = pref;
      r.tuning = tuning;
      r.seed = seed;
      r.steps = steps;
      r.plan = jget<std::string>(node, "plan", "full");
      r.lyrics = jget<std::string>(x, "lyrics", "");
      r.score = jget<std::string>(x, "score", "");
      r.voices = ids(jget(x, "voices", Json::array()));
      for (const auto& t : jget(x, "voice_texts", Json::array())) {
        r.voice_texts.push_back(t.is_string() ? t.get<std::string>()
                                              : std::string());
      }
      // The voice spoken in: the one named, else the row's first sound
      // -- else clip.
      r.voice = asset_of("voice");
      for (std::size_t i = 0; i < refs.size() && !r.voice; ++i) {
        if (kinds[i] == AssetKind::Audio) {
          r.voice = refs[i];
        }
      }
      for (std::size_t i = 0; i < refs.size() && !r.voice; ++i) {
        r.voice = refs[i];
      }
      const auto* m = model.empty() ? nullptr : _catalog.find(model);
      const bool speech =
          m ? m->has(models::Capability::TextToSpeech) : r.voice.has_value();
      if (speech) {
        r.seconds = seconds;
      } else {
        r.max_seconds = seconds;
      }
      job = generate_audio(std::move(r));
    }
    if (!job.ok()) {
      return job.error();
    }
    const auto rec = _jobs.get(*job);
    // Frozen, in the generation's command: undone with it.
    auto undo = join_(pid, *job);
    VALTZ_ASSIGN(Json now, p->generate_nodes());
    for (auto& n : now) {
      if (jget<std::string>(n, "id", "") == id) {
        Json st = {{"job", job->str()}, {"seed", seed},
                   {"seed_given", seed_given}, {"at", now_ms()}};
        if (rec && !rec->asset.is_nil()) {
          st["asset"] = rec->asset.str();
          if (auto a = p->asset(rec->asset); a.ok() && !a->recipe.is_nil()) {
            if (auto rr = p->recipe(a->recipe); rr.ok()) {
              st["model"] = rr->model;
            }
          }
        }
        n["started"] = st;
        node = n;
      }
    }
    VALTZ_TRY(p->set_generate_nodes(now));
    Json out = {{"node", node}, {"job", job->str()}};
    if (rec && !rec->asset.is_nil()) {
      out["asset"] = rec->asset.str();
    }
    return out;
  }

  static const std::map<std::string, std::string> commands = {
      {"add", "gennode.add"},         {"link", "gennode.link"},
      {"unlink", "gennode.unlink"},   {"order", "gennode.order"},
      {"set", "gennode.set"},         {"duplicate", "gennode.duplicate"},
      {"remove", "gennode.remove"}};
  const auto kind = commands.find(action);
  if (kind == commands.end()) {
    return make_error(Code::InvalidArgument,
                      std::format("no generate-node action '{}'", action));
  }
  auto undo = command_(pid, kind->second);
  VALTZ_ASSIGN(Json nodes, generate_nodes(pid));
  auto find = [&]() -> Result<Json*> {
    for (auto& n : nodes) {
      if (jget<std::string>(n, "id", "") == id) {
        return &n;
      }
    }
    return make_error(Code::NotFound, "no such generate node");
  };
  // An asset into a node: the text its prompt, the rest its row.
  auto link = [&](Json& n, const project::Asset& a) -> Status {
    const std::string target = jget<std::string>(n, "target", "");
    if (a.kind == AssetKind::Text) {
      const auto had = AssetId::parse(jget<std::string>(n, "prompt", ""));
      if (had && *had == a.id) {
        return make_error(Code::InvalidArgument, msg::kGenerateLinked,
                          {{"name", a.name}});
      }
      if (had) {
        const auto old = p->asset(*had);
        return make_error(Code::InvalidArgument, msg::kGenerateOneText,
                          {{"name", old.ok() ? old->name : a.name}});
      }
      n["prompt"] = a.id.str();
      return ok_status();
    }
    if (!reads(target, a.kind)) {
      return make_error(Code::InvalidArgument,
                        target == "image" ? msg::kGeneratePictureRow
                                          : msg::kGenerateSoundRow,
                        {{"name", a.name}});
    }
    Json& refs = n["refs"];
    if (!refs.is_array()) {
      refs = Json::array();
    }
    if (std::ranges::find(refs, Json(a.id.str())) != refs.end()) {
      return make_error(Code::InvalidArgument, msg::kGenerateLinked,
                        {{"name", a.name}});
    }
    refs.push_back(a.id.str());
    return ok_status();
  };
  auto unfrozen = [](const Json& n) -> Status {
    if (n.contains("started")) {
      return make_error(Code::InvalidArgument, msg::kGenerateHasRun);
    }
    return ok_status();
  };

  // Its row changed: a recorded row (the request's own, its tags read
  // against it) no longer is.
  auto row_changed = [](Json& n) {
    if (n.contains("extra") && n["extra"].is_object()) {
      for (const char* k : {"row", "voice", "voices", "voice_texts"}) {
        n["extra"].erase(k);
      }
    }
  };
  Json out;
  if (action == "add") {
    const std::string target = jget<std::string>(req, "target", "");
    if (!is_target(target)) {
      return make_error(Code::InvalidArgument,
                        "a generation makes an image, a video or audio");
    }
    Json n = defaults();
    n["id"] = Uuid::v7().str();
    n["target"] = target;
    n["refs"] = Json::array();
    n["created"] = now_ms();
    for (const auto& v : jget(req, "assets", Json::array())) {
      VALTZ_ASSIGN(project::Asset a, asset_arg(v));
      VALTZ_TRY(link(n, a));
    }
    nodes.push_back(n);
    out = n;
  } else if (action == "duplicate") {
    VALTZ_ASSIGN(Json* src, find());
    Json n = *src;
    const Json st = jget(n, "started", Json::object());
    n["id"] = Uuid::v7().str();
    n["created"] = now_ms();
    // Set up here now, not as it was asked elsewhere.
    n.erase("recorded");
    // Run, the seed it ran with goes along only when one was given: else
    // a new take. Not run, its own as it is.
    if (n.contains("started")) {
      n["seed"] = jget(st, "seed_given", false)
                      ? jget<std::int64_t>(st, "seed", -1)
                      : std::int64_t{-1};
      n.erase("started");
    }
    const auto at = std::ranges::find_if(nodes, [&](const Json& x) {
      return jget<std::string>(x, "id", "") == id;
    });
    nodes.insert(at + 1, n);
    out = n;
  } else if (action == "remove") {
    VALTZ_ASSIGN(Json* n, find());
    out = *n;
    Json kept = Json::array();
    for (auto& x : nodes) {
      if (jget<std::string>(x, "id", "") != id) {
        kept.push_back(std::move(x));
      }
    }
    nodes = std::move(kept);
  } else {
    VALTZ_ASSIGN(Json* n, find());
    VALTZ_TRY(unfrozen(*n));
    if (action == "link") {
      VALTZ_ASSIGN(project::Asset a, asset_arg(jget(req, "asset", Json())));
      VALTZ_TRY(link(*n, a));
      row_changed(*n);
    } else if (action == "unlink") {
      VALTZ_ASSIGN(project::Asset a, asset_arg(jget(req, "asset", Json())));
      const bool prompt = jget<std::string>(*n, "prompt", "") == a.id.str();
      Json& refs = (*n)["refs"];
      const auto before = refs.size();
      if (prompt) {
        n->erase("prompt");
      } else {
        refs.erase(std::remove(refs.begin(), refs.end(), Json(a.id.str())),
                   refs.end());
      }
      if (!prompt && refs.size() == before) {
        return make_error(Code::InvalidArgument, std::format(
            "\"{}\" is not linked to it", a.name));
      }
      row_changed(*n);
    } else if (action == "order") {
      // The row reordered: the same assets, each once.
      const Json given = jget(req, "refs", Json::array());
      std::vector<std::string> now, to;
      for (const auto& r : (*n)["refs"]) {
        now.push_back(r.get<std::string>());
      }
      for (const auto& r : given) {
        to.push_back(r.is_string() ? r.get<std::string>() : std::string());
      }
      auto a = now;
      auto b = to;
      std::ranges::sort(a);
      std::ranges::sort(b);
      if (a != b) {
        return make_error(Code::InvalidArgument,
                          "the row reordered holds what it held, once each");
      }
      (*n)["refs"] = given;
      row_changed(*n);
    } else if (action == "set") {
      const Json s = jget(req, "settings", Json::object());
      const Json known = defaults();
      for (const auto& [k, v] : s.items()) {
        if (!known.contains(k)) {
          return make_error(Code::InvalidArgument,
                            std::format("no setting '{}'", k));
        }
        if (k == "model" && v.is_string() && !v.get<std::string>().empty()) {
          const auto* m = _catalog.find(v.get<std::string>());
          if (!m) {
            return make_error(Code::NotFound, std::format(
                "no model '{}'", v.get<std::string>()));
          }
        }
        (*n)[k] = v;
      }
    }
    out = *n;
  }
  VALTZ_TRY(p->set_generate_nodes(nodes));
  return Json{{"node", out}};
}

void
Controller::record_generation_(ProjectId pid, JobId job, Json node)
{
  project::Project* p = project(pid);
  const auto rec = _jobs.get(job);
  if (!p || !rec || rec->asset.is_nil()) {
    return;
  }
  auto a = p->asset(rec->asset);
  auto r = a.ok() && !a->recipe.is_nil() ? p->recipe(a->recipe)
                                         : Result<project::Recipe>(
                                               make_error(Code::NotFound,
                                                          "no recipe"));
  if (!r.ok()) {
    return;
  }
  const auto asked = jget<std::int64_t>(node, "seed", -1);
  // As any node's: its settings, defaults for what the request left out.
  Json n = defaults();
  n.update(node);
  n["id"] = Uuid::v7().str();
  n["created"] = now_ms();
  n["recorded"] = true;
  if (!n.contains("refs")) {
    n["refs"] = Json::array();
  }
  n["started"] = {{"job", job.str()}, {"asset", rec->asset.str()},
                  {"seed", jget<std::int64_t>(r->params, "seed", asked)},
                  {"seed_given", asked >= 0}, {"model", r->model},
                  {"at", now_ms()}};
  auto nodes = p->generate_nodes();
  if (!nodes.ok()) {
    return;
  }
  nodes->push_back(std::move(n));
  if (auto st = p->set_generate_nodes(*nodes); !st.ok()) {
    VALTZ_LOG_WARN("atlas", "a generation's node not recorded: {}",
                   st.error().message);
  }
}

}  // namespace valtz
