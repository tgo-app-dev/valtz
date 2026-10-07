// EXTENSIONS: new models -- and what Valtz needs to offer them -- brought
// in by a package (DESIGN §8a; docs/EXTENSIONS.md is the author's
// reference).
//
// A package is a folder, `<name>.valtzext`:
//   manifest.cbor   the declaration: ONE CBOR map
//   plugins/        vpipe plugins: the backend, the package's only code
//   skills/         prompt-enhancement instructions, as text
//   ...             whatever else the manifest names (licenses)
//
// Valtz never runs an extension's code itself. The native part is a vpipe
// plugin, handed to the engine's session beside valtz-vpipe.so -- vpipe
// owns that ABI and its window, and at M4 the helper process isolates it.
// Everything Valtz takes in is DATA: catalog entries (capabilities,
// downloads, the graph shape and its parameters, tuning options), weight
// recognition rules, prompt and reference templates, enhancement skills
// -- read by the code that reads the built-in catalog, so an extension's
// model is offered, tuned, prompted and run exactly as a built-in one.
// A Valtz-side ABI (C++20, std types, Json across it) would cost far
// more to keep than a schema does, and a schema can be checked key by key.
//
// THE INTERFACE VERSION: one integer, FEATURES, and a WINDOW -- vpipe's
// plugin scheme, so one set of rules covers both halves of a package.
//  * kInterface changes only for a change an older manifest would be
//    MISREAD under: a key renamed, a meaning or a default changed. Each
//    change brings an UPGRADER that rewrites a manifest written for the
//    version before into the new one. The reader knows only the current
//    schema; older ones live on as small, tested rewrites.
//  * Everything additive is a new optional KEY (absent means the
//    behaviour from before it, so an older manifest needs nothing, and an
//    older host ignores it) -- or, where ignoring it would be wrong, a
//    FEATURE ("name/rev") the host lists and a manifest names in
//    `required_features`: the whole package's, or one model's.
//  * A host at N reads manifests written for kInterfaceOldest..N,
//    upgrading them. One written for a newer interface is read as it is
//    when its `interface_min` -- the oldest interface its author says
//    reads it correctly -- is at most N: by that promise, what it uses
//    from after N is optional.
//
// CBOR, because a manifest is low-density structured data: a map grows a
// key without a migration, a reader that does not know it passes over it
// (every field is read with a default, as records are), and bytes (an
// icon, a hash) need no encoding. Authors write JSON; `valtzctl ext pack`
// makes the CBOR, `valtzctl ext show` prints it back.

#ifndef VALTZ_EXT_EXTENSION_H
#define VALTZ_EXT_EXTENSION_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/models/catalog.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::ext {

// The interface this build implements, and the oldest it still reads.
inline constexpr int kInterface = 1;
inline constexpr int kInterfaceOldest = 1;

inline constexpr const char* kFormat = "valtz-extension";
inline constexpr const char* kManifestFile = "manifest.cbor";
inline constexpr const char* kPackageSuffix = ".valtzext";

// The features interface 1 ships with. A later host adds more; it never
// withdraws one while the interface that introduced it is in its window.
//   catalog          models, families, Auto's lists (catalog schema)
//   tuning-options   engine.vpipe.options: a family's own knobs
//   recognize        rules that file a dropped weight file
//   skills           prompt-enhancement instructions
//   prompt-template  prompting.template: the words around a prompt
//   vpipe-backend    vpipe plugins in the package
inline constexpr const char* kFeatureCatalog = "catalog/1";
inline constexpr const char* kFeatureTuningOptions = "tuning-options/1";
inline constexpr const char* kFeatureRecognize = "recognize/1";
inline constexpr const char* kFeatureSkills = "skills/1";
inline constexpr const char* kFeaturePromptTemplate = "prompt-template/1";
inline constexpr const char* kFeatureVpipeBackend = "vpipe-backend/1";

// The interface a host offers. `host()` is this build's; tests make
// others (an older or newer host, a chain of upgraders).
struct Interface {
  int                      current = kInterface;
  int                      oldest = kInterfaceOldest;
  std::vector<std::string> features;
  // The graph SHAPES the engine builds (catalog `engine.vpipe.shape`):
  // a model names the one its graph has. A shape whose contract changes
  // gets a new name, never a new meaning.
  std::vector<std::string> shapes;
  // upgraders[k - oldest] rewrites a manifest written for interface k
  // into k + 1; current - oldest of them.
  std::vector<std::function<Status(Json&)>> upgraders;

  bool has_feature(std::string_view f) const;
  bool has_shape(std::string_view s) const;

  static const Interface& host();
};

// The shape a model's graph has: its `engine.vpipe.shape`, else the one
// its capabilities imply ("diffusion-image", "minimax-h3", "yue2"); ""
// for a model the engine builds no graph of its own for (a LoRA, a TAE).
std::string shape_of(const models::ModelEntry& m);

// A verdict: empty `why` admits. `why` is a stable code the app
// localizes (docs/EXTENSIONS.md lists them), `args` its details.
struct Verdict {
  std::string why;
  Json        args = Json::object();
  bool ok() const { return why.empty(); }
};

// A manifest's bytes as its document (CBOR; at most 4 MiB).
Result<Json> decode_manifest(std::span<const std::uint8_t> bytes);

// Whether `host` reads `doc` at all: its format, id and interface (in the
// window, or newer with an `interface_min` it meets), and the features it
// requires.
Verdict admit(const Json& doc, const Interface& host);

// `doc` (admitted) rewritten up to `host.current` through the upgraders;
// one written for a newer interface is left as it is.
Status upgrade(Json& doc, const Interface& host);

// A vpipe plugin the package carries (manifest `vpipe.plugins`).
struct Plugin {
  std::filesystem::path    file;      // inside the package
  int                      abi = 0;   // the vpipe plugin ABI it targets
  std::vector<std::string> required;  // vpipe host features it needs
  std::vector<std::string> stages;    // stage types it registers: probed
};

// One package as this host takes it.
struct Extension {
  std::filesystem::path    dir;
  bool                     builtin = false;  // shipped inside the app
  std::string              id;       // reverse-DNS: "com.acme.video"
  std::string              version;
  Json                     name;     // text: a string or {lang: string}
  Json                     description;
  std::string              vendor;
  std::string              license;
  int                      interface = 0;      // as written
  int                      interface_min = 0;
  // "ready" | "partial" (some contributions withheld) | "disabled" |
  // "refused" (not read: `why`) | "backend-failed" (its plugins did not
  // load: nothing it declares is offered).
  std::string              state;
  std::string              why;
  Json                     why_args = Json::object();
  std::vector<Plugin>      plugins;
  // The manifest upgraded to the host's interface, less what is
  // withheld: what the catalog takes.
  Json                     doc = Json::object();
  std::vector<models::Withheld> withheld;

  bool admitted() const { return state == "ready" || state == "partial"; }
};

// One package read and judged: its manifest decoded, admitted, upgraded;
// its plugins' files found inside it; each model whose features or shape
// this host lacks withheld (the others still offered).
Extension read_package(const std::filesystem::path& dir,
                       const Interface& host = Interface::host());

// Where packages are looked for, in order: the app's own first.
struct Root {
  std::filesystem::path dir;
  bool                  builtin = false;
};

// The standard roots: Valtz.app/Contents/Extensions, then
// <support>/extensions, then $VALTZ_EXTENSIONS (':'-separated; dev).
std::vector<Root> standard_roots(const std::filesystem::path& support);

// Every `*.valtzext` under `roots`, each root's in name order. A second
// package with an id already seen is refused ("duplicate-id"); one the
// user turned off is "disabled" (read, so it can be listed).
std::vector<Extension> discover(const std::vector<Root>& roots,
                                const std::set<std::string>& disabled,
                                const Interface& host = Interface::host());

// The user's choices (<support>/extensions.json: {"disabled": [ids]}).
std::set<std::string> read_disabled(const std::filesystem::path& file);
Status write_disabled(const std::filesystem::path& file,
                      const std::set<std::string>& ids);

// For the app and `valtzctl ext`: the package as listed, text in `lang`.
Json to_json(const Extension& e, std::string_view lang);

}

#endif
