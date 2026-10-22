#include "valtz/models/capabilities.h"

#include <algorithm>

namespace valtz::models {

const char*
to_str(Availability a)
{
  switch (a) {
  case Availability::Ready:         return "ready";
  case Availability::NeedsDownload: return "needs-download";
  case Availability::NeedsMoreRam:  return "needs-more-ram";
  case Availability::NoEngine:      return "no-engine";
  case Availability::NotWired:      return "not-wired";
  case Availability::NotOffered:    return "not-offered";
  }
  return "?";
}

std::vector<CapabilityStatus>
resolve_capabilities(const Catalog& cat, const ModelStore& store,
                     const HardwareInfo& hw, const EngineSupport& engine_runs)
{
  std::vector<CapabilityStatus> out;
  for (auto cap : kAllCapabilities) {
    CapabilityStatus st;
    st.capability = cap;
    const ModelOption* first_fit = nullptr;
    const ModelOption* first_ready = nullptr;
    for (const ModelEntry* e : cat.serving(cap)) {
      ModelOption o;
      o.entry = e;
      o.install = store.info(*e);
      o.fits = hw.ram_gb() >= e->min_ram_gb;
      st.options.push_back(o);
    }
    for (const auto& o : st.options) {
      if (!o.fits) {
        continue;
      }
      if (!first_fit) {
        first_fit = &o;
      }
      // Ready with what it needs beside it (catalog `requires`: a
      // codec, a voice detector), not alone.
      const bool needs_met = std::ranges::all_of(
          o.entry->requires_models, [&](const std::string& r) {
            const ModelEntry* re = cat.find(r);
            return re && store.info(*re).state == InstallState::Installed;
          });
      if (!first_ready && o.install.state == InstallState::Installed &&
          needs_met) {
        first_ready = &o;
      }
    }
    if (st.options.empty()) {
      st.availability = Availability::NotOffered;
    } else if (!engine_runs) {
      st.availability = Availability::NoEngine;
    } else if (!engine_runs(cap)) {
      st.availability = Availability::NotWired;
      st.chosen = first_ready ? first_ready->entry->id
                  : first_fit ? first_fit->entry->id
                              : std::string();
    } else if (first_ready) {
      st.availability = Availability::Ready;
      st.chosen = first_ready->entry->id;
    } else if (first_fit) {
      st.availability = Availability::NeedsDownload;
      st.chosen = first_fit->entry->id;
    } else {
      st.availability = Availability::NeedsMoreRam;
    }
    out.push_back(std::move(st));
  }
  return out;
}

bool
fits_as_assistant(const ModelEntry& e, const HardwareInfo& hw)
{
  // An assistant decodes a token through every weight: they must all stay
  // RESIDENT -- within what the GPU keeps (Metal's recommended working
  // set), not just the RAM. Qwen3.8 27B's 20.7 GB on a 24 GB Mac (17.8 GB
  // of it) was paged in and out every token: a few words in minutes. A
  // Mixture of Experts reads only its `resident_gb` every token, its
  // experts from a cache that takes what is left.
  const double held = e.resident_gb > 0 ? e.resident_gb : e.disk_gb;
  return hw.ram_gb() >= e.min_ram_gb &&
         (hw.gpu_working_set_bytes == 0 ||
          held * 1e9 <= static_cast<double>(hw.gpu_working_set_bytes));
}

const ModelEntry*
pick_assistant(const Catalog& cat, const ModelStore& store,
               const HardwareInfo& hw)
{
  const ModelEntry* best_fit = nullptr;
  for (const ModelEntry* e : cat.serving(Capability::PromptEnhance)) {
    // One chosen by name alone (an abliterated helper) is never Auto's.
    if (e->role != "assistant" || e->by_name || !fits_as_assistant(*e, hw)) {
      continue;
    }
    // The best-ranked fitting model is the target; but an installed
    // smaller one beats a bigger one the user has not downloaded.
    if (!best_fit) {
      best_fit = e;
    }
    if (store.info(*e).state == InstallState::Installed) {
      return e;
    }
  }
  return best_fit;
}

void
to_json(Json& j, const CapabilityStatus& s)
{
  Json opts = Json::array();
  for (const auto& o : s.options) {
    opts.push_back({
      {"model", o.entry->id},
      {"name", o.entry->name},
      {"state", to_str(o.install.state)},
      {"fits", o.fits},
      {"downloading", o.downloading},
      {"disk_gb", o.entry->disk_gb},
      {"min_ram_gb", o.entry->min_ram_gb},
      {"dir", o.install.dir.string()},
    });
  }
  j = {
    {"capability", to_str(s.capability)},
    {"label", label(s.capability)},
    {"availability", to_str(s.availability)},
    {"chosen", s.chosen},
    {"options", opts},
  };
}

}
