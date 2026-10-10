// What this Mac can do: chip, memory, GPU, and the memory TIER that
// decides which model variants are offered.
//
// Tiers compare TOTAL RAM, not free RAM, on purpose (the same rule vpipe
// uses): the answer must be the same every time the app launches, or a
// user's "available models" list would flicker with whatever else is
// running. Free memory is a runtime concern for the scheduler.

#ifndef VALTZ_MODELS_HARDWARE_H
#define VALTZ_MODELS_HARDWARE_H

#include "valtz/base/json.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace valtz::models {

enum class ThermalState : std::uint8_t { Nominal, Fair, Serious, Critical };

struct HardwareInfo {
  std::string   chip;             // "Apple M5"
  std::string   model_id;         // "Mac17,4"
  std::string   os_version;       // "26.6.2"
  std::uint64_t ram_bytes = 0;
  std::uint32_t perf_cores = 0;
  std::uint32_t efficiency_cores = 0;
  std::uint32_t gpu_cores = 0;
  bool          gpu_matrix_cores = false;  // Apple10 GPU family (M5+)
  // Neural Engine cores: what the ANE driver reports, and at least what
  // the chip has -- 16 a die, 32 from M6 on; an Ultra is two dies.
  std::uint32_t ane_cores = 0;
  std::uint64_t gpu_working_set_bytes = 0; // recommendedMaxWorkingSetSize
  // Unified memory's bandwidth, GB/s, as Apple states it for the chip
  // (memory_bandwidth_of_chip); what a model's decode -- and a VLM's
  // prefill of many frames -- is bound by. 0 when the chip is not known.
  double        memory_bandwidth_gbs = 0;
  ThermalState  thermal = ThermalState::Nominal;

  std::uint32_t ram_gb() const noexcept
  {
    return static_cast<std::uint32_t>((ram_bytes + (512ull << 20)) >> 30);
  }
};

// The Neural Engine cores an Apple chip has by its name ("Apple M5
// Ultra"): 16 a die through M5, 32 from M6, an Ultra two dies; 0 when
// the name is not an M-series chip's.
std::uint32_t ane_cores_of_chip(std::string_view chip);

// Probe once per launch. `VALTZ_RAM_LIMIT_GB` overrides the RAM figure,
// to exercise a small-box tier on a big box.
// Unified memory's bandwidth by the chip's name ("Apple M5 Pro") and its
// GPU cores (a Max comes in two: M4 Max 410 or 546 GB/s), in GB/s, as
// Apple states it: M1 68, M2 / M3 100, M4 120, M5 153; Pro 200 (M3 Pro
// 150, M4 Pro 273, M5 Pro 307); Max and Ultra from 300 up. A later
// generation is taken as M5's of its tier. 0 for a chip not an M-series.
double memory_bandwidth_of_chip(std::string_view chip,
                                std::uint32_t gpu_cores);

HardwareInfo probe_hardware();

std::uint64_t free_disk_bytes(const std::filesystem::path&);

// Memory a new allocation could have NOW without anything swapping out:
// free, speculative and purgeable pages, and clean file-backed ones the
// kernel drops at will -- a runtime reading for a scheduler deciding
// how far ahead to run (an upscaler's groups, DESIGN §4f), never a tier.
// Under VALTZ_RAM_LIMIT_GB it is what that smaller box would have left.
// 0 when the kernel will not say.
std::uint64_t reclaimable_ram_bytes();
// The memory the whole Mac is using, as Activity Monitor's "Memory Used"
// counts it: app memory (anonymous pages, less purgeable ones), wired,
// and what the compressor occupies -- file cache left out, since the
// kernel drops it at will. The status bar's grey RAM bar. 0 when the
// kernel will not say.
std::uint64_t used_ram_bytes();

const char* to_str(ThermalState);
void to_json(Json&, const HardwareInfo&);

}

#endif
