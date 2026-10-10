// A generation's ACCELERATION and QUALITY options: what the Favor
// selector's presets settle for a model, and what its Custom changes.
//
// A preset (speed | balanced | quality -- the app's Fast, Med, Fine) is
// the catalog's per-preference table when the model has one
// (`engine.vpipe.presets.<preference>`): its step count, its few-step
// adapter -- `turbo`, candidate LoRAs in order, the first installed taken,
// each with its own step count and sigma shifts; [] for none -- and any
// option below by its key (sol_attn, sol_tau, sage_attn, i8_gemm,
// motion_cache, ...). What a table leaves out: int8 GEMMs on for speed and
// balanced wherever the GPU has matrix cores (the M5's neural
// accelerators), the catalog's `generate.i8_gemm` for quality; the steps
// and the model's `turbo` block as before; the rest off. A preset whose
// candidates are none of them installed NEEDS one (missing_loras): the
// app asks for it to be downloaded rather than run something else. Custom is every option at a value of the user's, starting
// from a preset's. ONE DEFINITION: the options a family offers, their
// ranges, whether this Mac and this install can have them, and how they
// constrain each other are decided here; the app draws what this says
// and the controller builds the job from the values it settles, so a
// value the panel shows is the value that runs.
//
// The options (keys as vpipe spells them, where it has them):
//   steps          int     denoising steps
//   loras          list    the LoRAs: [{"path", "scale", "on"}] -- a
//                          preset's is the catalog's few-step Turbo LoRA
//                          (`turbo.lora`, its file), on; others are
//                          one's own. Up to two on: vpipe's two slots,
//                          `lora` and `lora2` (one with HyperFlow, which
//                          takes the other). An older "turbo" override
//                          (true / false / the catalog's id / a file)
//                          still reads, into the list
//   dits, vaes     list    community checkpoints for the DiT and the VAE:
//                          [{"path", "on"}] -- a .safetensors or a folder;
//                          the one on runs in place of the model's own
//   hyperflow      bool    H3: Video Rebirth's 8-step flow-map adapter,
//                          in the first LoRA slot, the Turbo LoRA off;
//                          brings its own grid, so the steps are its 8
//   vdn            bool    H3: the VDN linear attention branch; replaces
//                          Sol-Attn on the blocks it covers
//   taomate        bool    H3: TaoMate's streaming METHOD -- the base
//                          model writes the soundtrack, its adapter (in
//                          the first LoRA slot, the Turbo LoRA off) the
//                          video in chunks against a clean K/V cache, 3
//                          steps a chunk. A preset runs it from the
//                          catalog's `taomate.min_ram_gb` (its cache
//                          alone is ~18 GB at 480p); text to clip only
//   taomate_lora   bool    H3, with taomate: its adapter run as an
//                          ordinary LoRA instead -- 3 steps, no
//                          soundtrack pass, no chunks, no cache -- at
//                          TaoMate's own shifts (`taomate.lora_only`:
//                          12 / 3); a clip may open on a picture
//   sol_attn       bool    Sol-Attn block routing
//   sol_tau        real    its threshold, in standard deviations
//   motion_cache   bool    H3: MotionCache, reused forwards
//   sage_attn      bool    SageAttention's int8 QK (M5 matrix cores)
//   i8_gemm        bool    int8 block GEMMs (M5 matrix cores)
//   ane_ffn        bool    the block's feed-forward on the Neural Engine,
//                          rows split with the GPU
//   ane_qkv        bool    its q|k|v projection there too (needs ane_ffn)
//   video_shift    int     H3: the video schedule's sigma shift
//   audio_shift    int     H3: the audio schedule's
//   shift          real    an image family's scheduler shift (Krea 2's
//                          is 0.3, so not a whole number)
//   <declared>             a family's OWN options (catalog
//                          `engine.vpipe.options`, an extension's
//                          knobs): bool / int / real, with the text the
//                          panel shows ("label", "note", "help",
//                          "group"); the graph builder lays the value on
//                          the stage the option names ("generate", or the
//                          family's "config" stage)
// A family offers the ones its catalog `engine.vpipe` block names: the
// acceleration tiers vpipe applies to it (`accel`), its adapters and
// branch (`turbo`, `hyperflow`, `vdn`), its shifts (`config`'s, or a
// `scheduler`).

#ifndef VALTZ_MODELS_TUNING_H
#define VALTZ_MODELS_TUNING_H

#include "valtz/base/json.h"
#include "valtz/models/catalog.h"

#include <filesystem>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace valtz::models {

// What decides availability beyond the catalog: this install and Mac.
struct TuningContext {
  bool turbo_installed = false;      // the catalog `turbo.lora` entry
  std::string turbo_name;            //   its name, as listed
  std::string turbo_path;            //   and its file
  bool hyperflow_installed = false;  // `hyperflow.lora`
  bool taomate_installed = false;    // `taomate.lora`
  bool vdn_installed = false;        // `vdn.branch`
  bool matrix_cores = false;         // HardwareInfo::gpu_matrix_cores
  // The rest of the machine (HardwareInfo): what decides whether Fast and
  // Med put the Neural Engine to work beside a GPU without matrix cores.
  std::uint32_t gpu_cores = 0;
  std::uint32_t ane_cores = 0;
  std::uint32_t ram_gb = 0;
  // Every LoRA of the model's family the catalog lists, by id: its name,
  // and its file when installed ("" when not) -- what a preset's table
  // picks its adapter from.
  struct Lora {
    std::string name;
    std::string path;
  };
  std::map<std::string, Lora> loras;
};

// The steps `preference` runs on `m`: with `turbo`, its turbo block's
// table; without, the preset's own count (`presets.<preference>.steps`),
// else the catalog's per-preference table, else the model's default
// scaled x0.5 / x1 / x1.5. An edit's defaults (`edit.defaults`) come first.
int preset_steps(const ModelEntry& m, std::string_view preference,
                 bool edit, bool turbo);

// The LoRAs `preference` needs on `m` and does not have: its table's
// candidates, when none of them is installed -- [{"id", "name"}], in the
// table's order; [] when it needs none, or has one. TaoMate in the
// adapter's place needs none -- as the preset runs it, or as `overrides`
// say: turned off there, the preset's LoRA is needed again.
Json missing_loras(const ModelEntry& m, const TuningContext& ctx,
                   std::string_view preference,
                   const Json& overrides = Json());

// The options `m` offers, as the app draws them:
//   {"family", "preference",
//    "values": {key: value, ...},      settled (see resolve_tuning)
//    "turbo": the preset's adapter, {"id", "name", "steps"} or null,
//    "missing": missing_loras,
//    "options": [{"key", "type": "bool" | "int" | "real" | "list",
//                 "available": bool, "why": "" | "not-installed" |
//                   "needs-matrix-cores" | "own-schedule",
//                 "min", "max", "step" (numbers),
//                 "excludes": [keys turned off with it on],
//                 "fixes": {key: value while it is on},
//                 "needs": key it refines (sol_tau: sol_attn),
//                 "extra": the loras list's {"slots", "turbo" (the
//                   catalog Turbo LoRA's file), "names" (file: name),
//                   "steps_on", "steps_off"}}, ...]}
// in the order a panel lists them.
Json tuning_options(const ModelEntry& m, const TuningContext& ctx,
                    std::string_view preference, bool edit,
                    const Json& overrides = Json::object());

// The values `m` runs with: `preference`'s, with `overrides` (keys of
// the list above) laid on -- held to their ranges, an option this
// install or Mac cannot have back at its preset value, and the rules
// applied (HyperFlow: a LoRA slot, not the Turbo LoRA, its 8 steps; VDN:
// no Sol-Attn; the steps following the Turbo LoRA turned the other way).
// Only the keys the family offers.
Json resolve_tuning(const ModelEntry& m, const TuningContext& ctx,
                    std::string_view preference, bool edit,
                    const Json& overrides = Json::object());

// What a dropped weight file (or folder) is, read from it: "lora" (its
// tensors are low-rank pairs), "vae" (encoder / decoder tensors, a VAE
// config, or a Comfy-Org VAE component), "dit" (another model), or ""
// (not weights this can read). A guess the panel files it by; vpipe
// decides whether it can run it.
std::string checkpoint_kind(const std::filesystem::path& path);

// What a dropped weight file is, by the catalog's recognition rules
// first (Catalog::recognize_rules: an extension knows its own
// checkpoints), then by checkpoint_kind:
//   {"kind": "lora" | "dit" | "vae" | "", "family": the family a rule
//    names ("" by the heuristics), "origin": the extension whose rule
//    matched}.
// A rule: {"kind", "family", "class_name": [a diffusers folder's
// _class_name, any of], "keys_any": [a tensor name contains one],
// "keys_all": [each is in some tensor name], "metadata": {key: glob of
// its value in the header's __metadata__, '*' any run}} -- every
// condition it states must hold, and one that states none never does.
Json classify_checkpoint(const std::filesystem::path& path,
                         const Json& rules);

// A LoRA as the file vpipe loads, as the LoRA list spells it: a folder (a
// PEFT export, a Hub repo) by its adapter_model.safetensors, else its
// largest .safetensors; one spelling for one file.
std::string lora_file(const std::filesystem::path& path);

}

#endif
