// What this machine can do right now, per capability.
//
// For each capability: which catalog models serve it, which of those
// FIT this machine's tier, which are installed, and the one the app
// would use. The model-management screen renders exactly this.

#ifndef VALTZ_MODELS_CAPABILITIES_H
#define VALTZ_MODELS_CAPABILITIES_H

#include "valtz/models/catalog.h"
#include "valtz/models/hardware.h"
#include "valtz/models/model-store.h"

#include <functional>
#include <string>
#include <vector>

namespace valtz::models {

enum class Availability : std::uint8_t {
  Ready,          // a fitting model is installed
  NeedsDownload,  // a fitting model exists but is not installed
  NeedsMoreRam,   // only models above this machine's tier serve it
  NoEngine,       // built without the execution engine
  NotWired,       // models exist, but the engine cannot run the op yet
  NotOffered,     // nothing in the catalog serves it yet
};

const char* to_str(Availability);

struct ModelOption {
  const ModelEntry* entry = nullptr;
  InstallInfo       install;
  bool              fits = false;
};

struct CapabilityStatus {
  Capability               capability;
  Availability             availability = Availability::NotOffered;
  std::string              chosen;  // model id the app would use
  std::vector<ModelOption> options;
};

// `engine_runs(c)`: can the engine execute capability `c` at all? (A
// model on disk is not a feature until the engine has a graph for it.)
using EngineSupport = std::function<bool(Capability)>;

std::vector<CapabilityStatus>
resolve_capabilities(const Catalog&, const ModelStore&, const HardwareInfo&,
                     const EngineSupport& engine_runs);

// The assistant LLM for this tier: the best-ranked fitting model, with
// installed ones preferred (9B on 16 GB, 27B from 24 GB).
const ModelEntry* pick_assistant(const Catalog&, const ModelStore&,
                                 const HardwareInfo&);

void to_json(Json&, const CapabilityStatus&);

}

#endif
