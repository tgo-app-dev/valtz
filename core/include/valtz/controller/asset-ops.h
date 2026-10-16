// ASSET OPERATIONS (DESIGN §4j): what is DONE TO an asset with no prompt
// -- a sound's speech transcribed, a clip summarized, a layer's picture or
// clip rendered larger, a song split into its voice and its accompaniment.
//
// They share ONE PROTOCOL, so a new one is a row here and a function in
// the controller, not a path of its own through the bridge, the app and
// valtzctl:
//   * each is NAMED, and says what it is done to -- the kinds of asset it
//     takes, each with the engine op that does it and the capability its
//     model needs -- and whether it works on a composition's LAYER (what
//     the layer shows, the result shown in its place: an upscale) or on
//     the asset itself (its results new assets beside it in the list);
//   * the core lists them as this Mac can run them
//     (Controller::asset_operations_json: the app's menus, valtzctl's
//     `ops`) and runs one asked for by its name
//     (Controller::run_asset_operation: the asset, the model -- "auto"
//     the best installed, else one downloading -- and its own options);
//   * each is ONE TASK of the queue (DESIGN §3a), one undoable command,
//     making new DERIVED assets from the asset -- one, or several from one
//     job (a separation's sources: each Output event names the one it is,
//     data "output") -- named after it, its recipe saying how it was made,
//     its progress how far into the asset its results reach.

#ifndef VALTZ_CONTROLLER_ASSET_OPS_H
#define VALTZ_CONTROLLER_ASSET_OPS_H

#include "valtz/models/catalog.h"
#include "valtz/project/records.h"

#include <span>
#include <string_view>

namespace valtz {

// What an operation does to one kind of asset: the engine op that runs
// it, the capability its model has.
struct AssetOpTake {
  project::AssetKind kind;
  std::string_view   op;
  models::Capability capability;
};

struct AssetOperation {
  std::string_view             name;   // what a request names it
  std::string_view             label;  // English (valtzctl); the app
                                       // words its own
  std::span<const AssetOpTake> takes;
  // On a LAYER of a composition (its source), the result shown in the
  // layer's place; else on the asset, the results beside it.
  bool                         on_layer = false;
  // What it makes, in order (a separation: the voice, then the rest).
  std::span<const std::string_view> makes;
};

// Every operation, in the order a menu lists them.
std::span<const AssetOperation> asset_operations();
const AssetOperation* find_asset_operation(std::string_view name);
// The operation an engine op does (a task's), else null.
const AssetOperation* asset_operation_of(std::string_view op);
// What `op` does to `kind`; null when it does not take that kind.
const AssetOpTake* asset_op_take(const AssetOperation& op,
                                 project::AssetKind kind);

}

#endif
