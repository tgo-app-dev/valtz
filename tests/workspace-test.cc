#include "testing.h"

#include "valtz/project/project-file.h"
#include "valtz/project/workspace.h"

#include <fstream>

using namespace valtz;
using namespace valtz::project;

namespace fs = std::filesystem;

namespace {

Result<AssetId>
text(Workspace& w, const std::string& name, const std::string& words,
     const std::string& kind = "text.add")
{
  auto cmd = w.undo().command(kind, {{"name", name}});
  VALTZ_ASSIGN(Asset a, w.project().add_text(name, words));
  return a.id;
}

bool
has(Project& p, AssetId id)
{
  return p.asset(id).ok();
}

std::size_t
blob_files(const fs::path& root)
{
  std::size_t n = 0;
  std::error_code ec;
  if (!fs::exists(root, ec)) {
    return 0;
  }
  for (auto it = fs::recursive_directory_iterator(root, ec);
       it != fs::recursive_directory_iterator(); ++it) {
    n += it->is_regular_file() ? 1 : 0;
  }
  return n;
}

}

// A record keeps what it does not know -- a newer Valtz's fields, an
// extension's data, a kind of its own -- through a read and a write.
TEST(workspace, records_keep_what_they_do_not_know)
{
  const Json in = {
      {"id", AssetId::make()},
      {"name", "mesh"},
      {"kind", "mesh"},
      {"origin", "source"},
      {"future", {{"a", 1}}},
      {"x", {{"com.example.3d", {{"vertices", 12}}}}},
      {"layers", Json::array({{{"id", ""},
                               {"name", "base"},
                               {"x", {{"com.example.3d", true}}}}})},
      {"modifiers", Json::array({{{"kind", "blur"},
                                  {"layer", ""},
                                  {"params", {{"r", 2}}},
                                  {"x", {{"com.example.fx", 1}}}}})},
  };
  const Asset a = in.get<Asset>();
  CHECK(a.kind == AssetKind::Other);
  const Json out = Json(a);
  CHECK(jget<std::string>(out, "kind", "") == "mesh");
  CHECK(out.contains("future") && out["future"]["a"] == 1);
  CHECK(out["x"]["com.example.3d"]["vertices"] == 12);
  CHECK(out["layers"][0]["x"]["com.example.3d"] == true);
  CHECK(out["modifiers"][0]["x"]["com.example.fx"] == 1);
  CHECK(out["modifiers"][0]["kind"] == "blur");
}

// Save, revert, and a working copy left dirty -- resumed with its history.
TEST(workspace, saves_reverts_and_resumes)
{
  const fs::path root = test::temp_dir("ws-save");
  const fs::path pkg = root / "Saved.valtz";
  const fs::path work = root / "work";
  auto made = Workspace::create(pkg, "Saved", work);
  REQUIRE_OK(made);
  Workspace& w = **made;
  CHECK(fs::exists(pkg / kProjectFile));
  CHECK(!w.dirty());
  auto a = text(w, "a", "first words");
  REQUIRE_OK(a);
  CHECK(w.dirty());
  // The new medium is the working copy's until a save.
  CHECK(blob_files(w.project().blobs().root()) == 1);
  CHECK(blob_files(pkg / "blobs") == 0);
  REQUIRE_OK(w.save());
  CHECK(!w.dirty());
  CHECK(blob_files(w.project().blobs().root()) == 0);
  CHECK(blob_files(pkg / "blobs") == 1);
  auto words = w.project().read_text(*a);
  REQUIRE_OK(words);
  CHECK(*words == "first words");      // read from the package's blobs

  // Revert: back to the save.
  auto b = text(w, "b", "second");
  REQUIRE_OK(b);
  CHECK(w.dirty());
  REQUIRE_OK(w.revert());
  CHECK(!w.dirty());
  CHECK(has(w.project(), *a));
  CHECK(!has(w.project(), *b));
  CHECK(!w.undo().next_undo());
  CHECK(blob_files(w.project().blobs().root()) == 0);

  // Left dirty: kept, and resumed with its change and history.
  auto c = text(w, "c", "third");
  REQUIRE_OK(c);
  REQUIRE_OK(w.close(false));
  made->reset();
  WorkspaceReport report;
  auto again = Workspace::open(pkg, work, &report);
  REQUIRE_OK(again);
  CHECK(report.recovered);
  CHECK(has((*again)->project(), *c));
  REQUIRE((*again)->undo().next_undo().has_value());
  CHECK((*again)->undo().next_undo()->kind == "text.add");
  REQUIRE_OK((*again)->undo().undo());
  CHECK(!has((*again)->project(), *c));
  CHECK(!(*again)->dirty());           // back where it was saved
  const fs::path dir = (*again)->dir();
  REQUIRE_OK((*again)->close(true));
  CHECK(!fs::exists(dir));
  // A package saved since a working copy was left: made again.
  auto fresh = Workspace::open(pkg, work, &report);
  REQUIRE_OK(fresh);
  CHECK(!report.recovered);
}

// Undo and redo: commands, a new one clearing redo, coalescing, joins.
TEST(workspace, undo_redo_coalesce_and_join)
{
  const fs::path root = test::temp_dir("ws-undo");
  auto made = Workspace::create(root / "U.valtz", "U", root / "work");
  REQUIRE_OK(made);
  Workspace& w = **made;
  Project& p = w.project();
  auto a = text(w, "a", "words");
  REQUIRE_OK(a);
  // A slider's steps on the same asset: one command.
  for (int i = 1; i <= 3; ++i) {
    auto cmd = w.undo().command("adjust", {}, a->str(), "");
    REQUIRE_OK(p.set_modifiers(*a, {{"adjust", "", {{"exposure", i}}}}));
  }
  auto mods = [&] {
    auto x = p.asset(*a);
    return x.ok() ? x->modifiers : std::vector<Modifier>{};
  };
  REQUIRE(mods().size() == 1);
  CHECK(mods()[0].params["exposure"] == 3);
  REQUIRE_OK(w.undo().undo());
  CHECK(mods().empty());               // all three at once
  REQUIRE_OK(w.undo().redo());
  CHECK(mods()[0].params["exposure"] == 3);
  // Another kind: a command of its own.
  {
    auto cmd = w.undo().command("asset.rename", {}, a->str());
    REQUIRE_OK(p.rename_asset(*a, "renamed"));
  }
  REQUIRE_OK(w.undo().undo());
  CHECK(p.asset(*a)->name == "a");
  CHECK(w.undo().next_redo()->kind == "asset.rename");
  // A new action: no redo.
  auto b = text(w, "b", "more");
  REQUIRE_OK(b);
  CHECK(!w.undo().next_redo());
  // Nested scopes are one command.
  {
    auto outer = w.undo().command("asset.capture");
    auto inner = w.undo().command("asset.rename");
    CHECK(inner.seq() == outer.seq());
    REQUIRE_OK(p.rename_asset(*b, "b2"));
  }
  CHECK(w.undo().next_undo()->kind == "asset.capture");
  // A job's commit joins its submission while that is the last command;
  // after another, it is a command of its own.
  std::uint64_t job = 0;
  AssetId c;
  {
    auto cmd = w.undo().command("generate", {{"name", "c"}});
    auto made_c = p.add_text("c", "submitted");
    REQUIRE_OK(made_c);
    c = made_c->id;
    job = cmd.seq();
  }
  {
    auto cmd = w.undo().join(job);
    REQUIRE_OK(p.rename_asset(c, "c, done"));
  }
  REQUIRE_OK(w.undo().undo());         // both, as one
  CHECK(!has(p, c));
  REQUIRE_OK(w.undo().redo());
  CHECK(p.asset(c)->name == "c, done");
  {
    auto cmd = w.undo().command("asset.rename");
    REQUIRE_OK(p.rename_asset(*b, "b3"));
  }
  {
    auto cmd = w.undo().join(job);
    CHECK(cmd.seq() != job);
    REQUIRE_OK(p.rename_asset(c, "c, again"));
  }
  REQUIRE_OK(w.undo().undo());
  CHECK(p.asset(c)->name == "c, done");
  CHECK(p.asset(*b)->name == "b3");
  // Written in no command: not undoable, but dirty.
  REQUIRE_OK(w.save());
  REQUIRE_OK(p.rename_asset(*b, "quietly"));
  CHECK(w.dirty());
}

// A file from a Valtz with extensions this one lacks -- or a newer one --
// opens, and its unknown parts are written back at the next save.
TEST(workspace, unknown_parts_are_kept)
{
  const fs::path root = test::temp_dir("ws-unknown");
  const fs::path pkg = root / "X.valtz";
  {
    auto made = Workspace::create(pkg, "X", root / "work");
    REQUIRE_OK(made);
    REQUIRE_OK(text(**made, "plain", "words"));
    REQUIRE_OK((*made)->save());
    REQUIRE_OK((*made)->close(true));
  }
  // What another Valtz would have written.
  auto doc = read_project_file(pkg / kProjectFile);
  REQUIRE_OK(doc);
  (*doc)["version"] = 3;                       // newer, readable by 2
  (*doc)["x"] = {{"com.example.timeline", {{"tracks", 2}}}};
  (*doc)["future"] = "kept";
  (*doc)["tables"]["timelines"] = Json::array({{{"id", "t1"}}});
  (*doc)["meta"]["future.key"] = 7;
  (*doc)["extensions"] = Json::array(
      {{{"id", "com.example.timeline"}, {"version", "1.0"}}});
  auto& asset = (*doc)["tables"]["assets"][0];
  asset["x"] = {{"com.example.timeline", {{"clip", "c1"}}}};
  asset["kind"] = "timeline";
  REQUIRE_OK(write_project_file(pkg / kProjectFile, *doc));
  CHECK((extension_data_in(*doc) ==
         std::vector<std::string>{"com.example.timeline"}));

  WorkspaceReport report;
  auto w = Workspace::open(pkg, root / "work", &report);
  REQUIRE_OK(w);
  CHECK(report.extensions.size() == 1);
  auto list = (*w)->project().assets();
  REQUIRE_OK(list);
  REQUIRE(list->size() == 1);
  CHECK(list->front().kind == AssetKind::Other);
  REQUIRE_OK((*w)->project().rename_asset(list->front().id, "touched"));
  REQUIRE_OK((*w)->save());
  auto back = read_project_file(pkg / kProjectFile);
  REQUIRE_OK(back);
  CHECK((*back)["version"] == kProjectFormat);
  CHECK((*back)["x"]["com.example.timeline"]["tracks"] == 2);
  CHECK((*back)["future"] == "kept");
  CHECK((*back)["tables"]["timelines"][0]["id"] == "t1");
  CHECK((*back)["meta"]["future.key"] == 7);
  CHECK((*back)["extensions"][0]["id"] == "com.example.timeline");
  const auto& a = (*back)["tables"]["assets"][0];
  CHECK(a["name"] == "touched");
  CHECK(a["kind"] == "timeline");
  CHECK(a["x"]["com.example.timeline"]["clip"] == "c1");
  REQUIRE_OK((*w)->close(true));

  // Readable only by a newer Valtz: refused by name.
  (*back)["readable_from"] = kProjectFormat + 1;
  REQUIRE_OK(write_project_file(pkg / kProjectFile, *back));
  auto newer = Workspace::open(pkg, root / "work");
  CHECK(!newer.ok());
  CHECK(newer.code() == Code::Unsupported);
}

// A format-1 package (a project.lmdb) opens; its first save is format 2.
TEST(workspace, a_format_1_package_is_saved_as_format_2)
{
  const fs::path root = test::temp_dir("ws-legacy");
  const fs::path pkg = root / "Old.valtz";
  AssetId id;
  {
    auto old = Project::create(pkg, "Old");
    REQUIRE_OK(old);
    auto a = (*old)->add_text("kept", "from before");
    REQUIRE_OK(a);
    id = a->id;
  }
  WorkspaceReport report;
  auto w = Workspace::open(pkg, root / "work", &report);
  REQUIRE_OK(w);
  CHECK(report.legacy);
  auto words = (*w)->project().read_text(id);
  REQUIRE_OK(words);
  CHECK(*words == "from before");
  CHECK((*w)->project().name() == "Old");
  REQUIRE_OK((*w)->save());
  CHECK(fs::exists(pkg / kProjectFile));
  CHECK(!fs::exists(pkg / kLegacyDatabase));
  REQUIRE_OK((*w)->close(true));
  auto again = Workspace::open(pkg, root / "work", &report);
  REQUIRE_OK(again);
  CHECK(!report.legacy);
  CHECK((*again)->project().asset(id).ok());
}

// Save As: a package of its own with every medium; a removed asset's
// medium kept for undo, and let go past the history's budget.
TEST(workspace, save_as_and_the_media_undo_keeps)
{
  const fs::path root = test::temp_dir("ws-save-as");
  auto made = Workspace::create_untitled(root / "untitled", "Untitled");
  REQUIRE_OK(made);
  Workspace& w = **made;
  CHECK(w.untitled());
  auto a = text(w, "a", "media of a");
  auto b = text(w, "b", "media of b");
  REQUIRE(a.ok() && b.ok());
  CHECK(!w.save().ok());               // untitled: Save As
  const fs::path pkg = root / "Named.valtz";
  REQUIRE_OK(w.save_as(pkg));
  CHECK(!w.untitled());
  CHECK(w.project().name() == "Named");
  CHECK(blob_files(pkg / "blobs") == 2);
  CHECK(!w.dirty());
  // Removed and saved: the package lets its medium go, the working copy
  // keeps it for the history.
  {
    auto cmd = w.undo().command("asset.remove", {}, a->str());
    REQUIRE_OK(w.project().remove_asset(*a));
  }
  REQUIRE_OK(w.save());
  CHECK(blob_files(pkg / "blobs") == 1);
  CHECK(blob_files(w.project().blobs().root()) == 1);
  REQUIRE_OK(w.undo().undo());
  auto words = w.project().read_text(*a);
  REQUIRE_OK(words);
  CHECK(*words == "media of a");
  REQUIRE_OK(w.undo().redo());
  // Past the budget, the history goes, and the medium with it.
  REQUIRE_OK(w.collect_garbage(0));
  CHECK(!w.undo().next_undo());
  CHECK(blob_files(w.project().blobs().root()) == 0);
}
