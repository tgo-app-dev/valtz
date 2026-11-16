// A clip SPLIT with the Mac this one is paired with over Thunderbolt
// (DESIGN §11c): the fleet's members' Thunderbolt networks probed for the
// cables between them, the pair's outlook, and a clip whose DiT may be
// split run as two ranks -- this Mac's here, the member's there as a
// fleet job -- their partial sums over the cable (vpipe's
// tensor_parallel, the plugin's valtz-tp).

#include "valtz/controller/controller.h"

#include "valtz/base/hash.h"
#include "valtz/base/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <numeric>
#include <set>

namespace valtz {

namespace fs = std::filesystem;

namespace {

double
steady_now()
{
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool
in_list(const Json& list, std::string_view what)
{
  return std::ranges::any_of(
      list.is_array() ? list : Json::array(), [&](const Json& x) {
        return x.is_string() && x.get_ref<const std::string&>() == what;
      });
}

// How long each rank waits for the other's link (and each exchange):
// both load at once -- this Mac's rank starts as the member's does -- so
// minutes, not the half hour of a split started by hand.
constexpr int kSplitWaitMs = 5 * 60 * 1000;

// How old a cable probe a split takes as it is.
constexpr double kFreshProbeSeconds = 10.0;
// A split rank's least band over RDMA (vpipe's TpShare min_band_rows).
constexpr int kSplitMinBandRows = 2304;

// Whether the member decodes a split clip: asked so, or -- "auto" -- its
// GPU the faster (this Mac's share of the heads under half).
bool
decodes_there(const std::string& how, double frac)
{
  return how == "there" || (how == "auto" && frac < 0.5);
}

}  // namespace

// ---- Thunderbolt ------------------------------------------------------

fleet::TbState
Controller::tb_state_() const
{
  std::lock_guard lk(_tb_mu);
  const double now = steady_now();
  if (_tb_state_at < 0 || now - _tb_state_at > 5.0) {
    _tb_state = fleet::thunderbolt_state();
    _tb_state_at = now;
  }
  fleet::TbState s = _tb_state;
  s.fleet_port = _tb_fleet_port;
  return s;
}

void
Controller::tb_refresh_()
{
  if (!_fleet || _shutting_down.load()) {
    return;
  }
  std::lock_guard lk(_tb_mu);
  if (_tb_probing) {
    return;
  }
  if (_tb_thread.joinable()) {
    _tb_thread.join();
  }
  _tb_probing = true;
  _tb_thread = std::thread([this] { tb_probe_(); });
}

void
Controller::tb_probe_()
{
  // The fleet's port, for a link proved by a connection where no echo
  // comes back (asked here, off the fleet's queue).
  const int port = jget(_fleet->status(), "port", 0);
  fleet::TbState before;
  {
    std::lock_guard lk(_tb_mu);
    before = _tb_state;
    _tb_fleet_port = port;
    _tb_state_at = -1;  // read again now
  }
  const fleet::TbState mine = tb_state_();
  std::map<std::string, TbMember> found;
  for (const auto& m : _fleet->members()) {
    const Json self = jget(m, "self", Json::object());
    if (!self.contains("thunderbolt")) {
      continue;
    }
    TbMember tm;
    tm.theirs = fleet::tb_state_from_json(self["thunderbolt"]);
    if (mine.linked() && tm.theirs.linked()) {
      tm.links = fleet::probe_thunderbolt(mine, tm.theirs);
    }
    found[jget<std::string>(m, "id", "")] = std::move(tm);
  }
  bool changed = false;
  {
    std::lock_guard lk(_tb_mu);
    auto key = [](const std::map<std::string, TbMember>& ms) {
      Json j = Json::object();
      for (const auto& [id, tm] : ms) {
        Json ls = Json::array();
        for (const auto& l : tm.links) {
          Json x = fleet::to_json(l);
          x.erase("rtt_ms");
          ls.push_back(std::move(x));
        }
        j[id] = std::move(ls);
      }
      return j;
    };
    changed = key(found) != key(_tb_members);
    _tb_members = std::move(found);
    _tb_probed_at = steady_now();
    _tb_probing = false;
  }
  // This Mac's own network changed (a cable in or out): the members told.
  fleet::TbState now = mine;
  now.fleet_port = before.fleet_port = 0;
  if (!(now == before)) {
    _fleet->self_changed();
  }
  if (changed) {
    post_("fleet.changed", JobId{});
  }
}

std::map<std::string, std::vector<fleet::TbLink>>
Controller::tb_links_(bool now, double fresh)
{
  if (!_fleet) {
    return {};
  }
  bool stale = false;
  {
    std::lock_guard lk(_tb_mu);
    stale = _tb_probed_at < 0 || steady_now() - _tb_probed_at > 15.0;
    // Asked for one this fresh, and none is: nothing.
    if (!now && fresh > 0 &&
        (_tb_probed_at < 0 || steady_now() - _tb_probed_at > fresh)) {
      return {};
    }
  }
  if (now) {
    // Probed here, after any probe under way.
    {
      std::unique_lock lk(_tb_mu);
      if (_tb_thread.joinable()) {
        std::thread t = std::move(_tb_thread);
        lk.unlock();
        t.join();
        lk.lock();
      }
      _tb_probing = true;
    }
    tb_probe_();
  } else if (stale) {
    tb_refresh_();
  }
  std::map<std::string, std::vector<fleet::TbLink>> out;
  std::lock_guard lk(_tb_mu);
  for (const auto& [id, tm] : _tb_members) {
    out[id] = tm.links;
  }
  return out;
}

// ---- the pair -----------------------------------------------------------

Json
Controller::pair_outlook_()
{
  if (!_fleet) {
    return Json();
  }
  const fleet::Config c = _fleet->config();
  if (!c.joined() || c.pair_member.empty()) {
    return Json();
  }
  const std::string name = c.pair_name.empty() ? c.pair_member : c.pair_name;
  Json out = {{"member", c.pair_member}, {"name", name},
              {"connected", false}, {"links", 0}, {"rdma", false},
              {"ready", false}};
  auto why = [&](const Message& m, MessageArgs args = {}) {
    args.emplace_back("member", name);
    out["why"] = message_fields(m, args);
    return out;
  };
  if (!_engine || !_engine->splits_dit()) {
    return why(msg::kSplitCannotHere);
  }
  Json self;
  for (const auto& m : _fleet->members()) {
    if (jget<std::string>(m, "id", "") == c.pair_member) {
      self = jget(m, "self", Json::object());
    }
  }
  if (!self.is_object()) {
    return why(msg::kSplitPairAway);
  }
  out["connected"] = true;
  const auto links = tb_links_(false)[c.pair_member];
  out["links"] = static_cast<int>(links.size());
  out["rdma"] = !links.empty() && c.pair_rdma &&
                std::ranges::any_of(links, &fleet::TbLink::rdma);
  Json ls = Json::array();
  for (const auto& l : links) {
    ls.push_back(fleet::to_json(l));
  }
  out["link_list"] = std::move(ls);
  // The share each Mac takes of H3's heads (the split models' widths).
  const double frac =
      c.pair_share > 0
          ? c.pair_share
          : fleet::gpu_share(_hw.gpu_cores, _hw.gpu_matrix_cores,
                         jget(self, "machine", Json::object()));
  // (The split models' widths: MiniMax H3's.)
  int heads = 56;
  for (const auto& e : _catalog.models()) {
    const int h = jget(jget(jget(e.engine, "vpipe", Json::object()),
                            "tensor_parallel", Json::object()),
                       "heads", 0);
    if (h > 1) {
      heads = h;
      break;
    }
  }
  const int here = std::clamp(static_cast<int>(std::lround(heads * frac)), 1,
                              heads - 1);
  out["heads"] = {here, heads - here};
  out["share"] = frac;
  out["auto_share"] = c.pair_share <= 0;
  out["decode"] = c.pair_decode == "auto" && _engine->splits_decode() &&
                          jget(self, "splits_decode", false)
                      ? "on both"
                  : decodes_there(c.pair_decode, frac) ? "there"
                                                       : "here";
  if (!jget(self, "splits", false)) {
    return why(msg::kSplitCannotThere);
  }
  if (links.empty()) {
    return why(msg::kSplitNoLink);
  }
  out["ready"] = true;
  return out;
}

Json
Controller::pair_traffic_()
{
  if (!_fleet) {
    return Json();
  }
  const fleet::Config c = _fleet->config();
  if (!c.joined() || c.pair_member.empty()) {
    return Json();
  }
  // The cables as last probed: never a probe here (the bar polls every
  // second), one started only where none has been made.
  std::vector<fleet::TbLink> links;
  bool probed = false;
  {
    std::lock_guard lk(_tb_mu);
    probed = _tb_probed_at >= 0;
    if (auto it = _tb_members.find(c.pair_member); it != _tb_members.end()) {
      links = it->second.links;
    }
  }
  if (!probed) {
    tb_refresh_();
  }
  Json out = {{"member", c.pair_member},
              {"name", c.pair_name.empty() ? c.pair_member : c.pair_name},
              {"links", static_cast<int>(links.size())},
              {"rdma", c.pair_rdma &&
                           std::ranges::any_of(links, &fleet::TbLink::rdma)}};
  double gbps = 0;
  for (const auto& l : links) {
    gbps += l.gbps;
  }
  if (gbps > 0) {
    out["capacity_bytes_per_s"] = gbps * 1e9 / 8;
  }
  const Json t = _engine ? _engine->split_traffic() : Json::object();
  const std::uint64_t sent = jget<std::uint64_t>(t, "sent_bytes", 0);
  const std::uint64_t got = jget<std::uint64_t>(t, "received_bytes", 0);
  {
    // A rate over a second at least: two windows polling together read
    // the last one's rather than a sliver's.
    std::lock_guard lk(_traffic_mu);
    const double now = steady_now();
    if (_traffic_at < 0) {
      _traffic_at = now;
      _traffic_sent = sent;
      _traffic_received = got;
    } else if (now - _traffic_at >= 0.9) {
      const double dt = now - _traffic_at;
      _traffic_sent_rate = sent >= _traffic_sent
                               ? (sent - _traffic_sent) / dt : 0;
      _traffic_received_rate = got >= _traffic_received
                                   ? (got - _traffic_received) / dt : 0;
      _traffic_at = now;
      _traffic_sent = sent;
      _traffic_received = got;
    }
    out["sent_bytes_per_s"] = std::round(_traffic_sent_rate);
    out["received_bytes_per_s"] = std::round(_traffic_received_rate);
  }
  out["active"] = jget<std::uint64_t>(t, "channels", 0) > 0;
  return out;
}

Result<Controller::SplitPlan>
Controller::split_plan_(const project::Recipe& r, const engine::ModelRef& ref,
                        const Json& carried, JobId r_job)
{
  auto quiet = [] {
    return make_error(Code::NotFound, "not split");
  };
  if (!_fleet) {
    return quiet();
  }
  const auto* m = _catalog.find(r.model);
  const Json mvp = m ? jget(m->engine, "vpipe", Json::object()) : Json();
  // A clip, or a PICTURE of a DiT that splits (catalog
  // `tensor_parallel`): H3's (MiniMax-H3-Image's own, P-H3-Image's
  // FL2VA: one latent frame of the same forward) and FLUX.3's. A picture
  // is always decoded HERE -- one frame, a fraction of a second -- the
  // member's rank denoising alone.
  const bool picture = (r.op == engine::kOpGenerateImage ||
                        r.op == engine::kOpEditImage) &&
                       mvp.contains("tensor_parallel");
  if (r.op != engine::kOpGenerateVideo && !picture) {
    return quiet();
  }
  if (const char* e = std::getenv("VALTZ_SPLIT");
      e != nullptr && std::string_view(e) == "0") {
    return quiet();
  }
  const fleet::Config c = _fleet->config();
  if (!c.joined() || c.pair_member.empty()) {
    return quiet();
  }
  const Json widths = jget(mvp, "tensor_parallel", Json());
  const int heads = jget(widths, "heads", 0);
  const int ffn = jget(widths, "ffn", 0);
  // Not a model that splits; or split as asked already (valtzctl --tp).
  if (heads < 2 || ffn < 2 || r.params.contains("tensor_parallel")) {
    return quiet();
  }
  const std::string name = c.pair_name.empty() ? c.pair_member : c.pair_name;
  auto no = [&](const Message& msg, MessageArgs args = {}) {
    args.emplace_back("member", name);
    return make_error(Code::Unsupported, msg, std::move(args));
  };
  if (!_engine || !_engine->splits_dit()) {
    return no(msg::kSplitCannotHere);
  }
  // A family split by a vpipe feature of its own (`needs`: "flux3",
  // "qwen-image-21"), here and -- below -- there.
  const std::string family_needs = jget<std::string>(widths, "needs", "");
  const std::string model_name = m ? m->name : r.model;
  const std::vector<std::string> families = _engine->split_families();
  if (!family_needs.empty() &&
      std::ranges::find(families, family_needs) == families.end()) {
    return no(msg::kSplitModelHere, {{"model", model_name}});
  }
  // A family whose rank binds no adapter (FLUX.3: vpipe refuses a
  // runtime LoRA on a rank): any LoRA, the catalog's or one's own.
  if (!jget(widths, "adapters", true)) {
    for (int k = 1; k <= engine::kLoraSlots; ++k) {
      const std::string id =
          k == 1 ? std::string("lora") : std::format("lora{}", k);
      const std::string file =
          k == 1 ? std::string("lora_file") : std::format("lora{}_file", k);
      if (!jget<std::string>(r.params, id.c_str(), "").empty() ||
          !jget<std::string>(r.params, file.c_str(), "").empty()) {
        return no(msg::kSplitNoAdapters, {{"model", model_name}});
      }
    }
  }
  // What a rank cannot run yet (vpipe refuses each by name): a DiT, VAE
  // or text encoder of Custom's own -- a community DiT is int8 ConvRot or
  // FP8, and a rank cuts only dense and affine weights.
  for (const char* k : {"dit_file", "vae_file", "encoder_weights",
                        "vae_weights"}) {
    if (!jget<std::string>(r.params, k, "").empty()) {
      return no(msg::kSplitOwnFiles);
    }
  }
  // A LoRA of this Mac's own goes with the member's half -- pushed to its
  // share first, as any job's own weights (DESIGN §11d) -- where Settings
  // › Fleet sends them; the catalog's (the Turbo LoRA, HyperFlow, Pika H3
  // Flex) are the member's own, by their ids.
  bool own_loras = false;
  for (int k = 1; k <= engine::kLoraSlots; ++k) {
    const std::string key =
        k == 1 ? std::string("lora_file") : std::format("lora{}_file", k);
    own_loras = own_loras || !jget<std::string>(r.params, key.c_str(), "")
                                  .empty();
  }
  if (own_loras && (!_share || !c.send_weights)) {
    return no(msg::kSplitOwnLoras);
  }
  // The Neural Engine's share -- the draft's, or the 2K refine's (its own
  // tuning) -- on both ranks, each its share's modules: where both Macs'
  // engines say so ("h3-ane", vpipe's tensor-parallel-ane/1; the member's
  // checked below). VDN and TaoMate: not on a rank.
  const Json t = jget(r.params, "tuning", Json::object());
  const Json rt = jget(r.params, "regenerate_tuning", Json::object());
  const bool regen = !jget<std::string>(r.params, "regenerate", "").empty();
  const bool ane =
      jget(t, "ane_ffn", false) || jget(t, "ane_qkv", false) ||
      (regen && (jget(rt, "ane_ffn", false) || jget(rt, "ane_qkv", false)));
  // MiniMax H3's alone (a family split by a feature of its own --
  // FLUX.3, Qwen-Image -- runs its rank without the Neural Engine).
  if (ane && !family_needs.empty()) {
    return no(msg::kSplitNeuralEngineModel, {{"model", model_name}});
  }
  if (ane && std::ranges::find(families, "h3-ane") == families.end()) {
    return no(msg::kSplitNeuralEngine);
  }
  if (jget(t, "vdn", false) || jget(t, "taomate", false) ||
      !ref.branch.empty() || (regen && jget(rt, "vdn", false))) {
    return no(msg::kSplitVdnTaomate);
  }
  if (jget<std::string>(carried, "latent", "") == "kept" ||
      carried.contains("cleaned") || r.params.contains("clean_guide")) {
    return no(msg::kSplitContinued);
  }
  if (busy_here_(r_job)) {
    return no(msg::kSplitBusyHere);
  }
  // The member: there, free, able, with the models.
  Json self;
  for (const auto& mm : _fleet->members()) {
    if (jget<std::string>(mm, "id", "") == c.pair_member) {
      self = jget(mm, "self", Json::object());
    }
  }
  if (!self.is_object()) {
    return no(msg::kSplitPairAway);
  }
  if (!jget(self, "accepting", false)) {
    return no(msg::kFleetNotTaking);
  }
  if (jget(self, "busy", false) || jget(self, "serving", false)) {
    return no(msg::kFleetBusy);
  }
  if (!jget(self, "splits", false)) {
    return no(msg::kSplitCannotThere);
  }
  if (!family_needs.empty() &&
      !in_list(jget(self, "split_families", Json()), family_needs)) {
    return no(msg::kSplitModelThere, {{"model", model_name}});
  }
  if (ane && !in_list(jget(self, "split_families", Json()), "h3-ane")) {
    return no(msg::kSplitNeuralEngine);
  }
  // Each Mac its share of the heads, by its GPU (or as set), the
  // feed-forward rows alike.
  const double frac =
      c.pair_share > 0
          ? c.pair_share
          : fleet::gpu_share(_hw.gpu_cores, _hw.gpu_matrix_cores,
                         jget(self, "machine", Json::object()));
  // Who makes the clip: the member, where it decodes (Settings › Fleet's
  // pair: the faster GPU, or as set) -- not a clip that keeps its latents
  // (they are this Mac's project's).
  // Or BOTH decode (Settings' Automatic, both Macs able; not a clip
  // regenerated past 768P, whose refine decodes): each its share of the
  // chunks, the member's part handed over the link -- this Mac makes the
  // clip, nothing sent back over the fleet's own network.
  const bool split_decode = !picture && c.pair_decode == "auto" && !regen &&
                            _engine->splits_decode() &&
                            jget(self, "splits_decode", false);
  const bool peer_keeps = !picture && !split_decode &&
                          decodes_there(c.pair_decode, frac) &&
                          !jget(r.params, "keep_latents", false);
  std::vector<std::string> needs = {r.model};
  for (const char* k : {"lora", "decoder", "encoder", "vad", "tagger",
                        "dit_model"}) {
    if (const auto id = jget<std::string>(r.params, k, ""); !id.empty()) {
      needs.push_back(id);
    }
  }
  // A clip past 768P: the refiner, its latent upscaler, the model it
  // refines on, the adapters it refines under (in context) -- and its
  // fast decoder where the member decodes.
  if (regen) {
    const auto rg = jget<std::string>(r.params, "regenerate", "");
    needs.push_back(rg);
    if (const auto* re = _catalog.find(rg)) {
      const Json rs = jget(jget(re->engine, "vpipe", Json::object()),
                           "regenerate", Json::object());
      for (const char* k : {"upscaler", "base"}) {
        if (const auto id = jget<std::string>(rs, k, ""); !id.empty()) {
          needs.push_back(id);
        }
      }
      for (const auto& a : jget(rs, "adapters", Json::array())) {
        if (a.is_string()) {
          needs.push_back(a.get<std::string>());
        }
      }
      if (peer_keeps && jget(rt, "fast_decoder", false)) {
        if (const auto fd = jget<std::string>(rs, "fast_decoder", "");
            !fd.empty()) {
          needs.push_back(fd);
        }
      }
    }
  }
  // Its own LoRAs taken with it: a member that takes them, with the room.
  if (own_loras) {
    std::uint64_t bytes = 0;
    for (int k = 1; k <= engine::kLoraSlots; ++k) {
      const std::string key =
          k == 1 ? std::string("lora_file") : std::format("lora{}_file", k);
      if (const auto f = jget<std::string>(r.params, key.c_str(), "");
          !f.empty()) {
        std::error_code ec;
        bytes += fs::file_size(f, ec);
      }
    }
    if (!jget(self, "takes_weights", false) ||
        jget<std::uint64_t>(self, "free_bytes", 0) <
            bytes + bytes / 10 + (2ull << 30)) {
      return no(msg::kSplitOwnLoras);
    }
  }
  for (const auto& id : needs) {
    if (!in_list(jget(self, "installed", Json()), id)) {
      const auto* e = _catalog.find(id);
      return no(msg::kSplitNoModelThere, {{"model", e ? e->name : id}});
    }
  }
  // The cables as probed in the last few seconds (a member joining, the
  // app's outlook), else probed now: a probe waits out every address
  // pair no cable joins, most of a second a clip.
  auto links = tb_links_(false, kFreshProbeSeconds)[c.pair_member];
  if (links.empty()) {
    links = tb_links_(true)[c.pair_member];
  }
  if (links.empty()) {
    return no(msg::kSplitNoLink);
  }
  // Whole groups of four heads where an int8 tier past Accurate runs (on
  // the draft or the refine): the out projection's share is then 512
  // columns a group, which every tier's rotation takes -- 11 heads' 1,408
  // would keep it on Accurate (vpipe's tp_fast_).
  const auto tiered = [](const Json& tj) {
    return jget(tj, "i8_gemm", false) &&
           jget<std::string>(tj, "i8_gemm_tier", "accurate") != "accurate";
  };
  // A family's own grain beside it: Krea 2's heads share a key and value
  // head four at a time (GQA: `head_group`), so a share is whole groups.
  const int tier_grain = (tiered(t) || (regen && tiered(rt))) &&
                                 heads >= 8 && heads % 4 == 0
                             ? 4
                             : 1;
  const int head_group = std::max(1, jget(widths, "head_group", 1));
  // The feed-forward's rows: the heads' fraction of them, in whole groups
  // (`ffn_grain`) where the rows are not a whole number a head -- an
  // affine pack's columns of fc2 are cut in whole groups of 64, and an
  // int8 tier's rotation takes a share in whole groups of 256.
  const auto [h0, f0] = fleet::split_rows(
      heads, ffn, frac, std::lcm(tier_grain, head_group),
      jget(widths, "ffn_grain", 1));
  // The member LISTENS on its end of each cable -- a port of its own,
  // told back as its half begins -- and this Mac connects from its end:
  // reaching out to the local network is the requester's (the app the
  // person works in, let do so for the fleet), never a serving member's,
  // which macOS may not have let (Local Network: "No route to host").
  // A socket a ROUTE -- two cables in two Thunderbolt Bridges are one --
  // and RDMA a CABLE, each weighed by its speed: a TB5 cable beside a
  // 20 Gb/s one carries four fifths of every partial (an even share
  // would wait on the slower one: half at a quarter of the speed).
  Json there = Json::array(), ifs = Json::array();
  Json rdma_here = Json::array(), rdma_there = Json::array();
  Json weights = Json::array();
  std::set<std::pair<std::string, std::string>> routes;
  bool weighed = true;
  for (const auto& l : links) {
    if (routes.insert({l.local_interface, l.remote_address}).second) {
      there.push_back(std::format("{}:0", l.remote_address));
      ifs.push_back(l.local_interface);
    }
    if (l.rdma()) {
      rdma_here.push_back(l.local_rdma);
      rdma_there.push_back(l.remote_rdma);
      weights.push_back(l.gbps);
      weighed = weighed && l.gbps > 0;
    }
  }
  SplitPlan plan;
  plan.own_weights = own_loras;
  plan.peer_keeps = peer_keeps;
  plan.split_decode = split_decode;
  plan.member = c.pair_member;
  plan.name = name;
  plan.links = static_cast<int>(links.size());
  plan.rdma = c.pair_rdma && !rdma_here.empty();
  plan.local = {{"rank", 0},
                {"ranks", 2},
                {"heads", {0, h0}},
                {"ffn", {0, f0}},
                {"params",
                 {{"role", "connect"}, {"links", Json::array()},
                  {"interfaces", ifs}, {"wait_ms", kSplitWaitMs}}}};
  plan.peer = {{"rank", 1},
               {"ranks", 2},
               {"heads", {h0, heads}},
               {"ffn", {f0, ffn}},
               {"params",
                {{"role", "listen"}, {"links", there},
                 {"wait_ms", kSplitWaitMs}}}};
  // The decode's share: the heads' (each Mac's GPU's), as chunks.
  if (split_decode) {
    const double d = static_cast<double>(h0) / heads;
    plan.local["decode_share"] = {0.0, d};
    plan.peer["decode_share"] = {d, 1.0};
  }
  // A MiniMax H3 rank (a clip's, an H3 picture's: no family of its own)
  // bands a prompt longer than its band where both Macs' vpipe reads
  // `prompt_bands` -- the ranks must cut the same bands. Unsaid, such a
  // prompt runs every block whole on both, as before.
  if (family_needs.empty() &&
      std::ranges::find(families, "h3-long-prompts") != families.end() &&
      in_list(jget(self, "split_families", Json()), "h3-long-prompts")) {
    plan.local["prompt_bands"] = true;
    plan.peer["prompt_bands"] = true;
  }
  // A PICTURE's rank bands where both Macs' vpipe reads the floor for its
  // family ("<family>-bands": FLUX.3's and Qwen-Image's /2, the others'
  // /1) -- over RDMA or TCP: without it each block crosses whole, its two
  // exchanges exposed. The same floor as a clip's over RDMA; a block of
  // fewer rows than two floors (1024²'s 4,096) stays whole.
  if (picture && !family_needs.empty()) {
    const std::string bands = family_needs + "-bands";
    if (std::ranges::find(families, bands) != families.end() &&
        in_list(jget(self, "split_families", Json()), bands)) {
      plan.local["min_band_rows"] = kSplitMinBandRows;
      plan.peer["min_band_rows"] = kSplitMinBandRows;
    }
  }
  if (plan.rdma) {
    plan.local["params"]["rdma"] = rdma_here;
    plan.peer["params"]["rdma"] = rdma_there;
    // Over RDMA a band's partial crosses in well under its compute:
    // fewer, taller bands, each GEMM and attention the fuller for it --
    // 2,304 rows at least (an 832 x 480 x 73 clip's four, against eight
    // of 1,152: its denoise 8.5 s against 10.4). Both ranks must cut the
    // same bands: only to a member that reads it (the vpipe that splits
    // the decode); larger clips keep their eighths, which past ~20,000
    // rows are taller anyway (taller still, the device refuses to
    // register the exchange's buffers).
    if (split_decode) {
      plan.local["min_band_rows"] = kSplitMinBandRows;
      plan.peer["min_band_rows"] = kSplitMinBandRows;
    }
    if (weighed && weights.size() > 1) {
      plan.local["params"]["rdma_weights"] = weights;
      plan.peer["params"]["rdma_weights"] = weights;
    }
  }
  return plan;
}

// ---- a split clip's two ranks -----------------------------------------

Status
Controller::run_split_(SplitPlan plan, engine::JobSpec spec,
                       const project::Recipe& r, const std::string& title,
                       engine::JobSink sink)
{
  auto s = std::make_shared<Split>();
  s->id = spec.id;
  s->member = plan.member;
  s->name = plan.name;
  s->title = title;
  // The member's rank: the same request, its share, its inputs by hash.
  fleet::Offer offer{spec.id, r.op, r.model, title, r.params, spec.inputs};
  for (auto& in : offer.inputs) {
    if (in.hash.is_zero()) {
      VALTZ_ASSIGN(in.hash, hash_file(in.path));
    }
  }
  offer.params["tensor_parallel"] = plan.peer;
  // Its half decodes nothing: the 2K refine's fast decoder is not asked
  // of it (nor need it have one). Or it MAKES THE CLIP: decoded, written
  // and sent back there, this Mac's rank denoising alone.
  if (plan.peer_keeps) {
    offer.params["split_keeps"] = true;
    s->peer_keeps = true;
  } else if (offer.params.contains("regenerate_tuning")) {
    offer.params["regenerate_tuning"]["fast_decoder"] = false;
  }
  // This Mac's own LoRAs go with it: served for the job by the model
  // share, copied there first, named by the share (DESIGN §11d).
  if (plan.own_weights) {
    std::uint64_t bytes = 0;
    VALTZ_ASSIGN(Json from, own_weights_(r.params, spec.id, &bytes));
    for (int k = 1; k <= engine::kLoraSlots; ++k) {
      offer.params.erase(k == 1 ? std::string("lora_file")
                                : std::format("lora{}_file", k));
    }
    offer.params["weights_from"] = std::move(from);
    sink = [this, job = spec.id, inner = std::move(sink)](
               const engine::JobEvent& ev) {
      if (ev.terminal()) {
        unshare_job_(job);
      }
      inner(ev);
    };
  }
  s->sink = sink;
  const fs::path out = spec.output_dir;
  s->whole = spec;
  Json tp = plan.local;
  tp["params"]["key"] = fleet::split_key(_fleet->config().key, s->id.str());
  spec.params["tensor_parallel"] = std::move(tp);
  if (plan.peer_keeps) {
    spec.params["denoise_only"] = true;
  }
  s->local = std::move(spec);
  {
    std::lock_guard lk(_splits_mu);
    _splits[s->id] = s;
  }
  _jobs.update(s->id, [&](JobRecord& j) { j.split_with = plan.name; });
  post_("job.runner", s->id, {{"runner", "local"},
                              {"split", plan.name},
                              {"links", plan.links},
                              {"rdma", plan.rdma}});
  VALTZ_LOG_INFO("fleet", "{}: split with {} -- {} link(s), {}{}; heads "
                 "[0, {}) here; decoded {}", title, plan.name, plan.links,
                 plan.rdma ? "RDMA" : "TCP",
                 plan.local.contains("min_band_rows")
                     ? std::format(", bands of {}+ rows",
                                   plan.local["min_band_rows"].get<int>())
                     : std::string(),
                 jget(plan.local, "heads", Json::array()).size() == 2
                     ? plan.local["heads"][1].get<int>()
                     : 0,
                 plan.split_decode ? "on both"
                 : plan.peer_keeps ? "there"
                                   : "here");
  // The member's half offered -- else, not sent, the clip made whole
  // here, why said.
  const auto send = [this, s, member = plan.member,
                     out](const fleet::Offer& o) {
    return _fleet->submit(member, o, out,
                          [this, s](const engine::JobEvent& ev) {
                            split_peer_event_(s, ev);
                          });
  };
  const auto unsplit = [this, s](const Status& why) -> Status {
    std::optional<engine::JobSpec> whole;
    {
      std::lock_guard lk(s->mu);
      if (s->ended || !s->whole) {
        return ok_status();
      }
      s->ended = true;
      whole = std::move(s->whole);
      s->whole.reset();
      s->local.reset();
    }
    {
      std::lock_guard lk(_splits_mu);
      _splits.erase(s->id);
    }
    _jobs.update(s->id, [](JobRecord& j) { j.split_with.clear(); });
    post_("job.runner", s->id, {{"runner", "local"},
                                {"unsplit", message_fields(why.error())}});
    return _engine->submit(std::move(*whole), s->sink);
  };
  // Its own LoRAs PUSHED to the member's share first (a member serving
  // may not connect out: DESIGN §11d), the half offered once they are
  // there; stopped meanwhile, nothing offered.
  if (plan.own_weights) {
    Json member;
    for (const auto& m : _fleet->members()) {
      if (jget<std::string>(m, "id", "") == plan.member) {
        member = m;
      }
    }
    auto after = [s, send, unsplit, offer,
                  id = plan.member](Status st) mutable {
      {
        std::lock_guard lk(s->mu);
        if (s->ended || s->cancelled) {
          return;
        }
      }
      if (st.ok()) {
        offer.params["weights_from"]["pushed_to"] = id;
        st = send(offer);
        if (st.ok()) {
          return;
        }
      }
      VALTZ_LOG_INFO("fleet", "{}: its weights not sent to {}: {}",
                     s->title, s->name, st.error().message);
      if (Status r = unsplit(st); !r.ok()) {
        engine::JobEvent f;
        f.job = s->id;
        f.kind = engine::JobEventKind::Failed;
        f.error = r.code();
        f.text = r.error().message;
        f.data = message_fields(r.error());
        s->sink(f);
      }
    };
    if (Status st = push_weights_(member, offer, after, s->sink); !st.ok()) {
      return unsplit(st);
    }
    return ok_status();
  }
  if (Status st = send(offer); !st.ok()) {
    return unsplit(st);
  }
  return ok_status();
}

void
Controller::split_peer_event_(const std::shared_ptr<Split>& s,
                              const engine::JobEvent& ev)
{
  std::unique_lock lk(s->mu);
  if (s->ended) {
    return;
  }
  switch (ev.kind) {
  case engine::JobEventKind::Started: {
    if (s->started || s->cancelled || !s->local) {
      return;
    }
    // Its rank began (loading its share), listening where it says: ours
    // goes to the engine, connecting there, the two meeting over the
    // cable.
    const Json at = jget(ev.data, "split_links", Json::array());
    if (!at.is_array() || at.empty()) {
      // A member that listens nowhere (a Valtz before it told its
      // links): its half stopped, the clip whole here.
      lk.unlock();
      _fleet->cancel(s->id);
      engine::JobEvent f = ev;
      f.kind = engine::JobEventKind::Failed;
      f.text = "it did not say where it listens";
      split_peer_event_(s, f);
      return;
    }
    s->started = true;
    s->local->params["tensor_parallel"]["params"]["links"] = at;
    engine::JobSpec spec = std::move(*s->local);
    s->local.reset();
    s->whole.reset();
    lk.unlock();
    VALTZ_LOG_INFO("fleet", "{}: {} began its half", s->title, s->name);
    Status st = _engine->submit(std::move(spec),
                                [this, s](const engine::JobEvent& e) {
                                  split_local_event_(s, e);
                                });
    if (!st.ok()) {
      _fleet->cancel(s->id);
      engine::JobEvent f;
      f.job = s->id;
      f.kind = engine::JobEventKind::Failed;
      f.error = st.code();
      f.text = st.error().message;
      f.data = message_fields(st.error());
      split_local_event_(s, f);
    }
    return;
  }
  case engine::JobEventKind::Finished: {
    s->peer_done = true;
    if (!s->peer_keeps || !s->started) {
      return;
    }
    // The member made the clip, its outputs come: the clip's end.
    s->ended = true;
    const bool local_running = !s->local_done;
    lk.unlock();
    {
      std::lock_guard g(_splits_mu);
      _splits.erase(s->id);
    }
    if (local_running) {
      (void)_engine->cancel(s->id);
    }
    s->sink(ev);
    return;
  }
  case engine::JobEventKind::Failed:
  case engine::JobEventKind::Cancelled: {
    s->peer_done = true;
    if (s->started && s->peer_keeps && s->local_done) {
      // Our rank is done; the member failed making the clip: the clip's
      // failure, its reason said.
      s->ended = true;
      const bool cancelled = s->cancelled;
      lk.unlock();
      {
        std::lock_guard g(_splits_mu);
        _splits.erase(s->id);
      }
      engine::JobEvent f = ev;
      if (!cancelled && ev.kind == engine::JobEventKind::Failed) {
        const MessageArgs args = {
            {"member", s->name},
            {"reason", ev.text.empty() ? std::string("failed") : ev.text}};
        f.text = render_message(msg::kSplitPeerFailed.english, args);
        f.data = message_fields(msg::kSplitPeerFailed, args);
      }
      s->sink(f);
      return;
    }
    if (s->started) {
      // Its half failed while ours runs: ours stopped, its reason said.
      if (ev.kind == engine::JobEventKind::Failed) {
        s->peer_error = ev.text.empty() ? std::string("failed") : ev.text;
      } else if (!s->cancelled) {
        s->peer_error = "stopped";
      }
      lk.unlock();
      (void)_engine->cancel(s->id);
      return;
    }
    s->ended = true;
    const bool cancelled = s->cancelled;
    std::optional<engine::JobSpec> whole = std::move(s->whole);
    s->whole.reset();
    s->local.reset();
    lk.unlock();
    {
      std::lock_guard g(_splits_mu);
      _splits.erase(s->id);
    }
    _jobs.update(s->id, [](JobRecord& j) { j.split_with.clear(); });
    if (cancelled || !whole) {
      engine::JobEvent c = ev;
      c.kind = engine::JobEventKind::Cancelled;
      s->sink(c);
      return;
    }
    // Declined, gone or failed before it began: the clip whole, here.
    VALTZ_LOG_INFO("fleet", "{}: {} did not take its half ({}): made "
                   "whole here", s->title, s->name, ev.text);
    post_("job.runner", s->id,
          {{"runner", "local"},
           {"unsplit",
            message_fields(msg::kSplitPeerFailed,
                           {{"member", s->name},
                            {"reason", ev.text.empty()
                                           ? std::string("declined")
                                           : ev.text}})}});
    Status st = _engine->submit(std::move(*whole), s->sink);
    if (!st.ok()) {
      engine::JobEvent f;
      f.job = s->id;
      f.kind = engine::JobEventKind::Failed;
      f.error = st.code();
      f.text = st.error().message;
      f.data = message_fields(st.error());
      s->sink(f);
    }
    return;
  }
  default: {
    // Its progress and text: ours tell the clip's -- but where the member
    // makes the clip, its outputs are the clip's, and once our rank is
    // done its progress (the decode, the writing) is too.
    if (!s->peer_keeps || !s->started) {
      return;
    }
    const bool pass = ev.kind == engine::JobEventKind::Output ||
                      s->local_done;
    lk.unlock();
    if (pass) {
      s->sink(ev);
    }
    return;
  }
  }
}

void
Controller::split_local_event_(const std::shared_ptr<Split>& s,
                               const engine::JobEvent& ev)
{
  if (!ev.terminal()) {
    s->sink(ev);
    return;
  }
  std::unique_lock lk(s->mu);
  if (s->ended) {
    return;
  }
  // The member makes the clip: our rank done is not the clip done -- its
  // outputs and its Finished are (split_peer_event_).
  if (s->peer_keeps && ev.kind == engine::JobEventKind::Finished) {
    s->local_done = true;
    return;
  }
  s->ended = true;
  const bool peer_running = !s->peer_done;
  const std::string peer_error = s->peer_error;
  const bool cancelled = s->cancelled;
  lk.unlock();
  {
    std::lock_guard g(_splits_mu);
    _splits.erase(s->id);
  }
  // The clip is here (or failed): the member's half has nothing left to
  // do but decode a copy of it -- stopped.
  if (peer_running) {
    _fleet->cancel(s->id);
  }
  if (ev.kind != engine::JobEventKind::Finished && !peer_error.empty() &&
      !cancelled) {
    engine::JobEvent f = ev;
    f.kind = engine::JobEventKind::Failed;
    const MessageArgs args = {{"member", s->name}, {"reason", peer_error}};
    f.text = render_message(msg::kSplitPeerFailed.english, args);
    f.data = message_fields(msg::kSplitPeerFailed, args);
    if (f.error == Code::Ok) {
      f.error = Code::Unsupported;
    }
    s->sink(f);
    return;
  }
  s->sink(ev);
}

}
