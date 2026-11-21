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
// Not a stage: the tensor-parallel transport, valtz-tp.
void register_tp(vpipe::VpipePluginContext*);
// Not a stage: far memory, another Mac's lent to an expert cache.
void register_far(vpipe::VpipePluginContext*);

}

#endif
