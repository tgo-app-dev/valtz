#include "testing.h"

#include "valtz/project/project.h"

#include <fstream>
#include <thread>

using namespace valtz;
using namespace valtz::project;

namespace {

// Pretend to be a builder: write an output into the project's tmp dir.
std::filesystem::path
fake_output(Project& p, const std::string& content)
{
  std::filesystem::create_directories(p.blobs().tmp_dir());
  auto path = p.blobs().make_tmp_path("txt");
  std::ofstream(path) << content;
  return path;
}

Result<AssetVersion>
fake_build(Project& p, AssetId id, const std::string& content)
{
  VALTZ_ASSIGN(Asset a, p.asset(id));
  VALTZ_ASSIGN(Recipe r, p.recipe(a.recipe));
  VALTZ_ASSIGN(auto inputs, p.resolve_inputs(r));
  Project::BuildRecord b;
  b.recipe = r;
  b.inputs = inputs;
  b.output = fake_output(p, content);
  b.engine = "test";
  return p.commit_build(id, std::move(b));
}

}

TEST(project, create_reopen_and_text_assets)
{
  auto pkg = test::temp_dir("proj") / "A.valtz";
  ProjectId pid;
  AssetId tid;
  {
    auto p = Project::create(pkg, "Alpha");
    REQUIRE_OK(p);
    pid = (*p)->id();
    auto t = (*p)->add_text("desc", "a red fox");
    REQUIRE_OK(t);
    tid = t->id;
  }
  auto p = Project::open(pkg);
  REQUIRE_OK(p);
  CHECK((*p)->id() == pid);
  CHECK((*p)->name() == "Alpha");
  auto txt = (*p)->read_text(tid);
  REQUIRE_OK(txt);
  CHECK(*txt == "a red fox");
  CHECK(!Project::create(pkg, "again").ok());  // no clobbering
}

TEST(project, identical_content_is_stored_once)
{
  auto pkg = test::temp_dir("proj") / "D.valtz";
  auto p = Project::create(pkg, "Dedup");
  REQUIRE_OK(p);
  auto a = (*p)->add_text("a", "same bytes");
  auto b = (*p)->add_text("b", "same bytes");
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  auto pa = (*p)->media_path(a->id);
  auto pb = (*p)->media_path(b->id);
  REQUIRE_OK(pa);
  REQUIRE_OK(pb);
  CHECK(*pa == *pb);
}

TEST(project, staleness_follows_content)
{
  auto pkg = test::temp_dir("proj") / "S.valtz";
  auto pr = Project::create(pkg, "Stale");
  REQUIRE_OK(pr);
  Project& p = **pr;

  auto src = p.add_text("character", "a red fox");
  REQUIRE_OK(src);
  Recipe r1;
  r1.op = "caption";
  r1.inputs = {{"source", src->id, 0}};
  auto d1 = p.define_derived("caption", AssetKind::Text, r1);
  REQUIRE_OK(d1);
  Recipe r2;
  r2.op = "contact-sheet";
  r2.deterministic = true;
  r2.inputs = {{"caption", d1->id, 0}};
  auto d2 = p.define_derived("sheet", AssetKind::ContactSheet, r2);
  REQUIRE_OK(d2);

  auto st = p.staleness(d1->id);
  REQUIRE_OK(st);
  CHECK(st->reason == StaleReason::NeverBuilt);

  // Plan builds d1 before d2.
  auto plan = p.plan_build({d2->id});
  REQUIRE_OK(plan);
  REQUIRE(plan->size() == 2);
  CHECK((*plan)[0].asset == d1->id);
  CHECK((*plan)[1].asset == d2->id);
  CHECK((*plan)[0].needs_confirmation);   // non-deterministic
  CHECK(!(*plan)[1].needs_confirmation);

  REQUIRE_OK(fake_build(p, d1->id, "caption v1"));
  REQUIRE_OK(fake_build(p, d2->id, "sheet v1"));
  CHECK(p.staleness(d1->id)->fresh());
  CHECK(p.staleness(d2->id)->fresh());
  CHECK(p.plan_build({d2->id})->empty());

  // Edit the source: d1 is stale (input changed), d2 transitively.
  REQUIRE_OK(p.update_text(src->id, "a red fox, winter coat"));
  CHECK(p.staleness(d1->id)->reason == StaleReason::InputChanged);
  CHECK(p.staleness(d2->id)->reason == StaleReason::InputStale);
  CHECK(p.plan_build({d2->id})->size() == 2);

  // Rebuild d1 with IDENTICAL output: d2's input content is unchanged.
  REQUIRE_OK(fake_build(p, d1->id, "caption v1"));
  CHECK(p.staleness(d2->id)->fresh());

  // Changing d1's recipe makes it stale for that reason.
  Recipe r1b = r1;
  r1b.params["style"] = "terse";
  REQUIRE_OK(p.set_recipe(d1->id, r1b));
  CHECK(p.staleness(d1->id)->reason == StaleReason::RecipeChanged);

  auto deps = p.dependents(src->id, true);
  REQUIRE_OK(deps);
  CHECK(deps->size() == 2);
}

TEST(project, recipe_cycles_are_refused)
{
  auto pkg = test::temp_dir("proj") / "C.valtz";
  auto pr = Project::create(pkg, "Cycle");
  REQUIRE_OK(pr);
  Project& p = **pr;
  auto src = p.add_text("s", "x");
  REQUIRE_OK(src);
  auto a = p.define_derived("a", AssetKind::Text,
                            Recipe{{}, "op", 1, "", Json::object(),
                                   {{"in", src->id, 0}}, false, 0});
  REQUIRE_OK(a);
  auto b = p.define_derived("b", AssetKind::Text,
                            Recipe{{}, "op", 1, "", Json::object(),
                                   {{"in", a->id, 0}}, false, 0});
  REQUIRE_OK(b);
  Recipe loop;
  loop.op = "op";
  loop.inputs = {{"in", b->id, 0}};
  CHECK(!p.set_recipe(a->id, loop).ok());
  loop.inputs = {{"in", a->id, 0}};
  CHECK(!p.set_recipe(a->id, loop).ok());
}

TEST(project, subjects_reference_assets)
{
  auto pkg = test::temp_dir("proj") / "U.valtz";
  auto pr = Project::create(pkg, "Subjects");
  REQUIRE_OK(pr);
  Project& p = **pr;
  auto desc = p.add_text("Alice", "tall, silver hair");
  REQUIRE_OK(desc);
  auto s = p.create_subject("Alice", "character");
  REQUIRE_OK(s);
  Subject sub = *s;
  sub.entries.push_back({"description", desc->id, 0, ""});
  REQUIRE_OK(p.put_subject(sub));
  auto refs = p.subjects_referencing(desc->id);
  REQUIRE_OK(refs);
  REQUIRE(refs->size() == 1);
  CHECK((*refs)[0] == sub.id);
  // Removing the entry clears the reverse index.
  sub.entries.clear();
  REQUIRE_OK(p.put_subject(sub));
  CHECK(p.subjects_referencing(desc->id)->empty());
}

// The generation history keeps its states by when they were -- not by
// when they were written (a base is recorded with its result, but was
// before it) -- and survives a reopen whole.
TEST(project, history_is_kept_in_time_order)
{
  auto pkg = test::temp_dir("proj") / "H.valtz";
  AssetId gen = AssetId::make();
  {
    auto pr = Project::create(pkg, "History");
    REQUIRE_OK(pr);
    Project& p = **pr;
    auto blob = p.blobs().put_bytes("pixels", "png");
    REQUIRE_OK(blob);
    HistoryEntry result;
    result.id = HistoryId::make();
    result.created_ms = 2000;
    result.role = "result";
    result.picture = *blob;
    result.width = 1248;
    result.height = 832;
    result.generation = gen;
    result.generation_version = 1;
    result.label = "a harbour at dusk";
    HistoryEntry base = result;
    base.id = HistoryId::make();  // made after the result's, yet before it
    base.created_ms = 1000;
    base.role = "base";
    base.rendered = true;
    // A clip made, later: its movie, frames and length.
    auto movie = p.blobs().put_bytes("frames", "mp4");
    REQUIRE_OK(movie);
    HistoryEntry clip = result;
    clip.id = HistoryId::make();
    clip.created_ms = 3000;
    clip.picture = *movie;
    clip.frames = 39;
    clip.seconds = 1.625;
    REQUIRE_OK(p.add_history(result));
    REQUIRE_OK(p.add_history(base));
    REQUIRE_OK(p.add_history(clip));
  }
  auto pr = Project::open(pkg);
  REQUIRE_OK(pr);
  auto h = (*pr)->history();
  REQUIRE_OK(h);
  REQUIRE(h->size() == 3);
  CHECK((*h)[0].role == "base");
  CHECK((*h)[0].rendered);
  CHECK((*h)[1].role == "result");
  CHECK(!(*h)[1].rendered);
  CHECK((*h)[1].width == 1248 && (*h)[1].height == 832);
  CHECK((*h)[1].generation == gen);
  CHECK((*h)[1].label == "a harbour at dusk");
  CHECK((*pr)->blobs().contains((*h)[1].picture));
  CHECK((*h)[1].frames == 0);              // a picture
  CHECK((*h)[2].frames == 39 && (*h)[2].seconds == 1.625);
}

TEST(project, refuses_cloud_synced_paths)
{
  const char* home = std::getenv("HOME");
  REQUIRE(home);
  auto p = std::filesystem::path(home) /
           "Library/Mobile Documents/com~apple~CloudDocs/x.valtz";
  CHECK(!check_local_volume(p).ok());
}

TEST(project, placement_policy)
{
  using media::MediaType;
  CHECK(links_in_place(Placement::Auto, MediaType::Video));
  CHECK(!links_in_place(Placement::Auto, MediaType::Image));
  CHECK(!links_in_place(Placement::Auto, MediaType::Text));
  CHECK(links_in_place(Placement::Link, MediaType::Image));
  CHECK(!links_in_place(Placement::Copy, MediaType::Video));
}

TEST(project, linked_original_moves_changes_and_goes_missing)
{
  auto dir = test::temp_dir("link");
  auto pkg = dir / "L.valtz";
  auto pr = Project::create(pkg, "Link");
  REQUIRE_OK(pr);
  Project& p = **pr;

  auto orig = dir / "camera-original.txt";
  std::ofstream(orig) << "take one";
  ImportOptions opts;
  opts.placement = Placement::Link;
  auto a = p.import_file(orig, opts);
  REQUIRE_OK(a);
  CHECK(a->linked);
  // Nothing was copied into the package.
  CHECK(std::filesystem::is_empty(p.blobs().root()));
  auto path = p.media_path(a->id);
  REQUIRE_OK(path);
  CHECK(std::filesystem::equivalent(*path, orig));

  // A derived asset built from it.
  Recipe r;
  r.op = "caption";
  r.inputs = {{"source", a->id, 0}};
  auto d = p.define_derived("caption", AssetKind::Text, r);
  REQUIRE_OK(d);
  {
    auto rr = p.recipe(p.asset(d->id)->recipe);
    REQUIRE_OK(rr);
    Project::BuildRecord b;
    b.recipe = *rr;
    b.inputs = *p.resolve_inputs(*rr);
    std::filesystem::create_directories(p.blobs().tmp_dir());
    b.output = p.blobs().make_tmp_path("txt");
    std::ofstream(b.output) << "a caption";
    REQUIRE_OK(p.commit_build(d->id, std::move(b)));
  }
  CHECK(p.staleness(d->id)->fresh());

  // Moved: found through the bookmark; content unchanged, still fresh.
  auto moved = dir / "renamed.txt";
  std::filesystem::rename(orig, moved);
  auto chk = p.check_link(a->id);
  REQUIRE_OK(chk);
  CHECK(chk->state == LinkState::Relocated);
  REQUIRE_OK(p.refresh_link(a->id));
  CHECK(p.check_link(a->id)->state == LinkState::Ok);
  CHECK(p.staleness(d->id)->fresh());

  // Edited in place: media_path refuses until refreshed; refresh makes
  // a new version and the caption goes stale by content.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  std::ofstream(moved) << "take two, longer";
  CHECK(p.check_link(a->id)->state == LinkState::Modified);
  CHECK(!p.media_path(a->id).ok());
  REQUIRE_OK(p.refresh_link(a->id));
  CHECK(p.asset(a->id)->head == 2);
  CHECK(p.staleness(d->id)->reason == StaleReason::InputChanged);

  // Gone.
  std::filesystem::remove(moved);
  CHECK(p.check_link(a->id)->state == LinkState::Missing);
  CHECK(p.media_path(a->id).code() == Code::NotFound);
}

// The same prompt run again gets a name of its own; a long one is cut at
// a full character with "…", the suffix inside the budget.
TEST(project, derived_names_are_unique_and_fit)
{
  auto pkg = test::temp_dir("proj") / "Names.valtz";
  auto pr = Project::create(pkg, "Names");
  REQUIRE_OK(pr);
  Project& p = **pr;
  auto def = [&](const std::string& name, std::size_t budget) {
    Recipe r;
    r.op = "generate-image";
    auto a = p.define_derived(name, AssetKind::Image, r, budget);
    return a.ok() ? a->name : std::string("<error>");
  };
  CHECK(def("a red fox", 48) == "a red fox");
  CHECK(def("a red fox", 48) == "a red fox (2)");
  CHECK(def("a red fox", 48) == "a red fox (3)");

  // Latin, then CJK: byte 48 falls inside a character.
  const std::string mixed =
      "A red fox \xe5\x9c\xa8\xe9\x9b\xaa\xe5\x9c\xb0\xe9\x87\x8c"
      "\xe5\xa5\x94\xe8\xb7\x91\xef\xbc\x8c\xe8\x83\x8c\xe6\x99\xaf"
      "\xe6\x98\xaf\xe8\xbf\x9c\xe5\xb1\xb1\xe5\x92\x8c\xe6\x9d\xbe"
      "\xe6\xa0\x91";
  const std::string first = def(mixed, 48);
  const std::string second = def(mixed, 48);
  CHECK(first.size() <= 48);
  CHECK(second.size() <= 48);
  CHECK(first.ends_with("\xe2\x80\xa6"));
  CHECK(second.ends_with("\xe2\x80\xa6 (2)"));
  CHECK(first != second);
  CHECK(to_text(Json(first)).find("\xef\xbf\xbd") == std::string::npos);
  CHECK(to_text(Json(second)).find("\xef\xbf\xbd") == std::string::npos);

  // Stored as returned.
  auto all = p.assets();
  REQUIRE_OK(all);
  std::size_t foxes = 0;
  for (const auto& a : *all) {
    if (a.name.starts_with("a red fox")) ++foxes;
  }
  CHECK(foxes == 3);
}

// An image modifier is recorded on the asset -- not on its bytes: no new
// version -- and survives a reopen; a layer of "" is the whole image.
TEST(project, modifiers_are_recorded_not_applied)
{
  auto pkg = test::temp_dir("proj") / "M.valtz";
  AssetId tid;
  {
    auto p = Project::create(pkg, "Mods");
    REQUIRE_OK(p);
    auto t = (*p)->add_text("pic", "stands in for a picture");
    REQUIRE_OK(t);
    tid = t->id;
    const auto head = t->head;
    REQUIRE_OK((*p)->set_modifiers(
        tid, {{"adjust", "", {{"exposure", 0.5}}},
              {"adjust", "layer-2", {{"vibrance", -0.25}}}}));
    auto a = (*p)->asset(tid);
    REQUIRE_OK(a);
    CHECK(a->head == head);              // the bytes did not change
  }
  auto p = Project::open(pkg);
  REQUIRE_OK(p);
  auto a = (*p)->asset(tid);
  REQUIRE_OK(a);
  REQUIRE(a->modifiers.size() == 2);
  CHECK(a->modifiers[0].kind == "adjust");
  CHECK(a->modifiers[0].layer.empty());
  CHECK(jget(a->modifiers[0].params, "exposure", 0.0) == 0.5);
  CHECK(a->modifiers[1].layer == "layer-2");
  REQUIRE_OK((*p)->set_modifiers(tid, {}));
  CHECK((*p)->asset(tid)->modifiers.empty());
}
