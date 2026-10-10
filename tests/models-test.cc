#include "testing.h"

#include "valtz/models/capabilities.h"
#include "valtz/models/hardware.h"
#include "valtz/models/tuning.h"

#include <fstream>
#include <set>

using namespace valtz;
using namespace valtz::models;

namespace {

HardwareInfo
box(std::uint32_t gb)
{
  HardwareInfo hw;
  hw.chip = "test";
  hw.ram_bytes = std::uint64_t{gb} << 30;
  return hw;
}

void
fake_install(const std::filesystem::path& root, const ModelEntry& e)
{
  auto dir = root / e.hf_path / e.subdir;
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "model.safetensors") << "x";
}

}

// The Neural Engine by the chip: 16 cores a die through M5, 32 from M6,
// an Ultra two dies.
TEST(models, neural_engine_cores_by_chip)
{
  CHECK(ane_cores_of_chip("Apple M1") == 16);
  CHECK(ane_cores_of_chip("Apple M5 Pro") == 16);
  CHECK(ane_cores_of_chip("Apple M5 Max") == 16);
  CHECK(ane_cores_of_chip("Apple M3 Ultra") == 32);
  CHECK(ane_cores_of_chip("Apple M5 Ultra") == 32);
  CHECK(ane_cores_of_chip("Apple M6") == 32);
  CHECK(ane_cores_of_chip("Apple M6 Max") == 32);
  CHECK(ane_cores_of_chip("Apple M6 Ultra") == 64);
  CHECK(ane_cores_of_chip("Apple M12 Pro") == 32);
  CHECK(ane_cores_of_chip("Intel(R) Core(TM) i9") == 0);
  // What this Mac reports: at least what its chip has.
  const auto hw = probe_hardware();
  CHECK(hw.ane_cores >= ane_cores_of_chip(hw.chip));
}

TEST(models, builtin_catalog_parses_and_links)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  CHECK(c->find("qwen3.5-9b") != nullptr);
  CHECK(c->find("z-image-turbo") != nullptr);
  auto t2i = c->serving(Capability::TextToImage);
  REQUIRE(!t2i.empty());
  for (std::size_t i = 1; i < t2i.size(); ++i) {
    CHECK(t2i[i - 1]->rank >= t2i[i]->rank);
  }
}

// A QUANTIZED VARIANT runs as its source: what it leaves out -- its
// capabilities, engine, prompting, family, the memory it needs -- is
// that one's; a variant of a model the catalog lacks is withheld.
TEST(models, a_quantized_variant_runs_as_its_source)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto* src = c->find("moss-tts-v1.5");
  const auto* q = c->find("moss-tts-v1.5-w8g64");
  REQUIRE(src && q);
  CHECK(q->quantize_from == "moss-tts-v1.5");
  CHECK(q->quantize_bits == 8 && q->quantize_group == 64);
  CHECK(q->has(Capability::TextToSpeech));
  CHECK(q->family == "moss-tts" && q->role == "audio");
  CHECK(q->engine == src->engine && q->prompting == src->prompting);
  CHECK(q->min_ram_gb == src->min_ram_gb);
  CHECK(q->requires_models == src->requires_models);
  CHECK(q->hf_path == "local/MOSS-TTS-v1.5-8bit");
  CHECK(q->rank > src->rank);  // Auto's pick when both are here
  // Speech is a family's feature of its own.
  const auto& fams = c->families();
  const auto f = std::ranges::find(fams, std::string("moss-tts"),
                                   &Family::id);
  REQUIRE(f != fams.end());
  CHECK(std::ranges::find(f->features, std::string("speech-gen")) !=
        f->features.end());
  Json doc = {{"models", Json::array({{{"id", "a"}, {"hf_path", "o/a"},
                                       {"quantize", {{"from", "nope"}}}}})}};
  CHECK(!Catalog::parse(doc).ok());
  Json bits = {{"models", Json::array(
                   {{{"id", "s"}, {"hf_path", "o/s"}},
                    {{"id", "a"}, {"hf_path", "o/a"},
                     {"quantize", {{"from", "s"}, {"bits", 3}}}}})}};
  CHECK(!Catalog::parse(bits).ok());
}

TEST(models, catalog_rejects_dangling_references)
{
  Json doc = {{"models", Json::array({{{"id", "a"}, {"hf_path", "o/a"},
                                       {"preview_with", "nope"}}})}};
  CHECK(!Catalog::parse(doc).ok());
  Json bad_cap = {{"models", Json::array({{{"id", "a"}, {"hf_path", "o/a"},
                                           {"capabilities",
                                            Json::array({"fly"})}}})}};
  CHECK(!Catalog::parse(bad_cap).ok());
}

TEST(models, assistant_follows_memory_tier)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  auto root = test::temp_dir("models");
  ModelStore empty({root});
  // Nothing installed: the tier decides -- the 27B from 32 GB.
  auto* a16 = pick_assistant(*c, empty, box(16));
  auto* a24 = pick_assistant(*c, empty, box(24));
  auto* a32 = pick_assistant(*c, empty, box(32));
  REQUIRE(a16 && a24 && a32);
  CHECK(a16->id == "qwen3.5-9b");
  CHECK(a24->id == "qwen3.5-9b");
  CHECK(a32->id == "qwen3.8-27b");
  // Its weights must stay resident: a GPU that keeps less than them
  // (a 24 GB M5 Pro keeps 17.8 GB) gets the 9B, whatever the RAM.
  HardwareInfo tight = box(32);
  tight.gpu_working_set_bytes = std::uint64_t{18} << 30;
  CHECK(pick_assistant(*c, empty, tight)->id == "qwen3.5-9b");
  tight.gpu_working_set_bytes = std::uint64_t{24} << 30;
  CHECK(pick_assistant(*c, empty, tight)->id == "qwen3.8-27b");
  // On 32 GB with only the 9B installed, use what is there.
  fake_install(root, *c->find("qwen3.5-9b"));
  ModelStore with9b({root});
  CHECK(pick_assistant(*c, with9b, box(32))->id == "qwen3.5-9b");
  // The uniform 4-bit 27B runs on 24 GB, at a third the 9B's speed there:
  // installed, it is still chosen only by name.
  fake_install(root, *c->find("qwen3.8-27b-4bit"));
  ModelStore both({root});
  HardwareInfo pro = box(24);
  pro.gpu_working_set_bytes = 19'069'468'672;  // a 24 GB M5 Pro's 17.8 GiB
  CHECK(pick_assistant(*c, both, pro)->id == "qwen3.5-9b");
}

TEST(models, capability_resolution)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  auto root = test::temp_dir("models");
  fake_install(root, *c->find("z-image-turbo"));
  ModelStore store({root});
  auto all = [](Capability) { return true; };
  auto caps = resolve_capabilities(*c, store, box(16), all);
  auto get = [&](Capability k) -> const CapabilityStatus* {
    for (const auto& s : caps) {
      if (s.capability == k) {
        return &s;
      }
    }
    return nullptr;
  };
  auto* t2i = get(Capability::TextToImage);
  REQUIRE(t2i);
  CHECK(t2i->availability == Availability::Ready);
  CHECK(t2i->chosen == "z-image-turbo");
  // MiniMax H3 streams on 16 GB: offered there (not installed here)...
  auto* t2v = get(Capability::TextToVideo);
  REQUIRE(t2v);
  CHECK(t2v->availability == Availability::NeedsDownload);
  // ...and nothing serves a smaller box.
  for (const auto& s : resolve_capabilities(*c, store, box(8), all)) {
    if (s.capability == Capability::TextToVideo) {
      CHECK(s.availability == Availability::NeedsMoreRam);
    }
  }
  auto* pe = get(Capability::PromptEnhance);
  REQUIRE(pe);
  CHECK(pe->availability == Availability::NeedsDownload);
  auto no_engine = resolve_capabilities(*c, store, box(16), nullptr);
  CHECK(no_engine[0].availability == Availability::NoEngine);
  // Installed models do not make an op the engine lacks "ready".
  auto t2i_only = [](Capability k) { return k == Capability::TextToImage; };
  auto partial = resolve_capabilities(*c, store, box(16), t2i_only);
  for (const auto& s : partial) {
    if (s.capability == Capability::PromptEnhance) {
      CHECK(s.availability == Availability::NotWired);
    }
  }
}

TEST(models, partial_download_is_not_installed)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  auto root = test::temp_dir("models");
  const auto* e = c->find("taef1");
  auto dir = root / e->hf_path;
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "model.safetensors.part") << "x";
  ModelStore store({root});
  CHECK(store.info(*e).state == InstallState::Partial);
}

// Auto's order is catalog data: per modality and op, in priority order,
// every id a real entry.
TEST(models, auto_order_is_catalog_data)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  CHECK((c->auto_order("image", "generate") ==
         std::vector<std::string>{"krea2-turbo", "qwen-image-2.1-turbo",
                                  "qwen-image-2.1", "z-image-turbo",
                                  "flux2-klein-9b"}));
  CHECK((c->auto_order("image", "edit") ==
         std::vector<std::string>{"qwen-image-2.1", "qwen-image-2.1-turbo",
                                  "flux2-klein-9b"}));
  CHECK((c->auto_order("video", "generate") ==
         std::vector<std::string>{"minimax-h3-fl2va"}));
  CHECK((c->auto_order("video", "edit") ==
         std::vector<std::string>{"minimax-h3-ref2va"}));
  // Songs first; speech when nothing sings -- and, with a voice in the
  // row (a sound's `edit`), speech alone, the 8-bit pack first.
  CHECK((c->auto_order("audio", "generate") ==
         std::vector<std::string>{"yue2-3b", "moss-tts-v1.5-w8g64",
                                  "moss-tts-v1.5"}));
  CHECK((c->auto_order("audio", "edit") ==
         std::vector<std::string>{"moss-tts-v1.5-w8g64", "moss-tts-v1.5"}));
  Json doc = {{"models", Json::array({{{"id", "a"}, {"hf_path", "o/a"}}})},
              {"auto", {{"image", {{"generate", {"a", "nope"}}}}}}};
  CHECK(!Catalog::parse(doc).ok());
}

// Ref2VA's Turbo LoRA (lightx2v, 8 steps) was distilled at video shift
// 6: it brings that shift, and gives back the model's 12 when it is
// turned off -- unless a shift is said.
TEST(models, ref2va_turbo_brings_its_shift)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* r = c->find("minimax-h3-ref2va");
  REQUIRE(r);
  CHECK(r->has(Capability::ReferenceToVideo));
  TuningContext t;
  const auto dir = test::temp_dir("tuning-ref2va");
  t.turbo_installed = true;
  t.turbo_path = (dir / "ref2va-8step.safetensors").string();
  std::ofstream(t.turbo_path) << "x";
  // Med's table names the 8-step adapter: installed, it runs.
  t.loras["minimax-h3-ref2va-turbo-8step"] = {"Ref2VA 8-step", t.turbo_path};
  Json v = resolve_tuning(*r, t, "balanced", false);
  CHECK(v["steps"] == 8);
  CHECK(v["video_shift"] == 6);
  CHECK(v["audio_shift"] == 3);
  Json off = v["loras"];
  off[0]["on"] = false;
  Json w = resolve_tuning(*r, t, "balanced", false, {{"loras", off}});
  CHECK(w["steps"] == 12);
  CHECK(w["video_shift"] == 12);
  Json said = resolve_tuning(*r, t, "balanced", false,
                             {{"loras", off}, {"video_shift", 9}});
  CHECK(said["video_shift"] == 9);
  // Without the adapter: the model's own.
  CHECK(resolve_tuning(*r, TuningContext{}, "balanced",
                       false)["video_shift"] == 12);
}

// Prompted in English only: Krea 2 and FLUX.2 klein -- the assistant
// translates for them; the rest read Chinese too.
TEST(models, english_only_models)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  auto lang = [&](const char* id) {
    const ModelEntry* m = c->find(id);
    return m ? jget<std::string>(m->prompting, "language", "") : "?";
  };
  CHECK(lang("krea2-turbo") == "english");
  CHECK(lang("flux2-klein-9b") == "english");
  for (const char* id : {"qwen-image-2.1", "minimax-h3-fl2va",
                         "minimax-h3-ref2va", "yue2-3b", "z-image-turbo"}) {
    CHECK(lang(id).empty());
  }
}

// A song's generator (YuE2): the flow matching's steps by preference
// (32 released: 16 / 32 / 48), the tiers its stage has, another decoder
// in place of its own -- and no LoRAs, no DiT, no shift.
// Favor for SPEECH (MOSS-TTS): Fast and Med hold its weights at 8 bits
// and run the int8 GEMMs (on matrix cores); Fine neither. No steps, no
// LoRA, no checkpoints of one's own -- its codec is its own.
TEST(models, tuning_for_speech)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  for (const char* id : {"moss-tts-v1.5", "moss-tts-v1.5-w8g64"}) {
    const ModelEntry* m = c->find(id);
    REQUIRE(m);
    TuningContext m5;
    m5.matrix_cores = true;
    for (const char* fast : {"speed", "balanced"}) {
      const Json v = resolve_tuning(*m, m5, fast, false, Json::object());
      CHECK(v["w8_weights"] == true && v["i8_gemm"] == true);
    }
    const Json fine = resolve_tuning(*m, m5, "quality", false,
                                     Json::object());
    CHECK(fine["w8_weights"] == false && fine["i8_gemm"] == false);
    // Without matrix cores, no int8 GEMMs at all.
    const Json m4 = resolve_tuning(*m, TuningContext{}, "speed", false,
                                   Json::object());
    CHECK(m4["i8_gemm"] == false && m4["w8_weights"] == true);
    const Json opts = tuning_options(*m, m5, "speed", false);
    std::vector<std::string> on;
    for (const auto& o : opts["options"]) {
      if (jget(o, "available", false)) {
        on.push_back(o["key"].get<std::string>());
      }
    }
    CHECK((on == std::vector<std::string>{"i8_gemm", "w8_weights"}));
    CHECK(!opts["values"].contains("steps"));
  }
}

TEST(models, tuning_for_a_song)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* y = c->find("yue2-3b");
  REQUIRE(y);
  CHECK(y->has(Capability::TextToAudio));
  CHECK(y->role == "audio");
  CHECK(c->find("yue2-vae"));
  TuningContext m5;
  m5.matrix_cores = true;
  CHECK(preset_steps(*y, "speed", false, false) == 16);
  CHECK(preset_steps(*y, "balanced", false, false) == 32);
  CHECK(preset_steps(*y, "quality", false, false) == 48);
  Json v = resolve_tuning(*y, m5, "balanced", false,
                          {{"sol_attn", true}, {"sage_attn", true},
                           {"ane_qkv", true}});
  CHECK(v["steps"] == 32);
  CHECK(v["sol_attn"] == true && v["sage_attn"] == true);
  CHECK(v["ane_qkv"] == false);  // not for this model
  CHECK(!v.contains("loras") && !v.contains("dits") && !v.contains("shift"));
  CHECK(v.contains("vaes"));
  Json opts = tuning_options(*y, m5, "speed", false);
  std::vector<std::string> keys;
  for (const auto& o : opts["options"]) {
    keys.push_back(o["key"].get<std::string>());
  }
  CHECK(std::ranges::find(keys, "loras") == keys.end());
  CHECK(std::ranges::find(keys, "vaes") != keys.end());
  CHECK(opts["values"]["steps"] == 16);
  // The family lists what it makes, and Auto tries it.
  bool listed = false;
  for (const auto& f : c->families()) {
    if (f.id == "yue2") {
      listed = f.features == std::vector<std::string>{"audio-gen"} &&
               f.members.size() == 3;
    }
  }
  CHECK(listed);
  CHECK(feature_capabilities("audio-gen") ==
        std::vector<Capability>{Capability::TextToAudio});
}

// One repo, two models: MiniMax-H3's partitions are installed apart.
TEST(models, a_subdir_is_its_own_install)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto* fl = c->find("minimax-h3-fl2va");
  const auto* ref = c->find("minimax-h3-ref2va");
  REQUIRE(fl && ref);
  CHECK(fl->hf_path == ref->hf_path);
  auto root = test::temp_dir("models-subdir");
  fake_install(root, *fl);
  ModelStore store({root});
  CHECK(store.info(*fl).state == InstallState::Installed);
  CHECK(store.info(*ref).state != InstallState::Installed);
  CHECK(store.info(*fl).dir == root / fl->hf_path / "FL2VA");
}

// One folder, several single-file models: madebyollin's taehv holds the
// MiniMax H3 TAE beside Wan's. Each is installed by its own FILE, and
// that file -- not the folder, which names both -- is what the engine is
// given. The H3 Turbo LoRA is the same shape, and the model it runs with
// names it (an unknown adapter is refused).
TEST(models, a_file_is_its_own_install)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto* h3 = c->find("taeh3");
  const auto* wan = c->find("taew2_1");
  REQUIRE(h3 && wan);
  CHECK(h3->hf_path == wan->hf_path);
  auto root = test::temp_dir("models-file");
  const auto dir = root / h3->hf_path;
  std::filesystem::create_directories(dir);
  std::ofstream(dir / "taew2_1.safetensors") << "x";
  {
    ModelStore store({root});
    CHECK(store.info(*h3).state == InstallState::Missing);
    CHECK(store.info(*wan).state == InstallState::Installed);
    CHECK(store.info(*wan).path() == dir / "taew2_1.safetensors");
  }
  std::ofstream(dir / "taeh3.safetensors.part") << "x";
  {
    ModelStore store({root});
    CHECK(store.info(*h3).state == InstallState::Partial);
  }
  std::ofstream(dir / "taeh3.safetensors") << "x";
  ModelStore store({root});
  CHECK(store.info(*h3).state == InstallState::Installed);
  CHECK(store.info(*h3).path() == dir / "taeh3.safetensors");
  CHECK(!h3->fetch_variant.empty());

  const auto* fl = c->find("minimax-h3-fl2va");
  REQUIRE(fl);
  CHECK(c->find(jget<std::string>(
            jget(jget(fl->engine, "vpipe", Json::object()), "turbo",
                 Json::object()),
            "lora", "")) != nullptr);
  const Json turbo = {{"vpipe", {{"turbo", {{"lora", "nope"}}}}}};
  Json bad = {{"models", Json::array({{{"id", "v"}, {"hf_path", "o/v"},
                                       {"engine", turbo}}})}};
  CHECK(!Catalog::parse(bad).ok());
}

// Favor's options (models/tuning.h): what a preset settles, what this
// Mac and install can have, and the rules Custom's values are held to.
TEST(models, tuning_settles_presets_and_custom)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* h3 = c->find("minimax-h3-fl2va");
  REQUIRE(h3);
  TuningContext all;
  all.turbo_installed = all.hyperflow_installed = all.vdn_installed = true;
  all.matrix_cores = true;
  const auto dir = test::temp_dir("tuning");
  all.turbo_path = (dir / "turbo.safetensors").string();
  std::ofstream(all.turbo_path) << "x";
  const std::string eight = (dir / "fl2va-8step.safetensors").string();
  std::ofstream(eight) << "x";
  all.loras["minimax-h3-turbo-8step"] = {"FL2VA 8-step", eight};

  // Med (balanced): its table's 8-step Turbo LoRA -- first in the list,
  // on, its 8 steps at its video shift 6 -- Sol-Attn at tau 0, Sage and
  // int8 GEMMs; no MotionCache.
  Json v = resolve_tuning(*h3, all, "balanced", false);
  CHECK(v["steps"] == 8);
  REQUIRE(v["loras"].size() == 1);
  CHECK(v["loras"][0]["on"] == true);
  CHECK(v["loras"][0]["path"].get<std::string>().ends_with(
      "fl2va-8step.safetensors"));
  CHECK(v["i8_gemm"] == true && v["sage_attn"] == true);
  CHECK(v["sol_attn"] == true && v["sol_tau"] == 0.0);
  CHECK(v["motion_cache"] == false);
  CHECK(v["video_shift"] == 6 && v["audio_shift"] == 3);
  CHECK(!v.contains("shift"));

  // Turbo off -- unchecked, taken out, or an older "turbo": false -- with
  // no steps said: the model's own balanced count at its own shift; back
  // on, its 8.
  Json off = v["loras"];
  off[0]["on"] = false;
  const Json w = resolve_tuning(*h3, all, "balanced", false,
                                {{"loras", off}});
  CHECK(w["steps"] == 12 && w["video_shift"] == 12);
  CHECK(resolve_tuning(*h3, all, "balanced", false,
                       {{"loras", Json::array()}})["steps"] == 12);
  CHECK(resolve_tuning(*h3, all, "balanced", false,
                       {{"turbo", false}})["steps"] == 12);
  CHECK(resolve_tuning(*h3, all, "balanced", false,
                       {{"loras", Json::array()},
                        {"turbo", "minimax-h3-turbo-8step"}})["steps"] == 8);
  // HyperFlow takes a slot, not with Turbo, and runs its own 8; VDN
  // replaces Sol.
  v = resolve_tuning(*h3, all, "speed", false,
                     {{"hyperflow", true}, {"vdn", true},
                      {"sol_attn", true}, {"steps", 3}});
  CHECK(v["hyperflow"] == true && v["loras"][0]["on"] == false);
  CHECK(v["steps"] == 8);
  CHECK(v["vdn"] == true && v["sol_attn"] == false);
  // Held to the ranges; a value of the wrong kind is not taken.
  v = resolve_tuning(*h3, all, "balanced", false,
                     {{"steps", 500}, {"sol_tau", -2.0},
                      {"sage_attn", "yes"}});
  CHECK(v["steps"] == 100);
  CHECK(v["sol_tau"] == 0.0);
  CHECK(v["sage_attn"] == true);   // not taken: the preset's

  // What this Mac and install cannot have stays at the preset, and says
  // why.
  TuningContext bare;  // no adapters, no matrix cores
  v = resolve_tuning(*h3, bare, "balanced", false,
                     {{"hyperflow", true}, {"sage_attn", true}});
  CHECK(v["hyperflow"] == false && v["sage_attn"] == false);
  CHECK(v["loras"].empty() && v["steps"] == 12);
  CHECK(v["i8_gemm"] == false);
  const Json opts = tuning_options(*h3, bare, "balanced", false);
  auto opt = [&](const char* k) {
    for (const auto& o : opts["options"]) {
      if (o["key"] == k) {
        return o;
      }
    }
    return Json();
  };
  CHECK(opt("hyperflow")["available"] == false);
  CHECK(opt("hyperflow")["why"] == "not-installed");
  CHECK(opt("sage_attn")["why"] == "needs-matrix-cores");
  CHECK(opt("hyperflow")["fixes"]["steps"] == 8);
  CHECK(opt("loras")["steps_on"] == 6 && opt("loras")["steps_off"] == 12);
  CHECK(opt("loras")["slots"] == 2);
  CHECK(opt("sol_tau")["needs"] == "sol_attn");
  // The Neural Engine: offered to every family, off; its q|k|v projection
  // only with its feed-forward.
  CHECK(opt("ane_ffn")["available"] == true);
  CHECK(opt("ane_qkv")["needs"] == "ane_ffn");
  v = resolve_tuning(*h3, all, "balanced", false, {{"ane_qkv", true}});
  CHECK(v["ane_ffn"] == false && v["ane_qkv"] == false);
  v = resolve_tuning(*h3, all, "balanced", false,
                     {{"ane_ffn", true}, {"ane_qkv", true}});
  CHECK(v["ane_ffn"] == true && v["ane_qkv"] == true);
  // Before M5 (no matrix cores), with a Neural Engine and at most 20 GPU
  // cores, Fast and Med lend the ANE the feed-forward: a picture model
  // its q|k|v projection too, MiniMax H3 only with 24 GB; Fine never.
  TuningContext m4 = bare;
  m4.ane_cores = 16;
  m4.gpu_cores = 10;
  m4.ram_gb = 16;
  v = resolve_tuning(*h3, m4, "speed", false);
  CHECK(v["ane_ffn"] == true && v["ane_qkv"] == false);
  m4.ram_gb = 24;
  v = resolve_tuning(*h3, m4, "balanced", false);
  CHECK(v["ane_ffn"] == true && v["ane_qkv"] == true);
  v = resolve_tuning(*h3, m4, "quality", false);
  CHECK(v["ane_ffn"] == false && v["ane_qkv"] == false);
  m4.ram_gb = 16;
  const ModelEntry* krea2 = c->find("krea2-turbo");
  const ModelEntry* yue2 = c->find("yue2-3b");
  REQUIRE(krea2 && yue2);
  for (const char* pref : {"speed", "balanced"}) {
    v = resolve_tuning(*krea2, m4, pref, false);
    CHECK(v["ane_ffn"] == true && v["ane_qkv"] == true);
  }
  v = resolve_tuning(*yue2, m4, "speed", false);  // a feed-forward only
  CHECK(v["ane_ffn"] == true && v["ane_qkv"] == false);
  // Custom can still take it off -- the q|k|v projection with it.
  v = resolve_tuning(*krea2, m4, "speed", false, {{"ane_ffn", false}});
  CHECK(v["ane_ffn"] == false && v["ane_qkv"] == false);
  // More GPU cores, no Neural Engine, or matrix cores (M5): the GPU's.
  TuningContext pro = m4;
  pro.gpu_cores = 30;
  TuningContext no_ane = m4;
  no_ane.ane_cores = 0;
  TuningContext m5 = m4;
  m5.matrix_cores = true;
  for (const TuningContext* t : {&pro, &no_ane, &m5}) {
    v = resolve_tuning(*krea2, *t, "speed", false);
    CHECK(v["ane_ffn"] == false && v["ane_qkv"] == false);
  }

  // Pictures: no MotionCache, no adapter to turn on; Sol only where
  // vpipe applies it; a shift only where there is a scheduler.
  const auto image = [&](const char* id) {
    const ModelEntry* m = c->find(id);
    CHECK(m != nullptr);
    return m ? tuning_options(*m, all, "balanced", false) : Json::object();
  };
  const Json krea = image("krea2-turbo");
  CHECK(!krea["values"].contains("motion_cache"));
  CHECK(krea["values"]["shift"] == 0.3);
  CHECK(krea["values"]["loras"].empty());
  bool sol = false, shift = true;
  // Bound first: a range-for over part of a temporary dangles.
  const Json qwen = image("qwen-image-2.1");
  const Json flux = image("flux2-klein-9b");
  for (const auto& o : qwen["options"]) {
    if (o["key"] == "sol_attn") {
      sol = o["available"].get<bool>();
    }
  }
  for (const auto& o : flux["options"]) {
    if (o["key"] == "shift") {
      shift = o["available"].get<bool>();
    }
  }
  CHECK(!sol);
  CHECK(!shift);
}

// FAVOR's presets as the catalog's tables say (DESIGN §10a): int8 GEMMs on
// Fast and Med wherever the GPU has matrix cores; Krea 2's attention per
// preset and Fine's 12 plain steps; MiniMax H3's adapters by preset -- the
// first installed candidate, else none, NEEDED -- Fine's MotionCache;
// Qwen-Image 2.1's Turbo LoRA on Fast, 25 and 40 steps without.
TEST(models, favor_presets_follow_the_catalog)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto dir = test::temp_dir("favor");
  const auto file = [&](const char* name) {
    const auto p = (dir / name).string();
    std::ofstream(p) << "x";
    return p;
  };
  TuningContext m5;
  m5.matrix_cores = true;
  TuningContext m4;   // no matrix cores
  const auto v = [&](const char* id, const TuningContext& ctx,
                     const char* p) {
    return resolve_tuning(*c->find(id), ctx, p, false);
  };
  // Every model with int8 GEMMs: Fast and Med take them on an M5, none
  // without matrix cores.
  for (const char* id : {"z-image-turbo", "flux2-klein-9b", "yue2-3b"}) {
    CHECK(v(id, m5, "speed")["i8_gemm"] == true);
    CHECK(v(id, m5, "balanced")["i8_gemm"] == true);
    CHECK(v(id, m4, "speed")["i8_gemm"] == false);
  }
  // Krea 2 Turbo.
  Json k = v("krea2-turbo", m5, "speed");
  CHECK(k["sol_attn"] == true && k["sol_tau"] == 0.0);
  CHECK(k["sage_attn"] == true && k["i8_gemm"] == true);
  k = v("krea2-turbo", m5, "balanced");
  CHECK(k["sol_attn"] == false && k["sage_attn"] == true);
  CHECK(k["i8_gemm"] == true);
  k = v("krea2-turbo", m5, "quality");
  CHECK(k["steps"] == 12);
  CHECK(k["sol_attn"] == false && k["sage_attn"] == false);
  CHECK(k["i8_gemm"] == false);
  // ... and on a Mac without matrix cores, Sage and int8 stay off.
  k = v("krea2-turbo", m4, "speed");
  CHECK(k["sol_attn"] == true && k["sage_attn"] == false);
  CHECK(k["i8_gemm"] == false);

  // MiniMax H3 FL2VA: Fast is its 4-step adapter on every Mac, whatever
  // its memory -- TaoMate's METHOD only when Custom asks for it.
  const ModelEntry& fl = *c->find("minimax-h3-fl2va");
  TuningContext h = m5;
  h.ram_gb = 24;
  CHECK(missing_loras(fl, h, "speed").size() == 1);   // the 4-step
  CHECK(missing_loras(fl, h, "quality").empty());     // Fine needs none
  h.loras["minimax-h3-turbo-4step"] = {"4-step", file("four.safetensors")};
  h.taomate_installed = true;
  h.loras["minimax-h3-turbo-3step"] = {"TaoMate", file("tao.safetensors")};
  CHECK(missing_loras(fl, h, "speed").empty());
  Json f;
  for (std::uint32_t gb : {16u, 24u, 64u}) {
    h.ram_gb = gb;
    f = resolve_tuning(fl, h, "speed", false);
    CHECK(f["steps"] == 4 && f["video_shift"] == 6);
    CHECK(f["loras"][0]["path"].get<std::string>().ends_with(
        "four.safetensors"));
    CHECK(f["loras"][0]["on"] == true);
    CHECK(f["sol_attn"] == true && f["sol_tau"] == 1.0);
    CHECK(f["sage_attn"] == true && f["i8_gemm"] == true);
    CHECK(f["ane_ffn"] == false && f["taomate"] == false);
  }
  // TaoMate when asked, in its own schedule, the Turbo LoRA and what it
  // replaces off...
  f = resolve_tuning(fl, h, "speed", false,
                     {{"taomate", true}, {"hyperflow", true}});
  CHECK(f["taomate"] == true && f["hyperflow"] == false);
  CHECK(f["steps"] == 3 && f["video_shift"] == 12 && f["audio_shift"] == 3);
  CHECK(f["loras"][0]["on"] == false);
  CHECK(f["taomate_lora"] == false);
  // ...or its adapter as a plain LoRA: still 3 steps at TaoMate's shifts
  // (a video shift asked for otherwise does not hold), the Turbo LoRA off;
  // never without TaoMate itself.
  f = resolve_tuning(fl, h, "speed", false,
                     {{"taomate", true}, {"taomate_lora", true},
                      {"video_shift", 6}});
  CHECK(f["taomate"] == true && f["taomate_lora"] == true);
  CHECK(f["steps"] == 3 && f["video_shift"] == 12 && f["audio_shift"] == 3);
  CHECK(f["loras"][0]["on"] == false);
  f = resolve_tuning(fl, h, "speed", false, {{"taomate_lora", true}});
  CHECK(f["taomate"] == false && f["taomate_lora"] == false);
  CHECK(f["steps"] == 4 && f["video_shift"] == 6);
  {
    bool listed = false;
    const Json fo = tuning_options(fl, h, "speed", false);
    for (const auto& o : fo["options"]) {
      if (o["key"] == "taomate_lora") {
        listed = true;
        CHECK(jget<std::string>(o, "needs", "") == "taomate");
      }
    }
    CHECK(listed);
  }
  // ...where it needs no Turbo LoRA, even with none here; without it, Fast
  // needs its 4-step -- not 8 plain steps at TaoMate's shifts.
  h.loras.erase("minimax-h3-turbo-4step");
  CHECK(missing_loras(fl, h, "speed", {{"taomate", true}}).empty());
  CHECK(missing_loras(fl, h, "speed").size() == 1);
  CHECK(missing_loras(fl, h, "speed", {{"taomate", false}}).size() == 1);
  // Before M5 (no matrix cores) with a Neural Engine: Fast the same 4-step
  // LoRA and Sol, the ANE's feed-forward in place of int8 GEMMs (and Sage)
  // -- as Med lends it, its q|k|v projection too from 24 GB.
  TuningContext a = m4;
  a.ane_cores = 16;
  a.gpu_cores = 10;
  a.ram_gb = 24;
  a.loras["minimax-h3-turbo-4step"] = {"4-step", file("four.safetensors")};
  a.loras["minimax-h3-turbo-8step"] = {"8-step", file("eight.safetensors")};
  f = resolve_tuning(fl, a, "speed", false);
  CHECK(f["steps"] == 4 && f["video_shift"] == 6);
  CHECK(f["loras"][0]["on"] == true);
  CHECK(f["sol_attn"] == true && f["sol_tau"] == 1.0);
  CHECK(f["sage_attn"] == false && f["i8_gemm"] == false);
  CHECK(f["ane_ffn"] == true && f["ane_qkv"] == true);
  const Json med = resolve_tuning(fl, a, "balanced", false);
  CHECK(med["ane_ffn"] == f["ane_ffn"] && med["ane_qkv"] == f["ane_qkv"]);
  CHECK(med["i8_gemm"] == false);
  // Fine: no adapter, Sage, int8, MotionCache; no Sol.
  h.turbo_installed = true;
  h.turbo_path = file("six.safetensors");
  f = resolve_tuning(fl, h, "quality", false);
  CHECK(f["steps"] == 16);
  CHECK(f["loras"].size() == 1 && f["loras"][0]["on"] == false);
  CHECK(f["sage_attn"] == true && f["i8_gemm"] == true);
  CHECK(f["motion_cache"] == true && f["sol_attn"] == false);
  // Ref2VA: its own 4-step on Fast (the model's shifts).
  const ModelEntry& rf = *c->find("minimax-h3-ref2va");
  TuningContext r = m5;
  r.loras["minimax-h3-ref2va-turbo-4step"] = {"4-step",
                                              file("r4.safetensors")};
  f = resolve_tuning(rf, r, "speed", false);
  CHECK(f["steps"] == 4 && f["video_shift"] == 12);
  CHECK(missing_loras(rf, r, "balanced").size() == 1);

  // Qwen-Image 2.1: Fast needs its Turbo LoRA; Med 25, Fine 40, plain.
  const ModelEntry& q = *c->find("qwen-image-2.1");
  CHECK(missing_loras(q, m5, "speed").size() == 1);
  Json qv = resolve_tuning(q, m5, "balanced", false);
  CHECK(qv["steps"] == 25 && qv["i8_gemm"] == true);
  qv = resolve_tuning(q, m5, "quality", true);   // an edit too
  CHECK(qv["steps"] == 40 && qv["i8_gemm"] == true);
  TuningContext qt = m5;
  qt.loras["qwen-image-21-turbo"] = {"Qwen turbo", file("q.safetensors")};
  qv = resolve_tuning(q, qt, "speed", false);
  CHECK(qv["steps"] == 6 && qv["loras"][0]["on"] == true);
  CHECK(qv["i8_gemm"] == true);
  // What the panel and the app are told.
  const Json opts = tuning_options(q, qt, "speed", false);
  CHECK(opts["turbo"]["name"] == "Qwen turbo");
  CHECK(opts["missing"].empty());

  // Qwen-Image 2.1 Turbo: a checkpoint distilled to a schedule of its own
  // -- its 8 steps whatever is preferred, edits too, and no LoRA to need.
  // Favor moves its arithmetic (int8 GEMMs on Fast and Med, none on
  // Fine); Custom offers no steps.
  const ModelEntry& t8 = *c->find("qwen-image-2.1-turbo");
  for (const char* p : {"speed", "balanced", "quality"}) {
    CHECK(missing_loras(t8, m5, p).empty());
    CHECK(preset_steps(t8, p, false, false) == 8);
    CHECK(preset_steps(t8, p, true, false) == 8);
  }
  Json tv = resolve_tuning(t8, m5, "speed", false);
  CHECK(tv["i8_gemm"] == true);
  tv = resolve_tuning(t8, m5, "balanced", true);
  CHECK(tv["i8_gemm"] == true);
  tv = resolve_tuning(t8, m5, "quality", false);
  CHECK(tv["i8_gemm"] == false);
  bool offers_steps = false;
  const Json t8opts = tuning_options(t8, m5, "balanced", false);
  for (const auto& o : t8opts["options"]) {
    offers_steps = offers_steps || o["key"] == "steps";
  }
  CHECK(!offers_steps);
}

// The LoRA list: two on at most, the first in list order (vpipe's two
// slots; one with HyperFlow); strengths held to 0..2; a file that is gone
// kept, off. An older "turbo" file goes first, in the catalog's place.
TEST(models, tuning_loras)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* h3 = c->find("minimax-h3-fl2va");
  REQUIRE(h3);
  const auto dir = test::temp_dir("loras");
  const auto touch = [&](const char* name) {
    const auto p = dir / name;
    std::ofstream(p) << "x";
    return p.string();
  };
  TuningContext ctx;
  ctx.turbo_installed = ctx.hyperflow_installed = true;
  ctx.turbo_name = "MiniMax H3 Turbo";
  ctx.turbo_path = touch("turbo.safetensors");
  const std::string a = touch("a.safetensors");
  const std::string b = touch("b.safetensors");
  const std::string d = touch("d.safetensors");
  Json over = {
    {"loras", Json::array({
       {{"path", a}, {"scale", 0.8}, {"on", true}},
       {{"path", b}, {"on", true}, {"scale", 5.0}},
       {{"path", d}, {"on", true}},
       {{"path", (dir / "gone.safetensors").string()}, {"on", true}},
     })},
  };
  Json v = resolve_tuning(*h3, ctx, "balanced", false, over);
  CHECK(v["steps"] == 12);                 // no Turbo LoRA in the list
  Json l = v["loras"];
  REQUIRE(l.size() == 4);
  CHECK(l[0]["on"] == true && l[0]["scale"] == 0.8);
  CHECK(l[1]["on"] == true && l[1]["scale"] == 2.0);  // held to 0..2
  CHECK(l[2]["on"] == false);              // two slots: the first two
  CHECK(l[3]["missing"] == true && l[3]["on"] == false);
  // HyperFlow takes one of the two.
  over["hyperflow"] = true;
  l = resolve_tuning(*h3, ctx, "balanced", false, over)["loras"];
  CHECK(l[0]["on"] == true && l[1]["on"] == false);
  // The Turbo LoRA is listed by its name.
  const Json opts = tuning_options(*h3, ctx, "balanced", false);
  for (const auto& o : opts["options"]) {
    if (o["key"] == "loras") {
      CHECK(o["names"].size() == 1);
      CHECK(o["names"].begin().value() == "MiniMax H3 Turbo");
    }
  }
  // An older "turbo" file: first, on, in the catalog's place, with the
  // few-step counts; one that is gone is listed, and changes nothing.
  v = resolve_tuning(*h3, ctx, "balanced", false, {{"turbo", a}});
  REQUIRE(v["loras"].size() == 2);
  CHECK(v["loras"][0]["path"].get<std::string>().ends_with(
      "a.safetensors"));
  CHECK(v["loras"][0]["on"] == true && v["loras"][1]["on"] == false);
  CHECK(v["steps"] == 6);
  v = resolve_tuning(*h3, ctx, "balanced", false,
                     {{"turbo", "/elsewhere/x.safetensors"}});
  REQUIRE(v["loras"].size() == 2);
  CHECK(v["loras"][0]["missing"] == true && v["loras"][1]["on"] == true);
  CHECK(v["steps"] == 6);
}

// What a dropped weight file is, read from it: low-rank pairs are a LoRA,
// encoder / decoder tensors a VAE, anything else a DiT; a folder by its
// config.
TEST(models, checkpoint_kind_reads_the_weights)
{
  const auto dir = test::temp_dir("weights");
  const auto write = [&](const char* name, const Json& header) {
    const auto p = dir / name;
    const std::string h = header.dump();
    std::ofstream f(p, std::ios::binary);
    const std::uint64_t n = h.size();
    f.write(reinterpret_cast<const char*>(&n), 8);
    f << h;
    return p;
  };
  const Json t = {{"dtype", "BF16"}, {"shape", {1}},
                  {"data_offsets", {0, 2}}};
  CHECK(checkpoint_kind(write("a.safetensors",
                              {{"blocks.0.attn.lora_A.weight", t},
                               {"blocks.0.attn.lora_B.weight", t}})) ==
        "lora");
  CHECK(checkpoint_kind(write("v.safetensors",
                              {{"decoder.conv_in.weight", t},
                               {"encoder.conv_in.weight", t}})) == "vae");
  CHECK(checkpoint_kind(write("d.safetensors",
                              {{"blocks.0.attn.qkv.weight", t},
                               {"blocks.0.mlp.fc1.weight", t}})) == "dit");
  // A Comfy-Org component names itself.
  CHECK(checkpoint_kind(write("c.safetensors",
                              {{"__metadata__", {{"minimax_h3_video_vae",
                                                  "{}"}}},
                               {"blocks.0.weight", t}})) == "vae");
  // A folder by its config.
  const auto vae = dir / "vae";
  std::filesystem::create_directories(vae);
  std::ofstream(vae / "config.json")
      << R"({"_class_name": "AutoencoderKLQwenImage"})";
  CHECK(checkpoint_kind(vae) == "vae");
  CHECK(checkpoint_kind(dir / "nothing.safetensors").empty());
  std::ofstream(dir / "notes.txt") << "hi";
  CHECK(checkpoint_kind(dir / "notes.txt").empty());
}

// Community DiT / VAE checkpoints: the one on (one per part) runs; a
// missing one stays listed, off.
TEST(models, tuning_checkpoints)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* krea = c->find("krea2-turbo");
  REQUIRE(krea);
  const auto dir = test::temp_dir("ckpts");
  std::ofstream(dir / "a.safetensors") << "x";
  std::ofstream(dir / "b.safetensors") << "x";
  const Json v = resolve_tuning(*krea, TuningContext{}, "balanced", false, {
    {"dits", Json::array({{{"path", (dir / "a.safetensors").string()},
                           {"on", true}},
                          {{"path", (dir / "b.safetensors").string()},
                           {"on", true}}})},
    {"vaes", Json::array({{{"path", (dir / "gone").string()},
                           {"on", true}}})},
  });
  REQUIRE(v["dits"].size() == 2);
  CHECK(v["dits"][0]["on"] == true && v["dits"][1]["on"] == false);
  CHECK(v["vaes"][0]["missing"] == true && v["vaes"][0]["on"] == false);
}

// Settings > Capabilities: every catalog model in one family, each
// feature made of known capabilities; and a model LINKED to a file or
// folder anywhere is there, ahead of the roots, until unlinked -- kept
// across a restart.
TEST(models, families_and_links)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  std::set<std::string> placed;
  for (const auto& f : c->families()) {
    for (const auto& ft : f.features) {
      CHECK(!feature_capabilities(ft).empty());
    }
    for (const auto& m : f.members) {
      CHECK(placed.insert(m.model).second);
      CHECK(!m.label.empty());
    }
  }
  for (const auto& m : c->models()) {
    CHECK(placed.count(m.id) == 1);
  }
  const ModelEntry* lora = c->find("minimax-h3-turbo-8step");
  const ModelEntry* folder = c->find("minimax-h3-vdn");
  REQUIRE(lora && folder);

  const auto dir = test::temp_dir("links");
  const auto links = dir / "model-links.json";
  std::filesystem::create_directories(dir / "root");
  std::filesystem::create_directories(dir / "comfy" / "loras");
  std::filesystem::create_directories(dir / "vdn" / "weights");
  std::ofstream(dir / "comfy" / "loras" / "small.safetensors") << "x";
  std::ofstream(dir / "comfy" / "loras" / "big.safetensors") << "xxxxxxxx";
  std::ofstream(dir / "vdn" / "weights" / "a.safetensors") << "x";
  {
    ModelStore store({dir / "root"}, links);
    CHECK(store.info(*lora).state == InstallState::Missing);
    // A LoRA's folder: its largest .safetensors.
    REQUIRE_OK(store.link(*lora, dir / "comfy" / "loras"));
    const auto i = store.info(*lora);
    CHECK(i.state == InstallState::Installed && i.linked);
    CHECK(i.path().filename() == "big.safetensors");
    // A folder model's folder; nothing there is refused.
    REQUIRE_OK(store.link(*folder, dir / "vdn"));
    CHECK(store.info(*folder).state == InstallState::Installed);
    CHECK(!store.link(*folder, dir / "root").ok());
    CHECK(!store.link(*lora, dir / "nowhere").ok());
  }
  {
    // Kept: a new store reads them back.
    ModelStore store({dir / "root"}, links);
    CHECK(store.info(*lora).linked);
    store.unlink(lora->id);
    CHECK(store.info(*lora).state == InstallState::Missing);
    CHECK(store.link_of(folder->id) == std::filesystem::absolute(dir / "vdn"));
  }
}

// Unified memory's bandwidth, by the chip (Apple's figures): what a video
// summary's frame interval is chosen by (DESIGN §4i).
TEST(models, memory_bandwidth_by_chip)
{
  using models::memory_bandwidth_of_chip;
  CHECK(memory_bandwidth_of_chip("Apple M5", 10) == 153);
  CHECK(memory_bandwidth_of_chip("Apple M5 Pro", 20) == 307);
  CHECK(memory_bandwidth_of_chip("Apple M3 Pro", 18) == 150);
  CHECK(memory_bandwidth_of_chip("Apple M2 Pro", 19) == 200);
  CHECK(memory_bandwidth_of_chip("Apple M4 Max", 32) == 410);
  CHECK(memory_bandwidth_of_chip("Apple M4 Max", 40) == 546);
  CHECK(memory_bandwidth_of_chip("Apple M1 Ultra", 64) == 800);
  CHECK(memory_bandwidth_of_chip("Apple M1", 8) == 68);
  // A later generation, as M5's of its tier.
  CHECK(memory_bandwidth_of_chip("Apple M7 Pro", 24) == 307);
  CHECK(memory_bandwidth_of_chip("Intel(R) Core(TM) i9", 0) == 0);
}
