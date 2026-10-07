// valtz-vpipe.so: the vpipe plugin through which Valtz exchanges data
// with its graphs. vpipe loads it into Valtz's own process (the session
// config's `plugins` list), so the buffers it hands across are plain
// pointers into one address space, kept alive by shared owners.
//
// Built against vpipe's installed plugin SDK only, like any third-party
// plugin; the stage-commands feature is required, so a host without it
// refuses the plugin at load, by name.

#include "stages.h"

#include "valtz-vpipe/exchange.h"

#include "plugin/plugin-abi.h"
#include "plugin/plugin-context.h"

namespace {

const char* const kRequires[] = {VPIPE_FEATURE_STAGE_COMMANDS, nullptr};

const VpipePluginInfo kInfo = {
  VPIPE_PLUGIN_INFO_SCHEMA,
  valtz::exchange::kPluginName,
  VALTZ_VERSION,
  "tgous",
  "MPL-2.0",
  "Valtz host exchange: valtz-source / valtz-sink / valtz-tap, "
  "zero-copy buffers and flow control over stage commands",
  kRequires,
};

void
register_plugin(vpipe::VpipePluginContext* ctx)
{
  valtz::plugin::register_source(ctx);
  valtz::plugin::register_sink(ctx);
  valtz::plugin::register_tap(ctx);
}

}

VPIPE_PLUGIN_DEFINE(&kInfo, register_plugin)
