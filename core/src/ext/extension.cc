#include "valtz/ext/extension.h"

#include <mach-o/dyld.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>

namespace valtz::ext {

namespace fs = std::filesystem;

namespace {

// A manifest is a declaration, not a payload: anything larger is a
// mistake (or not a manifest).
constexpr std::uintmax_t kMaxManifestBytes = 4u << 20;

// Reverse-DNS, lower case: "com.acme.video". It names the package in
// recipes and in the user's choices, so it is spelled one way.
bool
good_id(std::string_view id)
{
  if (id.empty() || id.size() > 128 || id.front() == '.' ||
      id.back() == '.' || id.find('.') == std::string_view::npos ||
      id.find("..") != std::string_view::npos) {
    return false;
  }
  return std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
           c == '-' || c == '_';
  });
}

// A path a manifest names, inside its package; empty when it climbs out.
fs::path
inside(const fs::path& dir, const std::string& rel)
{
  const fs::path p(rel);
  if (rel.empty() || p.is_absolute()) {
    return {};
  }
  const auto n = p.lexically_normal();
  if (n.empty() || *n.begin() == "..") {
    return {};
  }
  return dir / n;
}

std::vector<std::string>
strings(const Json& v)
{
  std::vector<std::string> out;
  for (const auto& s : v.is_array() ? v : Json::array()) {
    if (s.is_string()) {
      out.push_back(s.get<std::string>());
    }
  }
  return out;
}

void
refuse(Extension& e, std::string why, Json args = Json::object())
{
  e.state = "refused";
  e.why = std::move(why);
  e.why_args = std::move(args);
}

fs::path
executable_dir()
{
  char buf[PATH_MAX];
  std::uint32_t n = sizeof(buf);
  if (_NSGetExecutablePath(buf, &n) != 0) {
    return {};
  }
  std::error_code ec;
  const fs::path p = fs::canonical(buf, ec);
  return ec ? fs::path(buf).parent_path() : p.parent_path();
}

bool
is_package(const fs::path& p)
{
  std::error_code ec;
  return p.extension() == kPackageSuffix && fs::is_directory(p, ec);
}

}

bool
Interface::has_feature(std::string_view f) const
{
  return std::find(features.begin(), features.end(), f) != features.end();
}

bool
Interface::has_shape(std::string_view s) const
{
  return std::find(shapes.begin(), shapes.end(), s) != shapes.end();
}

const Interface&
Interface::host()
{
  static const Interface kHost = [] {
    Interface i;
    i.current = kInterface;
    i.oldest = kInterfaceOldest;
    i.features = {kFeatureCatalog,        kFeatureTuningOptions,
                  kFeatureRecognize,      kFeatureSkills,
                  kFeaturePromptTemplate, kFeatureVpipeBackend};
    // graph-builder.cc's builders: the picture one every image family
    // shares, MiniMax H3's clip with its soundtrack (and references),
    // YuE2's song, and the assistant's chat.
    i.shapes = {"diffusion-image", "minimax-h3", "yue2", "chat"};
    // No interface before 1: nothing to upgrade yet. The first change
    // that bumps kInterface adds its rewrite here.
    return i;
  }();
  return kHost;
}

std::string
shape_of(const models::ModelEntry& m)
{
  const auto said = jget<std::string>(
      jget(m.engine, "vpipe", Json::object()), "shape", "");
  if (!said.empty()) {
    return said;
  }
  using models::Capability;
  if (m.has(Capability::TextToImage) || m.has(Capability::ImageEdit)) {
    return "diffusion-image";
  }
  if (m.has(Capability::TextToVideo) || m.has(Capability::ImageToVideo) ||
      m.has(Capability::ReferenceToVideo)) {
    return "minimax-h3";
  }
  if (m.has(Capability::TextToAudio)) {
    return "yue2";
  }
  if (m.has(Capability::PromptEnhance) || m.has(Capability::Intent)) {
    return "chat";
  }
  return {};
}

Result<Json>
decode_manifest(std::span<const std::uint8_t> bytes)
{
  if (bytes.size() > kMaxManifestBytes) {
    return make_error(Code::InvalidArgument, "manifest is too large");
  }
  Json j = Json::from_cbor(bytes.begin(), bytes.end(), /*strict=*/true,
                           /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return make_error(Code::Corrupt, "manifest is not a CBOR map");
  }
  return j;
}

Verdict
admit(const Json& doc, const Interface& host)
{
  if (!doc.is_object() || jget<std::string>(doc, "format", "") != kFormat) {
    return {"not-an-extension"};
  }
  if (!good_id(jget<std::string>(doc, "id", ""))) {
    return {"bad-id", {{"id", jget<std::string>(doc, "id", "")}}};
  }
  const int iface = jget(doc, "interface", 0);
  if (iface <= 0) {
    return {"no-interface"};
  }
  // The oldest interface its author says reads it correctly: by default
  // the one it was written for.
  const int least = std::min(jget(doc, "interface_min", iface), iface);
  if (iface < host.oldest) {
    return {"too-old", {{"interface", iface}, {"oldest", host.oldest}}};
  }
  if (least > host.current) {
    return {"too-new", {{"interface", least}, {"current", host.current}}};
  }
  for (const auto& f : strings(jget(doc, "required_features",
                                    Json::array()))) {
    if (!host.has_feature(f)) {
      return {"needs-feature", {{"feature", f}}};
    }
  }
  return {};
}

Status
upgrade(Json& doc, const Interface& host)
{
  for (int k = jget(doc, "interface", 0); k < host.current; ++k) {
    const int i = k - host.oldest;
    if (i < 0 || i >= static_cast<int>(host.upgraders.size()) ||
        !host.upgraders[static_cast<std::size_t>(i)]) {
      return make_error(Code::Internal, std::format(
          "no upgrade from extension interface {} to {}", k, k + 1));
    }
    VALTZ_TRY(host.upgraders[static_cast<std::size_t>(i)](doc));
    doc["interface"] = k + 1;
  }
  return {};
}

Extension
read_package(const fs::path& dir, const Interface& host)
{
  Extension e;
  e.dir = dir;
  const fs::path file = dir / kManifestFile;
  std::error_code ec;
  const auto size = fs::file_size(file, ec);
  if (ec) {
    refuse(e, "no-manifest");
    return e;
  }
  if (size > kMaxManifestBytes) {
    refuse(e, "too-large", {{"bytes", size}});
    return e;
  }
  std::ifstream in(file, std::ios::binary);
  const std::vector<std::uint8_t> bytes(
      (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  auto decoded = decode_manifest(bytes);
  if (!decoded.ok()) {
    refuse(e, "unreadable", {{"detail", decoded.error().message}});
    return e;
  }
  Json doc = std::move(*decoded);
  // What it says of itself, before the verdict: a refused package is
  // still listed by its name.
  e.id = jget<std::string>(doc, "id", "");
  e.version = jget<std::string>(doc, "version", "");
  e.name = jget(doc, "name", Json(e.id));
  e.description = jget(doc, "description", Json(""));
  e.vendor = jget<std::string>(doc, "vendor", "");
  e.license = jget<std::string>(doc, "license", "");
  e.interface = jget(doc, "interface", 0);
  e.interface_min = std::min(jget(doc, "interface_min", e.interface),
                             e.interface);
  if (const Verdict v = admit(doc, host); !v.ok()) {
    refuse(e, v.why, v.args);
    return e;
  }
  if (const Status st = upgrade(doc, host); !st.ok()) {
    refuse(e, "upgrade-failed", {{"detail", st.error().message}});
    return e;
  }

  // Its backend: each plugin a file inside the package. One missing is
  // a broken package -- nothing it declares could run.
  for (const auto& p : jget(jget(doc, "vpipe", Json::object()), "plugins",
                            Json::array())) {
    Plugin pl;
    const auto rel = jget<std::string>(p, "file", "");
    pl.file = inside(dir, rel);
    pl.abi = jget(p, "abi", 0);
    pl.required = strings(jget(p, "required_features", Json::array()));
    pl.stages = strings(jget(p, "stages", Json::array()));
    if (pl.file.empty() || !fs::is_regular_file(pl.file, ec)) {
      e.state = "backend-failed";
      e.why = pl.file.empty() ? "bad-path" : "missing-file";
      e.why_args = {{"file", rel}};
      return e;
    }
    e.plugins.push_back(std::move(pl));
  }

  // A model this host cannot run as its author meant -- a feature it
  // lacks, a graph shape it does not build -- is withheld; the rest of
  // the package is still offered.
  if (doc.contains("models") && doc["models"].is_array()) {
    Json kept = Json::array();
    for (auto& m : doc["models"]) {
      const auto id = jget<std::string>(m, "id", "");
      std::string missing;
      for (const auto& f : strings(jget(m, "required_features",
                                        Json::array()))) {
        if (!host.has_feature(f)) {
          missing = f;
          break;
        }
      }
      const auto shape = jget<std::string>(
          jget(jget(m, "engine", Json::object()), "vpipe", Json::object()),
          "shape", "");
      if (!missing.empty()) {
        e.withheld.push_back({"model:" + id, "needs-feature", std::format(
            "{} needs extension feature {}", id, missing)});
      } else if (!shape.empty() && !host.has_shape(shape)) {
        e.withheld.push_back({"model:" + id, "needs-shape", std::format(
            "{} needs graph shape {}", id, shape)});
      } else {
        kept.push_back(std::move(m));
      }
    }
    doc["models"] = std::move(kept);
  }
  e.doc = std::move(doc);
  e.state = e.withheld.empty() ? "ready" : "partial";
  return e;
}

std::vector<Root>
standard_roots(const fs::path& support)
{
  std::vector<Root> out;
  // Valtz.app/Contents/MacOS/Valtz -> Contents/Extensions.
  if (const fs::path exe = executable_dir(); !exe.empty()) {
    out.push_back({exe / ".." / "Extensions", true});
  }
  out.push_back({support / "extensions", false});
  if (const char* env = std::getenv("VALTZ_EXTENSIONS"); env && *env) {
    std::string_view s(env);
    while (!s.empty()) {
      const auto colon = s.find(':');
      const auto part = s.substr(0, colon);
      if (!part.empty()) {
        out.push_back({fs::path(std::string(part)), false});
      }
      if (colon == std::string_view::npos) {
        break;
      }
      s.remove_prefix(colon + 1);
    }
  }
  return out;
}

std::vector<Extension>
discover(const std::vector<Root>& roots,
         const std::set<std::string>& disabled, const Interface& host)
{
  std::vector<Extension> out;
  std::set<std::string> seen;
  for (const auto& root : roots) {
    // A root is a folder of packages, or a package itself (a dev's
    // VALTZ_EXTENSIONS=/path/to/acme.valtzext).
    std::vector<fs::path> pkgs;
    if (is_package(root.dir)) {
      pkgs.push_back(root.dir);
    } else {
      std::error_code ec;
      for (const auto& d : fs::directory_iterator(root.dir, ec)) {
        if (is_package(d.path())) {
          pkgs.push_back(d.path());
        }
      }
      std::sort(pkgs.begin(), pkgs.end());
    }
    for (const auto& p : pkgs) {
      Extension e = read_package(p, host);
      e.builtin = root.builtin;
      if (e.state != "refused" && !seen.insert(e.id).second) {
        refuse(e, "duplicate-id", {{"id", e.id}});
      } else if (e.admitted() && disabled.contains(e.id)) {
        e.state = "disabled";
      }
      out.push_back(std::move(e));
    }
  }
  return out;
}

std::set<std::string>
read_disabled(const fs::path& file)
{
  std::ifstream in(file);
  if (!in) {
    return {};
  }
  const Json j = Json::parse(in, nullptr, /*allow_exceptions=*/false);
  const auto ids = strings(jget(j, "disabled", Json::array()));
  return {ids.begin(), ids.end()};
}

Status
write_disabled(const fs::path& file, const std::set<std::string>& ids)
{
  const Json j = {{"disabled", Json(std::vector<std::string>(ids.begin(),
                                                             ids.end()))}};
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << to_text(j, 2) << "\n";
    if (!out) {
      return make_error(Code::Io, std::format("cannot write {}",
                                              tmp.string()));
    }
  }
  std::error_code ec;
  fs::rename(tmp, file, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot write {}: {}",
                                            file.string(), ec.message()));
  }
  return {};
}

Json
to_json(const Extension& e, std::string_view lang)
{
  Json plugins = Json::array();
  for (const auto& p : e.plugins) {
    plugins.push_back({{"file", p.file.lexically_relative(e.dir).string()},
                       {"abi", p.abi},
                       {"required_features", p.required},
                       {"stages", p.stages}});
  }
  Json withheld = Json::array();
  for (const auto& w : e.withheld) {
    withheld.push_back({{"item", w.item}, {"why", w.why},
                        {"detail", w.detail}});
  }
  // What it brings, by kind.
  Json models = Json::array();
  for (const auto& m : jget(e.doc, "models", Json::array())) {
    models.push_back(jget<std::string>(m, "id", ""));
  }
  Json skills = Json::array();
  for (const auto& s : jget(e.doc, "skills", Json::array())) {
    skills.push_back(jget<std::string>(s, "id", ""));
  }
  return {
    {"id", e.id},
    {"version", e.version},
    {"name", text_in(e.name, lang)},
    {"description", text_in(e.description, lang)},
    {"vendor", e.vendor},
    {"license", e.license},
    {"interface", e.interface},
    {"interface_min", e.interface_min},
    {"builtin", e.builtin},
    {"dir", e.dir.string()},
    {"state", e.state},
    {"why", e.why},
    {"why_args", e.why_args},
    {"plugins", std::move(plugins)},
    {"withheld", std::move(withheld)},
    {"models", std::move(models)},
    {"skills", std::move(skills)},
    {"recognize",
     jget(e.doc, "recognize", Json::array()).size()},
  };
}

}
