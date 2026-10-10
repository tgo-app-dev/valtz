#include "testing.h"

#include "valtz/assist/assistant.h"
#include "valtz/controller/controller.h"
#include "valtz/ext/extension.h"
#include "valtz/models/tuning.h"

#include <fstream>

using namespace valtz;
namespace fs = std::filesystem;

namespace {

Json
manifest(int interface, std::string id = "com.acme.test")
{
  return {{"format", ext::kFormat}, {"id", std::move(id)},
          {"interface", interface}, {"version", "1.0.0"}};
}

// A package on disk: its manifest as CBOR, and any files beside it.
fs::path
write_package(const fs::path& dir, const Json& doc,
              const std::vector<std::pair<std::string, std::string>>&
                  files = {})
{
  fs::create_directories(dir);
  const auto bytes = to_cbor(doc);
  std::ofstream(dir / ext::kManifestFile, std::ios::binary)
      .write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  for (const auto& [rel, text] : files) {
    fs::create_directories((dir / rel).parent_path());
    std::ofstream(dir / rel) << text;
  }
  return dir;
}

// A safetensors file with this header (its tensors' bytes left out:
// recognition reads the header only).
fs::path
weights(const fs::path& p, const Json& header)
{
  const std::string h = header.dump();
  std::ofstream f(p, std::ios::binary);
  const std::uint64_t n = h.size();
  f.write(reinterpret_cast<const char*>(&n), 8);
  f << h;
  return p;
}

// A model an extension brings: an image family's, by the picture shape.
Json
acme_model(std::string id)
{
  return {{"id", id},
          {"name", {{"en", "Acme Image"}, {"zh-Hans", "Acme 图像"}}},
          {"family", "acme"}, {"role", "image"},
          {"hf_path", "acme/" + id},
          {"capabilities", {"text-to-image"}},
          {"min_ram_gb", 16}, {"rank", 1},
          {"preview_with", "taef2"},
          {"engine",
           {{"vpipe", {{"config_stage", "acme-model-config"},
                       {"accel", {"sage_attn"}},
                       {"defaults", {{"steps", 12}}}}}}},
          {"prompting", {{"reference_tag", "<ref{n}>"},
                         {"template", "acme style: {prompt}"},
                         {"enhance", {{"generate", "acme-t2i"}}}}}};
}

}

// The window: a host at N reads what was written for oldest..N, and what
// was written for later when its author says an older reader reads it
// correctly. The verdict names why not.
TEST(ext, admits_within_the_window)
{
  ext::Interface host;
  host.current = 3;
  host.oldest = 2;
  host.features = {"catalog/1"};
  CHECK(ext::admit(manifest(2), host).ok());
  CHECK(ext::admit(manifest(3), host).ok());
  const auto old = ext::admit(manifest(1), host);
  CHECK(old.why == "too-old");
  CHECK(jget(old.args, "oldest", 0) == 2);
  CHECK(ext::admit(manifest(4), host).why == "too-new");
  Json readable = manifest(4);
  readable["interface_min"] = 3;
  CHECK(ext::admit(readable, host).ok());
  Json later = manifest(5);
  later["interface_min"] = 4;
  const auto v = ext::admit(later, host);
  CHECK(v.why == "too-new");
  CHECK(jget(v.args, "interface", 0) == 4);

  Json need = manifest(3);
  need["required_features"] = {"catalog/1", "graph-template/1"};
  const auto nf = ext::admit(need, host);
  CHECK(nf.why == "needs-feature");
  CHECK(jget<std::string>(nf.args, "feature", "") == "graph-template/1");

  Json wrong = manifest(3);
  wrong["format"] = "something-else";
  CHECK(ext::admit(wrong, host).why == "not-an-extension");
  CHECK(ext::admit(manifest(3, "Acme Video"), host).why == "bad-id");
  CHECK(ext::admit(manifest(3, "acme"), host).why == "bad-id");
  Json none = manifest(3);
  none.erase("interface");
  CHECK(ext::admit(none, host).why == "no-interface");
  CHECK(!ext::admit(Json::array(), host).ok());

  // This build: its own interface, and the features it lists.
  const auto& me = ext::Interface::host();
  CHECK(me.current == ext::kInterface);
  CHECK(static_cast<int>(me.upgraders.size()) == me.current - me.oldest);
  CHECK(me.has_feature(ext::kFeatureCatalog));
  CHECK(me.has_shape("diffusion-image") && !me.has_shape("acme"));
}

// An older manifest is rewritten, step by step, into the current
// schema: the reader knows only that one.
TEST(ext, upgrades_old_manifests)
{
  ext::Interface host;
  host.current = 3;
  host.oldest = 1;
  // 1 -> 2: a model's "ram" became "min_ram_gb".
  host.upgraders.push_back([](Json& d) -> Status {
    for (auto& m : d["models"]) {
      if (m.contains("ram")) {
        m["min_ram_gb"] = m["ram"];
        m.erase("ram");
      }
    }
    return {};
  });
  // 2 -> 3: "tag" moved under prompting, as reference_tag.
  host.upgraders.push_back([](Json& d) -> Status {
    for (auto& m : d["models"]) {
      if (m.contains("tag")) {
        m["prompting"]["reference_tag"] = m["tag"];
        m.erase("tag");
      }
    }
    return {};
  });
  Json v1 = manifest(1);
  v1["models"] = {{{"id", "a"}, {"ram", 24}, {"tag", "<p{n}>"}}};
  REQUIRE(ext::admit(v1, host).ok());
  REQUIRE_OK(ext::upgrade(v1, host));
  CHECK(jget(v1, "interface", 0) == 3);
  CHECK(jget(v1["models"][0], "min_ram_gb", 0) == 24);
  CHECK(!v1["models"][0].contains("ram"));
  CHECK(jget<std::string>(v1["models"][0]["prompting"], "reference_tag",
                          "") == "<p{n}>");

  // Written for the current one, or a later one: left as it is.
  Json v3 = manifest(3);
  v3["models"] = {{{"id", "a"}, {"tag", "kept"}}};
  REQUIRE_OK(ext::upgrade(v3, host));
  CHECK(jget<std::string>(v3["models"][0], "tag", "") == "kept");

  // A host whose chain has a gap says so.
  ext::Interface broken = host;
  broken.upgraders.pop_back();
  Json v2 = manifest(2);
  CHECK(!ext::upgrade(v2, broken).ok());
}

// A package as read from disk: CBOR decoded, keys this host does not
// know passed over, its plugins found inside it, a model needing what
// this host lacks withheld while the rest is offered.
TEST(ext, reads_a_package)
{
  const auto root = test::temp_dir("ext-pkg");
  Json doc = manifest(ext::kInterface);
  doc["name"] = {{"en", "Acme"}, {"zh-Hans", "Acme 扩展"}};
  doc["vendor"] = "Acme Inc.";
  doc["a_key_from_a_later_valtz"] = {{"anything", 1}};
  doc["vpipe"] = {{"plugins", {{{"file", "plugins/acme.so"}, {"abi", 8},
                                {"required_features",
                                 {"stage-commands/1"}},
                                {"stages", {"acme-generate"}}}}}};
  Json later = acme_model("acme-later");
  later["required_features"] = {"graph-template/1"};
  Json shaped = acme_model("acme-shaped");
  shaped["engine"]["vpipe"]["shape"] = "acme-dit";
  doc["models"] = {acme_model("acme-v1"), later, shaped};
  const auto dir = write_package(root / "acme.valtzext", doc,
                                 {{"plugins/acme.so", "not a dylib"}});
  const auto e = ext::read_package(dir);
  CHECK(e.id == "com.acme.test");
  CHECK(e.state == "partial");
  CHECK(e.admitted());
  REQUIRE(e.plugins.size() == 1);
  CHECK(e.plugins[0].file == dir / "plugins/acme.so");
  CHECK(e.plugins[0].abi == 8);
  CHECK(e.plugins[0].stages == std::vector<std::string>{"acme-generate"});
  REQUIRE(e.withheld.size() == 2);
  CHECK(e.withheld[0].item == "model:acme-later");
  CHECK(e.withheld[0].why == "needs-feature");
  CHECK(e.withheld[1].why == "needs-shape");
  REQUIRE(e.doc["models"].size() == 1);
  CHECK(e.doc["models"][0]["id"] == "acme-v1");
  const Json j = ext::to_json(e, "zh-Hans");
  CHECK(jget<std::string>(j, "name", "") == "Acme 扩展");
  CHECK(jget<std::string>(ext::to_json(e, "fr"), "name", "") == "Acme");

  // A plugin outside the package, or not there: nothing of it runs.
  Json out = manifest(ext::kInterface);
  out["vpipe"] = {{"plugins", {{{"file", "../elsewhere.so"}, {"abi", 8}}}}};
  const auto o = ext::read_package(
      write_package(root / "out.valtzext", out));
  CHECK(o.state == "backend-failed");
  CHECK(o.why == "bad-path");
  out["vpipe"]["plugins"][0]["file"] = "plugins/gone.so";
  const auto g = ext::read_package(
      write_package(root / "gone.valtzext", out));
  CHECK(g.why == "missing-file");

  // Not a package: no manifest; a manifest that is not CBOR (authored
  // JSON, not packed).
  fs::create_directories(root / "empty.valtzext");
  CHECK(ext::read_package(root / "empty.valtzext").why == "no-manifest");
  fs::create_directories(root / "json.valtzext");
  std::ofstream(root / "json.valtzext" / ext::kManifestFile)
      << to_text(manifest(1));
  const auto js = ext::read_package(root / "json.valtzext");
  CHECK(js.state == "refused");
  CHECK(js.why == "unreadable");
  // Too new for this Valtz: refused, still listed by its id.
  const auto tn = ext::read_package(write_package(
      root / "new.valtzext", manifest(ext::kInterface + 1, "com.acme.new")));
  CHECK(tn.why == "too-new");
  CHECK(tn.id == "com.acme.new");
}

// Roots in order, each one's packages by name; the second package of an
// id is refused; a package the user turned off is listed, disabled; a
// root may be a package itself.
TEST(ext, discovers_in_order)
{
  const auto root = test::temp_dir("ext-roots");
  write_package(root / "app/b.valtzext", manifest(1, "com.acme.b"));
  write_package(root / "app/a.valtzext", manifest(1, "com.acme.a"));
  write_package(root / "user/a2.valtzext", manifest(1, "com.acme.a"));
  write_package(root / "user/c.valtzext", manifest(1, "com.acme.c"));
  write_package(root / "dev.valtzext", manifest(1, "com.acme.dev"));
  std::ofstream(root / "user/notes.txt") << "not a package";
  const auto found = ext::discover(
      {{root / "app", true}, {root / "user", false},
       {root / "dev.valtzext", false}, {root / "missing", false}},
      {"com.acme.c"});
  REQUIRE(found.size() == 5);
  CHECK(found[0].id == "com.acme.a" && found[0].builtin);
  CHECK(found[0].state == "ready");
  CHECK(found[1].id == "com.acme.b");
  CHECK(found[2].id == "com.acme.a" && found[2].why == "duplicate-id");
  CHECK(found[3].id == "com.acme.c" && found[3].state == "disabled");
  CHECK(found[4].id == "com.acme.dev" && found[4].state == "ready");

  const auto file = root / "extensions.json";
  CHECK(ext::read_disabled(file).empty());
  REQUIRE_OK(ext::write_disabled(file, {"com.acme.c", "com.acme.b"}));
  CHECK((ext::read_disabled(file) ==
         std::set<std::string>{"com.acme.b", "com.acme.c"}));
}

// What a package brings, laid over the built-in catalog: a model in a
// family of its own, Auto's list, a skill, weight rules -- each checked
// as the built-in catalog is, a broken entry withheld, not the package.
TEST(ext, catalog_takes_a_contribution)
{
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  const auto root = test::temp_dir("ext-cat");
  write_package(root, manifest(1), {{"skills/t2i.md", "Rewrite: {x}"}});
  Json doc = manifest(1);
  Json dup = acme_model("krea2-turbo");
  Json future = acme_model("acme-3d");
  future["capabilities"] = {"text-to-3d"};
  Json dangling = acme_model("acme-dangling");
  dangling["preview_with"] = "nope";
  doc["models"] = {acme_model("acme-v1"), dup, future, dangling};
  doc["families"] = {{{"id", "acme"},
                      {"name", {{"en", "Acme"}, {"zh-Hans", "Acme 家族"}}},
                      {"features", {"image-gen", "hologram"}},
                      {"members", {{{"model", "acme-v1"},
                                    {"label", "v1"}},
                                   "acme-dangling"}}}};
  doc["families"].push_back({{"name", "no id"}, {"members", {"acme-v1"}}});
  doc["auto"] = {{"image", {{"generate", {{"add", {"acme-v1", "nope"}},
                                          {"before", "z-image-turbo"}}}}}};
  doc["skills"] = {{{"id", "acme-t2i"}, {"file", "skills/t2i.md"}},
                   {{"id", "acme-gone"}, {"file", "skills/none.md"}},
                   {{"id", "acme-out"}, {"file", "../../etc/passwd"}}};
  doc["recognize"] = {{{"kind", "lora"}, {"family", "acme"},
                       {"keys_any", {"acme_blocks."}}},
                      {{"kind", "hologram"}}};
  const models::Contributor who{"com.acme.test", "1.0.0", root, "zh-Hans"};
  const auto w = cat->add(doc, who);

  const auto* m = cat->find("acme-v1");
  REQUIRE(m);
  CHECK(m->name == "Acme 图像");
  CHECK(m->origin == "com.acme.test" && m->origin_version == "1.0.0");
  CHECK(cat->find("acme-3d") == nullptr);
  CHECK(cat->find("acme-dangling") == nullptr);
  // The built-in model of that id is the built-in one still.
  CHECK(cat->find("krea2-turbo")->origin.empty());
  auto withheld = [&](const std::string& item, const std::string& why) {
    return std::any_of(w.begin(), w.end(), [&](const models::Withheld& x) {
      return x.item == item && x.why == why;
    });
  };
  CHECK(withheld("model:krea2-turbo", "duplicate-id"));
  CHECK(withheld("model:acme-3d", "unknown-capability"));
  CHECK(withheld("model:acme-dangling", "unknown-reference"));
  CHECK(withheld("family:acme", "unknown-feature"));
  CHECK(withheld("family:acme", "unknown-reference"));
  CHECK(withheld("auto:image/generate:nope", "unknown-reference"));
  CHECK(withheld("skill:acme-gone", "missing-file"));
  CHECK(withheld("skill:acme-out", "bad-path"));
  CHECK(withheld("recognize:1", "bad-entry"));
  CHECK(withheld("family:", "bad-entry"));

  const auto& fams = cat->families();
  const auto f = std::find_if(fams.begin(), fams.end(),
                              [](const auto& x) { return x.id == "acme"; });
  REQUIRE(f != fams.end());
  CHECK(f->name == "Acme 家族");
  CHECK(f->features == std::vector<std::string>{"image-gen"});
  REQUIRE(f->members.size() == 1);
  CHECK(f->members[0].label == "v1");
  CHECK((cat->auto_order("image", "generate") ==
         std::vector<std::string>{"krea2-turbo", "qwen-image-2.1-turbo",
                                  "qwen-image-2.1", "acme-v1",
                                  "z-image-turbo", "flux2-klein-9b"}));
  REQUIRE(cat->skill("acme-t2i"));
  CHECK(*cat->skill("acme-t2i") == "Rewrite: {x}");
  CHECK(cat->skill("qwen-image-2.1-t2i") == nullptr);  // not an ext's
  REQUIRE(cat->recognize_rules().size() == 1);
  CHECK(jget<std::string>(cat->recognize_rules()[0], "origin", "") ==
        "com.acme.test");
  Json j;
  to_json(j, *m);
  CHECK(jget<std::string>(j, "origin", "") == "com.acme.test");
  CHECK(ext::shape_of(*m) == "diffusion-image");
}

// A family's own knobs, declared by its extension: offered after
// Valtz's, with the text the panel shows; held to their ranges; a key
// Valtz has is not taken.
TEST(ext, declared_tuning_options)
{
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  Json m = acme_model("acme-v1");
  m["engine"]["vpipe"]["options"] = {
    {{"key", "acme_cache"}, {"type", "bool"},
     {"presets", {{"speed", true}}},
     {"label", {{"en", "Acme cache"}, {"zh-Hans", "Acme 缓存"}}},
     {"note", "Reuses steps"}, {"group", "compute"}},
    {{"key", "acme_window"}, {"type", "int"}, {"min", 1}, {"max", 8},
     {"default", 4}, {"stage", "config"}, {"label", "Window"}},
    {{"key", "acme_int8"}, {"type", "bool"}, {"requires", "matrix-cores"}},
    {{"key", "steps"}, {"type", "int"}, {"label", "Not Valtz's steps"}},
    {{"key", "Bad Key"}, {"type", "bool"}}};
  Json doc = manifest(1);
  doc["models"] = {m};
  CHECK(cat->add(doc, {"com.acme.test", "1", {}, "zh-Hans"}).empty());
  const auto* e = cat->find("acme-v1");
  REQUIRE(e);
  models::TuningContext ctx;
  const Json t = models::tuning_options(*e, ctx, "speed", false);
  const Json* cache = nullptr;
  const Json* window = nullptr;
  const Json* i8 = nullptr;
  int steps = 0;
  for (const auto& o : t["options"]) {
    const auto k = jget<std::string>(o, "key", "");
    cache = k == "acme_cache" ? &o : cache;
    window = k == "acme_window" ? &o : window;
    i8 = k == "acme_int8" ? &o : i8;
    steps += k == "steps" ? 1 : 0;
    CHECK(k != "Bad Key");
  }
  REQUIRE(cache && window && i8);
  CHECK(steps == 1);
  CHECK(jget(*cache, "declared", false));
  CHECK(jget<std::string>(*cache, "label", "") == "Acme 缓存");
  CHECK(jget<std::string>(*cache, "group", "") == "compute");
  CHECK(jget<std::string>(*window, "type", "") == "int");
  CHECK(!jget(*i8, "available", true));
  CHECK(jget<std::string>(*i8, "why", "") == "needs-matrix-cores");
  CHECK(jget(t["values"], "acme_cache", false));       // speed's preset
  CHECK(jget(t["values"], "acme_window", 0) == 4);
  const Json v = models::resolve_tuning(
      *e, ctx, "balanced", false,
      {{"acme_window", 20}, {"acme_int8", true}});
  CHECK(!jget(v, "acme_cache", true));                 // the default
  CHECK(jget(v, "acme_window", 0) == 8);               // held to its range
  CHECK(!jget(v, "acme_int8", true));                  // not on this Mac
}

// A dropped weight file, filed by an extension's rule before the
// heuristics: its kind and the family it belongs to.
TEST(ext, recognition_rules_file_weights)
{
  const auto dir = test::temp_dir("ext-weights");
  const Json t = {{"dtype", "BF16"}, {"shape", {1}},
                  {"data_offsets", {0, 2}}};
  const Json rules = {
    {{"kind", "lora"}, {"family", "acme"}, {"origin", "com.acme.test"},
     {"keys_any", {"acme_blocks."}},
     {"metadata", {{"modelspec.architecture", "acme-*"}}}},
    {{"kind", "dit"}, {"family", "acme"}, {"origin", "com.acme.test"},
     {"class_name", {"AcmeTransformer2DModel"}}},
    {{"kind", "vae"}}};  // states nothing: never matches
  const auto lora = weights(dir / "style.safetensors",
                            {{"__metadata__",
                              {{"modelspec.architecture", "acme-v1/lora"}}},
                             {"acme_blocks.0.attn.lora_A.weight", t}});
  Json k = models::classify_checkpoint(lora, rules);
  CHECK(jget<std::string>(k, "kind", "") == "lora");
  CHECK(jget<std::string>(k, "family", "") == "acme");
  CHECK(jget<std::string>(k, "origin", "") == "com.acme.test");
  // Its metadata says otherwise: the heuristics decide, no family.
  const auto other = weights(dir / "other.safetensors",
                             {{"__metadata__",
                               {{"modelspec.architecture", "flux"}}},
                              {"acme_blocks.0.attn.lora_A.weight", t},
                              {"acme_blocks.0.attn.lora_B.weight", t}});
  k = models::classify_checkpoint(other, rules);
  CHECK(jget<std::string>(k, "kind", "") == "lora");
  CHECK(jget<std::string>(k, "family", "x").empty());
  // A diffusers folder by its class.
  fs::create_directories(dir / "dit");
  std::ofstream(dir / "dit" / "config.json")
      << R"({"_class_name": "AcmeTransformer2DModel"})";
  k = models::classify_checkpoint(dir / "dit", rules);
  CHECK(jget<std::string>(k, "kind", "") == "dit");
  CHECK(jget<std::string>(k, "family", "") == "acme");
  CHECK(jget<std::string>(models::classify_checkpoint(dir / "dit",
                                                      Json::array()),
                          "family", "x").empty());
}

// The controller finds packages, offers what they bring -- the model,
// its skill, its rules, its prompt template -- and keeps the user's
// choice to turn one off for the next launch.
TEST(ext, controller_takes_extensions)
{
  const auto root = test::temp_dir("ext-ctl");
  Json doc = manifest(1);
  doc["name"] = "Acme";
  doc["models"] = {acme_model("acme-v1")};
  doc["families"] = {{{"id", "acme"}, {"name", "Acme"},
                      {"features", {"image-gen"}},
                      {"members", {"acme-v1"}}}};
  doc["skills"] = {{{"id", "acme-t2i"}, {"file", "skills/t2i.md"}}};
  doc["recognize"] = {{{"kind", "lora"}, {"family", "acme"},
                       {"keys_any", {"acme_blocks."}}}};
  write_package(root / "exts" / "acme.valtzext", doc,
                {{"skills/t2i.md", "Acme's own instructions."}});
  write_package(root / "exts" / "future.valtzext",
                manifest(ext::kInterface + 1, "com.acme.future"));

  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extension_roots = {root / "exts"};
  {
    auto c = Controller::create(cfg);
    REQUIRE_OK(c);
    const Json r = (*c)->extensions();
    CHECK(jget(r["interface"], "current", 0) == ext::kInterface);
    REQUIRE(r["extensions"].size() == 2);
    const Json& a = r["extensions"][0];
    CHECK(jget<std::string>(a, "id", "") == "com.acme.test");
    CHECK(jget<std::string>(a, "state", "") == "ready");
    CHECK(a["models"] == Json({"acme-v1"}));
    CHECK(jget<std::string>(r["extensions"][1], "why", "") == "too-new");
    const auto* m = (*c)->catalog().find("acme-v1");
    REQUIRE(m);
    CHECK(m->origin == "com.acme.test");
    CHECK((*c)->skill_text("acme-t2i") == "Acme's own instructions.");
    // Nothing downloaded: Valtz's own Qwen-Image guide.
    CHECK((*c)->skill_text("qwen-image-2.1-t2i") ==
          assist::rewrite_prompt("qwen-image-2.1-t2i"));
    CHECK((*c)->skill_text("nothing").empty());
    const Json fam = (*c)->capability_tree();
    bool listed = false;
    for (const auto& f : fam["families"]) {
      for (const auto& mem : f["members"]) {
        listed = listed || (jget<std::string>(mem, "model", "") ==
                                "acme-v1" &&
                            jget<std::string>(mem, "origin", "") ==
                                "com.acme.test");
      }
    }
    CHECK(listed);
    const Json t = {{"dtype", "BF16"}, {"shape", {1}},
                    {"data_offsets", {0, 2}}};
    const auto w = weights(root / "w.safetensors",
                           {{"acme_blocks.0.lora_A.weight", t}});
    CHECK(jget<std::string>((*c)->classify_weights(w), "family", "") ==
          "acme");
    CHECK(!(*c)->set_extension_enabled("com.acme.nobody", false).ok());
    REQUIRE_OK((*c)->set_extension_enabled("com.acme.test", false));
  }
  {
    // The next launch: listed, disabled, nothing of it offered.
    auto c = Controller::create(cfg);
    REQUIRE_OK(c);
    const Json r = (*c)->extensions();
    CHECK(jget<std::string>(r["extensions"][0], "state", "") ==
          "disabled");
    CHECK((*c)->catalog().find("acme-v1") == nullptr);
    CHECK((*c)->skill_text("acme-t2i").empty());
    REQUIRE_OK((*c)->set_extension_enabled("com.acme.test", true));
  }
  cfg.extensions = false;
  auto c = Controller::create(cfg);
  REQUIRE_OK(c);
  CHECK((*c)->extensions()["extensions"].empty());
}
