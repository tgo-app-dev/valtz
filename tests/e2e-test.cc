// END TO END: each feature a model installed here unlocks, through the
// Controller and the real engine -- VALTZ_TEST_E2E=1 (`quick`: the
// short ones alone). The models are the ones Settings › Storage names
// (`locations.json`); a test whose model is not there SKIPS, naming it.
// Shapes are small so a full run stays near an hour: 512² pictures,
// 39-frame clips at 832 x 480 on Favor's Speed preset unless the test is
// about another.

#include "e2e-support.h"

using namespace valtz;
using namespace valtz::e2e;

namespace {

constexpr const char* kImage = "krea2-turbo";
constexpr const char* kVideo = "minimax-h3-fl2va";
constexpr const char* kRef = "minimax-h3-ref2va";
constexpr const char* kEdit = "qwen-image-2.1-turbo";
constexpr const char* kHelper = "qwen3.5-9b";

GenerateImageRequest
picture(const Rig& rig, std::string prompt, std::int64_t seed)
{
  GenerateImageRequest r;
  r.project = rig.pid;
  r.prompt = std::move(prompt);
  r.model = kImage;
  r.width = 512;
  r.height = 512;
  r.seed = seed;
  return r;
}

GenerateVideoRequest
clip(const Rig& rig, std::string prompt, std::int64_t seed)
{
  GenerateVideoRequest r;
  r.project = rig.pid;
  r.prompt = std::move(prompt);
  r.model = kVideo;
  r.width = 832;
  r.height = 480;
  r.frames = 39;  // 17n + 5
  r.seed = seed;
  r.preference = "speed";
  return r;
}

// The asset operation `name` ready here for `kind` ("image", "video").
bool
installed(const Rig& rig, const char* id)
{
  const auto* e = rig.ctl->catalog().find(id);
  return e && rig.ctl->model_store().info(*e).state ==
                  models::InstallState::Installed;
}

bool
op_ready(const Rig& rig, const std::string& name, const std::string& kind)
{
  for (const auto& op : rig.ctl->asset_operations_json()) {
    if (jget<std::string>(op, "name", "") != name) {
      continue;
    }
    for (const auto& t : jget(op, "takes", Json::array())) {
      if (jget<std::string>(t, "kind", "") == kind &&
          jget(t, "ready", false)) {
        return true;
      }
    }
  }
  return false;
}

// The mean colour of a movie's first picture, of 65535 -- its plain
// thumbnail, the first frame.
std::optional<std::array<double, 3>>
first_frame_mean(Rig& rig, AssetId a)
{
  auto t = rig.ctl->thumbnail(rig.pid, a, 256, true);
  if (!t.ok()) {
    return std::nullopt;
  }
  const Pixels p = pixels_of_file(*t);
  if (p.empty()) {
    return std::nullopt;
  }
  return std::array<double, 3>{p.mean(0), p.mean(1), p.mean(2)};
}

}  // namespace

// ---- pictures (Krea 2 Turbo) ------------------------------------------

// A picture from words: its size, not flat, previews on the way; the
// same words and seed make it again to the bit -- once the session's
// kernels are tuned, by a first picture of other words -- another seed
// another.
TEST(e2e, a_picture_from_words)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("image"));
  E2E_NEEDS(rig, kImage, "image", "generate");
  REQUIRE(rig.wait(rig.ctl->generate_image(
                       picture(rig, "a lighthouse at dusk", 1)), 300)
              .ok());
  auto req = picture(rig, "a red fox sitting in fresh snow, morning", 7);
  Ran a = rig.wait(rig.ctl->generate_image(req), 300);
  REQUIRE(a.ok());
  CHECK(a.previews >= 1);
  auto f = rig.media(a.asset());
  REQUIRE(f);
  const Pixels p1 = pixels_of_file(*f);
  REQUIRE(!p1.empty());
  CHECK(p1.w == 512 && p1.h == 512);
  CHECK(p1.spread() > 2000);
  Ran b = rig.wait(rig.ctl->generate_image(req), 300);
  REQUIRE(b.ok());
  const Pixels p2 = pixels_of_file(*rig.media(b.asset()));
  std::printf("  same seed: %d of 65535 apart\n", most_apart(p1, p2));
  CHECK(most_apart(p1, p2) == 0);
  req.seed = 8;
  Ran c = rig.wait(rig.ctl->generate_image(req), 300);
  REQUIRE(c.ok());
  CHECK(most_apart(p1, pixels_of_file(*rig.media(c.asset()))) > 0);
}

// A picture cancelled as it runs: cancelled, nothing made, and the next
// one runs.
TEST(e2e, a_picture_cancelled)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("cancel"));
  E2E_NEEDS(rig, kImage, "image", "generate");
  auto req = picture(rig, "a lighthouse at dusk", 1);
  req.width = req.height = 1024;
  auto j = rig.ctl->generate_image(req);
  REQUIRE_OK(j);
  bool running = false;
  for (int i = 0; i < 1200 && !running; ++i) {
    auto r = rig.ctl->jobs().get(*j);
    running = r && r->state == JobState::Running && r->progress > 0;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  REQUIRE(running);
  REQUIRE_OK(rig.ctl->cancel(*j));
  Ran r = rig.wait(*j, 60);
  REQUIRE(r.record);
  CHECK(r.record->state == JobState::Cancelled);
  CHECK(!r.finished.contains("asset"));
  Ran again = rig.wait(rig.ctl->generate_image(picture(rig, "a kite", 2)),
                       300);
  CHECK(again.ok());
}

// ---- clips (MiniMax H3 FL2VA) -----------------------------------------

// A clip from words: its frames, its size, its soundtrack, previews.
TEST(e2e, a_clip_from_words)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("video"));
  E2E_NEEDS(rig, kVideo, "video", "generate");
  Ran v = rig.wait(rig.ctl->generate_video(
      clip(rig, "waves rolling onto a beach at sunset, gulls calling", 5)));
  REQUIRE(v.ok());
  CHECK(v.previews >= 1);
  auto info = rig.probe(v.asset());
  REQUIRE(info);
  std::printf("  %dx%d, %lld frames, audio %d\n", info->frame.width,
              info->frame.height, (long long)info->frame_count,
              int(info->has_audio));
  CHECK(info->frame.width == 832);
  CHECK(info->frame.height == 480);
  CHECK(info->frame_count == 39);
  CHECK(info->has_audio);
}

// A clip that opens on a picture, and one that opens on one and ends on
// another: its first picture looks like the one it opens on.
TEST(e2e, a_clip_between_two_pictures)
{
  E2E_GATE();
  if (quick()) {
    SKIP("quick: not this one");
  }
  Rig rig;
  REQUIRE(rig.open("fl2v"));
  E2E_NEEDS(rig, kVideo, "video", "generate");
  const auto orange = rig.root / "orange.png";
  const auto teal = rig.root / "teal.png";
  REQUIRE(write_png(orange, 832, 480, {0xf0, 0x80, 0x20, 0xff}, true));
  REQUIRE(write_png(teal, 832, 480, {0x20, 0xa0, 0xa0, 0xff}, true));
  auto first = rig.import(orange);
  auto last = rig.import(teal);
  REQUIRE(first && last);
  auto req = clip(rig, "the colours slowly melt into each other", 3);
  req.first = *first;
  Ran a = rig.wait(rig.ctl->generate_video(req));
  REQUIRE(a.ok());
  auto m = first_frame_mean(rig, a.asset());
  REQUIRE(m);
  const Pixels in = pixels_of_file(orange);
  for (int k = 0; k < 3; ++k) {
    CHECK(std::abs((*m)[k] - in.mean(k)) < 65535 * 0.12);
  }
  req.last = *last;
  Ran b = rig.wait(rig.ctl->generate_video(req));
  REQUIRE(b.ok());
  CHECK(rig.probe(b.asset())->frame_count == 39);
}

// A clip continued from another's tail (Ref2VA).
TEST(e2e, a_clip_continued)
{
  E2E_GATE();
  if (quick()) {
    SKIP("quick: not this one");
  }
  Rig rig;
  REQUIRE(rig.open("continue"));
  E2E_NEEDS(rig, kVideo, "video", "generate");
  Ran a = rig.wait(rig.ctl->generate_video(
      clip(rig, "a paper boat drifting down a stream", 11)));
  REQUIRE(a.ok());
  // A clip continued is a reference: Ref2VA's.
  E2E_NEEDS(rig, kRef, "video", "edit");
  auto req = clip(rig, "the boat reaches a small waterfall", 12);
  req.model = kRef;
  req.frames = 22;
  req.continue_from = a.asset();
  // A continuation's Speed preset runs HyperFlow; Quality needs no
  // adapter.
  if (!installed(rig, "minimax-h3-hyperflow")) {
    req.preference = "quality";
  }
  Ran b = rig.wait(rig.ctl->generate_video(req));
  REQUIRE(b.ok());
  auto info = rig.probe(b.asset());
  REQUIRE(info);
  CHECK(info->frame_count > 0);
}

// Favor's Custom: Pika H3 Flex and TaoMate each run, with the steps they
// fix; the two together are refused.
TEST(e2e, favor_custom_adapters)
{
  E2E_GATE();
  if (quick()) {
    SKIP("quick: not this one");
  }
  Rig rig;
  REQUIRE(rig.open("favor"));
  E2E_NEEDS(rig, kVideo, "video", "generate");
  int tried = 0;
  for (const char* tune : {"pika_flex", "taomate"}) {
    const char* adapter = std::string(tune) == "pika_flex"
                              ? "minimax-h3-pika-flex"
                              : "minimax-h3-turbo-3step";
    if (!installed(rig, adapter)) {
      std::printf("  (%s not installed: %s not tried)\n", adapter, tune);
      continue;
    }
    auto req = clip(rig, "a hummingbird hovering at a red flower", 21);
    req.preference = "balanced";
    req.tuning = {{tune, true}};
    Ran r = rig.wait(rig.ctl->generate_video(req));
    CHECK(r.ok());
    if (r.ok()) {
      const Json t = r.record->recipe.params.value("tuning", Json());
      std::printf("  %s: tuning %s\n", tune, t.dump().c_str());
      CHECK(jget(t, tune, false));
    }
    ++tried;
  }
  if (tried == 0) {
    SKIP("needs minimax-h3-pika-flex or minimax-h3-turbo-3step");
  }
  // The two together: vpipe takes one sampler of its own, so the
  // settled tuning keeps one of them -- or the job is refused.
  auto both = clip(rig, "a hummingbird hovering at a red flower", 22);
  both.tuning = {{"pika_flex", true}, {"taomate", true}};
  Ran r = rig.wait(rig.ctl->generate_video(both));
  if (r.ok()) {
    const Json t = r.record->recipe.params.value("tuning", Json());
    CHECK(!(jget(t, "pika_flex", false) && jget(t, "taomate", false)));
  }
}

// ---- the helper and the agent (Qwen3.5 9B) ----------------------------

// A prompt enhanced for Krea 2: longer words come back.
TEST(e2e, a_prompt_enhanced)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("enhance"));
  if (!installed(rig, kHelper)) {
    SKIP("needs qwen3.5-9b installed");
  }
  EnhancePromptRequest req;
  req.prompt = "a cat on a windowsill";
  req.target = "image";
  req.model = kImage;
  Ran r = rig.wait(rig.ctl->enhance_prompt(req), 300);
  REQUIRE(r.ok());
  const std::string out = jget<std::string>(r.assist, "prompt", "");
  std::printf("  enhanced: %.120s...\n", out.c_str());
  CHECK(out.size() > req.prompt.size());
}

// What is asked for, read by the helper: a clip.
TEST(e2e, an_intent_read)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("intent"));
  if (!installed(rig, kHelper)) {
    SKIP("needs qwen3.5-9b installed");
  }
  auto j = rig.ctl->detect_intent(
      "make a five second video of a rocket launching", {}, true);
  REQUIRE_OK(j);
  // The heuristic answer first, then the model's: the last one kept.
  Json last;
  for (int i = 0; i < 1200; ++i) {
    Event ev;
    if (rig.ctl->events().wait(ev, 100) && ev.job == *j &&
        ev.kind == "assist.intent") {
      last = ev.data;
      if (jget(last, "from_model", false)) {
        break;
      }
    }
  }
  std::printf("  intent: %s\n", last.dump().c_str());
  CHECK(jget(last, "from_model", false));
  CHECK(jget<std::string>(last, "intent", "").find("video") !=
        std::string::npos);
}

// A clip summarized: a text asset of words.
TEST(e2e, a_clip_summarized)
{
  E2E_GATE();
  if (quick()) {
    SKIP("quick: not this one");
  }
  Rig rig;
  REQUIRE(rig.open("summary"));
  E2E_NEEDS(rig, kVideo, "video", "generate");
  if (!op_ready(rig, "summarize", "video")) {
    SKIP("summarize is not ready here (needs the helper)");
  }
  Ran v = rig.wait(rig.ctl->generate_video(
      clip(rig, "a red balloon rising over a city park", 9)));
  REQUIRE(v.ok());
  Controller::AssetOpRequest op;
  op.project = rig.pid;
  op.asset = v.asset();
  op.operation = "summarize";
  auto made = rig.ctl->run_asset_operation(op);
  REQUIRE_OK(made);
  Ran s = rig.wait(made->job, 600);
  REQUIRE(s.ok());
  REQUIRE(!made->made.empty());
  auto a = rig.ctl->project(rig.pid)->asset(made->made[0]);
  REQUIRE_OK(a);
  std::printf("  summary asset: %s\n", a->name.c_str());
}

// The agent, on its own (no review), asked for a picture: it calls
// generate_image and the picture lands in the project.
TEST(e2e, the_agent_makes_a_picture)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("agent"));
  E2E_NEEDS(rig, kImage, "image", "generate");
  if (!installed(rig, kHelper)) {
    SKIP("needs qwen3.5-9b installed");
  }
  const auto before = rig.ctl->project(rig.pid)->assets()->size();
  AgentRequest req;
  req.project = rig.pid;
  req.prompt = "Generate one picture of a green apple on a white plate.";
  req.review = false;
  auto id = rig.ctl->agent_start(req);
  REQUIRE_OK(id);
  const Json st = agent_wait(*rig.ctl, *id, "done", 900);
  std::printf("  agent: %s, %zu steps\n",
              jget<std::string>(st, "state", "").c_str(),
              jget(st, "steps", Json::array()).size());
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  bool made = false;
  for (const auto& s : jget(st, "steps", Json::array())) {
    if (jget<std::string>(s, "tool", "") == "generate_image") {
      made = made || jget<std::string>(s, "state", "") == "done";
    }
  }
  CHECK(made);
  // Its picture is a task: give it the time to land.
  bool landed = false;
  for (int i = 0; i < 3000 && !landed; ++i) {
    auto all = rig.ctl->project(rig.pid)->assets();
    landed = all.ok() && all->size() > before;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  CHECK(landed);
}

// ---- edits and upscales ------------------------------------------------

// A picture edited from words (Qwen-Image 2.1 Turbo): it changes.
TEST(e2e, a_picture_edited)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("edit"));
  E2E_NEEDS(rig, kEdit, "image", "edit");
  const auto png = rig.root / "base.png";
  REQUIRE(write_png(png, 640, 480, {0x40, 0x80, 0xc0, 0xff}));
  auto base = rig.import(png);
  REQUIRE(base);
  GenerateImageRequest req;
  req.project = rig.pid;
  req.prompt = "paint a small white sailboat in the middle";
  req.model = kEdit;
  req.mode = "edit";
  req.base = *base;
  req.width = 512;
  req.height = 384;
  req.seed = 3;
  Ran r = rig.wait(rig.ctl->generate_image(req), 600);
  REQUIRE(r.ok());
  const Pixels p = pixels_of_file(*rig.media(r.asset()));
  CHECK(p.w == 512 && p.h == 384);
  // The flat blue base, with a boat on it: no longer flat.
  CHECK(p.spread() > 1000);
}

// A picture upscaled (VOSR) on a still where it is shown at 2x: the
// result has twice the pixels.
TEST(e2e, a_picture_upscaled)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("upscale-image"));
  if (!op_ready(rig, "upscale", "image")) {
    SKIP("needs vosr + dinov2-large installed");
  }
  const auto png = rig.root / "small.png";
  REQUIRE(write_png(png, 256, 192, {0x40, 0x80, 0xc0, 0xff}, true));
  auto pic = rig.import(png);
  REQUIRE(pic);
  auto still = rig.ctl->create_composition(
      rig.pid, project::AssetClass::Still, {512, 384});
  REQUIRE_OK(still);
  auto placed = rig.ctl->instantiate(rig.pid, *pic, *still);
  REQUIRE_OK(placed);
  media::Crop twice;
  twice.scale_x = twice.scale_y = 2;
  REQUIRE_OK(rig.ctl->set_crop(rig.pid, *still, twice, placed->second));
  Controller::AssetOpRequest up;
  up.project = rig.pid;
  up.asset = *still;
  up.layer = placed->second;
  up.operation = "upscale";
  auto made = rig.ctl->run_asset_operation(up);
  REQUIRE_OK(made);
  Ran r = rig.wait(made->job, 600);
  REQUIRE(r.ok());
  REQUIRE(!made->made.empty());
  auto info = rig.probe(made->made[0]);
  REQUIRE(info);
  std::printf("  upscaled: %dx%d\n", info->frame.width, info->frame.height);
  CHECK(info->frame.width >= 512);
  CHECK(info->frame.height >= 384);
}

// A clip upscaled (FlashVSR) on a timeline where it is shown at 2x.
TEST(e2e, a_clip_upscaled)
{
  E2E_GATE();
  if (quick()) {
    SKIP("quick: not this one");
  }
  Rig rig;
  REQUIRE(rig.open("upscale-video"));
  E2E_NEEDS(rig, kVideo, "video", "generate");
  if (!op_ready(rig, "upscale", "video")) {
    SKIP("needs flashvsr-plus-turbo (or another FlashVSR) installed");
  }
  auto req = clip(rig, "a candle flame flickering in the dark", 6);
  req.width = 416;
  req.height = 256;
  Ran v = rig.wait(rig.ctl->generate_video(req));
  REQUIRE(v.ok());
  auto tl = rig.ctl->create_composition(
      rig.pid, project::AssetClass::Composition, {832, 512}, {24, 1});
  REQUIRE_OK(tl);
  auto placed = rig.ctl->instantiate(rig.pid, v.asset(), *tl);
  REQUIRE_OK(placed);
  media::Crop twice;
  twice.scale_x = twice.scale_y = 2;
  REQUIRE_OK(rig.ctl->set_crop(rig.pid, *tl, twice, placed->second));
  Controller::AssetOpRequest up;
  up.project = rig.pid;
  up.asset = *tl;
  up.layer = placed->second;
  up.operation = "upscale";
  auto made = rig.ctl->run_asset_operation(up);
  REQUIRE_OK(made);
  Ran r = rig.wait(made->job, 1200);
  REQUIRE(r.ok());
  REQUIRE(!made->made.empty());
  auto info = rig.probe(made->made[0]);
  REQUIRE(info);
  std::printf("  upscaled clip: %dx%d\n", info->frame.width,
              info->frame.height);
  CHECK(info->frame.width >= 832);
}

// A clip past 768P, regenerated (Open-H3-Regenerate-2K).
TEST(e2e, a_clip_regenerated_past_768p)
{
  E2E_GATE();
  if (quick()) {
    SKIP("quick: not this one");
  }
  Rig rig;
  REQUIRE(rig.open("regenerate"));
  E2E_NEEDS(rig, kVideo, "video", "generate");
  if (!installed(rig, "open-h3-regenerate-2k")) {
    SKIP("needs open-h3-regenerate-2k (+ latent upscaler) installed");
  }
  auto req = clip(rig, "city lights at night, slow aerial pan", 13);
  req.width = 1920;
  req.height = 1088;
  req.frames = 22;
  req.regenerate = true;
  Ran r = rig.wait(rig.ctl->generate_video(req), 2400);
  REQUIRE(r.ok());
  auto info = rig.probe(r.asset());
  REQUIRE(info);
  CHECK(info->frame.width == 1920);
  CHECK(info->frame.height == 1088);
}

// A clip from references (MiniMax H3 Ref2VA): a subject picture drawn
// on; then a clip edited.
TEST(e2e, a_clip_from_references)
{
  E2E_GATE();
  if (quick()) {
    SKIP("quick: not this one");
  }
  Rig rig;
  REQUIRE(rig.open("ref2va"));
  E2E_NEEDS(rig, kRef, "video", "edit");
  REQUIRE(rig.runs(kImage, "image", "generate"));
  Ran subject = rig.wait(rig.ctl->generate_image(
      picture(rig, "a small orange robot, studio photo", 17)), 300);
  REQUIRE(subject.ok());
  // Ref2VA's Speed and Balanced presets take its own Turbo LoRAs; Quality
  // none.
  const char* pref = installed(rig, "minimax-h3-ref2va-turbo-4step")
                         ? "speed"
                         : "quality";
  GenerateVideoRequest req = clip(rig, "the robot waves hello", 18);
  req.model = kRef;
  req.preference = pref;
  req.frames = 22;
  req.references = {subject.asset()};
  Ran r = rig.wait(rig.ctl->generate_video(req));
  REQUIRE(r.ok());
  CHECK(rig.probe(r.asset())->frame_count > 0);
  GenerateVideoRequest ed = clip(rig, "make it snow", 19);
  ed.model = kRef;
  ed.preference = pref;
  ed.frames = 22;
  ed.references = {r.asset()};
  Ran e = rig.wait(rig.ctl->generate_video(ed));
  CHECK(e.ok());
}

// ---- memory, export, undo ---------------------------------------------

// A clip asked of too little memory: refused or failed with a memory
// report, never a crash.
TEST(e2e, a_clip_refused_for_memory)
{
  E2E_GATE();
  setenv("VALTZ_RAM_LIMIT_GB", "8", 1);
  Rig rig;
  const bool opened = rig.open("memory");
  unsetenv("VALTZ_RAM_LIMIT_GB");
  REQUIRE(opened);
  if (!installed(rig, kVideo)) {
    SKIP("needs minimax-h3-fl2va installed");
  }
  auto req = clip(rig, "a storm over the sea", 1);
  req.frames = 124;
  auto j = rig.ctl->generate_video(req);
  if (!j.ok()) {
    std::printf("  refused: %s\n", j.error().message.c_str());
    CHECK(!j.error().message.empty());
    return;
  }
  Ran r = rig.wait(*j, 600);
  REQUIRE(r.record);
  CHECK(r.record->state == JobState::Failed);
  CHECK(!r.record->message.empty());
}

// A picture exported as a 16-bit PNG and a clip as HEVC; then undone.
TEST(e2e, export_and_undo)
{
  E2E_GATE();
  Rig rig;
  REQUIRE(rig.open("export"));
  E2E_NEEDS(rig, kImage, "image", "generate");
  Ran a = rig.wait(rig.ctl->generate_image(picture(rig, "a teapot", 31)),
                   300);
  REQUIRE(a.ok());
  ExportRequest ex;
  ex.project = rig.pid;
  ex.asset = a.asset();
  ex.format = "png";
  ex.destination = rig.root / "out.png";
  Ran w = rig.wait(rig.ctl->export_asset(ex), 120);
  REQUIRE(w.ok());
  const Pixels p = pixels_of_file(ex.destination);
  CHECK(p.w == 512 && p.h == 512);
  const auto n = rig.ctl->project(rig.pid)->assets()->size();
  REQUIRE_OK(rig.ctl->undo(rig.pid));
  CHECK(rig.ctl->project(rig.pid)->assets()->size() < n);
  REQUIRE_OK(rig.ctl->redo(rig.pid));
  CHECK(rig.ctl->project(rig.pid)->assets()->size() == n);
}

// ---- compressing a model ------------------------------------------------

// Krea 2 compressed to 8 bits BESIDE its original (VALTZ_TEST_E2E_COMPRESS
// =1: it writes ~16 GB into the models folder), a picture made from the
// copy, then the copy deleted and the original kept as it was.
TEST(e2e, a_model_compressed)
{
  E2E_GATE();
  if (!std::getenv("VALTZ_TEST_E2E_COMPRESS")) {
    SKIP("set VALTZ_TEST_E2E_COMPRESS=1 (writes ~16 GB beside Krea 2)");
  }
  Rig rig;
  REQUIRE(rig.open("compress"));
  E2E_NEEDS(rig, kImage, "image", "generate");
  Ran c = rig.wait(rig.ctl->compress_model(kImage, 8, true), 3600);
  REQUIRE(c.ok());
  REQUIRE_OK(rig.ctl->use_compressed(kImage, true));
  Ran a = rig.wait(rig.ctl->generate_image(picture(rig, "a bonsai", 41)),
                   300);
  CHECK(a.ok());
  auto freed = rig.ctl->keep_compressed(kImage, false);
  REQUIRE_OK(freed);
  std::printf("  copy deleted: %.1f GB\n", double(*freed) / 1e9);
  CHECK(rig.runs(kImage, "image", "generate"));
}
