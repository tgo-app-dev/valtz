#include "testing.h"

#include "drawthings-fixture.h"

#include "valtz/models/capabilities.h"
#include "valtz/models/drawthings.h"
#include "valtz/models/hardware.h"
#include "valtz/models/model-check.h"
#include "valtz/models/model-store.h"
#include "valtz/models/tuning.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <map>
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

// A safetensors file as model-fetch writes it: its header, then every
// tensor's bytes -- or `cut` of them short, as a copy stopped midway.
void
write_safetensors(const std::filesystem::path& f,
                  const std::vector<std::pair<std::string,
                                              std::vector<int>>>& tensors,
                  std::size_t cut = 0)
{
  Json h = Json::object();
  std::uint64_t off = 0;
  for (const auto& [name, shape] : tensors) {
    std::uint64_t n = 2;
    for (int d : shape) {
      n *= static_cast<std::uint64_t>(d);
    }
    h[name] = {{"dtype", "BF16"}, {"shape", shape},
               {"data_offsets", {off, off + n}}};
    off += n;
  }
  std::string text = h.dump();
  text.append((8 - text.size() % 8) % 8, ' ');
  std::filesystem::create_directories(f.parent_path());
  std::ofstream out(f, std::ios::binary);
  std::uint64_t len = text.size();
  for (int i = 0; i < 8; ++i) {
    out.put(static_cast<char>((len >> (8 * i)) & 0xff));
  }
  out << text << std::string(off - std::min<std::uint64_t>(off, cut), '\0');
}

// A diffusers pipeline's folder, as Krea 2's download lays it out (its
// pipeline and DiT named `pipeline` / `dit`).
void
fake_pipeline(const std::filesystem::path& dir,
              const std::string& pipeline = "Krea2Pipeline",
              const std::string& dit = "Krea2Transformer2DModel")
{
  std::filesystem::create_directories(dir / "transformer");
  std::ofstream(dir / "model_index.json") << Json{
      {"_class_name", pipeline},
      {"transformer", {"diffusers", dit}},
      {"text_encoder", {"transformers", "Qwen3VLModel"}},
      {"vae", {"diffusers", "AutoencoderKLQwenImage"}},
      {"scheduler", Json::array({nullptr, nullptr})}}.dump();
  std::ofstream(dir / "transformer" / "config.json")
      << Json{{"_class_name", dit}}.dump();
  write_safetensors(dir / "transformer" / "dit-00001-of-00002.safetensors",
                    {{"blocks.0.attn.to_q.weight", {256, 256}},
                     {"blocks.0.norm.weight", {256}}});
  write_safetensors(dir / "transformer" / "dit-00002-of-00002.safetensors",
                    {{"blocks.1.attn.to_q.weight", {256, 256}}});
  std::ofstream(dir / "transformer" /
                "diffusion_pytorch_model.safetensors.index.json")
      << Json{{"weight_map",
               {{"blocks.0.attn.to_q.weight",
                 "dit-00001-of-00002.safetensors"},
                {"blocks.0.norm.weight", "dit-00001-of-00002.safetensors"},
                {"blocks.1.attn.to_q.weight",
                 "dit-00002-of-00002.safetensors"}}}}.dump();
  write_safetensors(dir / "text_encoder" / "model.safetensors",
                    {{"model.embed_tokens.weight", {512, 64}},
                     {"model.layers.0.mlp.up_proj.weight", {128, 64}}});
  write_safetensors(dir / "vae" / "diffusion_pytorch_model.safetensors",
                    {{"decoder.conv_in.weight", {16, 16, 3, 3}}});
}

// The keys of a check's notes, in order.
std::vector<std::string>
note_keys(const ModelCheck& c)
{
  std::vector<std::string> out;
  for (const auto& n : c.notes) {
    out.push_back(jget<std::string>(n.message, "key", ""));
  }
  return out;
}

bool
has_key(const ModelCheck& c, std::string_view key)
{
  const auto ks = note_keys(c);
  return std::ranges::find(ks, key) != ks.end();
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

TEST(models, an_abliterated_helper_is_chosen_by_name_alone)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* fn = c->find("qwen3.8-flash-next");
  REQUIRE(fn);
  CHECK(fn->by_name);
  // 128.6 GB on disk, 6.8 held at least: a 64 GB Mac whose GPU keeps 48
  // runs it -- its experts from a cache -- a 32 GB one does not.
  HardwareInfo m64 = box(64);
  m64.gpu_working_set_bytes = std::uint64_t{48} << 30;
  CHECK(fits_as_assistant(*fn, m64));
  HardwareInfo m32 = box(32);
  m32.gpu_working_set_bytes = std::uint64_t{24} << 30;
  CHECK(!fits_as_assistant(*fn, m32));
  // The only helper installed, on tgo-m5-3's 256 GB: still never Auto's.
  auto root = test::temp_dir("models-by-name");
  fake_install(root, *fn);
  ModelStore store({root});
  HardwareInfo ultra = box(256);
  ultra.gpu_working_set_bytes = std::uint64_t{192} << 30;
  const ModelEntry* a = pick_assistant(*c, store, ultra);
  REQUIRE(a);
  CHECK(a->id != fn->id);
  CHECK(a->id == "qwen3.8-27b");
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
                                  "flux3-klein", "flux2-klein-9b",
                                  "moss-image-sft", "moss-image-base"}));
  CHECK((c->auto_order("image", "edit") ==
         std::vector<std::string>{"qwen-image-2.1", "qwen-image-2.1-turbo",
                                  "flux3-klein", "flux2-klein-9b",
                                  "fibo2-edit-turbo"}));
  // A clip from words: FL2VA, the trained route; else Ref2VA from the
  // prompt alone (vpipe's prompt-only request).
  CHECK((c->auto_order("video", "generate") ==
         std::vector<std::string>{"minimax-h3-fl2va", "minimax-h3-ref2va"}));
  CHECK((c->auto_order("video", "edit") ==
         std::vector<std::string>{"minimax-h3-ref2va"}));
  // Songs first -- YuE2, then MiniMax-Music3 (far slower) -- speech when
  // nothing sings; and, with a voice in the row (a sound's `edit`),
  // speech alone: Drama 3 (the release, then its int4), then MOSS-TTS,
  // the 8-bit pack first.
  CHECK((c->auto_order("audio", "generate") ==
         std::vector<std::string>{"yue2-3b", "minimax-music3", "drama-3",
                                  "drama-3-int4", "moss-tts-v1.5-w8g64",
                                  "moss-tts-v1.5"}));
  CHECK((c->auto_order("audio", "edit") ==
         std::vector<std::string>{"drama-3", "drama-3-int4",
                                  "moss-tts-v1.5-w8g64", "moss-tts-v1.5"}));
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

// An UPSCALER offers its arithmetic alone -- it restores in its own one
// step: no steps, no LoRA list, no checkpoints, no shift. FlashVSR takes
// no Sol (its attention is routed by its own weights); VOSR does. Med's
// defaults: int8 GEMMs where the GPU has matrix cores.
TEST(models, tuning_for_an_upscaler)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  TuningContext m5;
  m5.matrix_cores = true;
  for (const char* id : {"flashvsr", "flashvsr-plus-turbo",
                         "flashvsr-plus-base", "vosr"}) {
    const ModelEntry* m = c->find(id);
    REQUIRE(m);
    const Json opts = tuning_options(*m, m5, "balanced", false);
    std::vector<std::string> keys;
    for (const auto& o : opts["options"]) {
      if (jget(o, "available", false)) {
        keys.push_back(o["key"].get<std::string>());
      }
    }
    const bool vosr = std::string_view(id) == "vosr";
    // FlashVSR's colour fix (its encoder's, declared in the catalog): on
    // for every release -- FlashVSR+'s own default is off.
    CHECK((keys == (vosr ? std::vector<std::string>{"sol_attn", "sol_tau",
                                                    "sage_attn", "i8_gemm"}
                         : std::vector<std::string>{"sage_attn", "i8_gemm",
                                                    "color_fix"})));
    if (!vosr) {
      CHECK(opts["values"]["color_fix"] == true);
    }
    for (const char* none : {"steps", "loras", "dits", "vaes", "shift"}) {
      CHECK(!opts["values"].contains(none));
    }
    CHECK(opts["values"]["i8_gemm"] == true);
    CHECK(opts["values"]["sage_attn"] == false);
    // Before M5: no int8 path, so neither int8 tier.
    const Json m4 = resolve_tuning(*m, TuningContext{}, "balanced", false,
                                   {{"i8_gemm", true}, {"sage_attn", true}});
    CHECK(m4["i8_gemm"] == false && m4["sage_attn"] == false);
  }
}

// Custom's quick pick of a model's ACCELERATION LoRAs: its few-step
// adapters as its catalog names them -- the `turbo` block's and every
// preset's candidates, each at its own steps and shifts, fewest steps
// first -- not HyperFlow's or TaoMate's adapter (switches of their own),
// and only the partition's own.
TEST(models, a_models_acceleration_loras_are_listed)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  auto fast_of = [&](const char* id) {
    TuningContext ctx;
    ctx.matrix_cores = true;
    ctx.loras["minimax-h3-turbo-8step"] = {"FL2VA 8", "/m/fl8.safetensors",
                                           "8-step Turbo LoRA, FL2VA"};
    Json f;
    const Json t = tuning_options(*c->find(id), ctx, "balanced", false);
    for (const auto& o : t["options"]) {
      if (o["key"] == "loras") {
        f = o["fast"];
      }
    }
    return f;
  };
  const Json fl = fast_of("minimax-h3-fl2va");
  REQUIRE(fl.is_array() && fl.size() == 3);
  CHECK(fl[0]["id"] == "minimax-h3-turbo-4step" && fl[0]["steps"] == 4);
  CHECK(fl[0]["config"]["video_shift"] == 6.0);
  CHECK(fl[1]["id"] == "minimax-h3-turbo" && fl[1]["steps"] == 6);
  CHECK(fl[2]["id"] == "minimax-h3-turbo-8step" && fl[2]["steps"] == 8);
  // Capabilities' name and its file when installed; "" when not.
  CHECK(fl[2]["name"] == "8-step Turbo LoRA, FL2VA");
  CHECK(fl[2]["path"] == "/m/fl8.safetensors");
  CHECK(fl[0]["path"] == "");
  const Json ref = fast_of("minimax-h3-ref2va");
  REQUIRE(ref.size() == 2);
  CHECK(ref[0]["id"] == "minimax-h3-ref2va-turbo-4step");
  CHECK(ref[1]["id"] == "minimax-h3-ref2va-turbo-8step");
  for (const Json* f : {&fl, &ref}) {
    for (const auto& x : *f) {
      CHECK(x["id"] != "minimax-h3-hyperflow");
      CHECK(x["id"] != "minimax-h3-turbo-3step");
    }
  }
  CHECK(fast_of("qwen-image-2.1").size() == 1);
  CHECK(fast_of("krea2-turbo").empty());
}

// A clip CONTINUED (TuningContext::continuation): Ref2VA's Fast and Med
// run HyperFlow, not lightx2v's few-step LoRAs -- whose continuations
// drift sideways -- at its own 8 steps and the model's shifts; HyperFlow
// not here, the model alone at its own steps. Nothing is asked to be
// downloaded. Not continued, the presets are as they were.
TEST(models, a_continuation_has_presets_of_its_own)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* m = c->find("minimax-h3-ref2va");
  REQUIRE(m);
  TuningContext ctx;
  ctx.matrix_cores = true;
  ctx.hyperflow_installed = true;
  ctx.loras["minimax-h3-ref2va-turbo-8step"] = {"8-step", "/m/r8.safetensors",
                                                ""};
  ctx.loras["minimax-h3-ref2va-turbo-4step"] = {"4-step", "/m/r4.safetensors",
                                                ""};
  auto on = [](const Json& v) {
    int n = 0;
    for (const auto& l : v["loras"]) {
      n += l["on"].get<bool>() ? 1 : 0;
    }
    return n;
  };
  Json v = resolve_tuning(*m, ctx, "balanced", false);
  CHECK(v["hyperflow"] == false && on(v) == 1);
  CHECK(v["video_shift"] == 6);
  ctx.continuation = true;
  for (const char* pref : {"speed", "balanced"}) {
    v = resolve_tuning(*m, ctx, pref, false);
    CHECK(v["hyperflow"] == true);
    CHECK(on(v) == 0);
    CHECK(v["steps"] == 8);
    CHECK(v["video_shift"] == 12);
    CHECK(missing_loras(*m, ctx, pref).empty());
    // Fast keeps its Sol (measured: no drift), Med runs without.
    CHECK(v["sol_attn"] == (std::string_view(pref) == "speed"));
  }
  // Fine as ever: the model alone.
  v = resolve_tuning(*m, ctx, "quality", false);
  CHECK(v["hyperflow"] == false && on(v) == 0 && v["steps"] == 16);
  // HyperFlow not here: the model alone at its own steps.
  ctx.hyperflow_installed = false;
  v = resolve_tuning(*m, ctx, "balanced", false);
  CHECK(v["hyperflow"] == false && on(v) == 0);
  CHECK(v["steps"] == 12 && v["video_shift"] == 12);
  // A picture model has no continuation of its own: its presets hold.
  const ModelEntry* fl = c->find("minimax-h3-fl2va");
  TuningContext plain;
  plain.continuation = true;
  CHECK(resolve_tuning(*fl, plain, "balanced", false) ==
        resolve_tuning(*fl, TuningContext{}, "balanced", false));
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

// MiniMax-Music3: a song from a description (in lines) and lyrics -- both
// needed -- no score planned, decoded by its own folder's vocoder. Its
// flow matching runs the model's 30 steps at every preference: Fast holds
// it as int8 Per Token, Med as int8 Fast (on matrix cores), Fine 16-bit.
// No LoRAs, no checkpoints of one's own.
TEST(models, minimax_music3_sings_a_caption_to_lyrics)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* m = c->find("minimax-music3");
  REQUIRE(m);
  CHECK(m->role == "audio" && m->has(Capability::TextToAudio));
  const Json vp = jget(m->engine, "vpipe", Json::object());
  CHECK(jget<std::string>(vp, "decoder", "") == m->id);
  const Json sb = jget(vp, "song", Json::object());
  CHECK(jget<std::string>(sb, "style", "") == "caption");
  CHECK(jget(sb, "lyrics_required", false) && !jget(sb, "score", true));
  CHECK(jget(vp, "sample_rate", 0) == 44100);
  for (const char* p : {"speed", "balanced", "quality"}) {
    CHECK(preset_steps(*m, p, false, false) == 30);
  }
  TuningContext m5;
  m5.matrix_cores = true;
  Json fast = resolve_tuning(*m, m5, "speed", false);
  CHECK(fast["i8_gemm"] == true && fast["i8_gemm_tier"] == "token");
  Json med = resolve_tuning(*m, m5, "balanced", false);
  CHECK(med["i8_gemm"] == true && med["i8_gemm_tier"] == "fast");
  CHECK(resolve_tuning(*m, m5, "quality", false)["i8_gemm"] == false);
  // Before M5: no int8 at all.
  CHECK(resolve_tuning(*m, TuningContext{}, "speed", false)["i8_gemm"] ==
        false);
  CHECK(!fast.contains("loras") && !fast.contains("dits"));
  std::vector<std::string> keys;
  const Json opts = tuning_options(*m, m5, "speed", false);
  for (const auto& o : opts["options"]) {
    keys.push_back(o["key"].get<std::string>());
  }
  CHECK(std::ranges::find(keys, "i8_gemm_tier") != keys.end());
  CHECK(std::ranges::find(keys, "loras") == keys.end());
  // Auto's second song model, its family listed.
  bool listed = false;
  for (const auto& f : c->families()) {
    listed = listed || (f.id == "minimax-music3" &&
                        f.features == std::vector<std::string>{"audio-gen"});
  }
  CHECK(listed);
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
  CHECK(opt("loras")["slots"] == kLoraSlots);
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
// preset -- Fast's Sol only where there are none -- and Fine's 12 plain
// steps; MiniMax H3's adapters by preset -- the first installed
// candidate, else none, NEEDED -- its int8 P*V and tiers, Fine's
// MotionCache; Qwen-Image 2.1's Turbo LoRA on Fast with Sage and the
// int8s, 25 and 40 steps without.
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
  // Krea 2 Turbo: Fast 6 steps, Sage with its int8 P*V, int8 Per Token
  // -- no Sol on an M5.
  Json k = v("krea2-turbo", m5, "speed");
  CHECK(k["steps"] == 6 && k["sol_attn"] == false);
  CHECK(k["sage_attn"] == true && k["sage_pv8"] == true);
  CHECK(k["i8_gemm"] == true && k["i8_gemm_tier"] == "token");
  k = v("krea2-turbo", m5, "balanced");
  CHECK(k["sol_attn"] == false && k["sage_attn"] == true);
  CHECK(k["i8_gemm"] == true);
  k = v("krea2-turbo", m5, "quality");
  CHECK(k["steps"] == 12);
  CHECK(k["sol_attn"] == false && k["sage_attn"] == false);
  CHECK(k["i8_gemm"] == false);
  // ... and on a Mac without matrix cores, Sage and int8 stay off: Fast
  // its 6 steps with Sol in their place (its `without_matrix_cores`).
  k = v("krea2-turbo", m4, "speed");
  CHECK(k["steps"] == 6);
  CHECK(k["sol_attn"] == true && k["sol_tau"] == 0.0);
  CHECK(k["sage_attn"] == false && k["sage_pv8"] == false);
  CHECK(k["i8_gemm"] == false && k["i8_gemm_tier"] == "accurate");
  k = v("krea2-turbo", m4, "balanced");
  CHECK(k["steps"] == 8 && k["sol_attn"] == false);

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
    // ...Sage's int8 P*V, on the prompt's and soundtrack's rows too, and
    // int8 Per Token.
    CHECK(f["sage_pv8"] == true && f["sage_pv8_all_rows"] == true);
    CHECK(f["i8_gemm_tier"] == "token");
    CHECK(f["ane_ffn"] == false && f["taomate"] == false);
  }
  {
    // Med: Sage without its int8 P*V (an artifact over a long clip,
    // 2026-10-23), int8 Accurate.
    TuningContext h8 = h;
    h8.loras["minimax-h3-turbo-8step"] = {"8-step",
                                          file("eight.safetensors")};
    const Json m = resolve_tuning(fl, h8, "balanced", false);
    CHECK(m["sage_attn"] == true && m["sage_pv8"] == false);
    CHECK(m["sage_pv8_all_rows"] == false);
    CHECK(m["i8_gemm"] == true && m["i8_gemm_tier"] == "accurate");
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
  CHECK(f["sage_pv8"] == false && f["sage_pv8_all_rows"] == false);
  CHECK(f["i8_gemm_tier"] == "accurate");
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
  CHECK(f["sage_pv8"] == true && f["sage_pv8_all_rows"] == true);
  CHECK(f["i8_gemm_tier"] == "token");
  CHECK(missing_loras(rf, r, "balanced").size() == 1);
  f = resolve_tuning(rf, r, "balanced", false);
  CHECK(f["sage_pv8"] == false && f["sage_pv8_all_rows"] == false);
  CHECK(f["i8_gemm_tier"] == "accurate");

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
  // ...with Sage, int8 Per Token and the VAE's int8 at Fast.
  CHECK(qv["sage_attn"] == true && qv["sage_pv8"] == false);
  CHECK(qv["i8_gemm"] == true && qv["i8_gemm_tier"] == "token");
  CHECK(qv["vae_i8_gemm"] == true && qv["vae_i8_gemm_tier"] == "fast");
  qv = resolve_tuning(q, qt, "balanced", false);
  CHECK(qv["sage_attn"] == false && qv["vae_i8_gemm"] == false);
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
  CHECK(tv["i8_gemm"] == true && tv["i8_gemm_tier"] == "token");
  CHECK(tv["sage_attn"] == true);
  CHECK(tv["vae_i8_gemm"] == true && tv["vae_i8_gemm_tier"] == "fast");
  // Before M5 none of them: Sage and the int8s need matrix cores.
  tv = resolve_tuning(t8, m4, "speed", false);
  CHECK(tv["sage_attn"] == false && tv["i8_gemm"] == false);
  CHECK(tv["i8_gemm_tier"] == "accurate" && tv["vae_i8_gemm"] == false);
  CHECK(tv["vae_i8_gemm_tier"] == "accurate");
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

// The int8 GEMMs' TIER (vpipe's i8_gemm_tier): a choice, offered only
// where the catalog names it -- the DiTs vpipe implements the tiers for,
// Qwen-Image 2.1 (and its Turbo), MiniMax H3, Krea 2, FLUX.2 klein and
// Z-Image; not FLUX.3 Klein's -- refining int8 GEMM, accurate unless
// asked; a value it does not list is not taken, a Mac without matrix
// cores keeps it at accurate, and so does the Neural Engine's
// feed-forward (vpipe runs the tiers off its route). A preset's table
// may name one: Fast runs Per Token (Qwen-Image 2.1 and its Turbo,
// MiniMax H3, Krea 2), Krea 2's Med too.
TEST(models, the_int8_gemms_tier_is_a_choice)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  TuningContext m5;
  m5.matrix_cores = true;
  TuningContext m4;
  const auto tier_of = [](const Json& opts) {
    for (const auto& o : opts["options"]) {
      if (o["key"] == "i8_gemm_tier") {
        return o;
      }
    }
    return Json();
  };
  for (const char* id : {"qwen-image-2.1", "qwen-image-2.1-turbo",
                         "minimax-h3-fl2va", "minimax-h3-ref2va",
                         "krea2-turbo", "flux2-klein-9b", "z-image-turbo"}) {
    const ModelEntry& m = *c->find(id);
    const Json o = tier_of(tuning_options(m, m5, "balanced", false));
    REQUIRE(o.is_object());
    CHECK(o["type"] == "choice" && o["available"] == true);
    CHECK(o["needs"] == "i8_gemm");
    CHECK((o["choices"] == Json::array({"accurate", "fast", "token",
                                        "token_plain"})));
    // Fast runs Per Token on all of them but FLUX.2 klein and Z-Image
    // (their tables); Krea 2's Med too (1.48x Accurate on the M5 Ultra,
    // the picture as close to bf16's); the rest Accurate.
    const std::string_view mid(id);
    const bool token = mid == "krea2-turbo";
    const bool fast_token = token || mid.starts_with("qwen-image") ||
                            mid.starts_with("minimax-h3");
    for (const char* p : {"speed", "balanced", "quality"}) {
      const std::string_view pref(p);
      const bool t = pref == "speed" ? fast_token
                                     : token && pref == "balanced";
      CHECK(resolve_tuning(m, m5, p, false)["i8_gemm_tier"] ==
            (t ? "token" : "accurate"));
      CHECK(resolve_tuning(m, m4, p, false)["i8_gemm_tier"] == "accurate");
    }
    Json v = resolve_tuning(m, m5, "balanced", false,
                            {{"i8_gemm_tier", "token"}});
    CHECK(v["i8_gemm"] == true && v["i8_gemm_tier"] == "token");
    v = resolve_tuning(m, m5, "balanced", false,
                       {{"i8_gemm_tier", "token_plain"}});
    CHECK(v["i8_gemm_tier"] == "token_plain");
    // A value it does not list is not taken: the preset's stands.
    const char* med = token ? "token" : "accurate";
    v = resolve_tuning(m, m5, "balanced", false,
                       {{"i8_gemm_tier", "fastest"}});
    CHECK(v["i8_gemm_tier"] == med);
    v = resolve_tuning(m, m5, "balanced", false, {{"i8_gemm_tier", 2}});
    CHECK(v["i8_gemm_tier"] == med);
    // Before M5: offered, not to be had.
    const Json o4 = tier_of(tuning_options(m, m4, "speed", false));
    CHECK(o4["available"] == false && o4["why"] == "needs-matrix-cores");
    v = resolve_tuning(m, m4, "speed", false, {{"i8_gemm_tier", "fast"}});
    CHECK(v["i8_gemm"] == false && v["i8_gemm_tier"] == "accurate");
    // The Neural Engine's feed-forward on: back to accurate, and said so.
    v = resolve_tuning(m, m5, "balanced", false,
                       {{"i8_gemm_tier", "token"}, {"ane_ffn", true}});
    CHECK(v["ane_ffn"] == true && v["i8_gemm_tier"] == "accurate");
    bool says = false;
    const Json listed = tuning_options(m, m5, "balanced", false);
    for (const auto& x : listed["options"]) {
      if (x["key"] == "ane_ffn") {
        const Json ex = jget(x, "excludes", Json::array());
        says = std::ranges::find(ex, Json("i8_gemm_tier")) != ex.end();
      }
    }
    CHECK(says);
  }
  // Families whose DiTs run the accurate tier alone are offered none.
  for (const char* id : {"flux3-klein", "yue2-3b", "flashvsr", "vosr",
                         "moss-tts-v1.5"}) {
    const ModelEntry& m = *c->find(id);
    CHECK(tier_of(tuning_options(m, m5, "balanced", false)).is_null());
    CHECK(!resolve_tuning(m, m5, "balanced", false,
                          {{"i8_gemm_tier", "token"}})
               .contains("i8_gemm_tier"));
  }
}

// SageAttention's int8 P*V (vpipe's `sage_pv8`): MiniMax H3's and the
// picture models', a switch that needs SageAttention and the matrix
// cores, on where a preset's table says (Krea 2's, MOSS-Image's and
// H3's Fast -- not H3's Med since 2026-10-23: an artifact over a long
// clip); H3's prompt and soundtrack rows on it too (`sage_pv8_all_rows`)
// a switch of H3's alone, needing it -- on H3's Fast.
TEST(models, sage_pv8_needs_sage)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  TuningContext m5;
  m5.matrix_cores = true;
  TuningContext m4;
  const auto option_of = [](const Json& opts, const char* key) {
    for (const auto& o : opts["options"]) {
      if (o["key"] == key) {
        return o;
      }
    }
    return Json();
  };
  const auto pv8_of = [&](const Json& opts) {
    return option_of(opts, "sage_pv8");
  };
  for (const char* id : {"minimax-h3-fl2va", "minimax-h3-ref2va",
                         "qwen-image-2.1", "qwen-image-2.1-turbo",
                         "krea2-turbo", "flux2-klein-9b", "flux3-klein",
                         "z-image-turbo", "moss-image-sft",
                         "moss-image-base"}) {
    const bool h3 = std::string_view(id).starts_with("minimax-h3");
    const ModelEntry& m = *c->find(id);
    const Json o = pv8_of(tuning_options(m, m5, "quality", false));
    REQUIRE(o.is_object());
    CHECK(o["type"] == "bool" && o["available"] == true);
    CHECK(o["needs"] == "sage_attn");
    const std::string_view mid(id);
    const bool fast = h3 || mid == "krea2-turbo" ||
                      mid.starts_with("moss-image");
    for (const char* p : {"speed", "balanced", "quality"}) {
      const std::string_view pref(p);
      const Json v = resolve_tuning(m, m5, p, false);
      CHECK(v["sage_pv8"] == (pref == "speed" && fast));
      if (h3) {
        CHECK(v["sage_pv8_all_rows"] == (pref == "speed"));
      }
      // Never before M5.
      CHECK(resolve_tuning(m, m4, p, false)["sage_pv8"] == false);
    }
    // With SageAttention on, it is taken.
    Json v = resolve_tuning(m, m5, "quality", false,
                            {{"sage_attn", true}, {"sage_pv8", true}});
    CHECK(v["sage_attn"] == true && v["sage_pv8"] == true);
    // Without SageAttention it is off, whatever is asked.
    v = resolve_tuning(m, m5, "quality", false,
                       {{"sage_pv8", true}, {"sage_attn", false}});
    CHECK(v["sage_pv8"] == false);
    // The rows switch: H3's, needing the int8 P*V.
    const Json rows =
        option_of(tuning_options(m, m5, "quality", false),
                  "sage_pv8_all_rows");
    CHECK(rows.is_object() == h3);
    if (h3) {
      CHECK(rows["needs"] == "sage_pv8");
      v = resolve_tuning(m, m5, "quality", false,
                         {{"sage_pv8", true}, {"sage_pv8_all_rows", true}});
      CHECK(v["sage_pv8_all_rows"] == true);
      v = resolve_tuning(m, m5, "quality", false,
                         {{"sage_pv8_all_rows", true}});
      CHECK(v["sage_pv8_all_rows"] == false);
    }
    // Before M5: offered, not to be had.
    const Json o4 = pv8_of(tuning_options(m, m4, "quality", false));
    CHECK(o4["available"] == false && o4["why"] == "needs-matrix-cores");
    CHECK(resolve_tuning(m, m4, "quality", false,
                         {{"sage_pv8", true}})["sage_pv8"] == false);
  }
  for (const char* id : {"flashvsr", "vosr", "yue2-3b"}) {
    const ModelEntry& m = *c->find(id);
    CHECK(pv8_of(tuning_options(m, m5, "balanced", false)).is_null());
    CHECK(!resolve_tuning(m, m5, "balanced", false, {{"sage_pv8", true}})
               .contains("sage_pv8"));
  }
}

// The CODEC's int8 (vpipe's vae-decode / vae-encode `i8_gemm` and its
// tier): offered for the models whose catalog names it, on at Fast in
// Fast's table alone, its tier Accurate | Fast and only with it on.
TEST(models, the_codecs_int8_is_a_switch_and_a_tier)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  TuningContext m5;
  m5.matrix_cores = true;
  TuningContext m4;
  const auto opt = [](const Json& opts, const char* key) {
    for (const auto& o : opts["options"]) {
      if (o["key"] == key) {
        return o;
      }
    }
    return Json();
  };
  for (const char* id : {"qwen-image-2.1", "qwen-image-2.1-turbo"}) {
    const ModelEntry& m = *c->find(id);
    const Json listed = tuning_options(m, m5, "speed", false);
    const Json on = opt(listed, "vae_i8_gemm");
    const Json tier = opt(listed, "vae_i8_gemm_tier");
    REQUIRE(on.is_object() && tier.is_object());
    CHECK(on["type"] == "bool" && on["available"] == true);
    CHECK(tier["type"] == "choice" && tier["needs"] == "vae_i8_gemm");
    CHECK((tier["choices"] == Json::array({"accurate", "fast"})));
    for (const char* p : {"speed", "balanced", "quality"}) {
      const bool fast = std::string_view(p) == "speed";
      const Json v = resolve_tuning(m, m5, p, false);
      CHECK(v["vae_i8_gemm"] == fast);
      CHECK(v["vae_i8_gemm_tier"] == (fast ? "fast" : "accurate"));
      const Json w = resolve_tuning(m, m4, p, false);
      CHECK(w["vae_i8_gemm"] == false && w["vae_i8_gemm_tier"] == "accurate");
    }
    Json v = resolve_tuning(m, m5, "balanced", false,
                            {{"vae_i8_gemm", true},
                             {"vae_i8_gemm_tier", "fast"}});
    CHECK(v["vae_i8_gemm"] == true && v["vae_i8_gemm_tier"] == "fast");
    // The DiT's own tier is another key: untouched.
    CHECK(v["i8_gemm_tier"] == "accurate");
    // A tier the codec has no word for, back to accurate.
    v = resolve_tuning(m, m5, "balanced", false,
                       {{"vae_i8_gemm", true},
                        {"vae_i8_gemm_tier", "token"}});
    CHECK(v["vae_i8_gemm_tier"] == "accurate");
    // Before M5: offered, not to be had.
    CHECK(opt(tuning_options(m, m4, "speed", false),
              "vae_i8_gemm")["why"] == "needs-matrix-cores");
    v = resolve_tuning(m, m4, "speed", false, {{"vae_i8_gemm", true}});
    CHECK(v["vae_i8_gemm"] == false);
  }
  // FLUX.2's 2D VAE -- FLUX.2 klein's, FLUX.3's, Z-Image's, MOSS-Image's,
  // Fibo 2.0's -- takes it too (vpipe eeec3c5c): offered, Fast at Fast on
  // an M5, nothing before one.
  for (const char* id : {"flux2-klein-9b", "flux3-klein", "z-image-turbo",
                         "moss-image-sft", "moss-image-base",
                         "fibo2-edit-turbo"}) {
    const ModelEntry& m = *c->find(id);
    const bool edit = std::string_view(id) == "fibo2-edit-turbo";
    const Json listed = tuning_options(m, m5, "speed", edit);
    CHECK(opt(listed, "vae_i8_gemm")["available"] == true);
    CHECK(opt(listed, "vae_i8_gemm_tier")["needs"] == "vae_i8_gemm");
    for (const char* p : {"speed", "balanced", "quality"}) {
      const bool fast = std::string_view(p) == "speed";
      const Json v = resolve_tuning(m, m5, p, edit);
      CHECK(v["vae_i8_gemm"] == fast);
      CHECK(v["vae_i8_gemm_tier"] == (fast ? "fast" : "accurate"));
      CHECK(resolve_tuning(m, m4, p, edit)["vae_i8_gemm"] == false);
    }
  }
  // A codec without vpipe's int8 is offered none.
  for (const char* id : {"krea2-turbo", "minimax-h3-fl2va"}) {
    const ModelEntry& m = *c->find(id);
    CHECK(opt(tuning_options(m, m5, "balanced", false),
              "vae_i8_gemm").is_null());
    CHECK(!resolve_tuning(m, m5, "balanced", false, {{"vae_i8_gemm", true}})
               .contains("vae_i8_gemm"));
  }
}

// Fibo 2.0 Edit Turbo (vpipe 17c53d63): an edit model alone -- gated,
// non-commercial, a family of its own -- in its own 4 steps at every
// preference, the int8 tiers on Fast and Med on an M5, nothing of the
// matrix cores before one (the ANE lent there, as to every picture DiT).
TEST(models, fibo2_edits_in_four_steps)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* m = c->find("fibo2-edit-turbo");
  REQUIRE(m);
  CHECK(m->family == "fibo2");
  CHECK(m->has(Capability::ImageEdit));
  CHECK(!m->has(Capability::TextToImage));
  CHECK(m->hf_path == "briaai/Fibo-2.0-edit-turbo");
  CHECK(m->gated);
  CHECK(m->license == "cc-by-nc-4.0");
  CHECK(m->min_ram_gb == 16);
  TuningContext m5;
  m5.matrix_cores = true;
  TuningContext m4;
  for (const char* p : {"speed", "balanced", "quality"}) {
    const Json v = resolve_tuning(*m, m5, p, true);
    CHECK(v["steps"] == 4);
    CHECK(v["i8_gemm"] == (std::string_view(p) != "quality"));
    CHECK(v["sage_attn"] == (std::string_view(p) != "quality"));
  }
  const Json fast = resolve_tuning(*m, m5, "speed", true);
  CHECK(fast["i8_gemm_tier"] == "token" && fast["sage_pv8"] == true);
  const Json before = resolve_tuning(*m, m4, "speed", true);
  CHECK(before["i8_gemm"] == false && before["sage_attn"] == false);
  bool found = false;
  for (const auto& f : c->families()) {
    if (f.id == "fibo2") {
      found = f.members.size() == 1 &&
              f.members[0].model == "fibo2-edit-turbo";
    }
  }
  CHECK(found);
}

// The LoRA list: eight on at most, the first in list order (vpipe's
// slots; seven with HyperFlow); strengths held to 0..2; a file that is
// gone kept, off. An older "turbo" file goes first, in the catalog's
// place.
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
  Json list = Json::array({
      {{"path", a}, {"scale", 0.8}, {"on", true}},
      {{"path", b}, {"on", true}, {"scale", 5.0}},
  });
  // Seven more, all asked on: nine in all.
  for (int i = 0; i < 7; ++i) {
    list.push_back({{"path", touch(std::format("s{}.safetensors", i)
                                       .c_str())},
                    {"on", true}});
  }
  list.push_back({{"path", (dir / "gone.safetensors").string()},
                  {"on", true}});
  Json over = {{"loras", list}};
  Json v = resolve_tuning(*h3, ctx, "balanced", false, over);
  CHECK(v["steps"] == 12);                 // no Turbo LoRA in the list
  Json l = v["loras"];
  REQUIRE(l.size() == 10);
  CHECK(l[0]["on"] == true && l[0]["scale"] == 0.8);
  CHECK(l[1]["on"] == true && l[1]["scale"] == 2.0);  // held to 0..2
  auto on = [](const Json& x) {
    int n = 0;
    for (const auto& e : x) {
      n += e["on"].get<bool>() ? 1 : 0;
    }
    return n;
  };
  CHECK(on(l) == kLoraSlots);              // eight slots: the first eight
  CHECK(l[7]["on"] == true && l[8]["on"] == false);
  CHECK(l[9]["missing"] == true && l[9]["on"] == false);
  // HyperFlow takes one of the eight.
  over["hyperflow"] = true;
  l = resolve_tuning(*h3, ctx, "balanced", false, over)["loras"];
  CHECK(on(l) == kLoraSlots - 1);
  CHECK(l[6]["on"] == true && l[7]["on"] == false);
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

// A folder offered for a model -- a link -- CHECKED (DESIGN §8b): the
// model found in or around it, the right one, whole. What a person picks
// by mistake -- the parent, a part, another model, the other partition,
// a copy cut short, a shard missing -- is found and said.
// DRAW THINGS CHECKPOINTS offered for a model are checked as its PARTS
// (models/drawthings.h): Qwen-Image 2.1's and MiniMax H3's DiTs, their
// Qwen3-VL text encoders (8B, 32B: told by width) and their VAEs, told as
// vpipe tells them -- another model's part, a store of nothing vpipe
// reads, one cut short, rotated 8-bit weights, a file named as another
// release refused -- the Turbo's DiT is told by its name alone, the
// base's tensors; a model takes only the parts its catalog names
// (Ref2VA's DiT is its own); a PyTorch .ckpt is not one; Draw Things'
// Models folder looked through for every part, a part with several files
// (an f16 beside an 8-bit) named for one to be chosen, the others
// linked. Custom's panel files a DiT as one. The store keeps the parts
// across a reload.
TEST(models, a_draw_things_model_is_checked)
{
  namespace fs = std::filesystem;
  using test::DrawThingsFixture;
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto* qi = c->find("qwen-image-2.1");
  const auto* qt = c->find("qwen-image-2.1-turbo");
  const auto* fl = c->find("minimax-h3-fl2va");
  const auto* rf = c->find("minimax-h3-ref2va");
  const auto* krea = c->find("krea2-turbo");
  REQUIRE(qi && qt && fl && rf && krea);
  const auto root = test::temp_dir("models-drawthings");
  const fs::path dt = root / "Models";
  fs::create_directories(dt);
  const fs::path dit = dt / "qwen_image_2.1_i8x.ckpt";
  const fs::path enc = dt / "qwen_3_vl_8b_instruct_i8x.ckpt";
  const fs::path vae = dt / "qwen_image_2.1_vae_f16.ckpt";
  const fs::path h3v = dt / "minimax_h3_vae_f16.ckpt";
  const fs::path junk = dt / "sd_v1.5_f16.ckpt";
  const fs::path tdit = dt / "qwen_image_2.1_turbo_i8x.ckpt";
  REQUIRE(DrawThingsFixture::qwen_image_21().write(tdit));
  REQUIRE(DrawThingsFixture::qwen_image_21().write(dit));
  REQUIRE(DrawThingsFixture::qwen3_vl(4096).write(enc));
  REQUIRE(DrawThingsFixture::qwen_image_21_vae().write(vae));
  REQUIRE(DrawThingsFixture::minimax_h3_vae().write(h3v));
  REQUIRE(DrawThingsFixture::unknown().write(junk));
  const fs::path big = root / "qwen_3_vl_32b_i8x.ckpt";
  REQUIRE(DrawThingsFixture::qwen3_vl(5120).write(big));

  // What each store is.
  DrawThingsStore s = read_drawthings(dit);
  CHECK(s.store && s.error.empty() && s.part == "dit");
  CHECK(s.family == "qwen-image-21");
  CHECK(s.tensors == 4 && s.eight_bit == 4 && !s.cut_short());
  s = read_drawthings(enc);
  CHECK(s.part == "encoder" && s.family == "qwen3-vl" && s.width == 4096);
  CHECK(read_drawthings(big).width == 5120);
  s = read_drawthings(vae);
  CHECK(s.part == "vae" && s.family == "qwen-image-21-vae");
  s = read_drawthings(h3v);
  CHECK(s.part == "vae" && s.family == "minimax-h3-vae");
  s = read_drawthings(junk);
  CHECK(s.store && s.part.empty());

  // Each part, for the model it is.
  ModelCheck ok = check_model(*qi, dit, &*c);
  CHECK(ok.usable && ok.confirmed && ok.path == dit);
  CHECK((ok.parts == std::map<std::string, fs::path>{{"dit", dit}}));
  CHECK(note_keys(ok).front() == "core.check_drawthings_dit");
  CHECK(has_key(ok, "core.check_drawthings_eight_bit"));
  CHECK(has_key(ok, "core.check_drawthings_configs"));
  CHECK(ok.to_json()["parts"]["dit"] == dit.string());
  const ModelCheck e8 = check_model(*qi, enc, &*c);
  CHECK(e8.usable && e8.parts.at("encoder") == enc);
  CHECK(note_keys(e8).front() == "core.check_drawthings_encoder");
  CHECK(!has_key(e8, "core.check_drawthings_eight_bit"));
  const ModelCheck v = check_model(*qi, vae, &*c);
  CHECK(v.usable && v.parts.at("vae") == vae);
  CHECK(has_key(v, "core.check_drawthings_texture_fix"));
  CHECK(check_model(*fl, big, &*c).usable);
  CHECK(check_model(*fl, h3v, &*c).parts.at("vae") == h3v);
  // The Turbo takes the shared encoder and VAE, and its own DiT -- by its
  // name, the tensors being the base's; Ref2VA no DiT.
  CHECK(check_model(*qt, enc, &*c).usable);
  CHECK(check_model(*qt, vae, &*c).usable);
  const ModelCheck td = check_model(*qt, tdit, &*c);
  CHECK(td.usable && td.parts.at("dit") == tdit);
  CHECK(note_keys(check_model(*qt, dit, &*c)).front() ==
        "core.check_drawthings_unnamed");
  CHECK(note_keys(check_model(*qi, tdit, &*c)).front() ==
        "core.check_drawthings_release");
  CHECK(check_model(*rf, big, &*c).usable);
  CHECK(check_model(*rf, h3v, &*c).usable);
  // Another model's part: by what it is.
  const ModelCheck w = check_model(*fl, enc, &*c);
  CHECK(note_keys(w).front() == "core.check_drawthings_wrong");
  CHECK(w.notes.front().message["args"]["found"] ==
        "Qwen3-VL 8B text encoder");
  CHECK(note_keys(check_model(*qi, big, &*c)).front() ==
        "core.check_drawthings_wrong");
  CHECK(note_keys(check_model(*qi, h3v, &*c)).front() ==
        "core.check_drawthings_wrong");
  CHECK(note_keys(check_model(*fl, dit, &*c)).front() ==
        "core.check_drawthings_wrong");
  CHECK(note_keys(check_model(*krea, dit, &*c)).front() ==
        "core.check_drawthings_not_for_model");
  CHECK(note_keys(check_model(*qi, junk, &*c)).front() ==
        "core.check_drawthings_other");
  // H3's: FL2VA's DiT, not a file named as Ref2VA's.
  const fs::path fd = root / "minimax_h3_fl2va_i8x.ckpt";
  const fs::path rd = root / "minimax_h3_ref2va_i8x.ckpt";
  REQUIRE(DrawThingsFixture::minimax_h3().write(fd));
  REQUIRE(DrawThingsFixture::minimax_h3().write(rd));
  CHECK(check_model(*fl, fd, &*c).usable);
  CHECK(note_keys(check_model(*fl, rd, &*c)).front() ==
        "core.check_drawthings_release");
  // Cut short; rotated 8-bit weights; the raw codec, no 8-bit note.
  auto cut = DrawThingsFixture::qwen_image_21();
  cut.short_by = 100;
  const fs::path cq = root / "cut.ckpt";
  REQUIRE(cut.write(cq));
  CHECK(read_drawthings(cq).cut_short());
  CHECK(note_keys(check_model(*qi, cq, &*c)).front() ==
        "core.check_cut_short");
  auto rot = DrawThingsFixture::qwen_image_21();
  rot.rotated = true;
  const fs::path rq = root / "rotated.ckpt";
  REQUIRE(rot.write(rq));
  CHECK(note_keys(check_model(*qi, rq, &*c)).front() ==
        "core.check_drawthings_rotated");
  auto raw = DrawThingsFixture::qwen_image_21();
  raw.codec = DrawThingsFixture::kRaw;
  const fs::path wq = root / "raw.ckpt";
  REQUIRE(raw.write(wq));
  const ModelCheck rawc = check_model(*qi, wq, &*c);
  CHECK(rawc.usable && !has_key(rawc, "core.check_drawthings_eight_bit"));
  // A PyTorch .ckpt (a zip): not a store, not a model's part.
  const fs::path pt = root / "LQ_proj_in.ckpt";
  std::ofstream(pt, std::ios::binary) << "PK\x03\x04 not sqlite";
  CHECK(!looks_like_drawthings(pt));
  const ModelCheck ptc = check_model(*qi, pt, &*c);
  CHECK(!ptc.usable && ptc.parts.empty());

  // Draw Things' Models chosen: every Qwen-Image part in it, found.
  ModelCheck in = check_model(*qi, dt, &*c);
  CHECK(in.usable && in.path == dt && in.moved == "down");
  CHECK((in.parts == std::map<std::string, fs::path>{
                         {"dit", dit}, {"encoder", enc}, {"vae", vae}}));
  CHECK(!has_key(in, "core.check_found_inside"));  // each part says it
  CHECK(in.notes.size() == 6);
  // ...H3 FL2VA's, its VAEs alone: that file.
  ModelCheck hv = check_model(*fl, dt, &*c);
  CHECK(hv.usable && hv.path == h3v);
  CHECK(has_key(hv, "core.check_found_inside"));
  CHECK((hv.parts == std::map<std::string, fs::path>{{"vae", h3v}}));
  // ...the Turbo's: its own DiT beside the shared two.
  ModelCheck tin = check_model(*qt, dt, &*c);
  CHECK(tin.usable && tin.parts.size() == 3);
  CHECK(tin.parts.at("dit") == tdit);
  // ...two DiTs (an f16 beside the 8-bit): which, asked -- the encoder
  // and VAE linked.
  const fs::path f16 = dt / "qwen_image_2.1_f16.ckpt";
  REQUIRE(DrawThingsFixture::qwen_image_21().write(f16));
  ModelCheck two = check_model(*qi, dt, &*c);
  CHECK(two.usable);
  CHECK((two.parts == std::map<std::string, fs::path>{
                          {"encoder", enc}, {"vae", vae}}));
  CHECK(note_keys(two).front() == "core.check_drawthings_ambiguous");
  CHECK(two.notes.front().level == ModelCheck::Level::Warning);
  // ...nothing but the two: refused, which asked.
  const fs::path only = root / "Two";
  fs::create_directories(only);
  fs::copy_file(dit, only / dit.filename());
  fs::copy_file(f16, only / f16.filename());
  ModelCheck none2 = check_model(*qi, only, &*c);
  CHECK(!none2.usable && none2.parts.empty());
  CHECK(note_keys(none2).front() == "core.check_drawthings_ambiguous");
  // A folder of neither: the download's own refusal.
  fs::create_directories(root / "empty");
  const ModelCheck none = check_model(*qi, root / "empty", &*c);
  CHECK(!none.usable && none.parts.empty());

  // Custom's panel: a DiT is one; an encoder or a VAE is not a weight
  // it takes.
  CHECK(checkpoint_kind(dit) == "dit");
  CHECK(checkpoint_kind(enc).empty());
  CHECK(checkpoint_kind(vae).empty());
  CHECK(checkpoint_kind(pt).empty());
  const Json k = classify_checkpoint(dit, Json::array());
  CHECK(k["kind"] == "dit" && k["family"] == "qwen-image-21");

  // The store keeps the parts linked apart, beside a model's link,
  // across a reload; a reader of strings alone keeps its links.
  const fs::path links = root / "model-links.json";
  {
    ModelStore store({root / "store"}, links);
    REQUIRE_OK(store.link_part(*qi, "dit", dit));
    REQUIRE_OK(store.link_part(*qi, "encoder", enc));
    REQUIRE_OK(store.link_part(*qi, "vae", vae));
    CHECK(store.part_links_of(qi->id).size() == 3);
    const InstallInfo i = store.info(*qi);
    CHECK(i.part("dit") == dit && i.part("encoder") == enc);
    CHECK(i.parts.at("vae").present);
    CHECK(i.state == InstallState::Missing);
    CHECK(!store.link_part(*qi, "dit", root / "gone.ckpt").ok());
  }
  {
    ModelStore store({root / "store"}, links);
    CHECK(store.part_links_of(qi->id).at("vae") == vae);
    fs::remove(vae);
    store.rescan();
    const InstallInfo i = store.info(*qi);
    CHECK(!i.parts.at("vae").present && i.part("vae").empty());
    CHECK(i.part("dit") == dit);
    store.unlink_parts(qi->id);
    CHECK(store.part_links_of(qi->id).empty());
    CHECK(store.info(*qi).parts.empty());
  }
}

// FISH AUDIO DRAMA 3: a family of its own, its two checkpoints -- the
// FP8 release and its int4 -- each a speech model of 16 speakers that
// takes no length, told apart by how their weights are stored.
// DFlash 2's drafters for Qwen3.8 27B and for Flash-Next are one class,
// told apart by their width -- each model names its own, and a link of
// one for the other is refused.
TEST(models, the_dflash_drafters_are_told_apart_by_their_width)
{
  namespace fs = std::filesystem;
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto* d27 = c->find("qwen3.8-27b-dflash2");
  const auto* dfn = c->find("qwen3.8-flash-next-dflash2");
  const auto* next = c->find("qwen3.8-flash-next");
  REQUIRE(d27 && dfn && next);
  CHECK(dfn->role == "drafter" && dfn->hf_path ==
                                      "incoai/Qwen3.8-Flash-Next-DFlash2");
  CHECK(jget<std::string>(jget(next->engine, "vpipe", Json::object()),
                          "dflash_drafter", "") == dfn->id);
  CHECK(jget(jget(jget(dfn->engine, "vpipe", Json::object()), "held_gb",
                  Json::object()), "8", 0.0) == 0.6);
  const auto root = test::temp_dir("models-dflash");
  auto fake = [&](const fs::path& dir, int width) {
    fs::create_directories(dir);
    std::ofstream(dir / "config.json")
        << R"({"architectures": ["DFlash2DraftModel"], "model_type": )"
           R"("qwen3", "dflash_config": {"block_size": 8}, )"
           R"("hidden_size": )" << width << "}";
    write_safetensors(dir / "model.safetensors", {{"fc.weight", {4, 4}}});
  };
  const fs::path a = root / "incoai" / "Qwen3.8-Flash-Next-DFlash2";
  const fs::path b = root / "incoai" / "Qwen3.8-27B-DFlash2";
  fake(a, 2560);
  fake(b, 5120);
  CHECK(check_model(*dfn, a, &*c).confirmed);
  CHECK(check_model(*d27, b, &*c).confirmed);
  const ModelCheck wrong = check_model(*d27, a, &*c);
  CHECK(!wrong.usable);
  CHECK(note_keys(wrong).front() == "core.check_wrong_model");
  CHECK(!check_model(*dfn, b, &*c).usable);
}

TEST(models, drama3_s_two_checkpoints_are_told_apart)
{
  namespace fs = std::filesystem;
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto& fams = c->families();
  const auto fam = std::ranges::find(fams, std::string("drama3"),
                                     &Family::id);
  REQUIRE(fam != fams.end());
  CHECK(fam->features == std::vector<std::string>{"speech-gen"});
  REQUIRE(fam->members.size() == 2);
  const auto* fp8 = c->find("drama-3");
  const auto* int4 = c->find("drama-3-int4");
  REQUIRE(fp8 && int4);
  for (const auto* m : {fp8, int4}) {
    CHECK(m->has(Capability::TextToSpeech));
    Json j;
    to_json(j, *m);
    CHECK(jget(j, "speakers", 0) == 16);
    CHECK(!jget(j, "speech_length", true));
  }
  Json moss;
  to_json(moss, *c->find("moss-tts-v1.5"));
  CHECK(jget(moss, "speakers", 0) == 1 && jget(moss, "speech_length", false));
  // Auto speaks in a voice with Drama 3 first, the release ahead.
  CHECK(fp8->min_ram_gb > int4->min_ram_gb);

  const auto root = test::temp_dir("models-drama3");
  auto fake = [&](const fs::path& dir, const char* method) {
    fs::create_directories(dir);
    std::ofstream(dir / "config.json")
        << R"({"model_type": "fish_omni", "quantization_config": )"
           R"({"quant_method": ")" << method << R"("}})";
    write_safetensors(dir / "model-00001-of-00001.safetensors",
                      {{"lm.weight", {4, 4}}});
  };
  const fs::path a = root / "fishaudio" / "drama-3-int4";
  fake(a, "drama3-int4");
  CHECK(check_model(*int4, a, &*c).usable);
  const ModelCheck wrong = check_model(*fp8, a, &*c);
  CHECK(!wrong.usable);
  CHECK(note_keys(wrong).front() == "core.check_wrong_model");
  const fs::path b = root / "fishaudio" / "drama-3";
  fake(b, "bsgemm-moe");
  CHECK(check_model(*fp8, b, &*c).usable);
  CHECK(!check_model(*int4, b, &*c).usable);
}

TEST(models, a_linked_folder_is_checked)
{
  namespace fs = std::filesystem;
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto* krea = c->find("krea2-turbo");
  REQUIRE(krea);
  const auto root = test::temp_dir("models-check");
  const fs::path pipe = root / "krea" / "Krea-2-Turbo";
  fake_pipeline(pipe);

  ModelCheck ok = check_model(*krea, pipe, &*c);
  CHECK(ok.usable && ok.confirmed);
  CHECK(ok.path == pipe);
  CHECK(ok.identity == "Krea2Pipeline");
  CHECK(ok.weight_files == 4);
  CHECK(ok.moved.empty());
  CHECK(note_keys(ok).front() == "core.check_confirmed");
  CHECK(ok.to_json()["notes"][0]["level"] == "info");

  // Its parent chosen, or a part of it, or one of its files: the
  // pipeline.
  ModelCheck down = check_model(*krea, root, &*c);
  CHECK(down.usable && down.path == pipe && down.moved == "down");
  CHECK(has_key(down, "core.check_found_inside"));
  ModelCheck up = check_model(*krea, pipe / "transformer", &*c);
  CHECK(up.usable && up.path == pipe && up.moved == "up");
  CHECK(has_key(up, "core.check_found_above"));
  CHECK(check_model(*krea, pipe / "transformer" /
                               "dit-00001-of-00002.safetensors", &*c)
            .path == pipe);

  // Another model: refused, named.
  const fs::path klein = root / "bfl" / "FLUX.2-klein-9B";
  fake_pipeline(klein, "Flux2KleinPipeline", "Flux2Transformer2DModel");
  ModelCheck wrong = check_model(*krea, klein, &*c);
  CHECK(!wrong.usable);
  CHECK(note_keys(wrong).front() == "core.check_wrong_model");
  CHECK(jget<std::string>(wrong.error()["args"], "found", "") ==
        "Flux2KleinPipeline");
  // Two copies inside: which one is the person's to say -- unless one is
  // named as the repo is.
  const fs::path both = root / "both";
  fake_pipeline(both / "a");
  fake_pipeline(both / "b");
  CHECK(note_keys(check_model(*krea, both, &*c)).front() ==
        "core.check_several");
  fake_pipeline(both / "Krea-2-Turbo");
  CHECK(check_model(*krea, both, &*c).path == both / "Krea-2-Turbo");
  // Nothing of it anywhere in it.
  const fs::path empty = root / "empty";
  fs::create_directories(empty / "x");
  CHECK(note_keys(check_model(*krea, empty, &*c)).front() ==
        "core.check_not_there");

  // Not whole: a part cut short, a shard missing, a tensor of the index
  // in none, a component missing, a download unfinished.
  const fs::path broken = root / "broken" / "Krea-2-Turbo";
  fake_pipeline(broken);
  write_safetensors(broken / "vae" / "diffusion_pytorch_model.safetensors",
                    {{"decoder.conv_in.weight", {16, 16, 3, 3}}}, 100);
  fs::remove(broken / "transformer" / "dit-00002-of-00002.safetensors");
  write_safetensors(broken / "transformer" /
                        "dit-00001-of-00002.safetensors",
                    {{"blocks.0.attn.to_q.weight", {256, 256}}});
  fs::remove_all(broken / "text_encoder");
  std::ofstream(broken / "transformer" / "dit-3.safetensors.part") << "x";
  ModelCheck bad = check_model(*krea, broken, &*c);
  CHECK(!bad.usable);
  for (const char* k : {"core.check_cut_short", "core.check_parts_missing",
                        "core.check_tensors_missing",
                        "core.check_component_missing",
                        "core.check_unfinished"}) {
    CHECK(has_key(bad, k));
  }
  // A Comfy-Org repack: not as its download is, run as it is.
  const fs::path repack = root / "repack";
  write_safetensors(repack / "diffusion_models" / "krea2.safetensors",
                    {{"x.weight", {64, 64}}});
  ModelCheck rp = check_model(*krea, repack, &*c);
  CHECK(rp.usable && !rp.confirmed);
  CHECK(note_keys(rp).front() == "core.check_unconfirmed");

  // One repo, two models: FL2VA is not its Ref2VA; the repo chosen, its
  // own folder in it.
  const auto* fl = c->find("minimax-h3-fl2va");
  REQUIRE(fl);
  const fs::path h3 = root / "MiniMax-H3";
  fake_pipeline(h3 / "FL2VA", "MiniMaxH3Pipeline", "MiniMaxH3DiTModel");
  fake_pipeline(h3 / "Ref2VA", "MiniMaxH3Pipeline", "MiniMaxH3DiTModel");
  CHECK(note_keys(check_model(*fl, h3 / "Ref2VA", &*c)).front() ==
        "core.check_wrong_part");
  ModelCheck repo = check_model(*fl, h3, &*c);
  CHECK(repo.usable && repo.path == h3 / "FL2VA");

  // A LoRA: a file whose tensors are a LoRA's -- or the largest in a
  // folder of them; a TAE's are not.
  const auto* lora = c->find("minimax-h3-turbo-4step");
  REQUIRE(lora);
  const fs::path loras = root / "loras";
  write_safetensors(loras / "turbo.safetensors",
                    {{"blocks.0.attn.to_q.lora_A.weight", {8, 64}},
                     {"blocks.0.attn.to_q.lora_B.weight", {64, 8}}});
  write_safetensors(loras / "small.safetensors",
                    {{"x.lora_A.weight", {2, 2}}});
  ModelCheck lc = check_model(*lora, loras, &*c);
  CHECK(lc.usable && lc.confirmed);
  CHECK(lc.path == loras / "turbo.safetensors");
  write_safetensors(root / "tae.safetensors",
                    {{"decoder.1.weight", {8, 8}}});
  CHECK(note_keys(check_model(*lora, root / "tae.safetensors", &*c))
            .front() == "core.check_wrong_tensors");
}

// A COMPRESSED copy (DESIGN §8b): what each width would take, from the
// headers -- a part's matrices at bits/8 + 4/64 bytes a weight, its norms
// and embeddings as they are, the rest hard-linked -- and the store
// finding the copy, marked, before the roots.
TEST(models, a_compressed_copy_is_estimated_and_found)
{
  namespace fs = std::filesystem;
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const auto* krea = c->find("krea2-turbo");
  REQUIRE(krea);
  REQUIRE(krea->compress.size() == 2);
  CHECK(krea->compress[0].target == "dit");
  CHECK(krea->compress[1].target == "text_encoder");
  const auto* klein = c->find("flux2-klein-9b");
  REQUIRE(klein && klein->compress.size() == 2);
  CHECK(jget(klein->compress[0].options_4bit, "awq", false));
  // Only the models whose engine runs the copy.
  CHECK(c->find("yue2-3b")->compress.empty());

  const auto root = test::temp_dir("models-compress");
  const fs::path dir = root / krea->hf_path;
  fake_pipeline(dir);
  CHECK(compress_part_folder(dir, "dit") == "transformer");
  CHECK(compress_part_folder(dir, "text_encoder") == "text_encoder");
  const auto e8 = estimate_compressed(dir, krea->compress, 8);
  const auto e4 = estimate_compressed(dir, krea->compress, 4);
  CHECK(e8.original > 0);
  CHECK(e4.total < e8.total && e8.total < e8.original);
  // The DiT's two 256 x 256 at 8 bits; its norm, the encoder's
  // embeddings as they are; its 128 x 64 at 8 bits.
  const std::uint64_t q8 = 2 * 256 * 256 * 17 / 16 + 256 * 2 +
                           512 * 64 * 2 + 128 * 64 * 17 / 16;
  const std::uint64_t meta = e8.written - q8;
  CHECK(meta < 2000);  // the two configs and the index, as they are

  // The store: a copy marked for it, before the roots; one marked for
  // another model, or not marked, is nothing.
  ModelStore store({root});
  CHECK(store.info(*krea).compressed_bits == 0);
  const fs::path copy = store.compressed_dir(krea->id, 4);
  CHECK(copy == root / "compressed" / "krea2-turbo-4bit");
  fake_pipeline(copy);
  store.rescan();
  CHECK(store.info(*krea).dir == dir);
  std::ofstream(copy / ModelStore::kCompressedMarker)
      << Json{{"model", "qwen-image-2.1"}, {"bits", 4}}.dump();
  store.rescan();
  CHECK(store.info(*krea).dir == dir);
  std::ofstream(copy / ModelStore::kCompressedMarker)
      << Json{{"model", krea->id}, {"bits", 4}}.dump();
  store.rescan();
  const auto info = store.info(*krea);
  CHECK(info.compressed_bits == 4);
  CHECK(info.dir == copy);
  CHECK(info.state == InstallState::Installed);

  // A compress block on a single-file model, or a part with no target,
  // is not taken.
  Json one = {{"models", Json::array(
                             {{{"id", "v"}, {"hf_path", "o/v"},
                               {"file", "v.safetensors"},
                               {"compress", {{"parts", Json::array(
                                   {{{"target", "dit"}}})}}}}})}};
  CHECK(!Catalog::parse(one).ok());
  Json bare = {{"models", Json::array(
                              {{{"id", "v"}, {"hf_path", "o/v"},
                                {"compress", {{"parts", Json::array(
                                    {Json::object()})}}}}})}};
  CHECK(!Catalog::parse(bare).ok());
}

// MOSS-IMAGE's Custom (vpipe 3b34e70a): its steps by preset -- the
// reference's 50 at Fine, 30 and 20 below, each two forwards under CFG --
// its guidance, an option of its own on the config stage, and its shift,
// which its config stage takes (`config_shift`, 5) rather than a
// scheduler; Base and SFT alike, and in Capabilities' tree.
TEST(models, moss_image_offers_its_steps_guidance_and_shift)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  TuningContext m5;
  m5.matrix_cores = true;
  const auto opt = [](const Json& opts, const char* key) {
    for (const auto& o : opts["options"]) {
      if (o["key"] == key) {
        return o;
      }
    }
    return Json();
  };
  for (const char* id : {"moss-image-sft", "moss-image-base"}) {
    const ModelEntry* m = c->find(id);
    REQUIRE(m);
    CHECK(m->family == "moss-image");
    CHECK(m->has(Capability::TextToImage));
    CHECK(!m->has(Capability::ImageEdit));
    CHECK(m->hf_path.starts_with("OpenMOSS-Team/MOSS-Image-"));
    // Streamed, as the other DiTs: no larger Mac asked for.
    CHECK(m->min_ram_gb == 16);
    const Json listed = tuning_options(*m, m5, "balanced", false);
    const Json shift = opt(listed, "shift");
    REQUIRE(shift.is_object());
    CHECK(shift["available"] == true);
    const Json g = opt(listed, "guidance_scale");
    REQUIRE(g.is_object());
    CHECK(g["type"] == "real");
    CHECK(jget(g, "declared", false));
    CHECK(opt(listed, "i8_gemm_tier").is_object());
    // Its VAE is FLUX.2's small decoder: the codec's int8 too (eeec3c5c).
    CHECK(opt(listed, "vae_i8_gemm").is_object());
    const Json fine = resolve_tuning(*m, m5, "quality", false);
    const Json med = resolve_tuning(*m, m5, "balanced", false);
    const Json fast = resolve_tuning(*m, m5, "speed", false);
    CHECK(fine["steps"] == 50 && med["steps"] == 30 && fast["steps"] == 18);
    CHECK(fine["shift"] == 5.0);
    CHECK(med["guidance_scale"] == 3.0);
    CHECK(fine["i8_gemm"] == false && med["i8_gemm"] == true);
    // Fast on an M5: Sage with its int8 P*V, int8 Per Token, no Sol;
    // without matrix cores its 18 steps with Sol in their place.
    CHECK(fast["sol_attn"] == false && fast["sage_attn"] == true);
    CHECK(fast["sage_pv8"] == true && fast["i8_gemm"] == true);
    CHECK(fast["i8_gemm_tier"] == "token");
    const Json old = resolve_tuning(*m, TuningContext{}, "speed", false);
    CHECK(old["steps"] == 18 && old["sol_attn"] == true);
    CHECK(old["sol_tau"] == 0.0 && old["sage_attn"] == false);
    const Json moved = resolve_tuning(*m, m5, "balanced", false,
                                      {{"guidance_scale", 5.0},
                                       {"shift", 4.0}});
    CHECK(moved["guidance_scale"] == 5.0 && moved["shift"] == 4.0);
  }
  bool listed = false;
  for (const auto& f : c->families()) {
    listed = listed || f.id == "moss-image";
  }
  CHECK(listed);
}

TEST(models, a_helpers_context_is_its_own_as_memory_holds_it)
{
  namespace fs = std::filesystem;
  const auto root = test::temp_dir("models-context");
  // A model that also sees, a hybrid: its text_config's, its K/V the
  // full-attention layers' alone (Qwen3.8 27B: 16 of 64, 64 KB a token).
  const fs::path vl = root / "vl";
  fs::create_directories(vl);
  Json types = Json::array();
  for (int i = 0; i < 64; ++i) {
    types.push_back(i % 4 == 3 ? "full_attention" : "linear_attention");
  }
  std::ofstream(vl / "config.json") << Json{
      {"model_type", "qwen3_5"},
      {"text_config", {{"max_position_embeddings", 262144},
                       {"num_hidden_layers", 64},
                       {"layer_types", types},
                       {"num_attention_heads", 24},
                       {"num_key_value_heads", 4},
                       {"head_dim", 256}}}}.dump();
  const LanguageContext big = language_context(vl);
  CHECK(big.tokens == 262144);
  CHECK(big.kv_bytes == 65536);
  // By its interval (Qwen3.5 9B: 8 of 32); a dense one's every layer, its
  // head the hidden size's share.
  const fs::path lm = root / "lm";
  fs::create_directories(lm);
  std::ofstream(lm / "config.json") << Json{
      {"max_position_embeddings", 262144}, {"num_hidden_layers", 32},
      {"full_attention_interval", 4}, {"num_key_value_heads", 4},
      {"head_dim", 256}}.dump();
  CHECK(language_context(lm).kv_bytes == 32768);
  std::ofstream(lm / "config.json") << Json{
      {"max_position_embeddings", 32768}, {"num_hidden_layers", 28},
      {"num_attention_heads", 16}, {"num_key_value_heads", 8},
      {"hidden_size", 2048}}.dump();
  CHECK(language_context(lm).tokens == 32768);
  CHECK(language_context(lm).kv_bytes == 28u * 2 * 8 * 128 * 2);

  // All of it on tgo-m5-3's 256 GB; on 24 GB what a quarter of the memory
  // the weights leave holds at the pool's peak; never under 32,768 (an
  // agent's head is ~15,000), never over the model's.
  constexpr std::uint64_t GB = 1ull << 30;
  const auto w27 = static_cast<std::uint64_t>(16.1e9);
  CHECK(helper_context(big, 256 * GB, w27) == 262144);
  CHECK(helper_context(big, 64 * GB, w27) == 100352);
  CHECK(helper_context(big, 24 * GB, w27) == 32768);
  CHECK(helper_context(big, 16 * GB, w27) == 32768);
  CHECK(helper_context(big, 0, w27) == 262144);
  CHECK(helper_context(language_context(lm), 256 * GB, 0) == 32768);

  // Saying none, unreadable, missing: 0 -- the engine's own then.
  const fs::path none = root / "none";
  fs::create_directories(none);
  std::ofstream(none / "config.json") << Json{{"model_type", "x"}}.dump();
  CHECK(language_context(none).tokens == 0);
  CHECK(helper_context(language_context(none), 24 * GB, 0) == 0);
  std::ofstream(none / "config.json") << "{not json";
  CHECK(language_context(none).tokens == 0);
  CHECK(language_context(root / "missing").tokens == 0);
}

// A helper shipped as a GGUF, no config.json beside it (Ternary-Bonsai 2's
// folder): its context and a token's K/V from the GGUF's header -- the
// model's file, not its vision projector -- read past arrays, strings and
// floats to the architecture's numbers.
TEST(models, a_gguf_helpers_context_is_its_headers)
{
  namespace fs = std::filesystem;
  const auto root = test::temp_dir("models-gguf");
  struct Out {
    std::string b;
    void u32(std::uint32_t v) { b.append(reinterpret_cast<char*>(&v), 4); }
    void u64(std::uint64_t v) { b.append(reinterpret_cast<char*>(&v), 8); }
    void str(const std::string& s) { u64(s.size()); b += s; }
    void key(const std::string& k, std::uint32_t type) { str(k); u32(type); }
  };
  auto write = [](const fs::path& f, const std::string& arch, bool full) {
    Out o;
    o.b = "GGUF";
    o.u32(3);
    o.u64(0);
    o.u64(full ? 14 : 2);
    o.key("general.architecture", 8);
    o.str(arch);
    o.key("general.sampling.temp", 6);
    o.u32(0x3f800000);
    if (full) {
      o.key("general.size_label", 8);
      o.str("27B");
      o.key("qwen35.block_count", 4);
      o.u32(64);
      o.key("qwen35.context_length", 4);
      o.u32(262144);
      o.key("qwen35.attention.head_count", 4);
      o.u32(24);
      // An array of numbers, and one of strings, before the rest.
      o.key("qwen35.rope.dimension_sections", 9);
      o.u32(5);
      o.u64(4);
      for (int i = 0; i < 4; ++i) { o.u32(11); }
      o.key("prism.hadamard.weight_names", 9);
      o.u32(8);
      o.u64(2);
      o.str("output.weight");
      o.str("blk.0.attn_qkv.weight");
      o.key("prism.hadamard.gdn_v_grouped", 7);
      o.b += '\x01';
      o.key("qwen35.attention.head_count_kv", 4);
      o.u32(4);
      o.key("qwen35.attention.key_length", 4);
      o.u32(256);
      o.key("qwen35.attention.value_length", 4);
      o.u32(256);
      o.key("qwen35.full_attention_interval", 4);
      o.u32(4);
      o.key("tokenizer.ggml.model", 8);
      o.str("gpt2");
      o.key("qwen35.context_length", 4);  // past the tokenizer: unread
      o.u32(7);
    }
    std::ofstream(f, std::ios::binary) << o.b;
  };
  const fs::path dir = root / "prism-ml" / "Ternary-Bonsai-2-27B-gguf";
  fs::create_directories(dir);
  // The projector sorts first; it is not the model.
  write(dir / "A-mmproj-BF16.gguf", "clip", false);
  write(dir / "Ternary-Bonsai-2-27B-PTQ1_0.gguf", "qwen35", true);
  const LanguageContext c = language_context(dir);
  CHECK(c.tokens == 262144);
  // 16 full-attention layers of 64, 4 K/V heads, 256 wide keys and
  // values, f16: 64 KB a token, as vpipe books Bonsai's K/V.
  CHECK(c.kv_bytes == 65536);
  CHECK(language_context(dir / "Ternary-Bonsai-2-27B-PTQ1_0.gguf").tokens ==
        262144);
  // Not a GGUF, nothing to read: none.
  const fs::path bad = root / "bad";
  fs::create_directories(bad);
  std::ofstream(bad / "model.gguf") << "GGML not quite";
  CHECK(language_context(bad).tokens == 0);
}

// Ternary-Bonsai 2 27B: a helper by name only (never Auto's), its PTQ1_0
// packing fetched from a repo of two, checked by its files, run at f16
// with no drafter, offered from 16 GB.
TEST(models, ternary_bonsai_is_a_helper_by_name)
{
  auto c = Catalog::builtin();
  REQUIRE_OK(c);
  const ModelEntry* b = c->find("ternary-bonsai-2-27b");
  REQUIRE(b);
  CHECK(b->role == "assistant" && b->by_name);
  CHECK(b->has(Capability::PromptEnhance));
  CHECK(b->fetch_variant == "PTQ1_0");
  CHECK(b->license == "apache-2.0");
  const Json vp = jget(b->engine, "vpipe", Json::object());
  CHECK(jget(vp, "mtp", true) == false);
  CHECK(jget<std::string>(vp, "compute_dtype", "") == "f16");
  CHECK(jget<std::string>(vp, "mtp_drafter", "").empty() &&
        jget<std::string>(vp, "dflash_drafter", "").empty());
  HardwareInfo air = box(16);
  air.gpu_working_set_bytes = std::uint64_t{11} << 30;
  CHECK(fits_as_assistant(*b, air));
  // Installed alone on a 24 GB Mac: still not Auto's.
  auto root = test::temp_dir("models-bonsai");
  const auto dir = root / b->hf_path;
  std::filesystem::create_directories(dir);
  for (const char* f : {"Ternary-Bonsai-2-27B-PTQ1_0.gguf",
                        "Ternary-Bonsai-2-27B-mmproj-BF16.gguf"}) {
    std::ofstream(dir / f) << "GGUF";
  }
  ModelStore store({root});
  CHECK(store.info(*b).state == InstallState::Installed);
  const ModelEntry* a = pick_assistant(*c, store, box(24));
  CHECK(!a || a->id != b->id);
  // Listed among the helpers.
  bool listed = false;
  for (const auto& f : c->families()) {
    for (const auto& m : f.members) {
      listed = listed || (f.id == "helpers" && m.model == b->id);
    }
  }
  CHECK(listed);
}

