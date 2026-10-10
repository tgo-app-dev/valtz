// The stages valtz-vpipe.so registers (see valtz-vpipe/exchange.h).

#ifndef VALTZ_VPIPE_PLUGIN_STAGES_H
#define VALTZ_VPIPE_PLUGIN_STAGES_H

namespace vpipe {
class VpipePluginContext;
}

namespace valtz::plugin {

void register_source(vpipe::VpipePluginContext*);
void register_sink(vpipe::VpipePluginContext*);
void register_tap(vpipe::VpipePluginContext*);
void register_summary(vpipe::VpipePluginContext*);

}

#endif
