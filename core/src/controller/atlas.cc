// The ATLAS (DESIGN §10a Atlas): the project as a graph, left to right --
// what each asset is made from, and through what.
//
// Laid out here, not in the app, so valtzctl prints what the app draws
// and a test can hold it. The layout is a layered one, kept simple
// enough for 16 000 assets: an asset's rank its longest path from what it
// is made from (Kahn's order -- no recursion, a chain of thousands of
// edits is deep), an asset made from nothing pulled right to just before
// what uses it, each op in the column before the asset it makes; in each
// column, rows by the barycentre of their neighbours, swept both ways a
// few times, overlaps pushed apart from above and from below and the two
// averaged. Each part of the graph (its nodes joined by edges) is laid
// apart, one under another -- the project's first.

#include "valtz/controller/controller.h"

#include <algorithm>
#include <limits>
#include <map>
#include <numeric>

namespace valtz {

namespace {

using project::AssetClass;

// Heights in slots: an asset's card, an op's half as tall. The app maps a
// slot to its points.
constexpr double kAssetSlots = 2;
constexpr double kOpSlots = 1;
constexpr double kGap = 0.25;
// Between two parts of the graph.
constexpr double kPartGap = 1;
// The assets joined to nothing, a grid of so many a row under the rest.
constexpr int kLoosePerRow = 8;
constexpr int kSweeps = 4;

struct Node {
  Json             json;
  bool             asset = false;
  int              owner = -1;  // an op: the asset node it makes
  int              rank = 0;    // an asset's
  int              column = 0;
  double           y = 0;
  double           height = kAssetSlots;
  double           order = 0;   // the first order: by creation
  std::vector<int> in;
  std::vector<int> out;
  int              part = -1;
  // An asset something is made from, or a layer shows: no leaf.
  bool             feeds = false;
};

struct Edge {
  int         from = 0;
  int         to = 0;
  std::string kind;  // input | made | layer | into | note
  std::string role;
};

double
centre(const Node& n)
{
  return n.y + n.height / 2;
}

// A column's nodes put at the rows they want, in that order, none over
// another: pushed down from the top, pushed up from the bottom, the two
// averaged (each keeps the gaps, so their mean does).
void
place(std::vector<int>& col, std::vector<Node>& nodes,
      const std::vector<double>& want)
{
  std::stable_sort(col.begin(), col.end(), [&](int a, int b) {
    if (want[a] != want[b]) {
      return want[a] < want[b];
    }
    return nodes[a].y < nodes[b].y;
  });
  const std::size_t n = col.size();
  std::vector<double> down(n);
  std::vector<double> up(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double w = want[col[i]];
    down[i] = i == 0 ? w
                     : std::max(w, down[i - 1] + nodes[col[i - 1]].height +
                                       kGap);
  }
  for (std::size_t k = n; k-- > 0;) {
    const double w = want[col[k]];
    up[k] = k + 1 == n ? w
                       : std::min(w, up[k + 1] - nodes[col[k]].height -
                                         kGap);
  }
  for (std::size_t i = 0; i < n; ++i) {
    nodes[col[i]].y = (down[i] + up[i]) / 2;
  }
}

// What a layer does to what it shows, as the composition's modifiers for
// it say: the look's parts, keyed or not, its time, its mask.
Json
layer_look(const project::Asset& comp, const project::Layer& l)
{
  Json look = Json::array();
  bool keyed = false;
  for (const auto& m : comp.modifiers) {
    if (m.layer != l.id) {
      continue;
    }
    keyed = keyed || m.params.contains("keys") ||
            m.params.contains("rotate_keys");
    if (m.kind == "adjust") {
      look.push_back("adjust");
    } else if (m.kind == "crop") {
      look.push_back("place");
      const double turn = m.params.value("rotate", 0.0);
      if (turn != 0 || m.params.contains("rotate_keys")) {
        look.push_back("turn");
      }
    } else if (m.kind == "speed") {
      look.push_back("speed");
    } else if (m.kind == "audio") {
      look.push_back("sound");
    }
  }
  if (keyed) {
    look.push_back("keyed");
  }
  if (!l.time.identity()) {
    look.push_back("timed");
  }
  if (l.mask) {
    look.push_back("mask");
  }
  if (!l.visible) {
    look.push_back("hidden");
  }
  if (l.locked) {
    look.push_back("locked");
  }
  if (l.source_version > 0) {
    look.push_back("pinned");
  }
  return look;
}

const char*
destination(const project::Asset& a)
{
  if (a.cls == AssetClass::Still) {
    return "still";
  }
  if (a.cls == AssetClass::Composition) {
    return a.kind == project::AssetKind::Audio ? "sound" : "timeline";
  }
  return "";
}

}  // namespace

Result<Json>
Controller::atlas(ProjectId pid)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(auto all, p->assets());
  const auto home = project_composition(pid);

  // By creation: the first order of every column, and the loose grid's.
  std::vector<std::size_t> by_age(all.size());
  std::iota(by_age.begin(), by_age.end(), std::size_t{0});
  std::stable_sort(by_age.begin(), by_age.end(),
                   [&](std::size_t a, std::size_t b) {
                     return all[a].created_ms < all[b].created_ms;
                   });

  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::map<AssetId, int> of;  // asset -> its node
  nodes.reserve(all.size() * 2);
  for (std::size_t k = 0; k < by_age.size(); ++k) {
    const auto& a = all[by_age[k]];
    Node n;
    n.asset = true;
    n.order = static_cast<double>(k);
    n.json = {{"id", "a:" + a.id.str()}, {"type", "asset"},
              {"asset", a.id.str()}};
    if (const char* d = destination(a); *d) {
      n.json["destination"] = d;
    }
    if (home && *home == a.id) {
      n.json["project"] = true;
    }
    of[a.id] = static_cast<int>(nodes.size());
    nodes.push_back(std::move(n));
  }
  auto edge = [&](int from, int to, std::string kind, std::string role) {
    nodes[from].out.push_back(to);
    nodes[to].in.push_back(from);
    nodes[from].feeds = nodes[from].feeds || kind == "input" ||
                        kind == "layer";
    edges.push_back({from, to, std::move(kind), std::move(role)});
  };
  auto op = [&](int owner, Json j, double height) {
    Node n;
    n.owner = owner;
    n.height = height;
    n.json = std::move(j);
    nodes.push_back(std::move(n));
    return static_cast<int>(nodes.size()) - 1;
  };

  for (std::size_t k = 0; k < by_age.size(); ++k) {
    const auto& a = all[by_age[k]];
    const int at = of[a.id];
    // What made it: its recipe's op, its inputs into it.
    std::optional<project::Recipe> r;
    if (a.origin == project::Origin::Derived && !a.recipe.is_nil()) {
      if (auto got = p->recipe(a.recipe); got.ok()) {
        r = std::move(*got);
      }
    }
    if (r && !r->op.empty()) {
      const int o = op(at, {{"id", "r:" + a.id.str()}, {"type", "op"},
                            {"op", r->op}, {"model", r->model},
                            {"into", a.id.str()}},
                       kOpSlots);
      nodes[o].order = nodes[at].order;
      for (const auto& in : r->inputs) {
        if (auto f = of.find(in.asset); f != of.end() && f->second != at) {
          edge(f->second, o, "input", in.role);
        }
      }
      edge(o, at, "made", "");
    } else if (a.from.is_object()) {
      // A flat copy of another (flatten): a note of where it came from,
      // not a tie -- the original may go.
      const auto src =
          AssetId::parse(jget<std::string>(a.from, "asset", ""));
      if (src) {
        if (auto f = of.find(*src); f != of.end() && f->second != at) {
          const int o = op(at, {{"id", "f:" + a.id.str()}, {"type", "op"},
                                {"op", "flatten"}, {"into", a.id.str()},
                                {"note", true}},
                           kOpSlots);
          nodes[o].order = nodes[at].order;
          edge(f->second, o, "note", "");
          edge(o, at, "made", "");
        }
      }
    }
    // A composition's layers: each an op between what it shows and it,
    // the top one first (as the Layers section lists them).
    if (a.cls != AssetClass::Still && a.cls != AssetClass::Composition) {
      continue;
    }
    const auto count = static_cast<int>(a.layers.size());
    for (int i = count; i-- > 0;) {
      const auto& l = a.layers[static_cast<std::size_t>(i)];
      if (!l.source) {
        continue;
      }
      const auto f = of.find(*l.source);
      if (f == of.end() || f->second == at) {
        continue;
      }
      const int o = op(at,
                       {{"id", "l:" + a.id.str() + ":" + l.id},
                        {"type", "layer"}, {"into", a.id.str()},
                        {"layer", l.id}, {"index", i}, {"name", l.name},
                        {"source", l.source->str()},
                        {"look", layer_look(a, l)},
                        {"in", l.time.in}, {"out", l.time.out},
                        {"offset", l.time.offset},
                        {"duration", l.time.duration}},
                       kOpSlots);
      nodes[o].order = nodes[at].order + 0.5 * (count - i) / (count + 1);
      edge(f->second, o, "layer", "");
      edge(o, at, "into", "");
    }
  }

  // Leaves: nothing made from them -- any version's recipe counts, as
  // remove_asset reads them -- on no layer, not the project's.
  for (const auto& a : all) {
    Node& n = nodes[of[a.id]];
    bool used = n.feeds || (home && *home == a.id);
    if (!used) {
      auto deps = p->dependents(a.id, false);
      used = !deps.ok() || !deps->empty();
    }
    n.json["leaf"] = !used;
  }

  // Ranks, assets only, in Kahn's order: an asset after everything it is
  // made from. What a cycle leaves (none should be) is ranked by what of
  // it was reached.
  const int count = static_cast<int>(nodes.size());
  auto preds = [&](int v) {
    std::vector<int> u;
    for (int o : nodes[v].in) {
      for (int x : nodes[o].in) {
        u.push_back(x);
      }
    }
    return u;
  };
  std::vector<int> waiting(static_cast<std::size_t>(count), 0);
  std::vector<int> queue;
  for (int v = 0; v < count; ++v) {
    if (!nodes[v].asset) {
      continue;
    }
    waiting[v] = static_cast<int>(preds(v).size());
    nodes[v].rank = nodes[v].in.empty() ? 0 : 1;
    if (waiting[v] == 0) {
      queue.push_back(v);
    }
  }
  std::vector<bool> ranked(static_cast<std::size_t>(count), false);
  std::vector<int> topo;
  for (std::size_t qi = 0;; ++qi) {
    if (qi == queue.size()) {
      // A cycle's: the first not reached, ranked as it stands.
      int next = -1;
      for (int v = 0; v < count && next < 0; ++v) {
        if (nodes[v].asset && !ranked[v]) {
          next = v;
        }
      }
      if (next < 0) {
        break;
      }
      queue.push_back(next);
    }
    const int v = queue[qi];
    if (ranked[v]) {
      continue;
    }
    ranked[v] = true;
    topo.push_back(v);
    for (int o : nodes[v].out) {
      const int w = nodes[o].owner;
      if (w < 0 || ranked[w]) {
        continue;
      }
      nodes[w].rank = std::max(nodes[w].rank, nodes[v].rank + 1);
      if (--waiting[w] == 0) {
        queue.push_back(w);
      }
    }
  }
  // Made from nothing, used by something: just before the first use.
  for (auto it = topo.rbegin(); it != topo.rend(); ++it) {
    Node& n = nodes[*it];
    if (!n.in.empty() || n.out.empty()) {
      continue;
    }
    int first = std::numeric_limits<int>::max();
    for (int o : n.out) {
      first = std::min(first, nodes[nodes[o].owner].rank);
    }
    n.rank = std::max(0, first - 1);
  }
  int columns = 1;
  for (auto& n : nodes) {
    n.column = n.asset ? 2 * n.rank : 2 * nodes[n.owner].rank - 1;
    columns = std::max(columns, n.column + 1);
  }

  // The parts: nodes joined by edges (union-find), the loose apart.
  std::vector<int> root(static_cast<std::size_t>(count));
  std::iota(root.begin(), root.end(), 0);
  auto find = [&](int v) {
    while (root[v] != v) {
      root[v] = root[root[v]];
      v = root[v];
    }
    return v;
  };
  for (const auto& e : edges) {
    root[find(e.from)] = find(e.to);
  }
  std::map<int, std::vector<int>> parts;
  std::vector<int> loose;
  for (int v = 0; v < count; ++v) {
    if (nodes[v].in.empty() && nodes[v].out.empty()) {
      loose.push_back(v);
    } else {
      parts[find(v)].push_back(v);
    }
  }
  // The project's part first, then by their oldest asset.
  std::vector<std::vector<int>> laid;
  for (auto& [r, members] : parts) {
    (void)r;
    laid.push_back(std::move(members));
  }
  auto first_age = [&](const std::vector<int>& m) {
    double o = std::numeric_limits<double>::max();
    for (int v : m) {
      o = std::min(o, nodes[v].order);
    }
    return o;
  };
  auto holds_home = [&](const std::vector<int>& m) {
    return std::any_of(m.begin(), m.end(), [&](int v) {
      return nodes[v].json.contains("project");
    });
  };
  std::stable_sort(laid.begin(), laid.end(),
                   [&](const auto& a, const auto& b) {
                     const bool ha = holds_home(a);
                     const bool hb = holds_home(b);
                     if (ha != hb) {
                       return ha;
                     }
                     return first_age(a) < first_age(b);
                   });

  std::vector<double> want(static_cast<std::size_t>(count), 0);
  double top = 0;
  for (auto& members : laid) {
    std::map<int, std::vector<int>> cols;
    for (int v : members) {
      cols[nodes[v].column].push_back(v);
    }
    // The first rows: by creation, stacked.
    for (auto& [c, col] : cols) {
      (void)c;
      std::stable_sort(col.begin(), col.end(), [&](int a, int b) {
        return nodes[a].order < nodes[b].order;
      });
      double y = 0;
      for (int v : col) {
        nodes[v].y = y;
        want[v] = y;
        y += nodes[v].height + kGap;
      }
    }
    // Each node toward the middle of what it is joined to on one side,
    // then the other.
    auto toward = [&](int v, const std::vector<int>& by) {
      if (by.empty()) {
        return nodes[v].y;
      }
      double s = 0;
      for (int u : by) {
        s += centre(nodes[u]);
      }
      return s / static_cast<double>(by.size()) - nodes[v].height / 2;
    };
    auto sweep = [&](bool rightward) {
      auto run = [&](std::vector<int>& col) {
        for (int v : col) {
          want[v] = toward(v, rightward ? nodes[v].in : nodes[v].out);
        }
        place(col, nodes, want);
      };
      if (rightward) {
        for (auto& [c, col] : cols) {
          (void)c;
          run(col);
        }
      } else {
        for (auto it = cols.rbegin(); it != cols.rend(); ++it) {
          run(it->second);
        }
      }
    };
    for (int s = 0; s < kSweeps; ++s) {
      sweep(true);
      sweep(false);
    }
    sweep(true);
    double lo = std::numeric_limits<double>::max();
    double hi = std::numeric_limits<double>::lowest();
    for (int v : members) {
      lo = std::min(lo, nodes[v].y);
      hi = std::max(hi, nodes[v].y + nodes[v].height);
    }
    for (int v : members) {
      nodes[v].y += top - lo;
      nodes[v].part = static_cast<int>(&members - laid.data());
    }
    top += hi - lo + kPartGap;
  }
  // The loose: by creation, a grid.
  std::stable_sort(loose.begin(), loose.end(), [&](int a, int b) {
    return nodes[a].order < nodes[b].order;
  });
  for (std::size_t i = 0; i < loose.size(); ++i) {
    Node& n = nodes[loose[i]];
    n.column = 2 * static_cast<int>(i % kLoosePerRow);
    n.y = top + static_cast<double>(i / kLoosePerRow) *
                    (kAssetSlots + kGap);
    n.json["loose"] = true;
    columns = std::max(columns, n.column + 1);
  }
  const double height =
      loose.empty()
          ? std::max(0.0, top - kPartGap)
          : top + static_cast<double>((loose.size() + kLoosePerRow - 1) /
                                      kLoosePerRow) *
                      (kAssetSlots + kGap) -
                kGap;

  Json out_nodes = Json::array();
  for (auto& n : nodes) {
    n.json["column"] = n.column;
    n.json["row"] = n.y;
    n.json["height"] = n.height;
    if (n.part >= 0) {
      n.json["part"] = n.part;
    }
    out_nodes.push_back(std::move(n.json));
  }
  Json out_edges = Json::array();
  for (const auto& e : edges) {
    Json j = {{"from", out_nodes[e.from]["id"]},
              {"to", out_nodes[e.to]["id"]}, {"kind", e.kind}};
    if (!e.role.empty()) {
      j["role"] = e.role;
    }
    out_edges.push_back(std::move(j));
  }
  return Json{{"nodes", std::move(out_nodes)},
              {"edges", std::move(out_edges)},
              {"columns", columns},
              {"height", height}};
}

}  // namespace valtz
