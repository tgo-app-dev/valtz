// The derived-asset dependency graph: staleness and rebuild planning.
//
// Make-like, with content hashes where make uses mtimes. A derived asset
// is up to date when its head version was built by its current recipe
// from exactly the input content its inputs resolve to NOW. The check is
// recursive -- an input that is itself stale makes its consumer stale,
// even though the input's head content has not changed yet -- because
// "build X" must mean "make X reflect the current sources", not "rerun
// X's last step".

#include "valtz/project/project.h"

#include <format>

#include "project/keys.h"

#include <algorithm>
#include <functional>
#include <unordered_set>

namespace valtz::project {

namespace {

// Deep graphs are legitimate (a turnaround sheet from a caption from a
// reference); unbounded ones are a corrupt database.
constexpr int kMaxDepth = 256;

}

namespace {

bool
drawn(const Asset& a)
{
  return a.cls == AssetClass::Still || a.cls == AssetClass::Composition ||
         a.cls == AssetClass::Markup;
}

}

Result<ContentHash>
Project::content_key_(const db::Txn& txn, AssetId id, std::uint32_t version,
                      int depth) const
{
  if (depth > 32) {
    return make_error(Code::Corrupt, "compositions nest in a cycle");
  }
  VALTZ_ASSIGN(auto a, get_asset_(txn, id));
  if (!a) {
    return make_error(Code::NotFound, "no such asset");
  }
  if (a->cls == AssetClass::Markup) {
    const Markup mk = a->markup.value_or(Markup{});
    return ContentHash::of(std::format(
        "markup|{}|{}x{}|{}", mk.raster.hash.hex(), mk.width, mk.height,
        to_text(mk.objects)));
  }
  if (!drawn(*a)) {
    const std::uint32_t n = version ? version : a->head;
    if (n == 0) {
      return make_error(Code::NotFound, std::format(
          "{} has not been built", a->name));
    }
    VALTZ_ASSIGN(auto v, get_version_(txn, id, n));
    if (!v) {
      return make_error(Code::NotFound, std::format(
          "{} version {} is missing", a->name, n));
    }
    return v->content;
  }
  // Its structure, and what each layer shows, as content.
  Json doc = {{"class", to_str(a->cls)}, {"kind", to_str(a->kind)}};
  Json ls = Json::array();
  for (const auto& l : a->layers) {
    Json lj = {{"id", l.id}, {"visible", l.visible}, {"mask", l.mask},
               {"in", l.time.in}, {"out", l.time.out},
               {"rate_num", l.time.rate.num}, {"rate_den", l.time.rate.den},
               {"offset", l.time.offset}, {"duration", l.time.duration}};
    if (l.source) {
      VALTZ_ASSIGN(ContentHash k, content_key_(txn, *l.source,
                                               l.source_version, depth + 1));
      lj["source"] = k.hex();
    }
    ls.push_back(std::move(lj));
  }
  doc["layers"] = std::move(ls);
  Json mods = Json::array();
  for (const auto& m : a->modifiers) {
    mods.push_back({{"kind", m.kind}, {"layer", m.layer},
                    {"params", m.params}});
  }
  doc["modifiers"] = std::move(mods);
  doc["canvas"] = media::to_json(a->canvas);
  doc["timeline"] = a->timeline_frames;
  doc["rate"] = {a->rate.num, a->rate.den};
  if (a->pages > 1) {
    doc["pages"] = a->pages;
  }
  Json ts = Json::array();
  for (const auto& t : a->transitions) {
    ts.push_back({t.from, t.to, t.kind});
  }
  doc["transitions"] = std::move(ts);
  return ContentHash::of(to_text(doc));
}

Result<ContentHash>
Project::content_key(AssetId id, std::uint32_t version) const
{
  ContentHash out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(out, content_key_(txn, id, version, 0));
    return ok_status();
  }));
  return out;
}

Result<Staleness>
Project::staleness_(const db::Txn& txn, AssetId id, int depth) const
{
  if (depth > kMaxDepth) {
    return make_error(Code::Corrupt, "dependency graph is too deep");
  }
  VALTZ_ASSIGN(auto a, get_asset_(txn, id));
  if (!a) {
    return make_error(Code::NotFound, "no such asset");
  }
  if (a->origin == Origin::Source) {
    return Staleness{};  // sources are fresh by definition
  }
  if (a->head == 0) {
    return Staleness{StaleReason::NeverBuilt, {}};
  }
  VALTZ_ASSIGN(auto head, get_version_(txn, id, a->head));
  if (!head) {
    return make_error(Code::Corrupt, "head version missing");
  }
  if (head->recipe != a->recipe) {
    return Staleness{StaleReason::RecipeChanged, {}};
  }
  VALTZ_ASSIGN(auto r, get_recipe_(txn, a->recipe));
  if (!r) {
    return make_error(Code::Corrupt, "recipe missing");
  }

  for (const auto& in : r->inputs) {
    VALTZ_ASSIGN(auto ia, get_asset_(txn, in.asset));
    if (!ia) {
      return Staleness{StaleReason::InputMissing, in.asset};
    }
    // A composition or a markup: what it draws, by its key.
    if (drawn(*ia)) {
      auto k = content_key_(txn, in.asset, 0, 0);
      auto built = std::find_if(head->inputs.begin(), head->inputs.end(),
          [&](const ResolvedInput& ri) {
            return ri.asset == in.asset && ri.role == in.role;
          });
      if (!k.ok()) {
        return Staleness{StaleReason::InputMissing, in.asset};
      }
      if (built == head->inputs.end() || built->hash != *k) {
        return Staleness{StaleReason::InputChanged, in.asset};
      }
      continue;
    }
    // Upstream first: a stale input will change when rebuilt.
    if (ia->origin == Origin::Derived && in.version == 0) {
      VALTZ_ASSIGN(auto up, staleness_(txn, in.asset, depth + 1));
      if (!up.fresh()) {
        return Staleness{StaleReason::InputStale, in.asset};
      }
    }
    std::uint32_t n = in.version ? in.version : ia->head;
    if (n == 0) {
      return Staleness{StaleReason::InputMissing, in.asset};
    }
    VALTZ_ASSIGN(auto iv, get_version_(txn, in.asset, n));
    if (!iv) {
      return Staleness{StaleReason::InputMissing, in.asset};
    }
    auto built = std::find_if(head->inputs.begin(), head->inputs.end(),
        [&](const ResolvedInput& ri) {
          return ri.asset == in.asset && ri.role == in.role;
        });
    if (built == head->inputs.end() || built->hash != iv->content) {
      return Staleness{StaleReason::InputChanged, in.asset};
    }
  }
  return Staleness{};
}

Result<Staleness>
Project::staleness(AssetId id) const
{
  Staleness out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    VALTZ_ASSIGN(out, staleness_(txn, id, 0));
    return ok_status();
  }));
  return out;
}

Result<std::vector<AssetId>>
Project::dependents(AssetId id, bool transitive) const
{
  std::vector<AssetId> out;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    std::unordered_set<AssetId> seen;
    std::vector<AssetId> frontier{id};
    while (!frontier.empty()) {
      AssetId cur = frontier.back();
      frontier.pop_back();
      VALTZ_ASSIGN(db::Cursor c, txn.cursor(_db.dependents));
      for (auto e = c.seek_exact(id_key(cur)); e; e = c.next_dup()) {
        auto d = id_from_bytes<AssetId>(e->value);
        if (seen.insert(d).second) {
          out.push_back(d);
          if (transitive) {
            frontier.push_back(d);
          }
        }
      }
    }
    return ok_status();
  }));
  return out;
}

Result<std::vector<BuildStep>>
Project::plan_build(const std::vector<AssetId>& targets) const
{
  std::vector<BuildStep> plan;
  VALTZ_TRY(_env->read([&](db::Txn& txn) -> Status {
    std::unordered_set<AssetId> done;     // visited and placed
    std::unordered_set<AssetId> onstack;  // cycle guard

    std::function<Status(AssetId, int)> visit =
        [&](AssetId id, int depth) -> Status {
      if (done.count(id)) {
        return ok_status();
      }
      if (depth > kMaxDepth || onstack.count(id)) {
        return make_error(Code::Corrupt, "dependency cycle");
      }
      VALTZ_ASSIGN(auto a, get_asset_(txn, id));
      if (!a) {
        return make_error(Code::NotFound, "no such asset");
      }
      if (a->origin == Origin::Source) {
        done.insert(id);
        return ok_status();
      }
      onstack.insert(id);
      VALTZ_ASSIGN(auto r, get_recipe_(txn, a->recipe));
      if (!r) {
        return make_error(Code::Corrupt, "recipe missing");
      }
      // Inputs first (post-order = dependency order).
      for (const auto& in : r->inputs) {
        if (in.version == 0) {
          VALTZ_TRY(visit(in.asset, depth + 1));
        }
      }
      onstack.erase(id);
      done.insert(id);

      VALTZ_ASSIGN(auto st, staleness_(txn, id, 0));
      bool upstream_planned = std::any_of(
          r->inputs.begin(), r->inputs.end(), [&](const RecipeInput& in) {
            return std::any_of(plan.begin(), plan.end(),
                               [&](const BuildStep& s) {
                                 return s.asset == in.asset;
                               });
          });
      if (!st.fresh() || upstream_planned) {
        StaleReason why = st.fresh() ? StaleReason::InputStale : st.reason;
        plan.push_back({id, a->recipe, why, !r->deterministic});
      }
      return ok_status();
    };

    for (const auto& t : targets) {
      VALTZ_TRY(visit(t, 0));
    }
    return ok_status();
  }));
  return plan;
}

}
