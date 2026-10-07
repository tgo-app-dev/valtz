#include "valtz/project/migrate.h"

#include "valtz/media/keyframes.h"
#include "valtz/project/records.h"

#include <algorithm>
#include <format>
#include <map>
#include <set>

namespace valtz::project {

namespace {

bool
composition_op(std::string_view op)
{
  return op == "project" || op == "capture" || op == "modify" ||
         op == "instance" || op == "unfreeze";
}

AssetClass
composition_class(AssetKind k)
{
  return k == AssetKind::Image ? AssetClass::Still : AssetClass::Composition;
}

bool
has_look(const Asset& a)
{
  return !a.modifiers.empty() || !a.layers.empty() || a.canvas.set() ||
         a.canvas.framed() || a.timeline_frames > 0;
}

// A "trim" modifier becomes its layer's marks; the rest stay.
void
trims_to_marks(Asset& a)
{
  std::vector<Modifier> keep;
  for (auto& m : a.modifiers) {
    if (m.kind != "trim") {
      keep.push_back(std::move(m));
      continue;
    }
    const media::Trim t = media::trim_from_json(m.params);
    for (auto& l : a.layers) {
      if (l.id == m.layer) {
        l.time.in = t.in;
        l.time.out = t.out;
        l.time.rate = t.rate;
      }
    }
  }
  a.modifiers = std::move(keep);
}

std::string
unique_name(std::set<std::string>& taken, const std::string& want)
{
  std::string name = want;
  for (int n = 2; taken.contains(name); ++n) {
    name = std::format("{} ({})", want, n);
  }
  taken.insert(name);
  return name;
}

}

std::uint32_t
document_schema(const Json& doc)
{
  const Json meta = jget(doc, "meta", Json::object());
  const Json head = jget(meta, "project", Json::object());
  return std::max<std::uint32_t>(1, jget<std::uint32_t>(head, "schema", 1));
}

Status
migrate_document(Json& doc)
{
  if (document_schema(doc) >= kSchemaVersion) {
    return ok_status();
  }
  if (!doc.is_object()) {
    return make_error(Code::Corrupt, "not a project document");
  }
  Json& tables = doc["tables"];
  if (!tables.is_object()) {
    tables = Json::object();
  }
  std::vector<Asset> assets;
  for (const auto& j : jget(tables, "assets", Json::array())) {
    assets.push_back(j.get<Asset>());
  }
  std::map<RecipeId, Recipe> recipes;
  for (const auto& j : jget(tables, "recipes", Json::array())) {
    auto r = j.get<Recipe>();
    recipes[r.id] = std::move(r);
  }
  // Each asset's versions' pictures, by number: a markup's frame.
  std::map<std::pair<AssetId, std::uint32_t>, media::MediaInfo> infos;
  for (const auto& j : jget(tables, "versions", Json::array())) {
    const auto v = j.get<AssetVersion>();
    infos[{v.asset, v.number}] = v.info;
  }
  std::map<AssetId, std::size_t> by_id;
  std::set<std::string> names;
  for (std::size_t i = 0; i < assets.size(); ++i) {
    by_id[assets[i].id] = i;
    names.insert(assets[i].name);
  }
  auto op_of = [&](const Asset& a) -> std::string {
    if (a.origin != Origin::Derived) {
      return {};
    }
    auto r = recipes.find(a.recipe);
    return r == recipes.end() ? std::string() : r->second.op;
  };

  // 1. Classes; a composition's own image named.
  for (auto& a : assets) {
    const std::string op = op_of(a);
    if (a.kind == AssetKind::Text) {
      a.cls = AssetClass::Flat;
    } else if (composition_op(op)) {
      a.cls = composition_class(a.kind);
      std::optional<RecipeInput> own;
      if (auto r = recipes.find(a.recipe); r != recipes.end()) {
        for (const auto& in : r->second.inputs) {
          if (in.role == "own" || (in.role == "base" && !own)) {
            own = in;
          }
        }
      }
      for (auto& l : a.layers) {
        if (l.own) {
          l.own = false;
          if (own) {
            l.source = own->asset;
            l.source_version = own->version;
          }
        }
      }
      if (a.layers.empty() && own) {
        Layer l;
        l.source = own->asset;
        l.source_version = own->version;
        a.layers.push_back(std::move(l));
      }
      trims_to_marks(a);
    } else if (a.origin == Origin::Derived) {
      a.cls = AssetClass::Generated;
    } else {
      a.cls = AssetClass::Flat;
    }
  }

  // 2. A look on a flat or generated asset goes to a composition of its
  // own, which shows the asset.
  std::vector<Asset> added;
  for (auto& a : assets) {
    if (a.cls != AssetClass::Flat && a.cls != AssetClass::Generated) {
      continue;
    }
    if (a.kind == AssetKind::Text || !has_look(a)) {
      continue;
    }
    Asset c;
    c.id = AssetId::make();
    c.name = unique_name(names, a.name + ", edited");
    c.kind = a.kind;
    c.origin = Origin::Source;
    c.cls = composition_class(a.kind);
    c.created_ms = std::max(a.modified_ms, a.created_ms);
    c.modified_ms = c.created_ms;
    c.folder = a.folder;
    c.layers = std::move(a.layers);
    for (auto& l : c.layers) {
      if (l.own) {
        l.own = false;
        l.source = a.id;
        l.source_version = 0;
      }
    }
    if (c.layers.empty()) {
      Layer l;
      l.source = a.id;
      c.layers.push_back(std::move(l));
    }
    c.modifiers = std::move(a.modifiers);
    c.canvas = a.canvas;
    c.timeline_frames = a.timeline_frames;
    trims_to_marks(c);
    a.layers.clear();
    a.modifiers.clear();
    a.canvas = {};
    a.timeline_frames = 0;
    added.push_back(std::move(c));
  }
  for (auto& c : added) {
    by_id[c.id] = assets.size();
    assets.push_back(std::move(c));
  }

  // 3. A markup layer's content becomes a markup asset it shows, the
  // composition's frame in size.
  auto frame_of = [&](const Asset& c) -> media::PixelSize {
    if (c.canvas.framed()) {
      return c.canvas.frame();
    }
    for (const auto& m : c.modifiers) {
      if (m.kind == "crop" && m.layer.empty()) {
        const auto k = media::keyed_crop_from_json(m.params);
        if (!k.place.keys.empty()) {
          const auto& cr = k.place.keys.front().value;
          if (cr.canvas.width > 0) {
            return cr.canvas;
          }
          if (cr.content.width > 0) {
            return cr.content;
          }
        }
      }
    }
    if (!c.layers.empty() && c.layers.front().source) {
      const auto it = by_id.find(*c.layers.front().source);
      if (it != by_id.end()) {
        const Asset& s = assets[it->second];
        const auto v = c.layers.front().source_version > 0
                           ? c.layers.front().source_version : s.head;
        if (auto i = infos.find({s.id, v}); i != infos.end()) {
          return {i->second.frame.width, i->second.frame.height};
        }
      }
    }
    return {};
  };
  std::vector<Asset> markups;
  for (auto& c : assets) {
    if (c.cls != AssetClass::Still && c.cls != AssetClass::Composition) {
      continue;
    }
    const media::PixelSize frame = frame_of(c);
    for (auto& l : c.layers) {
      if (!l.markup) {
        continue;
      }
      Asset m;
      m.id = AssetId::make();
      m.name = unique_name(names, std::format(
          "{} · {}", c.name, l.name.empty() ? std::string("Markup")
                                              : l.name));
      m.kind = AssetKind::Image;
      m.origin = Origin::Source;
      m.cls = AssetClass::Markup;
      m.created_ms = c.modified_ms;
      m.modified_ms = c.modified_ms;
      m.folder = c.folder;
      Markup mk = std::move(*l.markup);
      if (mk.width <= 0 || mk.height <= 0) {
        mk.width = frame.width;
        mk.height = frame.height;
      }
      m.markup = std::move(mk);
      l.markup.reset();
      l.source = m.id;
      l.source_version = 0;
      markups.push_back(std::move(m));
    }
  }
  for (auto& m : markups) {
    by_id[m.id] = assets.size();
    assets.push_back(std::move(m));
  }

  // 4. A clip stack sounded only its bottom clip: the others are muted,
  // so it sounds as it did.
  for (auto& c : assets) {
    if (c.cls != AssetClass::Composition || c.kind != AssetKind::Video) {
      continue;
    }
    bool bottom = true;
    for (const auto& l : c.layers) {
      if (!l.source) {
        continue;
      }
      const auto it = by_id.find(*l.source);
      if (it == by_id.end() ||
          assets[it->second].kind != AssetKind::Video) {
        continue;
      }
      if (bottom) {
        bottom = false;
        continue;
      }
      const bool has = std::ranges::any_of(c.modifiers, [&](const auto& m) {
        return m.kind == "audio" && m.layer == l.id;
      });
      if (!has) {
        media::KeyedSound k;
        k.keys.push_back({0, media::Sound{0, 0}});
        Modifier m;
        m.kind = "audio";
        m.layer = l.id;
        m.params = media::to_json(k);
        c.modifiers.push_back(std::move(m));
      }
    }
  }

  // 5. The project's composition: its newest "project" asset.
  std::optional<AssetId> project;
  std::int64_t newest = -1;
  for (const auto& a : assets) {
    if (op_of(a) == "project" && a.modified_ms > newest) {
      newest = a.modified_ms;
      project = a.id;
    }
  }

  Json out = Json::array();
  for (const auto& a : assets) {
    out.push_back(Json(a));
  }
  tables["assets"] = std::move(out);
  Json& meta = doc["meta"];
  if (!meta.is_object()) {
    meta = Json::object();
  }
  if (project && !meta.contains("composition")) {
    meta["composition"] = project->str();
  }
  meta["project"]["schema"] = kSchemaVersion;
  return ok_status();
}

}
