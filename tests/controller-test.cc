#include "testing.h"

#include "drawthings-fixture.h"

#include <algorithm>
#include <array>

#include "valtz/assist/transcript.h"
#include "valtz/controller/controller.h"
#include "valtz/media/camera.h"
#include "valtz/media/markup.h"
#include "valtz/media/model-input.h"
#include "valtz/media/movie.h"
#include "valtz/media/pdf.h"
#include "valtz/media/probe.h"
#include "valtz/media/sound.h"
#include "valtz/media/text-flow.h"
#include "valtz/project/migrate.h"
#include "valtz/project/project-file.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <functional>
#include <deque>
#include <map>
#include <mutex>
#include <chrono>
#include <thread>
#include <vector>

using namespace valtz;
namespace fs = std::filesystem;

TEST(controller, event_bus_coalesces_previews)
{
  EventBus bus;
  JobId j = JobId::make();
  for (int i = 0; i < 5; ++i) {
    Event e;
    e.kind = "job.preview";
    e.job = j;
    e.data = {{"step", i}};
    bus.post(e);
  }
  Event done;
  done.kind = "job.finished";
  done.job = j;
  bus.post(done);
  Event out;
  REQUIRE(bus.wait(out, 0));
  CHECK(out.kind == "job.preview");
  CHECK(jget(out.data, "step", -1) == 4);  // newest frame
  REQUIRE(bus.wait(out, 0));
  CHECK(out.kind == "job.finished");
  CHECK(!bus.wait(out, 0));
}

// An event naming an asset whose name holds a broken character (from a
// project written before names were cut on a character boundary) still
// reaches the app -- it used to be dropped, or to fail the whole reply.
TEST(controller, event_with_invalid_utf8_serializes)
{
  Event e;
  e.kind = "job.queued";
  e.job = JobId::make();
  e.data = {{"title", std::string("A red fox \xe5\x9c")}};
  std::string text;
  bool threw = false;
  try {
    text = e.to_json();
  } catch (...) {
    threw = true;
  }
  CHECK(!threw);
  CHECK(text.find("\"kind\":\"job.queued\"") != std::string::npos);
  CHECK(text.find("\xef\xbf\xbd") != std::string::npos);
}

TEST(controller, import_without_engine)
{
  auto root = test::temp_dir("ctl");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto c = Controller::create(cfg);
  REQUIRE_OK(c);
  auto pid = (*c)->create_project(root / "P.valtz", "P");
  REQUIRE_OK(pid);

  auto f = root / "note.txt";
  std::ofstream(f) << "hello";
  auto job = (*c)->import_files(*pid, {f});
  REQUIRE_OK(job);
  bool finished = false;
  bool changed = false;
  for (int i = 0; i < 50 && !finished; ++i) {
    Event ev;
    if (!(*c)->events().wait(ev, 100)) {
      continue;
    }
    changed |= ev.kind == "assets.changed";
    finished |= ev.job == *job && ev.kind == "job.finished";
  }
  CHECK(changed);
  CHECK(finished);
  auto assets = (*c)->project(*pid)->assets();
  REQUIRE_OK(assets);
  CHECK(assets->size() == 1);

  // Generation needs an engine; the request is refused up front or
  // fails as a job, never crashes.
  GenerateImageRequest req;
  req.project = *pid;
  req.prompt = "a fox";
  auto g = (*c)->generate_image(req);
  CHECK(!g.ok());
  // A song: words first, then a model to sing them; a plan it knows.
  GenerateAudioRequest song;
  song.project = *pid;
  song.prompt = "  ";
  auto empty = (*c)->generate_audio(song);
  REQUIRE(!empty.ok());
  CHECK(empty.error().key == msg::kPromptEmpty.key);
  song.prompt = "Lo-fi hip hop\n\n[Verse]\nrain on the glass";
  auto none = (*c)->generate_audio(song);
  REQUIRE(!none.ok());
  CHECK(none.error().key == msg::kNoAudioModel.key);
  song.plan = "chords";
  CHECK(!(*c)->generate_audio(song).ok());
}

// The prompt as an asset (DESIGN §10c): captured with its mentions made
// positional; the same words are the same prompt; a prompt nothing is
// made from changes in place; once something is, it stays as it was --
// a changed text is a new prompt.
TEST(controller, prompts_are_assets)
{
  auto root = test::temp_dir("ctl-prompt");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto c = Controller::create(cfg);
  REQUIRE_OK(c);
  auto pid = (*c)->create_project(root / "P.valtz", "P");
  REQUIRE_OK(pid);
  project::Project* p = (*c)->project(*pid);
  auto pic = p->add_text("stand-in", "x");  // a medium in the row
  REQUIRE_OK(pic);

  const std::string P(assist::kInlinePicture);
  // Nothing in the row for a tag yet: kept, a form to fill in.
  auto a = (*c)->capture_prompt(*pid, "a fox in <valtz_ref_img_0>", {},
                                {});
  REQUIRE_OK(a);
  auto ad = p->asset(*a);
  REQUIRE_OK(ad);
  CHECK(Controller::is_prompt(*ad));
  CHECK(*p->read_text(*a) == "a fox in <valtz_ref_img_0>");
  // The same words: the same prompt.
  auto same = (*c)->capture_prompt(*pid, "a fox in <valtz_ref_img_0>", {},
                                   {});
  REQUIRE_OK(same);
  CHECK(*same == *a);
  // Nothing made from it: changed in place.
  REQUIRE_OK((*c)->set_prompt_text(*pid, *a, "a fox in the snow"));
  CHECK(*p->read_text(*a) == "a fox in the snow");
  auto again = (*c)->capture_prompt(*pid, "a red fox", {}, {}, *a);
  REQUIRE_OK(again);
  CHECK(*again == *a);
  CHECK(*p->read_text(*a) == "a red fox");

  // Something made from it: it stays as it is.
  project::Recipe r;
  r.op = "generate-image";
  r.inputs.push_back({"prompt", *a, 2});
  REQUIRE_OK(p->define_derived("made", project::AssetKind::Image, r));
  auto refused = (*c)->set_prompt_text(*pid, *a, "a blue fox");
  REQUIRE(!refused.ok());
  CHECK(refused.error().key == msg::kPromptInUse.key);
  auto fork = (*c)->capture_prompt(*pid, "a blue fox", {}, {}, *a);
  REQUIRE_OK(fork);
  CHECK(*fork != *a);
  CHECK(*p->read_text(*a) == "a red fox");
  // A mention is captured as its medium's place in the row -- a text is
  // no medium a model reads, so it is dropped.
  auto m = (*c)->capture_prompt(*pid, "lit like " + P, {pic->id},
                                {pic->id});
  REQUIRE_OK(m);
  CHECK(*p->read_text(*m) == "lit like");
  CHECK(!(*c)->set_prompt_text(*pid, pic->id, "y").ok());  // not a prompt
}

// A prompt NAMED by the person (DESIGN §10c: the Prompt Editor's tab):
// captured with a name, it is named so; renamed, it is too; either way
// its name is kept as its words change, where one its words name follows
// them -- and "" names it from its words again.
TEST(controller, a_prompt_keeps_the_name_it_is_given)
{
  auto root = test::temp_dir("ctl-prompt-name");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto c = Controller::create(cfg);
  REQUIRE_OK(c);
  auto pid = (*c)->create_project(root / "P.valtz", "P");
  REQUIRE_OK(pid);
  project::Project* p = (*c)->project(*pid);
  auto name = [&](AssetId id) { return p->asset(id)->name; };

  // Its words name it, and follow them.
  auto a = (*c)->capture_prompt(*pid, "a fox in snow", {}, {});
  REQUIRE_OK(a);
  CHECK(name(*a) == "a fox in snow");
  REQUIRE_OK((*c)->set_prompt_text(*pid, *a, "a fox at dusk"));
  CHECK(name(*a) == "a fox at dusk");
  // Renamed: kept as its words change, in place or captured again.
  REQUIRE_OK((*c)->rename_asset(*pid, *a, "Hero shot"));
  REQUIRE_OK((*c)->set_prompt_text(*pid, *a, "a fox at noon"));
  CHECK(name(*a) == "Hero shot");
  auto same = (*c)->capture_prompt(*pid, "a fox at night", {}, {}, *a);
  REQUIRE_OK(same);
  CHECK(*same == *a);
  CHECK(name(*a) == "Hero shot");
  // "": its words again, and following them.
  REQUIRE_OK((*c)->rename_asset(*pid, *a, ""));
  CHECK(name(*a) == "a fox at night");
  REQUIRE_OK((*c)->set_prompt_text(*pid, *a, "a fox asleep"));
  CHECK(name(*a) == "a fox asleep");

  // Captured with a name: a new prompt named so; the draft it came from
  // renamed in place; one already holding those words named too.
  auto b = (*c)->capture_prompt(*pid, "a heron", {}, {}, std::nullopt,
                                "Bird");
  REQUIRE_OK(b);
  CHECK(name(*b) == "Bird");
  auto b2 = (*c)->capture_prompt(*pid, "a heron wading", {}, {}, *b);
  REQUIRE_OK(b2);
  CHECK(*b2 == *b);
  CHECK(name(*b) == "Bird");
  REQUIRE_OK((*c)->capture_prompt(*pid, "a heron wading", {}, {}, *b,
                                  "Wader"));
  CHECK(name(*b) == "Wader");
  auto found = (*c)->capture_prompt(*pid, "a fox asleep", {}, {},
                                    std::nullopt, "Sleeper");
  REQUIRE_OK(found);
  CHECK(*found == *a);
  CHECK(name(*a) == "Sleeper");

  // Something made from it: a changed text is a new prompt, named as
  // asked -- the old one keeps its name.
  project::Recipe r;
  r.op = "generate-image";
  r.inputs.push_back({"prompt", *b, p->asset(*b)->head});
  REQUIRE_OK(p->define_derived("made", project::AssetKind::Image, r));
  auto fork = (*c)->capture_prompt(*pid, "two herons", {}, {}, *b,
                                   "Wader");
  REQUIRE_OK(fork);
  CHECK(*fork != *b);
  CHECK(name(*fork) == "Wader");
  CHECK(name(*b) == "Wader");
  // Any other asset: no empty name.
  auto t = p->add_text("notes", "x");
  REQUIRE_OK(t);
  CHECK(!(*c)->rename_asset(*pid, t->id, "").ok());
}

// The assistant's suggestion APPLIED (DESIGN §10c): the words before it
// are kept as a prompt, and the suggestion is a prompt made from them --
// its upstream, which stays as it was. Applied again, the same prompts;
// the enhanced one edited before Start changes in place, its upstream
// kept; enhanced again, a chain; undone whole.
TEST(controller, an_applied_suggestion_keeps_the_words_before_it)
{
  auto root = test::temp_dir("ctl-prompt-enhanced");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto c = Controller::create(cfg);
  REQUIRE_OK(c);
  auto pid = (*c)->create_project(root / "P.valtz", "P");
  REQUIRE_OK(pid);
  project::Project* p = (*c)->project(*pid);
  auto text = [&](AssetId id) { return *p->read_text(id); };
  auto upstream = [&](AssetId id) -> std::optional<AssetId> {
    auto r = p->recipe(p->asset(id)->recipe);
    if (!r.ok() || r->op != "enhance-prompt" || r->inputs.size() != 1 ||
        r->inputs[0].role != "prompt") {
      return std::nullopt;
    }
    return r->inputs[0].asset;
  };

  Controller::SuggestionApplied req;
  req.project = *pid;
  req.before = "a fox";
  req.enhanced = "A red fox trotting through fresh snow at dawn, "
                 "<valtz_ref_img_0> behind it";
  req.written_for = "krea2-turbo";
  auto r = (*c)->apply_suggestion(req);
  REQUIRE_OK(r);
  auto before = p->asset(r->before);
  auto after = p->asset(r->enhanced);
  REQUIRE_OK(before);
  REQUIRE_OK(after);
  CHECK(Controller::is_prompt(*before) && Controller::is_prompt(*after));
  CHECK(text(r->before) == "a fox");
  CHECK(text(r->enhanced) == req.enhanced);  // a tag kept, a form
  CHECK(upstream(r->enhanced) == r->before);
  CHECK(after->name.starts_with("A red fox trotting through fresh snow"));
  CHECK(p->recipe(after->recipe)->params.value("written_for", "") ==
        "krea2-turbo");
  CHECK(p->staleness(r->enhanced)->fresh());
  // The words before it stay as they were: a change is a new prompt.
  auto refused = (*c)->set_prompt_text(*pid, r->before, "a grey fox");
  REQUIRE(!refused.ok());
  CHECK(refused.error().key == msg::kPromptInUse.key);
  auto tweak = (*c)->capture_prompt(*pid, "a grey fox", {}, {}, r->before);
  REQUIRE_OK(tweak);
  CHECK(*tweak != r->before);
  CHECK(text(r->before) == "a fox");

  // The same suggestion applied to the same words: the same prompts.
  req.from = r->before;
  auto again = (*c)->apply_suggestion(req);
  REQUIRE_OK(again);
  CHECK(again->before == r->before);
  CHECK(again->enhanced == r->enhanced);
  // Start on it: kept. Edited first: changed in place -- a draft --
  // still made from the words before it, fresh.
  auto start = (*c)->capture_prompt(*pid, req.enhanced, {}, {},
                                    r->enhanced);
  REQUIRE_OK(start);
  CHECK(*start == r->enhanced);
  auto edited = (*c)->capture_prompt(*pid, "A red fox at dusk", {}, {},
                                     r->enhanced);
  REQUIRE_OK(edited);
  CHECK(*edited == r->enhanced);
  CHECK(text(r->enhanced) == "A red fox at dusk");
  CHECK(upstream(r->enhanced) == r->before);
  CHECK(p->staleness(r->enhanced)->fresh());
  CHECK(p->asset(r->enhanced)->name == "A red fox at dusk");

  // Enhanced again from it, named by the person: a chain, the name the
  // new one's.
  Controller::SuggestionApplied more;
  more.project = *pid;
  more.before = "A red fox at dusk";
  more.from = r->enhanced;
  more.enhanced = "A red fox at dusk, low sun, long shadows";
  more.name = "Hero";
  auto chain = (*c)->apply_suggestion(more);
  REQUIRE_OK(chain);
  CHECK(chain->before == r->enhanced);
  CHECK(upstream(chain->enhanced) == r->enhanced);
  CHECK(p->asset(chain->enhanced)->name == "Hero");
  CHECK(p->asset(r->enhanced)->name == "A red fox at dusk");
  // Undone: the new one gone, the one before it a draft again.
  REQUIRE_OK((*c)->undo(*pid));
  CHECK(!p->asset(chain->enhanced).ok());
  REQUIRE_OK((*c)->set_prompt_text(*pid, r->enhanced, "A red fox"));
  CHECK(upstream(r->enhanced) == r->before);
  // Nothing before it: nothing to keep.
  more.before = "  ";
  auto empty = (*c)->apply_suggestion(more);
  REQUIRE(!empty.ok());
  CHECK(empty.error().key == msg::kPromptEmpty.key);
}

namespace {

// A small 8-bit sRGB PNG: `w` x `h` of one colour (grey by default).
bool
write_png(const std::filesystem::path& path, size_t w = 16, size_t h = 16,
          std::array<std::uint8_t, 4> rgba = {0x80, 0x80, 0x80, 0xff})
{
  std::vector<std::uint8_t> px(w * h * 4);
  for (size_t i = 0; i < w * h; ++i) {
    for (int k = 0; k < 4; ++k) {
      px[i * 4 + k] = rgba[k];
    }
  }
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 8, w * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(std::strlen(path.c_str())), false);
  CGImageDestinationRef dst =
      CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
  CGImageDestinationAddImage(dst, img, nullptr);
  bool ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  CFRelease(url);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return ok;
}

// Import `f` and wait for it to land.
std::optional<AssetId>
import_and_wait(Controller& c, ProjectId pid, const std::filesystem::path& f)
{
  auto job = c.import_files(pid, {f});
  if (!job.ok()) {
    return std::nullopt;
  }
  for (int i = 0; i < 50; ++i) {
    Event ev;
    if (c.events().wait(ev, 100) && ev.job == *job &&
        ev.kind == "job.finished") {
      break;
    }
  }
  auto assets = c.project(pid)->assets();
  if (!assets.ok() || assets->empty()) {
    return std::nullopt;
  }
  return assets->back().id;
}

}

// The anonymous session is incognito: an ephemeral project keeps its
// thumbnails inside its own package, and the shared cache sees nothing
// of it. A regular project uses the shared cache as before.
TEST(controller, ephemeral_project_keeps_thumbnails_in_its_package)
{
  auto root = test::temp_dir("ctl-eph");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto c = Controller::create(cfg);
  REQUIRE_OK(c);
  auto png = root / "pic.png";
  REQUIRE(write_png(png));

  // The anonymous session: an untitled working copy, nothing of it
  // outside its own folder.
  auto anon = (*c)->create_untitled(root / "Anon", "anon");
  REQUIRE_OK(anon);
  REQUIRE_OK((*c)->set_ephemeral(*anon, true));
  auto a = import_and_wait(**c, *anon, png);
  REQUIRE(a.has_value());
  const auto before = (*c)->cache().size_bytes();
  auto t = (*c)->thumbnail(*anon, *a, 64);
  REQUIRE_OK(t);
  const std::string pkg = (root / "Anon").string();
  CHECK(t->string().starts_with(pkg));
  CHECK(std::filesystem::exists(*t));
  CHECK((*c)->cache().size_bytes() == before);

  auto named = (*c)->create_project(root / "Named.valtz", "named");
  REQUIRE_OK(named);
  auto b = import_and_wait(**c, *named, png);
  REQUIRE(b.has_value());
  auto t2 = (*c)->thumbnail(*named, *b, 64);
  REQUIRE_OK(t2);
  CHECK(!t2->string().starts_with((root / "Named.valtz").string()));
  CHECK((*c)->cache().size_bytes() > before);

  CHECK(!(*c)->set_ephemeral(ProjectId::make(), true).ok());
}

namespace {

// The 8-bit RGBA (sRGB, premultiplied) of pixel (x, y) of a picture, top
// row first.
std::array<int, 4>
pixel_of(CGImageRef img, int x, int y)
{
  std::array<int, 4> out{-1, -1, -1, -1};
  const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
  std::vector<std::uint8_t> px(w * h * 4);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 8, w * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
  const size_t i = (static_cast<size_t>(y) * w + x) * 4;
  for (int k = 0; k < 4; ++k) {
    out[k] = px[i + k];
  }
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return out;
}

// The same, of a picture file.
std::array<int, 4>
pixel_at(const std::filesystem::path& file, int x, int y)
{
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(file.c_str()),
      static_cast<CFIndex>(std::strlen(file.c_str())), false);
  CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(url);
  if (!src) {
    return {-1, -1, -1, -1};
  }
  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
  CFRelease(src);
  if (!img) {
    return {-1, -1, -1, -1};
  }
  const auto out = pixel_of(img, x, y);
  CGImageRelease(img);
  return out;
}

}

namespace {

// A controller with no engine, in a fresh temp dir, and a project in it.
struct Bench {
  fs::path                    root;
  std::unique_ptr<Controller> ctl;
  ProjectId                   pid;

  explicit Bench(const std::string& tag)
  {
    root = test::temp_dir(tag);
    ControllerConfig cfg;
    cfg.support_root = root / "support";
    cfg.model_roots = {root / "models"};
    cfg.with_engine = false;
    auto made = Controller::create(cfg);
    if (made.ok()) {
      ctl = std::move(*made);
      auto p = ctl->create_project(root / "P.valtz", tag);
      if (p.ok()) {
        pid = *p;
      }
    }
  }
  bool ok() const { return ctl && !pid.is_nil(); }
  project::Project& p() { return *ctl->project(pid); }
  std::optional<AssetId>
  png(const std::string& name, size_t w, size_t h,
      std::array<std::uint8_t, 4> rgba)
  {
    if (!write_png(root / name, w, h, rgba)) {
      return std::nullopt;
    }
    return import_and_wait(*ctl, pid, root / name);
  }
  std::vector<project::Layer>
  layers(AssetId id)
  {
    // Bound first: a range-for over a temporary Result dangles.
    const auto a = p().asset(id);
    return a.ok() ? a->layers : std::vector<project::Layer>{};
  }
};

// A tone as a 16-bit mono WAV at 48 kHz: `seconds` of a sine of
// amplitude `amp`.
bool
write_tone(const fs::path& path, double seconds, double hz, double amp)
{
  const std::uint32_t rate = 48000;
  const auto n = static_cast<std::uint32_t>(seconds * rate);
  std::ofstream f(path, std::ios::binary);
  auto u32 = [&](std::uint32_t v) { f.write(reinterpret_cast<char*>(&v), 4); };
  auto u16 = [&](std::uint16_t v) { f.write(reinterpret_cast<char*>(&v), 2); };
  f.write("RIFF", 4);
  u32(36 + n * 2);
  f.write("WAVEfmt ", 8);
  u32(16);
  u16(1);
  u16(1);
  u32(rate);
  u32(rate * 2);
  u16(2);
  u16(16);
  f.write("data", 4);
  u32(n * 2);
  for (std::uint32_t i = 0; i < n; ++i) {
    const double v = amp * std::sin(2 * M_PI * hz * i / rate);
    u16(static_cast<std::uint16_t>(static_cast<std::int16_t>(v * 32767)));
  }
  return static_cast<bool>(f);
}

// A sound file's samples, as float, its first channel at 48 kHz.
std::vector<float>
read_sound(const fs::path& path)
{
  std::vector<float> out;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(std::strlen(path.c_str())), false);
  ExtAudioFileRef f = nullptr;
  const OSStatus st = ExtAudioFileOpenURL(url, &f);
  CFRelease(url);
  if (st != noErr) {
    return out;
  }
  AudioStreamBasicDescription c{};
  c.mSampleRate = 48000;
  c.mFormatID = kAudioFormatLinearPCM;
  c.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  c.mChannelsPerFrame = 2;
  c.mBitsPerChannel = 32;
  c.mBytesPerFrame = 8;
  c.mFramesPerPacket = 1;
  c.mBytesPerPacket = 8;
  ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat,
                          sizeof(c), &c);
  std::vector<float> buf(8192 * 2);
  for (;;) {
    AudioBufferList abl;
    abl.mNumberBuffers = 1;
    abl.mBuffers[0].mNumberChannels = 2;
    abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(buf.size() * 4);
    abl.mBuffers[0].mData = buf.data();
    UInt32 frames = 8192;
    if (ExtAudioFileRead(f, &frames, &abl) != noErr || frames == 0) {
      break;
    }
    for (UInt32 i = 0; i < frames; ++i) {
      out.push_back(buf[i * 2]);
    }
  }
  ExtAudioFileDispose(f);
  return out;
}

double
rms(const std::vector<float>& s, double from, double to)
{
  const auto a = static_cast<std::size_t>(from * 48000);
  const auto b = std::min(s.size(), static_cast<std::size_t>(to * 48000));
  double sum = 0;
  for (std::size_t i = a; i < b; ++i) {
    sum += static_cast<double>(s[i]) * s[i];
  }
  return b > a ? std::sqrt(sum / static_cast<double>(b - a)) : 0;
}

// A tone's pitch between two times: its upward zero crossings a second.
double
hertz(const std::vector<float>& s, double from, double to)
{
  const auto a = static_cast<std::size_t>(from * 48000);
  const auto b = std::min(s.size(), static_cast<std::size_t>(to * 48000));
  int ups = 0;
  for (std::size_t i = a + 1; i < b; ++i) {
    ups += s[i - 1] < 0 && s[i] >= 0;
  }
  return b > a ? ups / (static_cast<double>(b - a) / 48000) : 0;
}

}

// A COMPOSITION's layers (DESIGN §6a): a flat picture has none -- it is
// shown in a composition, its edited copy -- whose layers are added,
// given what they show, looked, hidden, moved (every one, layer 0 too,
// on a frame of its own), renamed and removed (never the last), and drawn
// into one picture.
TEST(composition, layers_and_flatten)
{
  Bench b("comp-layers");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto base = b.png("base.png", 16, 16, {0x80, 0x80, 0x80, 0xff});
  auto red = b.png("red.png", 8, 8, {0xff, 0, 0, 0xff});
  REQUIRE(base && red);
  // Flat: shown as it is, no layers of its own.
  CHECK(!ctl.add_layer(pid, *base).ok());
  CHECK(!ctl.set_adjustments(pid, *base, {}).ok());
  CHECK(b.p().asset(*base)->cls == project::AssetClass::Flat);

  // Its edited copy: a still, its frame the picture's, layer 0 showing it.
  auto comp = ctl.derive_modified(pid, *base, "edited");
  REQUIRE_OK(comp);
  auto a = b.p().asset(*comp);
  REQUIRE_OK(a);
  CHECK(a->cls == project::AssetClass::Still);
  CHECK((a->canvas.frame() == media::PixelSize{16, 16}));
  REQUIRE(a->layers.size() == 1);
  CHECK(a->layers[0].id.empty() && a->layers[0].source == *base);

  auto l1 = ctl.add_layer(pid, *comp);
  REQUIRE_OK(l1);
  CHECK(*l1 == "1");
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 8, 8)[1] > 100);   // grey: green present
  // The red picture on layer 1: centred at its own size.
  REQUIRE_OK(ctl.set_layer_source(pid, *comp, "1", *red));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 8, 8)[0] > 200 && pixel_at(flat, 8, 8)[1] < 40);
  CHECK(pixel_at(flat, 1, 1)[1] > 100);   // grey round it
  // Its look: desaturated, the red is grey too.
  REQUIRE_OK(ctl.set_adjustments(pid, *comp,
                                 media::adjustments_from_json(
                                     {{"saturation", -1.0}}), "1"));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  const auto grey = pixel_at(flat, 8, 8);
  CHECK(std::abs(grey[0] - grey[1]) < 30);
  // The live look, over what is recorded; and the layer alone.
  REQUIRE_OK(ctl.flatten(pid, *comp, flat,
                         Controller::LayerLook{"1", {}, {}}));
  CHECK(pixel_at(flat, 8, 8)[0] > 200);
  REQUIRE_OK(ctl.flatten(pid, *comp, flat, std::nullopt, std::string("1")));
  CHECK(pixel_at(flat, 1, 1)[3] == 0);
  // Hidden: the layer below shows through.
  REQUIRE_OK(ctl.set_layer_visible(pid, *comp, "1", false));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 8, 8)[1] > 100);

  // Order: every layer moves on a frame of its own, layer 0 too.
  auto l2 = ctl.add_layer(pid, *comp, "");
  REQUIRE_OK(l2);
  auto order = [&] {
    std::string o;
    for (const auto& l : b.layers(*comp)) {
      o += l.id.empty() ? "_" : l.id;
    }
    return o;
  };
  CHECK(order() == "_21");
  REQUIRE_OK(ctl.move_layer(pid, *comp, "2", 1));
  CHECK(order() == "_12");
  REQUIRE_OK(ctl.move_layer(pid, *comp, "", 1));
  CHECK(order() == "1_2");
  REQUIRE_OK(ctl.move_layer(pid, *comp, "", -5));
  CHECK(order() == "_12");
  REQUIRE_OK(ctl.rename_layer(pid, *comp, "2", "sky"));
  CHECK(b.layers(*comp)[2].name == "sky");
  // Never itself.
  CHECK(!ctl.set_layer_source(pid, *comp, "2", *comp).ok());
  // Removed with its look; the last one stays.
  REQUIRE_OK(ctl.remove_layer(pid, *comp, "1"));
  for (const auto& m : b.p().asset(*comp)->modifiers) {
    CHECK(m.layer != "1");
  }
  REQUIRE_OK(ctl.remove_layer(pid, *comp, "2"));
  CHECK(!ctl.remove_layer(pid, *comp, "").ok());
}

// A still's PAGES (DESIGN §6a): drawn a page at a time -- each layer on
// its pages, its look keyed by page from its own first -- a page added
// between two (what runs across it continues, its keys staying with their
// pages: each page shows what it showed) and removed (the layers only it
// had go with it), undone whole; flattened a picture a page; exported a
// file a page, numbered.
TEST(composition, a_still_has_pages)
{
  Bench b("comp-pages");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto grey = b.png("grey.png", 40, 20, {0x80, 0x80, 0x80, 0xff});
  auto red = b.png("red.png", 4, 4, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 4, 4, {0, 0, 0xff, 0xff});
  REQUIRE(grey && red && blue);
  auto still = ctl.create_composition(pid, project::AssetClass::Still,
                                      {40, 20});
  REQUIRE_OK(still);
  const AssetId c = *still;
  auto l0 = ctl.add_layer(pid, c);
  REQUIRE_OK(l0);
  REQUIRE_OK(ctl.set_layer_source(pid, c, *l0, *grey));
  auto l1 = ctl.add_layer(pid, c, *l0);
  REQUIRE_OK(l1);
  REQUIRE_OK(ctl.set_layer_source(pid, c, *l1, *red));
  // Pages are a still's: a timeline has none, and one page stays.
  auto clip = ctl.create_composition(pid, project::AssetClass::Composition,
                                     {40, 20});
  REQUIRE_OK(clip);
  CHECK(!ctl.add_page(pid, *clip).ok());
  CHECK(!ctl.remove_page(pid, c, 0).ok());
  auto p1 = ctl.add_page(pid, c);
  REQUIRE_OK(p1);
  CHECK(*p1 == 1);
  REQUIRE_OK(ctl.add_page(pid, c));
  CHECK(b.p().asset(c)->pages == 3);
  // The red square keyed across the pages: left, middle, right.
  media::KeyedCrop k;
  media::Crop left;
  left.content = {4, 4};
  left.offset_x = -0.4;
  media::Crop right = left;
  right.offset_x = 0.4;
  k.place.keys = {{0, left}, {2, right}};
  REQUIRE_OK(ctl.set_crop_keys(pid, c, k, *l1));
  // Blue on the middle page alone, over the red.
  auto l2 = ctl.add_layer(pid, c, *l1, std::int64_t{1});
  REQUIRE_OK(l2);
  REQUIRE_OK(ctl.set_layer_source(pid, c, *l2, *blue));
  CHECK(b.layers(c)[2].time.offset == 1);
  CHECK(b.layers(c)[2].time.duration == 1);
  const auto flat = b.root / "page.png";
  auto at = [&](std::int64_t page, int x) {
    if (!ctl.flatten(pid, c, flat, std::nullopt, std::nullopt, {}, page)
             .ok()) {
      return '?';
    }
    const auto px = pixel_at(flat, x, 10);
    return px[2] > 200 ? 'b' : px[0] > 200 ? 'r' : 'g';
  };
  CHECK(at(0, 4) == 'r' && at(0, 20) == 'g');
  CHECK(at(1, 20) == 'b' && at(1, 4) == 'g');
  CHECK(at(2, 36) == 'r' && at(2, 20) == 'g');

  // A page between the first two: the red half way there, the rest as
  // they were, a page on.
  auto made = ctl.add_page(pid, c, std::int64_t{0});
  REQUIRE_OK(made);
  CHECK(*made == 1);
  CHECK(b.p().asset(c)->pages == 4);
  CHECK(at(0, 4) == 'r');
  CHECK(at(1, 12) == 'r' && at(1, 20) == 'g');
  CHECK(at(2, 20) == 'b');
  CHECK(at(3, 36) == 'r');
  // The blue page gone, and the blue with it; the others close up.
  REQUIRE_OK(ctl.remove_page(pid, c, 2));
  CHECK(b.layers(c).size() == 2);
  CHECK(at(1, 12) == 'r');
  CHECK(at(2, 36) == 'r');
  CHECK(!ctl.remove_page(pid, c, 3).ok());
  // Undone whole: the page and its layer.
  REQUIRE_OK(ctl.undo(pid));
  CHECK(b.p().asset(c)->pages == 4);
  CHECK(at(2, 20) == 'b');

  // A layer's pages: from the second to the last.
  REQUIRE_OK(ctl.set_layer_pages(pid, c, *l2, 1, 0));
  CHECK(at(1, 20) == 'b' && at(3, 20) == 'b' && at(0, 20) == 'g');
  // Two layers on different pages are not one picture.
  CHECK(!ctl.merge_layers(pid, c, *l1, *l2).ok());

  // Flattened, a picture a page.
  auto first = ctl.flatten_asset(pid, c, "Doc");
  REQUIRE_OK(first);
  int pictures = 0;
  const auto all = b.p().assets();
  for (const auto& a : all.ok() ? *all : std::vector<project::Asset>{}) {
    if (a.name.starts_with("Doc, page ")) {
      ++pictures;
    }
  }
  CHECK(pictures == 4);
  CHECK(b.p().asset(*first)->name == "Doc, page 1");

  // Exported pages are numbered beside the file asked for, sorting as
  // they come.
  CHECK(page_path("/x/Doc.png", 0, 3) == fs::path("/x/Doc-1.png"));
  CHECK(page_path("/x/Doc.png", 0, 12) == fs::path("/x/Doc-01.png"));
  CHECK(page_path("/x/Doc.png", 11, 12) == fs::path("/x/Doc-12.png"));
}

// CANVAS SIZE: a composition on a bigger or smaller canvas, anchored --
// what is at the anchor stays put, resizes add up -- clear around it;
// back to its own frame, unset.
TEST(composition, canvas_size_is_anchored)
{
  using media::StackCanvas;
  // The rule alone: a 100 x 80 frame.
  const media::PixelSize own{100, 80};
  StackCanvas c = media::resize_canvas({}, own, {200, 100}, 0.5, 0.5);
  CHECK(c.width == 200 && c.height == 100 && c.x == 50 && c.y == 10);
  c = media::resize_canvas(c, own, {150, 100}, 0, 0);   // from the left
  CHECK(c.x == 50 && c.y == 10);
  c = media::resize_canvas(c, own, {100, 100}, 1, 1);   // from the right
  CHECK(c.x == 0 && c.y == 10);
  CHECK(!media::resize_canvas(c, own, {100, 80}, 0.5, 0.5).set());
  CHECK((media::stack_canvas_from_json(media::to_json(c)) == c));

  Bench b("comp-canvas");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  auto base = b.png("base.png", 16, 16, {0x80, 0x80, 0x80, 0xff});
  REQUIRE(base);
  // Not on a flat picture; on its edited copy.
  CHECK(!ctl.set_canvas(b.pid, *base, {32, 24}, 1, 1).ok());
  auto comp = ctl.derive_modified(b.pid, *base);
  REQUIRE_OK(comp);
  REQUIRE_OK(ctl.set_canvas(b.pid, *comp, {32, 24}, 1, 1));
  // A composition's frame is its size: resized, the frame is the canvas.
  auto a = b.p().asset(*comp);
  CHECK((a->canvas.frame() == media::PixelSize{32, 24}));
  CHECK(a->canvas.width == 32 && a->canvas.x == 0 && a->canvas.y == 0);
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.flatten(b.pid, *comp, flat));
  CHECK(pixel_at(flat, 31, 23)[1] > 100);   // the picture, bottom right
  CHECK(pixel_at(flat, 20, 12)[1] > 100);
  CHECK(pixel_at(flat, 2, 2)[3] == 0);      // clear top left
  CHECK(pixel_at(flat, 14, 20)[3] == 0);
  REQUIRE_OK(ctl.set_canvas(b.pid, *comp, {8, 8}, 0.5, 0.5));
  a = b.p().asset(*comp);
  CHECK((a->canvas.frame() == media::PixelSize{8, 8}));
  REQUIRE_OK(ctl.flatten(b.pid, *comp, flat));
  CHECK(pixel_at(flat, 1, 4)[3] == 0);
  CHECK(pixel_at(flat, 6, 4)[3] == 255);
  // A still: no timeline.
  CHECK(!ctl.set_timeline(b.pid, *comp, 48).ok());
}

// The PROJECT is one composition (DESIGN §6a): none until its first
// result -- a picture makes a still -- or one set up; a result of another
// kind is not placed; a composition of sound alone takes sounds only; set
// up anew, the old one stays an asset; the project's own cannot go.
TEST(composition, the_project_is_one_composition)
{
  Bench b("comp-project");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(write_tone(b.root / "tone.wav", 1, 440, 0.5));
  auto tone = import_and_wait(ctl, pid, b.root / "tone.wav");
  REQUIRE(red && tone);
  CHECK(!ctl.project_composition(pid));

  auto still = ctl.place_in_project(pid, *red);
  REQUIRE_OK(still);
  REQUIRE(*still);
  CHECK(ctl.project_composition(pid) == **still);
  CHECK(b.p().asset(**still)->cls == project::AssetClass::Still);
  // A sound into a picture project: not placed.
  auto no = ctl.place_in_project(pid, *tone);
  REQUIRE_OK(no);
  CHECK(!*no);
  CHECK(!ctl.instantiate(pid, *tone, **still).ok());

  // A composition of sound alone, set up as the project's.
  auto sound = ctl.create_composition(pid, project::AssetClass::Composition,
                                      {0, 0});
  REQUIRE_OK(sound);
  CHECK(ctl.project_composition(pid) == **still);  // not asked to be
  auto sa = b.p().asset(*sound);
  CHECK(sa->kind == project::AssetKind::Audio);
  CHECK((sa->rate == Rational{1000, 1}));
  REQUIRE_OK(ctl.set_project_composition(pid, *sound));
  auto took = ctl.place_in_project(pid, *tone);
  REQUIRE_OK(took);
  CHECK(*took && **took == *sound);
  CHECK(b.layers(*sound)[0].source == *tone);
  CHECK(!*ctl.place_in_project(pid, *red));
  CHECK(!ctl.instantiate(pid, *red, *sound).ok());

  // Set up anew: the old one stays, as an asset; the project's stays.
  auto fresh = ctl.create_composition(pid, project::AssetClass::Still,
                                      {32, 24}, {}, "Fresh", true);
  REQUIRE_OK(fresh);
  CHECK(ctl.project_composition(pid) == *fresh);
  CHECK(b.p().asset(*sound).ok());
  CHECK(!ctl.remove_asset(pid, *fresh).ok());
  CHECK(!ctl.create_composition(pid, project::AssetClass::Still, {0, 0})
             .ok());
  // In a still nothing sounds; a picture placed is its new take.
  auto into = ctl.place_in_project(pid, *red);
  REQUIRE_OK(into);
  CHECK(*into && **into == *fresh);
  // The frame stays: it was set up at a size, not sized by a take.
  CHECK((b.p().asset(*fresh)->canvas.frame() == media::PixelSize{32, 24}));
}

// An engine that takes jobs and runs them when the test says: what the
// TASK QUEUE (DESIGN §3a) is seen through without a model.
class HeldEngine final : public engine::Engine {
public:
  std::string description() const override { return "held"; }
  bool available() const override { return true; }
  bool supports(std::string_view) const override { return true; }

  Status
  submit(engine::JobSpec spec, engine::JobSink sink) override
  {
    std::lock_guard lk(_mu);
    _jobs.push_back({std::move(spec), std::move(sink)});
    return ok_status();
  }

  // The jobs of `op` held now (an agent's turns: submitted from its own
  // thread).
  std::vector<JobId>
  held_of(std::string_view op)
  {
    std::lock_guard lk(_mu);
    std::vector<JobId> ids;
    for (const auto& j : _jobs) {
      if (j.spec.op == op) {
        ids.push_back(j.spec.id);
      }
    }
    return ids;
  }

  // An agent's turn ended as the helper ends one: its record, the end.
  void
  turn(JobId id, Json record)
  {
    engine::JobSink sink;
    {
      std::lock_guard lk(_mu);
      auto it = find(id);
      sink = it->sink;
      _jobs.erase(it);
    }
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Output;
    ev.text = jget<std::string>(record, "text", "");
    ev.data = std::move(record);
    sink(ev);
    engine::JobEvent end;
    end.job = id;
    end.kind = engine::JobEventKind::Finished;
    sink(end);
  }

  void
  drop_context(const std::string& key) override
  {
    std::lock_guard lk(_mu);
    _dropped.push_back(key);
  }

  std::vector<std::string>
  dropped()
  {
    std::lock_guard lk(_mu);
    return _dropped;
  }

  // A queued job is dropped at once; a running one is only asked to
  // stop (`finish` may still deliver its result, as a graph past its
  // last check does).
  Status
  cancel(JobId id) override
  {
    auto it = find(id);
    if (it == _jobs.end()) {
      return make_error(Code::NotFound, "no such job");
    }
    if (it->running) {
      it->stop = true;
      return ok_status();
    }
    auto sink = it->sink;
    _jobs.erase(it);
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Cancelled;
    sink(ev);
    return ok_status();
  }

  void shutdown() override {}

  void
  start(JobId id)
  {
    auto it = find(id);
    it->running = true;
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Started;
    it->sink(ev);
  }

  // Its result, `file` -- and what it made beside it, `data` (a
  // transcript's lines) -- then the end.
  void
  finish(JobId id, const fs::path& file, Json data = Json())
  {
    auto it = find(id);
    auto sink = it->sink;
    const fs::path out = it->spec.output_dir / std::format(
        "{}{}", id.str(), file.extension().string());
    fs::copy_file(file, out, fs::copy_options::overwrite_existing);
    _jobs.erase(it);
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Output;
    ev.output = out;
    if (auto info = media::probe_file(out); info.ok()) {
      ev.output_info = *info;
    }
    if (out.extension() == ".txt") {
      ev.output_info.type = media::MediaType::Text;
      ev.output_info.uti = "public.utf8-plain-text";
    }
    ev.data = std::move(data);
    sink(ev);
    engine::JobEvent end;
    end.job = id;
    end.kind = engine::JobEventKind::Finished;
    sink(end);
  }

  // Several results, each the output it is (an Output event's data
  // "output": a separation's sources), then the end.
  void
  finish_outputs(JobId id,
                 const std::vector<std::pair<std::string, fs::path>>& outs)
  {
    auto it = find(id);
    auto sink = it->sink;
    const fs::path dir = it->spec.output_dir;
    _jobs.erase(it);
    for (const auto& [name, file] : outs) {
      const fs::path out = dir / std::format(
          "{}-{}{}", id.str(), name, file.extension().string());
      fs::copy_file(file, out, fs::copy_options::overwrite_existing);
      engine::JobEvent ev;
      ev.job = id;
      ev.kind = engine::JobEventKind::Output;
      ev.output = out;
      if (auto info = media::probe_file(out); info.ok()) {
        ev.output_info = *info;
      }
      ev.data = {{"output", name}};
      sink(ev);
    }
    engine::JobEvent end;
    end.job = id;
    end.kind = engine::JobEventKind::Finished;
    sink(end);
  }

  // Ended with nothing to hand over: a download done.
  void
  end(JobId id)
  {
    auto it = find(id);
    auto sink = it->sink;
    _jobs.erase(it);
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Finished;
    sink(ev);
  }

  std::size_t held() const { return _jobs.size(); }

  // A running job asked to stop (cancel), and its graph stopped: ended
  // cancelled.
  bool
  stopping(JobId id)
  {
    auto it = find(id);
    return it != _jobs.end() && it->stop;
  }

  void
  stopped(JobId id)
  {
    auto it = find(id);
    auto sink = it->sink;
    _jobs.erase(it);
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Cancelled;
    sink(ev);
  }

  // The spec a job was submitted with.
  const engine::JobSpec*
  spec(JobId id)
  {
    auto it = find(id);
    return it == _jobs.end() ? nullptr : &it->spec;
  }

  // Ended in failure, `why` as vpipe would say it.
  void
  fail(JobId id, std::string why)
  {
    auto it = find(id);
    auto sink = it->sink;
    _jobs.erase(it);
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Failed;
    ev.error = Code::Io;
    ev.text = std::move(why);
    sink(ev);
  }

private:
  struct Job {
    engine::JobSpec spec;
    engine::JobSink sink;
    bool            running = false;
    bool            stop = false;
  };
  std::mutex               _mu;
  std::vector<std::string> _dropped;
  std::vector<Job>::iterator
  find(JobId id)
  {
    return std::ranges::find_if(_jobs, [&](const Job& j) {
      return j.spec.id == id;
    });
  }
  std::vector<Job> _jobs;
};

// A fleet member for a controller to send to (DESIGN §11): it says it
// runs pictures and chats with every catalog model, and keeps what it is
// offered -- each served at once, and failed: where a job WENT is what
// is looked at.
class OfferedHost final : public fleet::Host {
public:
  Json
  fleet_self() override
  {
    Json installed = Json::array();
    if (auto cat = models::Catalog::builtin(); cat.ok()) {
      for (const auto& m : cat->models()) {
        installed.push_back(m.id);
      }
    }
    return {{"ops", {"generate-image", "chat"}},
            {"installed", installed},
            {"runs", {"image/generate/krea2-turbo"}},
            {"assistant", "qwen3.5-9b"},
            {"busy", false},
            {"machine", {{"chip", "test"}, {"ram_gb", 64}}}};
  }

  Status
  fleet_serve(const fleet::Offer& o, const std::string&, const fs::path&,
              engine::JobSink sink) override
  {
    {
      std::lock_guard lk(mu);
      offered.push_back(o.op + "/" + o.model);
    }
    std::thread([id = o.id, sink] {
      engine::JobEvent e;
      e.job = id;
      e.kind = engine::JobEventKind::Failed;
      e.error = Code::Io;
      e.text = "served";
      sink(e);
    }).detach();
    return ok_status();
  }

  void fleet_cancel(JobId) override {}
  void fleet_note(std::string, Json) override {}

  std::size_t
  count()
  {
    std::lock_guard lk(mu);
    return offered.size();
  }

  std::mutex               mu;
  std::vector<std::string> offered;
};

bool
eventually(const std::function<bool()>& f, int seconds)
{
  for (int i = 0; i < seconds * 20; ++i) {
    if (f()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

// Where this Mac's jobs run (DESIGN §11), as its OFFLOAD POLICIES say --
// per task, a model's own first -- and what it SHARES: a controller on a
// held engine and a member over the loopback.
TEST(controller, offload_policies_send_jobs_and_sharing_withholds)
{
  if (!std::getenv("VALTZ_TEST_FLEET")) {
    SKIP("set VALTZ_TEST_FLEET=1 (two members on this Mac's loopback)");
  }
  auto root = test::temp_dir("ctl-offload");
  const fs::path krea = root / "models" / "krea" / "Krea-2-Turbo";
  fs::create_directories(krea);
  std::ofstream(krea / "model.safetensors") << "w";
  const std::string key = fleet::derive_key("Offload", "a-long-test-secret");
  auto conf = [&](const char* name, bool discoverable) {
    fleet::Config c;
    c.member_id = Uuid::v7().str();
    c.member_name = name;
    c.fleet = "Offload";
    c.key = key;
    c.discoverable = discoverable;
    (void)fleet::save_config(root / (std::string(name) + ".json"), c);
  };
  conf("member", true);
  conf("here", false);
  OfferedHost host;
  auto member = fleet::make_fleet(root / "member.json",
                                  root / "member-files", host);
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.fleet = true;
  cfg.fleet_config = root / "here.json";
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  int port = 0;
  REQUIRE(eventually([&] {
    port = jget(member->status(), "port", 0);
    return port > 0;
  }, 10));
  ctl.fleet_connect("127.0.0.1", port);
  auto connected = [&](bool accepting) {
    return eventually([&] {
      const Json ms = jget(ctl.fleet_status(), "members", Json::array());
      return ms.size() == 1 &&
             jget(jget(ms[0], "self", Json::object()), "accepting",
                  !accepting) == accepting;
    }, 15);
  };
  REQUIRE(connected(true));
  auto pid = ctl.create_project(root / "O.valtz", "offload");
  REQUIRE_OK(pid);
  auto ask = [&](const char* words) {
    GenerateImageRequest req;
    req.project = *pid;
    req.prompt = words;
    req.model = "krea2-turbo";
    req.width = 64;
    req.height = 64;
    return ctl.generate_image(req);
  };
  auto set = [&](const Json& j) { return ctl.fleet_configure(j); };

  // Local first, the default: here while this Mac is free.
  auto j1 = ask("a red fox");
  REQUIRE_OK(j1);
  CHECK(engine->held() == 1);
  // Busy now (j1 waits here). Never sent: it waits here too.
  REQUIRE_OK(set({{"offload", {{"tasks", {{"generate-image", "never"}}}}}}));
  auto j2 = ask("a blue boat");
  REQUIRE_OK(j2);
  CHECK(engine->held() == 2);
  CHECK(host.count() == 0);
  // Local first again: busy, so the member takes it.
  REQUIRE_OK(set({{"offload", {{"tasks", {{"generate-image", ""}}}}}}));
  auto j3 = ask("a green kite");
  REQUIRE_OK(j3);
  CHECK(eventually([&] { return host.count() == 1; }, 15));
  CHECK(engine->held() == 2);
  engine->fail(*j1, "done");
  engine->fail(*j2, "done");
  // Free here; the member's job over. Fleet first, set on the model: the
  // member takes it though this Mac is free.
  REQUIRE(eventually([&] {
    const Json ms = jget(ctl.fleet_status(), "members", Json::array());
    return ms.size() == 1 &&
           !jget(jget(ms[0], "self", Json::object()), "serving", true);
  }, 15));
  REQUIRE_OK(set({{"offload", {{"models", {{"krea2-turbo",
                                            "fleet-first"}}}}}}));
  auto j4 = ask("a grey heron");
  REQUIRE_OK(j4);
  CHECK(eventually([&] { return host.count() == 2; }, 15));
  CHECK(engine->held() == 0);
  {
    std::lock_guard lk(host.mu);
    CHECK(host.offered.back() == "generate-image/krea2-turbo");
  }
  // Always on the fleet, the member not taking jobs: refused, nothing run
  // here -- and Krea 2 runs nowhere, for the model field.
  const auto* k2 = ctl.catalog().find("krea2-turbo");
  REQUIRE(k2 != nullptr);
  fleet::Config mc = member->config();
  mc.accept = "never";
  REQUIRE_OK(member->set_config(mc));
  REQUIRE(eventually([&] {
    port = jget(member->status(), "port", 0);
    return port > 0;
  }, 10));
  ctl.fleet_connect("127.0.0.1", port);
  REQUIRE(connected(false));
  REQUIRE_OK(set({{"offload", {{"models", {{"krea2-turbo", "always"}}}}}}));
  CHECK(!ctl.runs(*k2, "image", "generate"));
  auto j5 = ask("a white swan");
  CHECK(!j5.ok());
  CHECK(engine->held() == 0);
  REQUIRE_OK(set({{"offload", {{"models", {{"krea2-turbo", ""}}}}}}));
  CHECK(ctl.runs(*k2, "image", "generate"));
  // A policy or a name that is none: refused, nothing changed.
  CHECK(!set({{"offload", {{"tasks", {{"generate-image", "sometimes"}}}}}})
             .ok());
  CHECK(!set({{"share", {{"tasks", {{"bake-bread", false}}}}}}).ok());

  // Sharing: chat and Krea 2 withheld -- this Mac no longer says it runs
  // them, so no member offers them.
  REQUIRE_OK(set({{"share", {{"tasks", {{"chat", false}}},
                             {"models", {{"krea2-turbo", false}}}}}}));
  const Json me = jget(ctl.fleet_status(), "self", Json::object());
  auto has = [](const Json& list, const char* x) {
    return std::ranges::find(list, Json(x)) != list.end();
  };
  CHECK(!has(jget(me, "ops", Json::array()), "chat"));
  CHECK(has(jget(me, "ops", Json::array()), "generate-image"));
  CHECK(!has(jget(me, "installed", Json::array()), "krea2-turbo"));
  CHECK(jget<std::string>(me, "assistant", "x").empty());
  const Json c = jget(ctl.fleet_status(), "config", Json::object());
  CHECK(jget(jget(c, "withheld", Json::object()), "tasks", Json::array()) ==
        Json::array({"chat"}));
  // The member hears it.
  CHECK(eventually([&] {
    const auto ms = member->members();
    return ms.size() == 1 &&
           !has(jget(jget(ms[0], "self", Json::object()), "installed",
                     Json::array()),
                "krea2-turbo");
  }, 15));
  ctl.shutdown();
  member->stop();
  fs::remove_all(root);
}

// A project CLOSED with tasks under way (2026-10-15): its queued ones
// dropped, its running one asked to stop -- nothing works on a project
// that is gone (they ran on, holding the machine, their results
// dropped); another project's task goes on.
TEST(controller, closing_a_project_stops_its_tasks)
{
  auto root = test::temp_dir("ctl-close-tasks");
  const fs::path krea = root / "models" / "krea" / "Krea-2-Turbo";
  fs::create_directories(krea);
  std::ofstream(krea / "model.safetensors") << "w";
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto a = ctl.create_project(root / "A.valtz", "a");
  REQUIRE_OK(a);
  auto b = ctl.create_project(root / "B.valtz", "b");
  REQUIRE_OK(b);
  auto ask = [&](ProjectId pid, const char* words) {
    GenerateImageRequest req;
    req.project = pid;
    req.prompt = words;
    req.model = "krea2-turbo";
    req.width = 64;
    req.height = 64;
    return ctl.generate_image(req);
  };
  auto j1 = ask(*a, "a red fox");
  REQUIRE_OK(j1);
  auto j2 = ask(*a, "a blue boat");
  REQUIRE_OK(j2);
  auto k1 = ask(*b, "a green kite");
  REQUIRE_OK(k1);
  engine->start(*j1);
  REQUIRE(ctl.tasks().size() == 3);
  REQUIRE_OK(ctl.close_project(*a));
  // The queued one dropped; the running one asked to stop; B's waits on.
  auto r2 = ctl.jobs().get(*j2);
  REQUIRE(r2);
  CHECK(r2->state == JobState::Cancelled);
  CHECK(engine->stopping(*j1));
  CHECK(!engine->stopping(*k1));
  CHECK(engine->held() == 2);
  // Stopped: it ends cancelled, and only B's task is left.
  engine->stopped(*j1);
  auto r1 = ctl.jobs().get(*j1);
  REQUIRE(r1);
  CHECK(r1->state == JobState::Cancelled);
  const Json ts = ctl.tasks();
  REQUIRE(ts.size() == 1);
  CHECK(jget<std::string>(ts[0], "job", "") == k1->str());
  ctl.shutdown();
  fs::remove_all(root);
}

// The TASK QUEUE (DESIGN §3a): each generation asked for is a task with
// its asset from the request, listed in the order it runs; the project
// is edited and undone while they run; undoing a task's request
// withdraws it -- queued or running, its result never committed.
TEST(controller, tasks_queue_and_are_withdrawn_by_undo)
{
  auto root = test::temp_dir("ctl-tasks");
  // Krea 2 Turbo, "installed": a weights file where it is looked for.
  const fs::path krea = root / "models" / "krea" / "Krea-2-Turbo";
  fs::create_directories(krea);
  std::ofstream(krea / "model.safetensors") << "w";
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "T.valtz", "tasks");
  REQUIRE_OK(pid);
  auto ask = [&](const char* words) {
    GenerateImageRequest req;
    req.project = *pid;
    req.prompt = words;
    req.model = "krea2-turbo";
    req.width = 64;
    req.height = 64;
    return ctl.generate_image(req);
  };
  auto j1 = ask("a red fox");
  REQUIRE_OK(j1);
  auto j2 = ask("a blue boat");
  REQUIRE_OK(j2);
  // Two tasks, in the order asked, each with its asset already.
  Json ts = ctl.tasks();
  REQUIRE(ts.size() == 2);
  CHECK(jget<std::string>(ts[0], "job", "") == j1->str());
  CHECK(jget<std::string>(ts[1], "job", "") == j2->str());
  CHECK(jget<int>(ts[0], "position", -1) == 0);
  CHECK(jget<int>(ts[1], "position", -1) == 1);
  CHECK(jget<std::string>(ts[1], "state", "") == "queued");
  CHECK(jget<std::string>(ts[0], "kind", "") == "generate");
  const auto a1 = AssetId::parse(jget<std::string>(ts[0], "asset", ""));
  const auto a2 = AssetId::parse(jget<std::string>(ts[1], "asset", ""));
  REQUIRE(a1 && a2 && *a1 != *a2);
  CHECK(ctl.project(*pid)->asset(*a2).ok());
  engine->start(*j1);
  CHECK(jget<std::string>(ctl.tasks()[0], "state", "") == "running");

  // Edited and undone while they run.
  auto f = ctl.create_folder(*pid, "Refs");
  REQUIRE_OK(f);
  REQUIRE_OK(ctl.undo(*pid));
  CHECK(ctl.folders(*pid)->empty());
  // The second request taken back: withdrawn from the queue, its asset
  // gone with it.
  REQUIRE_OK(ctl.undo(*pid));
  CHECK(engine->held() == 1);
  CHECK(ctl.tasks().size() == 1);
  CHECK(!ctl.project(*pid)->asset(*a2).ok());
  // The first lands in its asset.
  auto px = root / "px.png";
  REQUIRE(write_png(px, 64, 64, {0x20, 0x80, 0xff, 0xff}));
  engine->finish(*j1, px);
  CHECK(ctl.tasks().empty());
  auto v1 = ctl.project(*pid)->version(*a1);
  REQUIRE_OK(v1);
  CHECK(v1->number == 1);

  // Running when its request is undone: its result is dropped, and it
  // ends cancelled.
  auto j3 = ask("a green kite");
  REQUIRE_OK(j3);
  const auto a3 = AssetId::parse(jget<std::string>(ctl.tasks()[0], "asset",
                                                   ""));
  REQUIRE(a3);
  engine->start(*j3);
  REQUIRE_OK(ctl.undo(*pid));
  CHECK(ctl.tasks().empty());
  engine->finish(*j3, px);
  CHECK(!ctl.project(*pid)->asset(*a3).ok());
  auto rec = ctl.jobs().get(*j3);
  REQUIRE(rec);
  CHECK(rec->state == JobState::Cancelled);
  // Redone, the request is back -- the asset, with no result.
  REQUIRE_OK(ctl.redo(*pid));
  auto back = ctl.project(*pid)->asset(*a3);
  REQUIRE_OK(back);
  CHECK(back->head == 0);

  // An EXPORT asked for while generations run and wait is in a line of
  // its own (the engine's export lane): first in it, the generations'
  // places as they were.
  auto j4 = ask("a yellow tram");
  REQUIRE_OK(j4);
  engine->start(*j4);
  auto j5 = ask("a grey owl");
  REQUIRE_OK(j5);
  ExportRequest ex;
  ex.project = *pid;
  ex.asset = *a1;
  ex.format = "png";
  ex.destination = root / "fox.png";
  auto xj = ctl.export_asset(ex);
  REQUIRE_OK(xj);
  ts = ctl.tasks();
  REQUIRE(ts.size() == 3);
  for (const auto& t : ts) {
    const auto job = jget<std::string>(t, "job", "");
    const int at = jget<int>(t, "position", -1);
    if (job == xj->str()) {
      CHECK(jget<std::string>(t, "kind", "") == "export");
      CHECK(at == 0);
    } else {
      CHECK(at == (job == j4->str() ? 0 : 1));
    }
  }
}

// Downloads run BESIDE the work (DESIGN §3a): while one downloads, a
// picture of a model that is here goes to the engine at once; one that
// needs what is downloading waits for that alone -- out of the engine's
// queue -- and goes on when it lands, resolved as it landed; its
// download failing or stopped, it fails saying so; cancelled, it waits
// no more. What is neither here nor coming is refused as before. Auto
// and a preset's LoRA count what is on its way.
// The ASSET OPERATIONS (DESIGN §4j) share one protocol: listed with what
// each takes, asked for by name -- refused, by name, on what it does not
// take -- and a SEPARATION is one task making two sounds: each output
// committed to its own asset, beside the song; rebuilt alone, one keeps
// its own and drops the other's.
TEST(controller, a_song_is_split_into_its_voice_and_accompaniment)
{
  auto root = test::temp_dir("ctl-separate");
  // BS-RoFormer's vocals model, "installed": its checkpoint where it is
  // looked for.
  const fs::path repo = root / "models" / "pcunwa" /
                        "BS-Roformer-Resurrection";
  fs::create_directories(repo);
  std::ofstream(repo / "BS-Roformer-Resurrection.ckpt") << "w";
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "S.valtz", "separate");
  REQUIRE_OK(pid);

  // Listed: each operation, what it takes, whether it runs here.
  const Json ops = ctl.asset_operations_json();
  std::vector<std::string> names;
  Json separate;
  for (const auto& o : ops) {
    names.push_back(jget<std::string>(o, "name", ""));
    if (names.back() == "separate") {
      separate = o;
    }
  }
  CHECK((names == std::vector<std::string>{"transcribe", "summarize",
                                           "separate", "upscale"}));
  REQUIRE(separate.is_object());
  CHECK(!jget(separate, "on_layer", true));
  CHECK(separate["makes"] == Json::array({"vocals", "accompaniment"}));
  REQUIRE(separate["takes"].size() == 1);
  const Json take = separate["takes"][0];
  CHECK(jget<std::string>(take, "kind", "") == "audio");
  CHECK(jget(take, "ready", false));
  CHECK(jget<std::string>(take, "model", "") == "bs-roformer-vocals");
  CHECK(jget<std::string>(take, "family", "") == "separate");
  CHECK(find_asset_operation("upscale")->on_layer);
  CHECK(asset_operation_of(engine::kOpSeparateAudio)->name == "separate");
  CHECK(asset_operation_of(engine::kOpUpscaleImage)->name == "upscale");

  auto folder = ctl.create_folder(*pid, "Songs");
  REQUIRE_OK(folder);
  REQUIRE(write_tone(root / "song.wav", 1, 440, 0.5));
  auto song = import_and_wait(ctl, *pid, root / "song.wav");
  REQUIRE(song);
  REQUIRE_OK(ctl.move_asset(*pid, *song, *folder));
  REQUIRE(write_png(root / "pic.png", 16, 16, {0x40, 0x80, 0xc0, 0xff}));
  auto pic = import_and_wait(ctl, *pid, root / "pic.png");
  REQUIRE(pic);
  auto ask = [&](AssetId a, const char* op) {
    Controller::AssetOpRequest req;
    req.project = *pid;
    req.asset = a;
    req.operation = op;
    return ctl.run_asset_operation(std::move(req));
  };
  // A picture is not separated; nothing is "whittled".
  auto refused = ask(*pic, "separate");
  REQUIRE(!refused.ok());
  CHECK(refused.error().key == msg::kSeparateNeedsSound.key);
  auto unknown = ask(*song, "whittle");
  REQUIRE(!unknown.ok());
  CHECK(unknown.error().key == msg::kUnknownOperation.key);

  // One task, two sounds, beside the song.
  auto split = ask(*song, "separate");
  REQUIRE_OK(split);
  REQUIRE(split->made.size() == 2);
  project::Project* p = ctl.project(*pid);
  auto voice = p->asset(split->made[0]);
  auto rest = p->asset(split->made[1]);
  REQUIRE_OK(voice);
  REQUIRE_OK(rest);
  CHECK(voice->name.ends_with(" vocals"));
  CHECK(rest->name.ends_with(" accompaniment"));
  CHECK(voice->name.starts_with(p->asset(*song)->name));
  CHECK(voice->folder == *folder);
  CHECK(rest->folder == *folder);
  const Json ts = ctl.tasks();
  REQUIRE(ts.size() == 1);
  CHECK(jget<std::string>(ts[0], "operation", "") == "separate");
  CHECK(ts[0]["assets"] == Json::array({voice->id.str(), rest->id.str()}));
  const engine::JobSpec* spec = engine->spec(split->job);
  REQUIRE(spec != nullptr);
  CHECK(spec->op == engine::kOpSeparateAudio);
  CHECK(jget<std::string>(spec->params, "checkpoint", "") ==
        "BS-Roformer-Resurrection.ckpt");
  CHECK(jget<std::string>(spec->params, "output", "") == "vocals");
  REQUIRE(spec->inputs.size() == 1);
  CHECK(spec->inputs[0].role == "source");

  // Each output into its own asset, as the model names it.
  REQUIRE(write_tone(root / "voice.wav", 1, 880, 0.3));
  REQUIRE(write_tone(root / "band.wav", 1, 220, 0.3));
  engine->finish_outputs(split->job, {{"vocals", root / "voice.wav"},
                                      {"accompaniment", root / "band.wav"}});
  CHECK(ctl.tasks().empty());
  auto vv = p->version(voice->id);
  auto rv = p->version(rest->id);
  REQUIRE_OK(vv);
  REQUIRE_OK(rv);
  CHECK(vv->number == 1);
  CHECK(rv->number == 1);
  CHECK(vv->content != rv->content);
  CHECK(jget<std::string>(rv->executed, "output", "") == "accompaniment");
  CHECK(p->recipe(rv->recipe)->id == rest->recipe);

  // The accompaniment rebuilt alone: its own kept, the voice dropped.
  auto again = ctl.build_asset(*pid, rest->id);
  REQUIRE_OK(again);
  engine->finish_outputs(*again, {{"vocals", root / "band.wav"},
                                  {"accompaniment", root / "voice.wav"}});
  CHECK(p->version(rest->id)->number == 2);
  CHECK(p->version(voice->id)->number == 1);

  // Undone, the request goes: both sounds with it.
  REQUIRE_OK(ctl.undo(*pid));
  REQUIRE_OK(ctl.undo(*pid));
  CHECK(!p->asset(voice->id).ok());
  CHECK(!p->asset(rest->id).ok());
}

namespace {

// A diffusers pipeline's folder as a download lays it out: its pipeline
// and DiT named, a DiT and an encoder of one real (tiny) safetensors
// each.
void
fake_pipeline(const fs::path& dir, const std::string& pipeline,
              const std::string& dit)
{
  auto st = [](const fs::path& f, const std::string& tensor) {
    fs::create_directories(f.parent_path());
    std::string h = Json{{tensor, {{"dtype", "BF16"},
                                   {"shape", {64, 64}},
                                   {"data_offsets", {0, 8192}}}}}.dump();
    h.append((8 - h.size() % 8) % 8, ' ');
    std::ofstream out(f, std::ios::binary);
    for (int i = 0; i < 8; ++i) {
      out.put(static_cast<char>((h.size() >> (8 * i)) & 0xff));
    }
    out << h << std::string(8192, '\0');
  };
  fs::create_directories(dir / "transformer");
  std::ofstream(dir / "model_index.json") << Json{
      {"_class_name", pipeline}, {"transformer", {"diffusers", dit}},
      {"text_encoder", {"transformers", "Qwen3VLModel"}}}.dump();
  std::ofstream(dir / "transformer" / "config.json")
      << Json{{"_class_name", dit}}.dump();
  st(dir / "transformer" / "model.safetensors", "blocks.0.to_q.weight");
  st(dir / "text_encoder" / "model.safetensors", "layers.0.q_proj.weight");
}

}

// A DOWNLOAD REPLACED BY A SMALLER COPY OF ITSELF (DESIGN §8b): Valtz's
// own download only -- not vpipe's, not a link -- its parts quantized in
// passes, the copy CHECKED before the download is deleted (a bad one
// keeps it, a stopped one leaves nothing), found where the model is,
// and gone once the download is fetched again. And a LINK is checked
// first: another model's folder refused, a parent's the model in it.
// Draw Things checkpoints linked as a model's PARTS (Settings ›
// Capabilities): Draw Things' Models folder chosen, every part found in
// it checked and kept apart from the model -- which brings its configs
// and tokenizer, and is said to be wanted while it is not here -- and
// every job of the model runs them: its DiT and VAE unless Custom names
// its own, its text encoder always; undone, the model's own.
TEST(controller, a_draw_things_model_is_linked_and_run)
{
  using test::DrawThingsFixture;
  auto root = test::temp_dir("ctl-drawthings");
  const fs::path vpipe = root / "vpipe-models";
  fake_pipeline(vpipe / "Qwen" / "Qwen-Image-2.1", "QwenImage21Pipeline",
                "QwenImage21Transformer2DModel");
  const fs::path dt = root / "Models";
  fs::create_directories(dt);
  const fs::path dit = dt / "qwen_image_2.1_i8x.ckpt";
  const fs::path enc = dt / "qwen_3_vl_8b_instruct_i8x.ckpt";
  const fs::path vae = dt / "qwen_image_2.1_vae_f16.ckpt";
  const fs::path h3 = dt / "minimax_h3_vae_f16.ckpt";
  REQUIRE(DrawThingsFixture::qwen_image_21().write(dit));
  REQUIRE(DrawThingsFixture::qwen3_vl(4096).write(enc));
  REQUIRE(DrawThingsFixture::qwen_image_21_vae().write(vae));
  REQUIRE(DrawThingsFixture::minimax_h3_vae().write(h3));
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models", vpipe};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto member = [&](const std::string& id) {
    const Json tree = ctl.capability_tree();
    for (const auto& f : tree["families"]) {
      for (const auto& m : f["members"]) {
        if (m["model"] == id) {
          return m;
        }
      }
    }
    return Json();
  };
  CHECK((member("qwen-image-2.1")["drawthings"] ==
         Json::array({"dit", "encoder", "vae"})));
  CHECK((member("minimax-h3-ref2va")["drawthings"] ==
         Json::array({"encoder", "vae"})));
  CHECK(member("krea2-turbo")["drawthings"].empty());

  // Draw Things' Models chosen: Qwen-Image's three parts, linked apart.
  auto l = ctl.link_model("qwen-image-2.1", dt);
  REQUIRE_OK(l);
  CHECK(jget(*l, "linked", false));
  CHECK((*l)["parts"]["dit"] == dit.string());
  CHECK((*l)["parts"]["encoder"] == enc.string());
  CHECK((*l)["parts"]["vae"] == vae.string());
  Json m = member("qwen-image-2.1");
  CHECK(m["state"] == "installed" && m["source"] == "vpipe");
  CHECK(m["parts"].size() == 3);
  CHECK(jget(m["parts"]["encoder"], "present", false));
  // Not for Krea 2.
  auto k = ctl.link_model("krea2-turbo", dit);
  REQUIRE_OK(k);
  CHECK(!jget(*k, "linked", true));
  CHECK(!member("krea2-turbo").contains("parts"));
  // H3 FL2VA is not here: its VAEs linked, its download said to be wanted
  // (its configs and tokenizer).
  auto f = ctl.link_model("minimax-h3-fl2va", dt);
  REQUIRE_OK(f);
  CHECK(jget(*f, "linked", false));
  CHECK((*f)["notes"][0]["key"] == "core.check_drawthings_needs_model");
  CHECK(member("minimax-h3-fl2va")["state"] == "missing");
  CHECK(member("minimax-h3-fl2va")["parts"].contains("vae"));

  auto pid = ctl.create_project(root / "D.valtz", "drawthings");
  REQUIRE_OK(pid);
  auto ask = [&](const Json& tuning) {
    GenerateImageRequest req;
    req.project = *pid;
    req.prompt = "a red fox";
    req.model = "qwen-image-2.1";
    req.width = 64;
    req.height = 64;
    req.tuning = tuning;
    return ctl.generate_image(req);
  };
  auto j = ask(Json::object());
  REQUIRE_OK(j);
  const engine::JobSpec* js = engine->spec(*j);
  REQUIRE(js);
  CHECK(js->model.dit == dit);
  CHECK(js->model.encoder_weights == enc);
  CHECK(js->model.vae_weights == vae);
  CHECK(js->model.vae.empty());
  CHECK(jget<std::string>(js->params, "encoder_weights", "") ==
        enc.string());
  // Custom's own DiT and VAE on: those, the encoder still Draw Things'.
  const fs::path own = root / "own-dit.safetensors";
  const fs::path ownv = root / "own-vae";
  std::ofstream(own) << "x";
  fs::create_directories(ownv);
  auto c = ask({{"dits", {{{"path", own.string()}, {"on", true}}}},
                {"vaes", {{{"path", ownv.string()}, {"on", true}}}}});
  REQUIRE_OK(c);
  const engine::JobSpec* cs = engine->spec(*c);
  REQUIRE(cs);
  CHECK(cs->model.dit == fs::weakly_canonical(own));
  CHECK(cs->model.vae == fs::weakly_canonical(ownv));
  CHECK(cs->model.vae_weights.empty());
  CHECK(cs->model.encoder_weights == enc);
  // Undone: the model's own.
  REQUIRE_OK(ctl.unlink_model("qwen-image-2.1", true));
  CHECK(!member("qwen-image-2.1").contains("parts"));
  auto u = ask(Json::object());
  REQUIRE_OK(u);
  REQUIRE(engine->spec(*u));
  CHECK(engine->spec(*u)->model.dit.empty());
  CHECK(engine->spec(*u)->model.encoder_weights.empty());
  CHECK(engine->spec(*u)->model.vae_weights.empty());
}

TEST(controller, a_download_is_compressed_and_a_link_checked)
{
  auto root = test::temp_dir("ctl-compress");
  const fs::path models = root / "models";
  const fs::path vpipe = root / "vpipe-models";
  const fs::path krea = models / "krea" / "Krea-2-Turbo";
  fake_pipeline(krea, "Krea2Pipeline", "Krea2Transformer2DModel");
  fake_pipeline(vpipe / "Qwen" / "Qwen-Image-2.1", "QwenImage21Pipeline",
                "QwenImage21Transformer2DModel");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {models, vpipe};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto member = [&](const std::string& id) {
    const Json tree = ctl.capability_tree();
    for (const auto& f : tree["families"]) {
      for (const auto& m : f["members"]) {
        if (m["model"] == id) {
          return m;
        }
      }
    }
    return Json();
  };

  // What each width would take; why not, for vpipe's files.
  Json m = member("krea2-turbo");
  CHECK(jget(m["compress"], "ready", false));
  REQUIRE(m["compress"]["widths"].size() == 2);
  CHECK(m["compress"]["widths"][0]["bits"] == 8);
  CHECK(jget<std::uint64_t>(m["compress"]["widths"][1], "bytes", 0) <
        jget<std::uint64_t>(m["compress"]["widths"][0], "bytes", 0));
  CHECK(member("qwen-image-2.1")["compress"]["why"] == "vpipe");
  CHECK(ctl.compress_model("qwen-image-2.1", 8).error().key ==
        msg::kCompressNotOurs.key);
  CHECK(ctl.compress_model("krea2-turbo", 6).error().key ==
        msg::kCompressBits.key);
  CHECK(ctl.compress_model("yue2-3b", 8).error().key ==
        msg::kNotCompressible.key);
  CHECK(ctl.compress_model("z-image-turbo", 8).error().key ==
        msg::kModelNotInstalled.key);

  // Asked: one job, its passes the catalog's parts.
  auto job = ctl.compress_model("krea2-turbo", 8);
  REQUIRE_OK(job);
  auto again = ctl.compress_model("krea2-turbo", 8);
  REQUIRE_OK(again);
  CHECK(*again == *job);
  const auto* spec = engine->spec(*job);
  REQUIRE(spec != nullptr);
  CHECK(spec->op == engine::kOpQuantizeModel);
  CHECK(spec->model.dir == krea);
  CHECK(jget(spec->params, "bits", 0) == 8);
  REQUIRE(spec->params["parts"].size() == 2);
  CHECK(spec->params["parts"][0]["target"] == "dit");
  CHECK(spec->params["parts"][1]["target"] == "text_encoder");
  const fs::path staging = jget<std::string>(spec->params, "output", "");
  CHECK(staging.filename() == ".krea2-turbo-8bit.compressing");
  CHECK(ctl.jobs().get(*job)->purpose == "compress");

  // vpipe's passes write it: checked, marked, in place -- the download
  // and the folders it leaves empty deleted, the passes before the last.
  fake_pipeline(staging, "Krea2Pipeline", "Krea2Transformer2DModel");
  fs::create_directories(staging.string() + ".pass1");
  engine->end(*job);
  const fs::path dest = models / "compressed" / "krea2-turbo-8bit";
  CHECK(ctl.jobs().get(*job)->state == JobState::Finished);
  CHECK(fs::exists(dest / models::ModelStore::kCompressedMarker));
  CHECK(!fs::exists(staging));
  CHECK(!fs::exists(staging.string() + ".pass1"));
  CHECK(!fs::exists(krea));
  CHECK(!fs::exists(models / "krea"));
  m = member("krea2-turbo");
  CHECK(m["compressed"]["bits"] == 8);
  CHECK(jget<std::uint64_t>(m["compressed"], "original_bytes", 0) > 0);
  CHECK(m["compress"]["why"] == "compressed");
  CHECK(m["path"] == dest.string());
  CHECK(ctl.compress_model("krea2-turbo", 4).error().key ==
        msg::kAlreadyCompressed.key);
  auto checked = ctl.check_model("krea2-turbo");
  REQUIRE_OK(checked);
  CHECK(jget(*checked, "confirmed", false));

  // Fetched again: the original back, the copy gone.
  auto dl = ctl.download_model("krea2-turbo");
  REQUIRE_OK(dl);
  fake_pipeline(krea, "Krea2Pipeline", "Krea2Transformer2DModel");
  engine->end(*dl);
  CHECK(!fs::exists(dest));
  m = member("krea2-turbo");
  CHECK(m["compressed"].is_null());
  CHECK(m["path"] == krea.string());

  // A copy that fails its check keeps the download; a stopped one leaves
  // nothing.
  auto bad = ctl.compress_model("krea2-turbo", 4);
  REQUIRE_OK(bad);
  const fs::path staging4 =
      jget<std::string>(engine->spec(*bad)->params, "output", "");
  fake_pipeline(staging4, "Flux2KleinPipeline", "Flux2Transformer2DModel");
  engine->end(*bad);
  CHECK(ctl.jobs().get(*bad)->state == JobState::Failed);
  CHECK(fs::exists(krea));
  CHECK(!fs::exists(staging4));
  CHECK(!fs::exists(models / "compressed" / "krea2-turbo-4bit"));
  auto stop = ctl.compress_model("krea2-turbo", 4);
  REQUIRE_OK(stop);
  fs::create_directories(staging4);
  REQUIRE_OK(engine->cancel(*stop));
  CHECK(!fs::exists(staging4));
  CHECK(fs::exists(krea));

  // klein at 4 bits: vpipe's own recipe, calibrated (AWQ); at 8, plain.
  const fs::path elsewhere = root / "elsewhere";
  fake_pipeline(elsewhere / "bfl" / "FLUX.2-klein-9B", "Flux2KleinPipeline",
                "Flux2Transformer2DModel");
  // A link, checked: Krea 2's folder is not klein; the folder holding
  // klein's, the one in it.
  auto wrong = ctl.link_model("flux2-klein-9b", krea);
  REQUIRE_OK(wrong);
  CHECK(!jget(*wrong, "linked", true));
  CHECK((*wrong)["notes"][0]["key"] == "core.check_wrong_model");
  CHECK(member("flux2-klein-9b")["state"] == "missing");
  auto good = ctl.link_model("flux2-klein-9b", elsewhere);
  REQUIRE_OK(good);
  CHECK(jget(*good, "linked", false));
  CHECK((*good)["path"] == (elsewhere / "bfl" / "FLUX.2-klein-9B").string());
  CHECK(member("flux2-klein-9b")["source"] == "link");
  CHECK(member("flux2-klein-9b")["compress"]["why"] == "link");
  CHECK(ctl.compress_model("flux2-klein-9b", 4).error().key ==
        msg::kCompressNotOurs.key);
  REQUIRE_OK(ctl.unlink_model("flux2-klein-9b"));
  fake_pipeline(models / "black-forest-labs" / "FLUX.2-klein-9B",
                "Flux2KleinPipeline", "Flux2Transformer2DModel");
  ctl.rescan_models();
  auto k4 = ctl.compress_model("flux2-klein-9b", 4);
  REQUIRE_OK(k4);
  const Json parts = engine->spec(*k4)->params["parts"];
  CHECK(jget(parts[0]["options"], "awq", false));
  CHECK(jget(parts[1]["options"], "awq_clip", false));
  REQUIRE_OK(engine->cancel(*k4));
  auto k8 = ctl.compress_model("flux2-klein-9b", 8);
  REQUIRE_OK(k8);
  CHECK(!engine->spec(*k8)->params["parts"][0]["options"].contains("awq"));
  REQUIRE_OK(engine->cancel(*k8));
}

// A compressed copy TRIED FIRST (DESIGN §8b): asked to keep the
// original, the compress deletes nothing -- the copy in use beside its
// download -- until one of the two is kept: the original used again, the
// copy kept (its original deleted, as a compress that does not keep it
// ends) or the original kept (the copy deleted). Nothing is deleted while
// a task runs.
TEST(controller, a_compressed_copy_is_tried_beside_its_original)
{
  auto root = test::temp_dir("ctl-compress-keep");
  const fs::path models = root / "models";
  const fs::path krea = models / "krea" / "Krea-2-Turbo";
  fake_pipeline(krea, "Krea2Pipeline", "Krea2Transformer2DModel");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {models};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto member = [&](const std::string& id) {
    const Json tree = ctl.capability_tree();
    for (const auto& f : tree["families"]) {
      for (const auto& m : f["members"]) {
        if (m["model"] == id) {
          return m;
        }
      }
    }
    return Json();
  };
  const fs::path dest = models / "compressed" / "krea2-turbo-8bit";
  auto compress = [&] {
    auto job = ctl.compress_model("krea2-turbo", 8, true);
    REQUIRE_OK(job);
    const fs::path staging =
        jget<std::string>(engine->spec(*job)->params, "output", "");
    fake_pipeline(staging, "Krea2Pipeline", "Krea2Transformer2DModel");
    engine->end(*job);
    CHECK(ctl.jobs().get(*job)->state == JobState::Finished);
  };
  CHECK(ctl.use_compressed("krea2-turbo", false).error().key ==
        msg::kNotBothKept.key);

  // Kept: both on disk, the copy in use.
  compress();
  CHECK(fs::exists(dest / models::ModelStore::kCompressedMarker));
  CHECK(fs::exists(krea));
  Json m = member("krea2-turbo");
  CHECK(m["compressed"]["bits"] == 8);
  CHECK(jget(m["compressed"], "in_use", false));
  CHECK(jget(m["compressed"], "original_kept", false));
  CHECK(jget<std::uint64_t>(m["compressed"], "bytes", 0) > 0);
  CHECK(m["path"] == dest.string());
  CHECK(m["compress"]["why"] == "compressed");
  CHECK(ctl.compress_model("krea2-turbo", 4).error().key ==
        msg::kCompressedBesideOriginal.key);

  // The original in use again; the copy set aside, still listed.
  REQUIRE_OK(ctl.use_compressed("krea2-turbo", false));
  m = member("krea2-turbo");
  CHECK(m["path"] == krea.string());
  CHECK(m["compressed"]["bits"] == 8);
  CHECK(!jget(m["compressed"], "in_use", true));
  CHECK(jget(m["compressed"], "original_kept", false));
  CHECK(fs::exists(dest));
  REQUIRE_OK(ctl.use_compressed("krea2-turbo", false));
  REQUIRE_OK(ctl.use_compressed("krea2-turbo", true));
  CHECK(member("krea2-turbo")["path"] == dest.string());

  // Nothing deleted under a task.
  fake_pipeline(models / "black-forest-labs" / "FLUX.2-klein-9B",
                "Flux2KleinPipeline", "Flux2Transformer2DModel");
  ctl.rescan_models();
  auto busy = ctl.compress_model("flux2-klein-9b", 8);
  REQUIRE_OK(busy);
  CHECK(ctl.keep_compressed("krea2-turbo", false).error().key ==
        msg::kWeightsBusy.key);
  REQUIRE_OK(engine->cancel(*busy));

  // The original kept: the copy deleted, the compressed folder with it.
  auto freed = ctl.keep_compressed("krea2-turbo", false);
  REQUIRE_OK(freed);
  CHECK(*freed > 0);
  CHECK(!fs::exists(dest));
  CHECK(!fs::exists(models / "compressed"));
  m = member("krea2-turbo");
  CHECK(m["compressed"].is_null());
  CHECK(m["path"] == krea.string());
  CHECK(jget(m["compress"], "ready", false));

  // Again, the copy kept: its original deleted, as without keeping it.
  compress();
  REQUIRE_OK(ctl.use_compressed("krea2-turbo", false));
  freed = ctl.keep_compressed("krea2-turbo", true);
  REQUIRE_OK(freed);
  CHECK(*freed > 0);
  CHECK(!fs::exists(krea));
  CHECK(!fs::exists(models / "krea"));
  m = member("krea2-turbo");
  CHECK(m["path"] == dest.string());
  CHECK(jget(m["compressed"], "in_use", false));
  CHECK(!jget(m["compressed"], "original_kept", true));
  CHECK(ctl.keep_compressed("krea2-turbo", false).error().key ==
        msg::kNotBothKept.key);
  CHECK(ctl.compress_model("krea2-turbo", 4).error().key ==
        msg::kAlreadyCompressed.key);
}

// FLUX.3 Image Klein makes pictures up to 4096 a side (catalog
// `max_edge`, which the app reads to offer its 4K size): asked for that,
// the job is; past it, refused by name before any graph.
TEST(controller, flux3_klein_makes_up_to_4096_a_side)
{
  auto root = test::temp_dir("ctl-flux3");
  const fs::path models = root / "models";
  fake_pipeline(models / "black-forest-labs" / "flux-3-image-klein",
                "Flux3Pipeline", "Flux3Transformer2DModel");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {models};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  const auto* flux3 = ctl.catalog().find("flux3-klein");
  REQUIRE(flux3);
  CHECK(models::max_edge(*flux3) == 4096);
  CHECK(Json(*flux3)["max_edge"] == 4096);
  CHECK(!Json(*ctl.catalog().find("krea2-turbo")).contains("max_edge"));
  auto pid = ctl.create_project(root / "F.valtz", "flux3");
  REQUIRE_OK(pid);
  GenerateImageRequest req;
  req.project = *pid;
  req.prompt = "A wide-angle photograph of a bison in steam";
  req.model = "flux3-klein";
  req.width = 4096;
  req.height = 4096;
  auto job = ctl.generate_image(req);
  REQUIRE_OK(job);
  const auto* spec = engine->spec(*job);
  REQUIRE(spec != nullptr);
  CHECK(spec->model.id == "flux3-klein");
  CHECK(jget(spec->params, "width", 0) == 4096);
  req.width = 5000;
  auto big = ctl.generate_image(req);
  REQUIRE(!big.ok());
  CHECK(big.error().key == msg::kSizeTooLarge.key);
  CHECK(big.error().message.find("4096") != std::string::npos);
}

TEST(controller, a_job_waits_only_for_what_it_needs_downloading)
{
  auto root = test::temp_dir("ctl-downloads");
  const fs::path models = root / "models";
  auto install = [&](const char* hf_path) {
    fs::create_directories(models / hf_path);
    std::ofstream(models / hf_path / "model.safetensors") << "w";
  };
  install("krea/Krea-2-Turbo");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {models};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "D.valtz", "downloads");
  REQUIRE_OK(pid);
  auto ask = [&](const char* words, const char* model) {
    GenerateImageRequest req;
    req.project = *pid;
    req.prompt = words;
    req.model = model;
    req.width = 64;
    req.height = 64;
    return ctl.generate_image(req);
  };
  auto state = [&](JobId j) {
    auto r = ctl.jobs().get(j);
    return r ? r->state : JobState::Queued;
  };

  // Z-Image downloading: one job, however often asked.
  auto dl = ctl.download_model("z-image-turbo");
  REQUIRE_OK(dl);
  CHECK(ctl.downloading("z-image-turbo"));
  auto again = ctl.download_model("z-image-turbo");
  REQUIRE_OK(again);
  CHECK(*again == *dl);
  CHECK(engine->held() == 1);

  // Krea 2 is here: to the engine at once, beside the download.
  auto k = ask("a red fox", "krea2-turbo");
  REQUIRE_OK(k);
  CHECK(engine->spec(*k) != nullptr);
  // Z-Image is on its way: its picture waits, out of the engine's queue,
  // listed after the one in line, saying what it waits for.
  auto z = ask("a blue boat", "z-image-turbo");
  REQUIRE_OK(z);
  CHECK(engine->spec(*z) == nullptr);
  CHECK(engine->held() == 2);
  Json ts = ctl.tasks();
  REQUIRE(ts.size() == 2);
  CHECK(jget<std::string>(ts[0], "job", "") == k->str());
  CHECK(!ts[0].contains("waiting"));
  CHECK(jget<std::string>(ts[1], "job", "") == z->str());
  CHECK(jget<std::string>(ts[1], "state", "") == "queued");
  CHECK(jget<int>(ts[1], "position", -1) == 1);
  CHECK(jget(ts[1], "waiting", Json::array()) ==
        Json::array({"Z-Image Turbo"}));

  // It lands: the job goes to the engine, its model where it landed.
  install("Tongyi-MAI/Z-Image-Turbo");
  engine->end(*dl);
  CHECK(!ctl.downloading("z-image-turbo"));
  const engine::JobSpec* zs = engine->spec(*z);
  REQUIRE(zs != nullptr);
  CHECK(zs->model.id == "z-image-turbo");
  CHECK(zs->model.dir == models / "Tongyi-MAI/Z-Image-Turbo");
  CHECK(!ctl.tasks()[1].contains("waiting"));

  // Its download failing, a job waiting for it fails, saying why.
  auto dq = ctl.download_model("qwen-image-2.1");
  REQUIRE_OK(dq);
  auto q = ask("a green kite", "qwen-image-2.1");
  REQUIRE_OK(q);
  CHECK(engine->spec(*q) == nullptr);
  engine->fail(*dq, "HTTP 500");
  CHECK(state(*q) == JobState::Failed);
  CHECK(ctl.jobs().get(*q)->message.find("download failed") !=
        std::string::npos);
  CHECK(engine->spec(*q) == nullptr);

  // Neither here nor coming: refused, as before.
  CHECK(!ask("a grey whale", "flux2-klein-9b").ok());

  // Cancelled while it waits: it waits no more, and the download landing
  // sends nothing.
  auto df = ctl.download_model("flux2-klein-9b");
  REQUIRE_OK(df);
  auto f = ask("a yellow tram", "flux2-klein-9b");
  REQUIRE_OK(f);
  REQUIRE_OK(ctl.cancel(*f));
  CHECK(state(*f) == JobState::Cancelled);
  install("black-forest-labs/FLUX.2-klein-9B");
  engine->end(*df);
  CHECK(engine->spec(*f) == nullptr);

  // Auto: no clip model here, MiniMax H3 on its way -- Auto's pick,
  // waiting for it. Pictures: Krea 2, which runs now, over one coming.
  auto pick = [&](const char* modality, const char* op) {
    for (const auto& a : ctl.auto_models()) {
      if (a.modality == modality && a.op == op) {
        return a;
      }
    }
    return Controller::AutoPick{};
  };
  CHECK(pick("video", "generate").chosen.empty());
  auto dh = ctl.download_model("minimax-h3-fl2va");
  REQUIRE_OK(dh);
  CHECK(pick("video", "generate").chosen == "minimax-h3-fl2va");
  CHECK(pick("video", "generate").waits);
  CHECK(pick("image", "generate").chosen == "krea2-turbo");
  CHECK(!pick("image", "generate").waits);

  // A preset's Turbo LoRA downloading is no longer missing: the preset
  // runs with it, and its job will wait for it.
  const auto* h3 = ctl.catalog().find("minimax-h3-fl2va");
  REQUIRE(h3 != nullptr);
  const Json before = ctl.preset_summary(*h3, "speed");
  const Json miss = jget(before, "missing", Json::array());
  REQUIRE(!miss.empty());
  const auto lora = jget<std::string>(miss[0], "id", "");
  REQUIRE_OK(ctl.download_model(lora));
  const Json after = ctl.preset_summary(*h3, "speed");
  CHECK(jget(after, "missing", Json::array()).empty());
  CHECK(!jget<std::string>(after, "turbo", "").empty());
}

// Settings › Capabilities' Pause and Cancel (DESIGN §8): a download
// PAUSED keeps what came down -- the model partial, a download of it
// again going on from there -- and one CANCELLED deletes it once its job
// lets go of the files (at once, queued or paused before); in a repo
// holding other models' files, only its own.
TEST(controller, a_download_is_paused_or_cancelled)
{
  auto root = test::temp_dir("ctl-download-stop");
  const fs::path models = root / "models";
  auto write = [&](const fs::path& rel) {
    fs::create_directories((models / rel).parent_path());
    std::ofstream(models / rel) << "weights";
  };
  write("krea/Krea-2-Turbo/model.safetensors");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {models};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "S.valtz", "stops");
  REQUIRE_OK(pid);
  auto on_disk = [&](const char* id) {
    models::ModelStore store({models});
    return store.info(*ctl.catalog().find(id)).state;
  };

  // PAUSED while it runs: asked to stop, then stopped -- what waited for
  // it fails, saying so; what came down stays, the model partial.
  const fs::path z = "Tongyi-MAI/Z-Image-Turbo";
  auto dl = ctl.download_model("z-image-turbo");
  REQUIRE_OK(dl);
  engine->start(*dl);
  write(z / "config.json");
  write(z / "transformer/diffusion.safetensors.part");
  GenerateImageRequest req;
  req.project = *pid;
  req.prompt = "a blue boat";
  req.model = "z-image-turbo";
  req.width = 64;
  req.height = 64;
  auto boat = ctl.generate_image(req);
  REQUIRE_OK(boat);
  REQUIRE_OK(ctl.stop_download("z-image-turbo", false));
  CHECK(engine->stopping(*dl));
  CHECK(ctl.downloading("z-image-turbo"));
  engine->stopped(*dl);
  CHECK(!ctl.downloading("z-image-turbo"));
  CHECK(ctl.jobs().get(*dl)->state == JobState::Cancelled);
  CHECK(ctl.jobs().get(*boat)->state == JobState::Failed);
  CHECK(ctl.jobs().get(*boat)->message.find("stopped") !=
        std::string::npos);
  CHECK(fs::exists(models / z / "config.json"));
  CHECK(fs::exists(models / z / "transformer/diffusion.safetensors.part"));
  CHECK(on_disk("z-image-turbo") == models::InstallState::Partial);
  // Nothing running to pause now.
  Status idle = ctl.stop_download("z-image-turbo", false);
  REQUIRE(!idle.ok());
  CHECK(idle.error().key == msg::kNoDownloadRunning.key);

  // Downloaded again, then CANCELLED: its files stay while the job
  // holds them, and go -- the empty folder above with them -- once it
  // has stopped.
  auto again = ctl.download_model("z-image-turbo");
  REQUIRE_OK(again);
  engine->start(*again);
  REQUIRE_OK(ctl.stop_download("z-image-turbo", true));
  CHECK(fs::exists(models / z / "config.json"));
  engine->stopped(*again);
  CHECK(!fs::exists(models / z));
  CHECK(!fs::exists(models / "Tongyi-MAI"));
  CHECK(on_disk("z-image-turbo") == models::InstallState::Missing);
  CHECK(fs::exists(models / "krea/Krea-2-Turbo/model.safetensors"));

  // Queued, cancelled: dropped at once, and what an earlier try left
  // deleted with it.
  write("Qwen/Qwen-Image-2.1/model-00001-of-00004.safetensors.part");
  auto queued = ctl.download_model("qwen-image-2.1");
  REQUIRE_OK(queued);
  REQUIRE_OK(ctl.stop_download("qwen-image-2.1", true));
  CHECK(engine->spec(*queued) == nullptr);
  CHECK(!fs::exists(models / "Qwen"));

  // Paused a launch ago -- no job: deleted at once.
  write("black-forest-labs/FLUX.2-klein-9B/vae/model.safetensors.part");
  REQUIRE_OK(ctl.stop_download("flux2-klein-9b", true));
  CHECK(!fs::exists(models / "black-forest-labs"));

  // One file of a repo holding several: its partial copy goes, the
  // other model's file and the repo's own files stay.
  const fs::path lx = "lightx2v/Minimax-h3-Turbo";
  write(lx / "minimax_h3_fl2v_turbo_8step_v1.0_768p_bf16.safetensors");
  write(lx / "README.md");
  auto four = ctl.download_model("minimax-h3-turbo-4step");
  REQUIRE_OK(four);
  engine->start(*four);
  write(lx / "minimax_h3_fl2v_turbo_4step_v1.2_768p_bf16.safetensors.part");
  REQUIRE_OK(ctl.stop_download("minimax-h3-turbo-4step", true));
  engine->stopped(*four);
  CHECK(!fs::exists(
      models / lx /
      "minimax_h3_fl2v_turbo_4step_v1.2_768p_bf16.safetensors.part"));
  CHECK(fs::exists(
      models / lx / "minimax_h3_fl2v_turbo_8step_v1.0_768p_bf16.safetensors"));
  CHECK(fs::exists(models / lx / "README.md"));

  // A partition of a repo whose other partition is here: its own folder
  // goes, the other stays.
  const fs::path h3 = "MiniMaxAI/MiniMax-H3";
  write(h3 / "FL2VA/transformer/model.safetensors");
  write(h3 / "Ref2VA/transformer/model.safetensors.part");
  REQUIRE_OK(ctl.stop_download("minimax-h3-ref2va", true));
  CHECK(!fs::exists(models / h3 / "Ref2VA"));
  CHECK(fs::exists(models / h3 / "FL2VA/transformer/model.safetensors"));

  // Finished before the cancel took: whole, kept.
  auto tae = ctl.download_model("taeh3");
  REQUIRE_OK(tae);
  engine->start(*tae);
  REQUIRE_OK(ctl.stop_download("taeh3", true));
  write("madebyollin/taehv/taeh3.safetensors");
  engine->end(*tae);
  CHECK(fs::exists(models / "madebyollin/taehv/taeh3.safetensors"));
  CHECK(on_disk("taeh3") == models::InstallState::Installed);
}

// The same with the REAL engine and a real download (DESIGN §3a; the
// network): a model fetched into a scratch root -- VALTZ_TEST_DOWNLOAD
// names it, `1` Qwen3-ASR 0.6B (1 GB) -- while Krea 2, installed here,
// makes a small picture: the picture starts before the download ends
// (the engine's two lanes), and both finish. Then Z-Image's download
// (31 GB) begins, a Z-Image picture waits for it, and stopping the
// download fails the picture saying so -- its partial files deleted
// with the scratch root.
TEST(controller, a_real_download_runs_beside_a_generation)
{
  const char* want = std::getenv("VALTZ_TEST_DOWNLOAD");
  if (!want) {
    SKIP("set VALTZ_TEST_DOWNLOAD=1 or a catalog id (downloads; Krea 2 "
         "installed)");
  }
  const std::string fetch = std::string(want) == "1" ? "qwen3-asr-0.6b"
                                                     : want;
  auto root = test::temp_dir("ctl-real-download");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  // Downloads go to the scratch root; installed models are read where
  // they are.
  cfg.model_roots = {root / "models"};
  for (const auto& r : models::ModelStore::default_roots(
           AppPaths::standard().models)) {
    cfg.model_roots.push_back(r);
  }
  cfg.extensions = false;
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  const auto* krea = ctl.catalog().find("krea2-turbo");
  REQUIRE(krea != nullptr);
  if (!ctl.runs_here(*krea, "image", "generate")) {
    SKIP("Krea 2 Turbo is not installed (or no engine)");
  }
  const auto* got = ctl.catalog().find(fetch);
  REQUIRE(got != nullptr);
  // What is on disk, read afresh.
  auto installed = [&](const models::ModelEntry& e) {
    return models::ModelStore(cfg.model_roots).info(e);
  };
  if (installed(*got).state == models::InstallState::Installed) {
    SKIP("the model to download is installed already: name another");
  }
  auto pid = ctl.create_project(root / "R.valtz", "real");
  REQUIRE_OK(pid);
  using clock = std::chrono::steady_clock;
  const auto t0 = clock::now();
  auto at = [&] {
    return std::chrono::duration<double>(clock::now() - t0).count();
  };
  auto state = [&](JobId j) {
    auto r = ctl.jobs().get(j);
    return r ? r->state : JobState::Queued;
  };
  auto ended = [&](JobId j) {
    const JobState s = state(j);
    return s != JobState::Queued && s != JobState::Running;
  };

  auto dl = ctl.download_model(fetch);
  REQUIRE_OK(dl);
  REQUIRE(eventually([&] { return state(*dl) == JobState::Running; }, 30));
  GenerateImageRequest req;
  req.project = *pid;
  req.prompt = "a red fox in snow";
  req.model = "krea2-turbo";
  req.width = 256;
  req.height = 256;
  auto gen = ctl.generate_image(req);
  REQUIRE_OK(gen);
  // The picture starts while the download runs: the lanes.
  REQUIRE(eventually([&] { return state(*gen) != JobState::Queued; }, 60));
  const double started = at();
  const bool downloading = !ended(*dl);
  std::printf("  picture started at %.1f s, the download %s\n", started,
              downloading ? "still running" : "already over");
  CHECK(downloading);
  REQUIRE(eventually([&] { return ended(*gen) && ended(*dl); }, 600));
  std::printf("  both over at %.1f s: picture %s, download %s\n", at(),
              to_str(state(*gen)), to_str(state(*dl)));
  CHECK(state(*gen) == JobState::Finished);
  CHECK(state(*dl) == JobState::Finished);
  CHECK(installed(*got).state == models::InstallState::Installed);
  CHECK(installed(*got).dir.string().starts_with(
      (root / "models").string()));

  // A picture of a model downloading waits for it; the download stopped,
  // it fails saying so.
  auto dz = ctl.download_model("z-image-turbo");
  REQUIRE_OK(dz);
  REQUIRE(eventually([&] { return state(*dz) == JobState::Running; }, 30));
  req.model = "z-image-turbo";
  req.prompt = "a blue boat";
  auto zj = ctl.generate_image(req);
  REQUIRE_OK(zj);
  Json ts = ctl.tasks();
  REQUIRE(ts.size() == 1);
  CHECK(jget(ts[0], "waiting", Json::array()) ==
        Json::array({"Z-Image Turbo"}));
  std::this_thread::sleep_for(std::chrono::seconds(3));
  CHECK(state(*zj) == JobState::Queued);
  REQUIRE_OK(ctl.cancel(*dz));
  REQUIRE(eventually([&] { return ended(*zj) && ended(*dz); }, 60));
  CHECK(state(*dz) == JobState::Cancelled);
  CHECK(state(*zj) == JobState::Failed);
  CHECK(ctl.jobs().get(*zj)->message.find("stopped") != std::string::npos);
  ctl.shutdown();
  std::error_code ec;
  fs::remove_all(root, ec);
}

// A picture file's pixels, 16-bit RGBA as decoded: two equal to the bit
// decode alike.
std::vector<std::uint16_t>
pixels_of_file(const std::filesystem::path& file)
{
  std::vector<std::uint16_t> out;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(file.c_str()),
      static_cast<CFIndex>(std::strlen(file.c_str())), false);
  CGImageSourceRef src = CGImageSourceCreateWithURL(url, nullptr);
  CFRelease(url);
  if (!src) {
    return out;
  }
  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
  CFRelease(src);
  if (!img) {
    return out;
  }
  const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
  out.resize(w * h * 4);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      out.data(), w, h, 16, w * 8, cs,
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
          kCGBitmapByteOrder16Little);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  CGImageRelease(img);
  return out;
}

// A generation's ENCODINGS kept between jobs (DESIGN §4l), with the real
// engine and the models installed here -- VALTZ_TEST_KEPT=1 (`video` adds
// MiniMax H3). In one session -- the kernels tuned once, so a picture
// made twice alike is the same to the bit -- Krea 2 makes a picture from
// words, its conditioning made and kept, then again from the same words
// and seed, the conditioning FED: the two pictures are one. Another seed
// is fed too. Qwen-Image 2.1 edits a picture: its conditioning (the
// words and the picture, its text encoder a VLM) and the picture's
// latents made, then both fed -- the same edit -- then other words: the
// latents fed, the conditioning made. A clip (`video`): H3's 32B prompt
// reader made, then fed.
TEST(controller, a_generations_encodings_are_kept_and_fed)
{
  const char* want = std::getenv("VALTZ_TEST_KEPT");
  if (!want) {
    SKIP("set VALTZ_TEST_KEPT=1 (Krea 2 Turbo, Qwen-Image 2.1 installed; "
         "`video`: MiniMax H3 FL2VA too)");
  }
  auto root = test::temp_dir("ctl-kept");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = models::ModelStore::default_roots(
      AppPaths::standard().models);
  cfg.extensions = false;
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto runs = [&](const char* id, const char* modality, const char* op) {
    const auto* e = ctl.catalog().find(id);
    return e && ctl.runs_here(*e, modality, op);
  };
  if (!runs("krea2-turbo", "image", "generate")) {
    SKIP("Krea 2 Turbo is not installed (or no engine)");
  }
  auto pid = ctl.create_project(root / "K.valtz", "kept");
  REQUIRE_OK(pid);
  std::uint64_t seen = 0;
  // What the engine said since the last ask, and how often it said `what`
  // in it.
  auto since = [&] {
    std::string text;
    const Json lines = ctl.log_since(seen);
    for (const auto& l : jget(lines, "rows", Json::array())) {
      text += jget<std::string>(l, "text", "") + "\n";
    }
    seen = jget<std::uint64_t>(lines, "next", seen);
    return text;
  };
  auto count = [](const std::string& text, std::string_view what) {
    int n = 0;
    for (auto at = text.find(what); at != std::string::npos;
         at = text.find(what, at + what.size())) {
      ++n;
    }
    return n;
  };
  // The largest difference at a pixel's channel, of 65535.
  auto most = [](const std::vector<std::uint16_t>& a,
                 const std::vector<std::uint16_t>& b) {
    if (a.size() != b.size()) {
      return 65536;
    }
    int m = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
      m = std::max(m, std::abs(int(a[i]) - int(b[i])));
    }
    return m;
  };
  auto finished = [&](Result<JobId> j) -> std::optional<AssetId> {
    if (!j.ok()) {
      std::printf("  refused: %s\n", j.error().message.c_str());
      return std::nullopt;
    }
    const JobId id = *j;
    if (!eventually([&] {
          auto r = ctl.jobs().get(id);
          return r && r->state != JobState::Queued &&
                 r->state != JobState::Running;
        }, 900)) {
      return std::nullopt;
    }
    auto r = ctl.jobs().get(id);
    if (!r || r->state != JobState::Finished) {
      std::printf("  job %s: %s %s\n", id.str().c_str(),
                  r ? to_str(r->state) : "?", r ? r->message.c_str() : "");
      return std::nullopt;
    }
    return r->asset;
  };
  auto pixels = [&](AssetId a) {
    auto p = ctl.project(*pid)->media_path(a);
    return p.ok() ? pixels_of_file(*p) : std::vector<std::uint16_t>();
  };
  auto prepare = [&](AssetId a) {
    auto v = ctl.project(*pid)->version(a);
    return v.ok() ? jget(jget(v->timing, "phases", Json::object()),
                         "prepare", -1.0)
                  : -1.0;
  };
  // A file imported, as the asset of its name -- waited for a minute (a
  // busy Mac imports slower than import_and_wait waits).
  auto import = [&](const fs::path& f) -> std::optional<AssetId> {
    if (!ctl.import_files(*pid, {f}).ok()) {
      return std::nullopt;
    }
    std::optional<AssetId> got;
    eventually([&] {
      auto all = ctl.project(*pid)->assets();
      if (!all.ok()) {
        return false;
      }
      for (const auto& a : *all) {
        if (a.name == f.filename().string()) {
          got = a.id;
        }
      }
      return got.has_value();
    }, 60);
    return got;
  };
  using clock = std::chrono::steady_clock;
  auto timed = [&](auto&& f) {
    const auto t0 = clock::now();
    auto r = f();
    return std::make_pair(
        std::move(r),
        std::chrono::duration<double>(clock::now() - t0).count());
  };

  GenerateImageRequest req;
  req.project = *pid;
  req.model = "krea2-turbo";
  req.width = 512;
  req.height = 512;
  // A first picture of other words: the session's kernels tuned by it.
  req.prompt = "a lighthouse at dusk";
  req.seed = 1;
  REQUIRE(finished(ctl.generate_image(req)));
  since();
  req.prompt = "a red fox sitting in fresh snow, morning light";
  req.seed = 7;
  auto [a1, t1] = timed([&] { return finished(ctl.generate_image(req)); });
  REQUIRE(a1);
  CHECK(count(since(), "conditioning: made and kept") == 1);
  auto [a2, t2] = timed([&] { return finished(ctl.generate_image(req)); });
  REQUIRE(a2);
  CHECK(count(since(), "conditioning: kept from an earlier job, fed") == 1);
  auto a3 = finished(ctl.generate_image(req));
  REQUIRE(a3);
  since();
  const auto p1 = pixels(*a1), p2 = pixels(*a2), p3 = pixels(*a3);
  REQUIRE(!p1.empty());
  std::printf("  krea 2: made %.1f s (prepare %.2f), fed %.1f s (prepare "
              "%.2f); made-fed %d, fed-fed %d of 65535\n", t1, prepare(*a1),
              t2, prepare(*a2), most(p1, p2), most(p2, p3));
  CHECK(p1 == p2);
  CHECK(p2 == p3);
  req.seed = 8;
  auto a4 = finished(ctl.generate_image(req));
  REQUIRE(a4);
  CHECK(count(since(), "conditioning: kept from an earlier job, fed") == 1);
  CHECK(pixels(*a4) != p1);

  if (runs("qwen-image-2.1", "image", "edit")) {
    const auto png = root / "base.png";
    REQUIRE(write_png(png, 640, 480, {0x40, 0x80, 0xc0, 0xff}));
    auto base = import(png);
    REQUIRE(base);
    GenerateImageRequest ed;
    ed.project = *pid;
    ed.prompt = "paint a small white sailboat in the middle";
    ed.model = "qwen-image-2.1";
    ed.base = *base;
    ed.width = 512;
    ed.height = 384;
    ed.seed = 3;
    ed.steps = 8;
    // Tuned first, as above, on another picture.
    const auto other = root / "other.png";
    REQUIRE(write_png(other, 640, 480, {0xc0, 0x80, 0x40, 0xff}));
    auto warm = import(other);
    REQUIRE(warm);
    ed.base = *warm;
    ed.prompt = "paint a lighthouse on the left";
    REQUIRE(finished(ctl.generate_image(ed)));
    ed.base = *base;
    ed.prompt = "paint a small white sailboat in the middle";
    since();
    auto [e1, u1] = timed([&] { return finished(ctl.generate_image(ed)); });
    REQUIRE(e1);
    CHECK(count(since(), ": made and kept") == 2);  // conditioning, latents
    auto [e2, u2] = timed([&] { return finished(ctl.generate_image(ed)); });
    REQUIRE(e2);
    CHECK(count(since(), "kept from an earlier job, fed") == 2);
    auto e3 = finished(ctl.generate_image(ed));
    REQUIRE(e3);
    since();
    const auto q1 = pixels(*e1), q2 = pixels(*e2), q3 = pixels(*e3);
    REQUIRE(!q1.empty());
    std::printf("  qwen-image edit: made %.1f s (prepare %.2f), fed %.1f s "
                "(prepare %.2f); made-fed %d, fed-fed %d of 65535\n", u1,
                prepare(*e1), u2, prepare(*e2), most(q1, q2), most(q2, q3));
    CHECK(q1 == q2);
    CHECK(q2 == q3);
    // Other words: the latents fed, the conditioning made.
    ed.prompt = "paint a small red kite in the sky";
    REQUIRE(finished(ctl.generate_image(ed)));
    const std::string said = since();
    CHECK(count(said, "latents: kept from an earlier job, fed") == 1);
    CHECK(count(said, "conditioning: made and kept") == 1);
  } else {
    std::printf("  (Qwen-Image 2.1 is not installed: its edit not tried)\n");
  }

  if (std::string(want) == "video" &&
      runs("minimax-h3-fl2va", "video", "generate")) {
    GenerateVideoRequest vr;
    vr.project = *pid;
    vr.prompt = "a fox trotting through snow, birdsong";
    vr.model = "minimax-h3-fl2va";
    vr.width = 832;
    vr.height = 480;
    vr.frames = 39;
    vr.seed = 5;
    since();
    auto [v1, w1] = timed([&] { return finished(ctl.generate_video(vr)); });
    REQUIRE(v1);
    CHECK(count(since(), "conditioning: made and kept") == 1);
    auto [v2, w2] = timed([&] { return finished(ctl.generate_video(vr)); });
    REQUIRE(v2);
    CHECK(count(since(), "conditioning: kept from an earlier job, fed") ==
          1);
    std::printf("  h3 clip: made %.1f s (prepare %.2f), fed %.1f s (prepare "
                "%.2f)\n", w1, prepare(*v1), w2, prepare(*v2));
    // Opening on a picture: its latent made, then fed.
    const auto pic = root / "first.png";
    REQUIRE(write_png(pic, 832, 480, {0xe0, 0xe8, 0xf0, 0xff}));
    auto first = import(pic);
    REQUIRE(first);
    vr.first = *first;
    since();
    REQUIRE(finished(ctl.generate_video(vr)));
    CHECK(count(since(), "keyframe: made and kept") == 1);
    REQUIRE(finished(ctl.generate_video(vr)));
    std::string said = since();
    CHECK(count(said, "keyframe: kept from an earlier job, fed") == 1);
    CHECK(count(said, "conditioning: kept from an earlier job, fed") == 1);
  }
  ctl.shutdown();
  std::error_code ec;
  fs::remove_all(root, ec);
}

// A GATED model's download takes the person's Hugging Face token: it
// reaches the fetch in the job's params and nowhere else -- no event, no
// job record, no log line carries it -- and Hugging Face's refusal is
// said in words the person can act on.
TEST(controller, a_gated_download_takes_a_token_kept_nowhere)
{
  auto root = test::temp_dir("ctl-gated");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  const std::string token = "hf_secretTokenForTheTest";

  // The gated models are said to be, in Capabilities' tree.
  int gated = 0;
  for (const auto& f : jget(ctl.capability_tree(), "families",
                            Json::array())) {
    for (const auto& m : jget(f, "members", Json::array())) {
      const auto id = jget<std::string>(m, "model", "");
      if (jget(m, "gated", false)) {
        ++gated;
        CHECK(id == "krea2-turbo" || id == "flux2-klein-9b" ||
              id == "flux3-klein");
      }
    }
  }
  CHECK(gated == 3);

  // What everything the controller says carries: never the token.
  auto drain = [&] {
    std::string said;
    Event ev;
    while (ctl.events().wait(ev, 20)) {
      said += ev.to_json();
    }
    return said;
  };
  auto ask = [&](const char* id, const std::string& t) -> JobId {
    auto j = ctl.download_model(id, t);
    CHECK(j.ok());
    return j.ok() ? *j : JobId{};
  };
  JobId j = ask("krea2-turbo", token);
  const engine::JobSpec* s = engine->spec(j);
  REQUIRE(s != nullptr);
  CHECK(jget<std::string>(s->params, "hf_token", "") == token);
  std::string said = drain();
  engine->fail(j, "ModelFetchStage('model-fetch'): download of "
                  "'model.safetensors' failed: HTTP 401");
  Json failed;
  Event ev;
  while (ctl.events().wait(ev, 20)) {
    said += ev.to_json();
    if (ev.job == j && ev.kind == "job.failed") failed = ev.data;
  }
  CHECK(jget<std::string>(failed, "key", "") == "core.download_needs_token");
  CHECK(jget<std::string>(jget(failed, "args", Json::object()), "model",
                          "") == "Krea 2 Turbo");
  // The license not accepted: 403.
  JobId j2 = ask("flux2-klein-9b", token);
  engine->fail(j2, "xet grant for 'black-forest-labs/FLUX.2-klein-9B': "
                   "HTTP 403");
  failed = Json();
  while (ctl.events().wait(ev, 20)) {
    said += ev.to_json();
    if (ev.job == j2 && ev.kind == "job.failed") failed = ev.data;
  }
  CHECK(jget<std::string>(failed, "key", "") == "core.download_not_granted");
  // Not gated: vpipe's words as they are.
  JobId j3 = ask("z-image-turbo", "");
  CHECK(jget<std::string>(engine->spec(j3)->params, "hf_token", "x")
        == "x");
  engine->fail(j3, "download of 'x' failed: HTTP 401");
  failed = Json();
  while (ctl.events().wait(ev, 20)) {
    said += ev.to_json();
    if (ev.job == j3 && ev.kind == "job.failed") failed = ev.data;
  }
  CHECK(jget<std::string>(failed, "key", "") == "");
  CHECK(jget<std::string>(failed, "message", "").find("HTTP 401")
        != std::string::npos);

  CHECK(said.find(token) == std::string::npos);
  CHECK(to_text(ctl.log_since(0)).find(token) == std::string::npos);
  for (JobId id : {j, j2}) {
    auto rec = ctl.jobs().get(id);
    REQUIRE(rec);
    CHECK(rec->title.find(token) == std::string::npos);
    CHECK(rec->message.find(token) == std::string::npos);
  }
  // Nothing under the support folder holds it either.
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    std::ifstream in(e.path(), std::ios::binary);
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    CHECK(body.find(token) == std::string::npos);
  }
}

// A model and its QUANTIZED VARIANT are one model: the file run is the
// one Favor holds its weights at -- Fast and Med the 8-bit pack, Fine the
// bf16 -- and either when only it is installed.
TEST(controller, favor_picks_a_models_quantized_variant)
{
  auto root = test::temp_dir("ctl-variant");
  auto install = [&](const char* dir) {
    fs::create_directories(root / "models" / dir);
    std::ofstream(root / "models" / dir / "model.safetensors") << "w";
  };
  install("OpenMOSS-Team/MOSS-TTS-v1.5");
  install("local/MOSS-TTS-v1.5-8bit");
  install("OpenMOSS-Team/MOSS-Audio-Tokenizer");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.engine_factory = [] {
    return std::unique_ptr<engine::Engine>(std::make_unique<HeldEngine>());
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "V.valtz", "variants");
  REQUIRE_OK(pid);
  auto run = [&](const char* preference) -> std::string {
    GenerateAudioRequest req;
    req.project = *pid;
    req.prompt = "Good evening.";
    req.model = "moss-tts-v1.5";
    req.preference = preference;
    auto job = ctl.generate_audio(req);
    if (!job.ok()) {
      return "refused: " + job.error().message;
    }
    auto rec = ctl.jobs().get(*job);
    return rec ? rec->recipe.model : "";
  };
  CHECK(run("speed") == "moss-tts-v1.5-w8g64");
  CHECK(run("balanced") == "moss-tts-v1.5-w8g64");
  CHECK(run("quality") == "moss-tts-v1.5");
  // Only the pack: Fine runs it too.
  fs::remove_all(root / "models" / "OpenMOSS-Team" / "MOSS-TTS-v1.5");
  ctl.rescan_models();
  CHECK(run("quality") == "moss-tts-v1.5-w8g64");
}

// DRAMA 3's voices: each cloned from its sound AND its words. A voice
// with no transcript is transcribed first -- Transcribe's own asset, in
// the same command -- and the speech HELD, listed but not sent, until it
// lands; then sent with what the voice says and where its clip is cut. A
// transcript made already is used; words given need none. A held speech
// is cancelled alone, and fails when what it waits for does.
TEST(controller, drama3_speaks_in_voices_it_has_heard)
{
  auto root = test::temp_dir("ctl-drama3");
  auto install = [&](const fs::path& dir, const char* file) {
    fs::create_directories(root / "models" / dir);
    std::ofstream(root / "models" / dir / file) << "w";
  };
  install("fishaudio/drama-3-int4", "model.safetensors");
  install("mlx-community/Qwen3-ASR-0.6B-8bit", "model.safetensors");
  install("tgo-app-dev/vpipe-supplement", "silero-vad-unified-v6.tar");
  fs::create_directories(root / "models" / "tgo-app-dev" /
                         "vpipe-supplement" / "silero_vad_unified_v6" /
                         "silero.mlpackage");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "D.valtz", "drama");
  REQUIRE_OK(pid);
  REQUIRE(write_tone(root / "anna.wav", 6, 220, 0.3));
  REQUIRE(write_tone(root / "ben.wav", 6, 330, 0.3));
  auto anna = import_and_wait(ctl, *pid, root / "anna.wav");
  auto ben = import_and_wait(ctl, *pid, root / "ben.wav");
  REQUIRE(anna && ben);

  // A scene in two voices, neither heard yet: both transcribed first.
  GenerateAudioRequest req;
  req.project = *pid;
  req.model = "drama-3-int4";
  req.prompt = "Anna: [warm] Good morning.\nBen: [sleepy] Is it?";
  req.row = {*anna, *ben};
  req.seed = 5;
  auto job = ctl.generate_audio(req);
  REQUIRE_OK(job);
  const auto heard = engine->held_of(engine::kOpTranscribeAudio);
  REQUIRE(heard.size() == 2);
  CHECK(engine->held_of(engine::kOpGenerateSpeech).empty());
  auto rec = ctl.jobs().get(*job);
  REQUIRE(rec);
  CHECK(rec->state == JobState::Queued);
  CHECK(jget<std::string>(rec->recipe.params, "text", "") ==
        "<|speaker:0|>[warm] Good morning.\n<|speaker:1|>[sleepy] Is it?");
  CHECK(rec->recipe.params["speakers"] == Json::array({"Anna", "Ben"}));
  Json task;
  for (const auto& t : ctl.tasks()) {
    if (jget<std::string>(t, "job", "") == job->str()) {
      task = t;
    }
  }
  REQUIRE(task.is_object());
  CHECK(task["waiting_for"] ==
        Json::array({"anna.wav transcript", "ben.wav transcript"}));

  // Each heard: the speech goes once both have landed.
  auto transcript = [&](const char* words, double end) {
    assist::Transcript t;
    t.seconds = 6;
    t.lines = {{0.3, end, words, "English"}};
    return Json{{"transcript", assist::to_json(t)}};
  };
  std::ofstream(root / "t.txt") << "# transcript\n";
  engine->finish(heard[0], root / "t.txt",
                 transcript("We asked over twenty people.", 3.9));
  CHECK(engine->held_of(engine::kOpGenerateSpeech).empty());
  engine->finish(heard[1], root / "t.txt",
                 transcript("Nobody saw it.", 2.5));
  const auto spoken = engine->held_of(engine::kOpGenerateSpeech);
  REQUIRE(spoken.size() == 1);
  CHECK(spoken[0] == *job);
  const engine::JobSpec* spec = engine->spec(*job);
  REQUIRE(spec);
  const Json vs = jget(spec->params, "voices", Json::array());
  REQUIRE(vs.size() == 2);
  CHECK(jget<std::string>(vs[0], "text", "") ==
        "We asked over twenty people.");
  CHECK(std::abs(jget(vs[0], "seconds", 0.0) - 4.2) < 1e-6);
  CHECK(jget<std::string>(vs[1], "text", "") == "Nobody saw it.");
  int voices = 0;
  for (const auto& in : spec->inputs) {
    voices += in.role == "voice";
  }
  CHECK(voices == 2);

  // Heard already: sent at once, nothing transcribed again.
  req.prompt = "A single line, in Anna's voice.";
  req.row = {*anna};
  auto again = ctl.generate_audio(req);
  REQUIRE_OK(again);
  CHECK(engine->held_of(engine::kOpTranscribeAudio).empty());
  CHECK(engine->held_of(engine::kOpGenerateSpeech).size() == 2);
  CHECK(jget<std::string>(
            jget(engine->spec(*again)->params, "voices", Json::array())[0],
            "text", "") == "We asked over twenty people.");

  // Words given: no transcript.
  REQUIRE(write_tone(root / "cara.wav", 4, 440, 0.3));
  auto cara = import_and_wait(ctl, *pid, root / "cara.wav");
  REQUIRE(cara);
  req.row = {*cara};
  req.voice_texts = {"Hello there."};
  auto given = ctl.generate_audio(req);
  REQUIRE_OK(given);
  CHECK(engine->held_of(engine::kOpTranscribeAudio).empty());
  CHECK(jget<std::string>(
            jget(engine->spec(*given)->params, "voices", Json::array())[0],
            "text", "") == "Hello there.");
  req.voice_texts.clear();

  // Held, then cancelled: it alone goes; its voice is still heard.
  auto held = ctl.generate_audio(req);
  REQUIRE_OK(held);
  const auto cara_heard = engine->held_of(engine::kOpTranscribeAudio);
  REQUIRE(cara_heard.size() == 1);
  REQUIRE_OK(ctl.cancel(*held));
  CHECK(ctl.jobs().get(*held)->state == JobState::Cancelled);
  CHECK(engine->held_of(engine::kOpTranscribeAudio).size() == 1);
  // ...and another held for it fails when its transcription does.
  auto failing = ctl.generate_audio(req);
  REQUIRE_OK(failing);
  CHECK(engine->held_of(engine::kOpTranscribeAudio).size() == 1);
  engine->fail(cara_heard[0], "could not hear it");
  const auto f = ctl.jobs().get(*failing);
  REQUIRE(f);
  CHECK(f->state == JobState::Failed);
  CHECK(f->message.find("was not made") != std::string::npos);

  // Nothing said in a voice: refused when it lands.
  REQUIRE(write_tone(root / "dee.wav", 4, 550, 0.3));
  auto dee = import_and_wait(ctl, *pid, root / "dee.wav");
  REQUIRE(dee);
  req.row = {*dee};
  auto silent = ctl.generate_audio(req);
  REQUIRE_OK(silent);
  const auto dee_heard = engine->held_of(engine::kOpTranscribeAudio);
  REQUIRE(dee_heard.size() == 1);
  assist::Transcript none;
  none.seconds = 4;
  engine->finish(dee_heard[0], root / "t.txt",
                 {{"transcript", assist::to_json(none)}});
  const auto s = ctl.jobs().get(*silent);
  REQUIRE(s);
  CHECK(s->state == JobState::Failed);
  CHECK(s->message.find("No speech was heard in dee.wav") != std::string::npos);
}

// INSTANTIATION puts an asset on a NEW LAYER that shows it -- not its
// layers copied in: in a blank selected layer, else right above it; a
// composition shown so is drawn whole; never itself or what shows it.
TEST(composition, instantiate_is_a_new_layer)
{
  Bench b("comp-instantiate");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 16, 16, {0, 0, 0xff, 0xff});
  REQUIRE(red && blue);
  // Nothing yet: the project is made of it.
  auto first = ctl.instantiate(pid, *red, std::nullopt);
  REQUIRE_OK(first);
  const AssetId view = first->first;
  CHECK(ctl.project_composition(pid) == view);
  CHECK(first->second.empty());
  // On top, by default.
  auto put = ctl.instantiate(pid, *blue, std::nullopt);
  REQUIRE_OK(put);
  CHECK(put->first == view && put->second == "1");
  REQUIRE(b.layers(view).size() == 2);
  CHECK(b.layers(view)[1].source == *blue);
  // A blank layer takes it in place; any other has it right above.
  auto blank = ctl.add_layer(pid, view, "");
  REQUIRE_OK(blank);
  auto filled = ctl.instantiate(pid, *red, view, *blank);
  REQUIRE_OK(filled);
  CHECK(filled->second == *blank);
  CHECK(b.layers(view)[1].id == *blank && b.layers(view)[1].source == *red);
  auto above = ctl.instantiate(pid, *blue, view, std::string());
  REQUIRE_OK(above);
  CHECK(b.layers(view)[1].id == above->second);
  CHECK(b.layers(view)[2].id == *blank);

  // A composition on a layer: drawn whole, its look and all.
  auto s = ctl.derive_modified(pid, *blue, "dim blue");
  REQUIRE_OK(s);
  media::Adjustments dim;
  dim.exposure = -2;
  REQUIRE_OK(ctl.set_adjustments(pid, *s, dim));
  auto top = ctl.instantiate(pid, *s, view);
  REQUIRE_OK(top);
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.flatten(pid, view, flat));
  const auto px = pixel_at(flat, 8, 8);
  CHECK(px[2] > 40 && px[2] < 170 && px[0] < 40);   // blue, dimmed
  // Never into itself, nor into what it shows.
  CHECK(!ctl.instantiate(pid, view, view).ok());
  CHECK(!ctl.instantiate(pid, view, *s).ok());
  // Text is not shown on a layer.
  auto words = ctl.capture_prompt(pid, "a red fox", {}, {});
  REQUIRE_OK(words);
  CHECK(!ctl.instantiate(pid, *words, view).ok());
}

// A CLIP put in a BLANK layer of a timeline -- dropped on the stage, or
// on the layer's row -- is laid after what is there, and a length set
// shorter grows to hold it; a picture, or a layer that is not blank,
// goes where it is put.
TEST(composition, a_clip_on_a_blank_layer_follows_the_content)
{
  Bench b("comp-sequence");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  // A shot: a timeline of one second (a composition that is a clip).
  auto shot = ctl.create_composition(pid, project::AssetClass::Composition,
                                     {16, 16}, {24, 1}, "shot");
  REQUIRE_OK(shot);
  auto in_shot = ctl.instantiate(pid, *red, *shot);
  REQUIRE_OK(in_shot);
  project::LayerTime second;
  second.duration = 24;
  REQUIRE_OK(ctl.set_layer_time(pid, *shot, in_shot->second, second));
  CHECK(b.p().asset(*shot)->kind == project::AssetKind::Video);
  auto cut = ctl.create_composition(pid, project::AssetClass::Composition,
                                    {16, 16}, {24, 1}, "cut");
  REQUIRE_OK(cut);
  auto first = ctl.instantiate(pid, *shot, *cut);
  REQUIRE_OK(first);
  REQUIRE_OK(ctl.set_timeline(pid, *cut, 30));
  // Into a blank layer: from the content's end (24), not the playhead
  // (5); the length set (30) grows to 48.
  auto blank = ctl.add_layer(pid, *cut, first->second);
  REQUIRE_OK(blank);
  auto next = ctl.instantiate(pid, *shot, *cut, *blank, 5);
  REQUIRE_OK(next);
  CHECK(next->second == *blank);
  CHECK(b.layers(*cut)[1].time.offset == 24);
  CHECK(b.p().asset(*cut)->timeline_frames == 48);
  auto len = ctl.composition_length(pid, *cut);
  REQUIRE_OK(len);
  CHECK(*len == 48);
  // Undone as one: the layer blank again, the length 30.
  REQUIRE_OK(ctl.undo(pid));
  CHECK(!b.layers(*cut)[1].source);
  CHECK(b.p().asset(*cut)->timeline_frames == 30);
  REQUIRE_OK(ctl.redo(pid));
  CHECK(b.layers(*cut)[1].time.offset == 24);
  // Dropped on a blank layer's row: the same.
  auto row = ctl.add_layer(pid, *cut, *blank);
  REQUIRE_OK(row);
  REQUIRE_OK(ctl.set_layer_source(pid, *cut, *row, *shot));
  CHECK(b.layers(*cut)[2].time.offset == 48);
  CHECK(b.p().asset(*cut)->timeline_frames == 72);
  // A layer that is not blank: a new one right above, where it was put.
  auto over = ctl.instantiate(pid, *shot, *cut, *row, 7);
  REQUIRE_OK(over);
  CHECK(b.layers(*cut)[3].id == over->second);
  CHECK(b.layers(*cut)[3].time.offset == 7);
  // A picture in a blank layer: where it was put.
  auto pic = ctl.add_layer(pid, *cut, over->second);
  REQUIRE_OK(pic);
  REQUIRE_OK(ctl.instantiate(pid, *red, *cut, *pic, 5));
  CHECK(b.layers(*cut)[4].time.offset == 5);
  CHECK(b.p().asset(*cut)->timeline_frames == 72);
}

// A RESULT LANDS (DESIGN §3a) as a new layer of the composition active
// when it was asked for, at its own size -- a smaller picture centred on
// a larger canvas, the canvas kept -- or, with none or one that cannot
// show it, in a composition of its own (the project's when there is
// none). One command.
TEST(composition, a_result_lands_in_a_new_layer_or_its_own)
{
  Bench b("comp-land");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 32, 32, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 16, 16, {0, 0, 0xff, 0xff});
  REQUIRE(red && blue);
  // Nothing active: its own still, at its size -- the project's.
  auto own = ctl.place_result(pid, *red, std::nullopt);
  REQUIRE_OK(own);
  CHECK(ctl.project_composition(pid) == own->first);
  auto still = b.p().asset(own->first);
  REQUIRE_OK(still);
  CHECK(still->cls == project::AssetClass::Still);
  CHECK(still->canvas.frame_w == 32 && still->canvas.frame_h == 32);
  // Onto it: a new layer on top, the frame as it was.
  auto put = ctl.place_result(pid, *blue, own->first);
  REQUIRE_OK(put);
  CHECK(put->first == own->first && !put->second.empty());
  REQUIRE(b.layers(own->first).size() == 2);
  CHECK(b.layers(own->first)[0].source == *red);
  CHECK(b.layers(own->first)[1].source == *blue);
  CHECK(b.p().asset(own->first)->canvas.frame_w == 32);
  // Centred at its own size: blue in the middle, red at the edge.
  const auto flat = b.root / "land.png";
  REQUIRE_OK(ctl.flatten(pid, own->first, flat));
  CHECK(pixel_at(flat, 16, 16)[2] > 200);
  CHECK(pixel_at(flat, 2, 2)[0] > 200);
  // A blank selected layer takes it in place.
  auto blank = ctl.add_layer(pid, own->first, put->second);
  REQUIRE_OK(blank);
  auto filled = ctl.place_result(pid, *red, own->first, *blank);
  REQUIRE_OK(filled);
  CHECK(filled->second == *blank);
  // One command: undone, the layer is blank again.
  REQUIRE_OK(ctl.undo(pid));
  CHECK(!b.layers(own->first)[2].source);
  // A sound cannot go into a still: its own composition of sound alone,
  // not the project's.
  REQUIRE(write_tone(b.root / "tone.wav", 1, 440, 0.5));
  auto tone = import_and_wait(ctl, pid, b.root / "tone.wav");
  REQUIRE(tone);
  auto sound = ctl.place_result(pid, *tone, own->first);
  REQUIRE_OK(sound);
  CHECK(sound->first != own->first);
  CHECK(b.p().asset(sound->first)->kind == project::AssetKind::Audio);
  CHECK(ctl.project_composition(pid) == own->first);
  // Named on from a number of its own: "x (2)" taken makes "x (3)".
  REQUIRE_OK(ctl.rename_asset(pid, *red, "kite (2)"));
  auto again = ctl.place_result(pid, *red, std::nullopt);
  REQUIRE_OK(again);
  CHECK(b.p().asset(again->first)->name == "kite (3)");
}

// How a project was last looked at -- the app's window size -- is saved
// with it and read back at the next open; never a command, and setting
// what is there already changes nothing.
TEST(controller, a_projects_view_state_is_saved_with_it)
{
  auto root = test::temp_dir("ctl-view");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  const auto pkg = root / "W.valtz";
  auto pid = ctl.create_project(pkg, "W");
  REQUIRE_OK(pid);
  REQUIRE_OK(ctl.save_project(*pid));
  auto empty = ctl.view_state(*pid);
  REQUIRE_OK(empty);
  CHECK(empty->empty());
  const Json v = {{"window", {{"width", 1024}, {"height", 768}}}};
  REQUIRE_OK(ctl.set_view_state(*pid, v));
  CHECK(jget(ctl.project_state(*pid), "dirty", false));
  CHECK(jget(ctl.project_state(*pid), "undo", Json()).is_null());
  REQUIRE_OK(ctl.save_project(*pid));
  REQUIRE_OK(ctl.set_view_state(*pid, v));
  CHECK(!jget(ctl.project_state(*pid), "dirty", true));
  REQUIRE_OK(ctl.close_project(*pid));
  auto again = ctl.open_project(pkg);
  REQUIRE_OK(again);
  auto back = ctl.view_state(*again);
  REQUIRE_OK(back);
  CHECK(*back == v);
}

// The project's OUTPUT (DESIGN §6b): set up with it -- a timeline at its
// frame rate, which the timeline keeps (a clip at another resampled to
// it) and which changes only while the timeline is empty; a first result
// that makes the project takes it too. Exported, its sound and pictures
// are made its own: channels, rate, colour.
TEST(composition, the_project_output_is_set_up_and_kept)
{
  Bench b("comp-output");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto d = ctl.project_output(pid);
  REQUIRE_OK(d);
  CHECK(d->color == "rec709" && d->fps == Rational(24, 1));
  CHECK(d->channels == 2 && d->sample_rate == 48000);
  // Its clips kept as ProRes 422 HQ -- a project from before the setting
  // too -- or as set; an unknown format refused by name.
  CHECK(d->intermediate == "prores422hq");
  CHECK(Json::object().get<project::OutputSettings>().intermediate ==
        "prores422hq");
  project::OutputSettings kept = *d;
  kept.intermediate = "prores4444";
  REQUIRE_OK(ctl.set_project_output(pid, kept));
  CHECK(ctl.project_output(pid)->intermediate == "prores4444");
  CHECK(Json(kept).get<project::OutputSettings>() == kept);
  kept.intermediate = "dnxhr";
  auto unknown = ctl.set_project_output(pid, kept);
  REQUIRE(!unknown.ok());
  CHECK(unknown.error().key == msg::kIntermediateUnknown.key);
  REQUIRE_OK(ctl.undo(pid));
  CHECK(ctl.project_output(pid)->intermediate == "prores422hq");
  project::OutputSettings o;
  o.color = "rec2020";
  o.fps = Rational(30, 1);
  o.channels = 1;
  o.sample_rate = 44100;
  auto view = ctl.set_up_project(pid, project::AssetClass::Composition,
                                 {64, 36}, o);
  REQUIRE_OK(view);
  CHECK(ctl.project_composition(pid) == *view);
  CHECK(b.p().asset(*view)->rate == Rational(30, 1));
  CHECK(*ctl.project_output(pid) == o);
  // Empty, its rate follows the output; with a layer showing something,
  // it stays.
  o.fps = Rational(25, 1);
  REQUIRE_OK(ctl.set_project_output(pid, o));
  CHECK(b.p().asset(*view)->rate == Rational(25, 1));
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  REQUIRE_OK(ctl.instantiate(pid, *red, *view));
  o.fps = Rational(60, 1);
  auto refused = ctl.set_project_output(pid, o);
  REQUIRE(!refused.ok());
  CHECK(refused.error().key == msg::kOutputRateSet.key);
  o.fps = Rational(25, 1);
  o.color = "no-such";
  CHECK(!ctl.set_project_output(pid, o).ok());
  // Undone, the output as it was.
  o.color = "srgb";
  REQUIRE_OK(ctl.set_project_output(pid, o));
  REQUIRE_OK(ctl.undo(pid));
  CHECK(ctl.project_output(pid)->color == "rec2020");

  // A first result making the project: at the project's rate.
  Bench b2("comp-output-claim");
  REQUIRE(b2.ok());
  project::OutputSettings o2;
  o2.fps = Rational(30, 1);
  REQUIRE_OK(b2.ctl->set_project_output(b2.pid, o2));
  auto shot = b2.ctl->create_composition(
      b2.pid, project::AssetClass::Composition, {16, 16}, {24, 1}, "shot",
      false, false);
  REQUIRE_OK(shot);
  auto made = b2.ctl->place_result(b2.pid, *shot, std::nullopt);
  REQUIRE_OK(made);
  CHECK(b2.ctl->project_composition(b2.pid) == made->first);
  CHECK(b2.p().asset(made->first)->rate == Rational(30, 1));
}

// A sound made a project's: mono at 44.1 kHz from 48 kHz stereo; a
// picture in another output colour, tagged so.
TEST(media, sound_and_pictures_are_made_an_outputs_own)
{
  const auto root = test::temp_dir("output-media");
  REQUIRE(write_tone(root / "tone.wav", 1, 440, 0.5));
  REQUIRE_OK(media::convert_sound(root / "tone.wav", root / "mono.wav", 1,
                                  44100));
  auto s = media::probe_file(root / "mono.wav");
  REQUIRE_OK(s);
  CHECK(s->audio_channels == 1);
  CHECK(s->audio_sample_rate == 44100);
  CHECK(std::abs(s->duration.seconds() - 1.0) < 0.05);
  REQUIRE(write_png(root / "red.png", 16, 16, {0xff, 0, 0, 0xff}));
  const auto c = media::output_color("rec2020");
  REQUIRE(c.has_value());
  REQUIRE_OK(media::convert_picture(root / "red.png", root / "red-2020.png",
                                    *c));
  auto pic = media::probe_file(root / "red-2020.png");
  REQUIRE_OK(pic);
  CHECK(pic->frame.width == 16);
  CHECK(pic->frame.color.is_wide_gamut());
  CHECK(!media::output_color("no-such").has_value());
}

// A TEXT BOX (media/markup.h): its words wrapped within its width, and
// only the lines that fit whole in its height drawn -- cut at a line,
// never through one; nothing outside the box.
TEST(media, a_text_box_wraps_and_cuts_at_a_line)
{
  const auto root = test::temp_dir("markup-box");
  const Json box = {
    {"id", "t"}, {"kind", "text"}, {"box", true},
    {"x0", 20}, {"y0", 20}, {"x1", 220}, {"y1", 20 + 36 * 1.5},
    {"stroke", {1, 0, 0, 1}},
    {"text", "Wrapped words wrapped words wrapped words wrapped words "
             "wrapped words wrapped words"},
    {"font", {{"family", "Helvetica"}, {"size", 30}}}};
  const auto out = root / "box.png";
  REQUIRE_OK(media::render_markup({}, {400, 200}, Json::array({box}), {},
                                  out));
  // Any ink in a band of rows, or columns.
  auto ink = [&](int x0, int x1, int y0, int y1) {
    for (int y = y0; y < y1; y += 2) {
      for (int x = x0; x < x1; x += 2) {
        if (pixel_at(out, x, y)[3] > 0) {
          return true;
        }
      }
    }
    return false;
  };
  CHECK(ink(20, 220, 20, 50));     // the first line, drawn
  CHECK(!ink(20, 220, 58, 120));   // the second: no room for all of it
  CHECK(!ink(224, 400, 0, 200));   // wrapped: nothing past the box
  CHECK(!ink(0, 400, 120, 200));
}

// A text box measured LINE BY LINE (text_fit's all_lines): a box as tall
// as the height told for k lines sets exactly those k lines -- the bytes
// told -- whatever its line and paragraph spacing; a pixel less, one
// fewer. Its first line stands at the box's top however far apart its
// lines are (the spacing lies between them).
TEST(media, a_text_is_measured_line_by_line)
{
  Json box = {
    {"id", "t"}, {"kind", "text"}, {"box", true},
    {"x0", 0}, {"y0", 0}, {"x1", 260}, {"y1", 1},
    {"stroke", {0, 0, 0, 1}},
    {"text", "Wrapped words wrapped words wrapped words wrapped words "
             "wrapped words wrapped words\nA second paragraph of words "
             "wrapped in the box."},
    {"font", {{"family", "Georgia"}, {"size", 24}}},
    {"align", "justified"}, {"line_spacing", 1.6},
    {"paragraph_spacing", 14}};
  const auto all = media::text_fit(box, true);
  REQUIRE(all.line_ends.size() >= 4);
  REQUIRE(all.line_heights.size() == all.line_ends.size());
  CHECK(all.line_ends.back() ==
        jget<std::string>(box, "text", "").size());
  for (std::size_t k = 1; k < all.line_heights.size(); ++k) {
    CHECK(all.line_heights[k] > all.line_heights[k - 1]);
    Json b = box;
    b["y1"] = all.line_heights[k - 1];
    const auto f = media::text_fit(b);
    CHECK(f.lines == static_cast<int>(k));
    CHECK(f.shown == all.line_ends[k - 1]);
    b["y1"] = all.line_heights[k - 1] - 1;
    CHECK(media::text_fit(b).lines == static_cast<int>(k) - 1);
  }
  // Wider apart, the first line where it was: the height of one line the
  // same, of two more.
  Json tight = box;
  tight["line_spacing"] = 1.0;
  const auto t = media::text_fit(tight, true);
  CHECK(std::abs(t.line_heights[0] - all.line_heights[0]) <= 1);
  CHECK(all.line_heights[1] - t.line_heights[1] > 24 * 0.5);
  // Kept by normalizing; an unknown alignment is left-set.
  Json odd = box;
  odd["align"] = "sideways";
  const Json n = media::normalize_objects(Json::array({box, odd}));
  CHECK(jget<std::string>(n[0], "align", "") == "justified");
  CHECK(jget(n[0], "line_spacing", 0.0) == 1.6);
  CHECK(jget(n[0], "paragraph_spacing", 0.0) == 14);
  CHECK(jget<std::string>(n[1], "align", "") == "left");
}

// EFFECTS on a markup object (media/markup.h): a shadow where it is put
// -- `dy` DOWN on the canvas, whatever the bitmap's own y -- as sharp or
// as blurred as asked; a glow around a shape; the object drawn ONCE (a
// translucent fill not darkened by its passes). Kept by normalizing, a
// kind not known kept as it is.
TEST(media, a_markup_object_casts_a_shadow_and_glows)
{
  const auto root = test::temp_dir("markup-fx");
  const Json shadowed = {
    {"id", "s"}, {"kind", "rect"},
    {"x0", 100}, {"y0", 100}, {"x1", 200}, {"y1", 200},
    {"fill", {1, 0, 0, 0.5}}, {"width", 0},
    {"effects", Json::array({{{"kind", "shadow"}, {"color", {0, 0, 1, 1}},
                               {"blur", 0}, {"dx", 150}, {"dy", 120}}})}};
  const Json glowing = {
    {"id", "g"}, {"kind", "ellipse"},
    {"x0", 300}, {"y0", 60}, {"x1", 360}, {"y1", 120},
    {"fill", {1, 1, 1, 1}}, {"width", 0},
    {"effects", Json::array({{{"kind", "glow"}, {"color", {0, 1, 0, 1}},
                               {"radius", 12}}})}};
  const auto out = root / "fx.png";
  REQUIRE_OK(media::render_markup({}, {420, 340}, Json::array({shadowed,
                                                                glowing}),
                                  {}, out));
  // The fill, translucent: drawn once (half its alpha, not three
  // quarters). Its shadow: 150 right, 120 down -- blue, sharp.
  const auto in = pixel_at(out, 150, 150);
  CHECK(in[3] >= 120 && in[3] <= 136);
  // (The shadow of a half-clear fill is half clear.)
  const auto sh = pixel_at(out, 300, 270);
  CHECK(sh[2] >= 120 && sh[3] >= 120 && sh[3] <= 136 && sh[0] < 10);
  CHECK(pixel_at(out, 300, 205)[3] == 0);  // above where it lies
  CHECK(pixel_at(out, 60, 60)[3] == 0);
  // The glow: green just outside the ellipse, fading, none far off.
  const auto near = pixel_at(out, 330, 126);
  CHECK(near[1] > 60 && near[3] > 60 && near[0] < 10);
  CHECK(pixel_at(out, 330, 170)[3] == 0);
  CHECK(pixel_at(out, 330, 90)[0] > 240);  // the white inside
  // Kept: ranges held, an unknown kind as it is.
  Json odd = glowing;
  odd["effects"] = Json::array({{{"kind", "glow"}, {"radius", -5}},
                                {{"kind", "bevel"}, {"depth", 3}}});
  const Json n = media::normalize_objects(Json::array({shadowed, odd}));
  REQUIRE(n[0]["effects"].size() == 1);
  CHECK(jget(n[0]["effects"][0], "dy", 0.0) == 120);
  REQUIRE(n[1]["effects"].size() == 2);
  CHECK(jget(n[1]["effects"][0], "radius", -1.0) == 0);
  CHECK(jget(n[1]["effects"][1], "depth", 0) == 3);
}

// A POLYGON (its corners in its box's units: the box moved, they move)
// and GRADIENTS (media/markup.h): a linear one red to blue across a
// rectangle, carried on past its ends; a radial one white at its centre
// to black at its edge; a text box's words in a gradient, nothing
// between them. Kept by normalizing, stops sorted; one stop is none.
TEST(media, a_polygon_and_gradients_are_drawn)
{
  const auto root = test::temp_dir("markup-grad");
  const Json tri = {
    {"id", "p"}, {"kind", "polygon"},
    {"x0", 20}, {"y0", 20}, {"x1", 120}, {"y1", 120},
    {"points", {{0.5, 0}, {1, 1}, {0, 1}}},
    {"fill", {0, 1, 0, 1}}, {"stroke", {0, 0, 0, 1}}, {"width", 0}};
  const Json lin = {
    {"id", "l"}, {"kind", "rect"},
    {"x0", 150}, {"y0", 20}, {"x1", 350}, {"y1", 70}, {"width", 0},
    {"fill", {0, 0, 0, 0}},
    {"gradient", {{"kind", "linear"},
                  {"stops", {{{"at", 1}, {"color", {0, 0, 1, 1}}},
                             {{"at", 0}, {"color", {1, 0, 0, 1}}}}},
                  {"from", {0.25, 0.5}}, {"to", {0.75, 0.5}}}}};
  const Json rad = {
    {"id", "r"}, {"kind", "ellipse"},
    {"x0", 150}, {"y0", 100}, {"x1", 250}, {"y1", 200}, {"width", 0},
    {"gradient", {{"kind", "radial"},
                  {"stops", {{{"at", 0}, {"color", {1, 1, 1, 1}}},
                             {{"at", 1}, {"color", {0, 0, 0, 1}}}}},
                  {"from", {0.5, 0.5}}, {"to", {1, 0.5}}}}};
  const Json words = {
    {"id", "t"}, {"kind", "text"}, {"box", true},
    {"x0", 20}, {"y0", 220}, {"x1", 380}, {"y1", 300},
    {"text", "MMMM"}, {"stroke", {0, 0, 0, 1}},
    {"font", {{"family", "Helvetica"}, {"size", 64}, {"bold", true}}},
    {"gradient", {{"kind", "linear"},
                  {"stops", {{{"at", 0}, {"color", {1, 0, 0, 1}}},
                             {{"at", 1}, {"color", {0, 0, 1, 1}}}}}}}};
  const auto out = root / "grad.png";
  REQUIRE_OK(media::render_markup({}, {400, 320},
                                  Json::array({tri, lin, rad, words}), {},
                                  out));
  // The triangle: inside green, outside its edges clear.
  CHECK(pixel_at(out, 70, 100)[1] > 240);
  CHECK(pixel_at(out, 30, 30)[3] == 0);
  CHECK(pixel_at(out, 110, 30)[3] == 0);
  // Linear: red up to a quarter in, blue from three quarters, between a
  // blend; carried on past both ends.
  const auto l0 = pixel_at(out, 160, 45), l1 = pixel_at(out, 340, 45);
  const auto lm = pixel_at(out, 250, 45);
  CHECK(l0[0] > 240 && l0[2] < 15);
  CHECK(l1[2] > 240 && l1[0] < 15);
  CHECK(lm[0] > 90 && lm[0] < 170 && lm[2] > 90 && lm[2] < 170);
  // Radial: white at the centre, darker toward the edge, clipped to the
  // ellipse.
  CHECK(pixel_at(out, 200, 150)[0] > 240);
  CHECK(pixel_at(out, 240, 150)[0] < 60);
  CHECK(pixel_at(out, 152, 102)[3] == 0);
  // The words: red at their left, blue at their right; none between the
  // lines' bottom and the box's.
  int reds = 0, blues = 0;
  for (int x = 20; x < 380; ++x) {
    for (int y = 225; y < 290; y += 2) {
      const auto px = pixel_at(out, x, y);
      if (px[3] > 200 && x < 80 && px[0] > 180 && px[2] < 80) {
        ++reds;
      }
      if (px[3] > 200 && x > 150 && x < 280 && px[2] > px[0]) {
        ++blues;
      }
    }
  }
  CHECK(reds > 20);
  CHECK(blues > 20);
  // Kept: the stops sorted; a polygon's points; one stop: no gradient.
  Json one = lin;
  one["gradient"]["stops"] = Json::array({{{"at", 0.5},
                                           {"color", {1, 0, 0, 1}}}});
  const Json n = media::normalize_objects(Json::array({tri, lin, one}));
  REQUIRE(n.size() == 3);
  CHECK(n[0]["points"].size() == 3);
  CHECK(jget(n[1]["gradient"]["stops"][0], "at", -1.0) == 0);
  CHECK(jget<std::string>(n[1]["gradient"], "kind", "") == "linear");
  CHECK(!n[2].contains("gradient"));
}

// A PATH (media/markup.h, 2026-10-15): four smooth points round a square
// are a circle -- filled where the diamond of their corners is not, clear
// past the circle; three round a circle bulge past their own box, which
// what it covers says; an OPEN one is a line -- its stroke drawn, no fill
// whatever it has. Kept by normalizing: the smooth points by their place,
// a path's openness; a polygon of corners says neither.
TEST(media, a_path_curves_through_its_points)
{
  const auto root = test::temp_dir("markup-path");
  const Json circle = {
    {"id", "c"}, {"kind", "polygon"},
    {"x0", 100}, {"y0", 100}, {"x1", 300}, {"y1", 300},
    {"points", {{0.5, 0}, {1, 0.5}, {0.5, 1}, {0, 0.5}}},
    {"smooth", {true, true, true, true}},
    {"fill", {0, 1, 0, 1}}, {"width", 0}};
  const Json open = {
    {"id", "o"}, {"kind", "polygon"},
    {"x0", 20}, {"y0", 330}, {"x1", 220}, {"y1", 380},
    {"points", {{0, 1}, {0.5, 0}, {1, 1}}},
    {"smooth", {false, true, false}}, {"open", true},
    {"fill", {1, 0, 0, 1}}, {"stroke", {0, 0, 1, 1}}, {"width", 6}};
  const auto out = root / "path.png";
  REQUIRE_OK(media::render_markup({}, {400, 400},
                                  Json::array({circle, open}), {}, out));
  // Inside the circle, outside the diamond (265 - 135 > 100); past the
  // circle (106 from its centre), clear.
  CHECK(pixel_at(out, 265, 135)[1] > 240);
  CHECK(pixel_at(out, 135, 265)[1] > 240);
  CHECK(pixel_at(out, 277, 123)[3] == 0);
  CHECK(pixel_at(out, 200, 200)[1] > 240);
  // The open curve: its stroke through its middle point, nothing filled
  // under it.
  const auto top = pixel_at(out, 120, 330);
  CHECK(top[2] > 200 && top[3] > 200);
  CHECK(pixel_at(out, 120, 370)[3] == 0);
  // Three smooth points evenly round a circle: their box ends at the
  // lower two (y 250), the curve at the circle's foot (300).
  const Json three = {
    {"id", "t"}, {"kind", "polygon"},
    {"x0", 113.4}, {"y0", 100}, {"x1", 286.6}, {"y1", 250},
    {"points", {{0.5, 0}, {1, 1}, {0, 1}}},
    {"smooth", {true, true, true}}, {"width", 0}};
  const auto b = media::object_bounds(three);
  CHECK(b[1] <= 100.5);
  CHECK(b[1] + b[3] > 295 && b[1] + b[3] < 305);
  Json corners = three;
  corners.erase("smooth");
  CHECK(std::abs(media::object_bounds(corners)[3] - 150) < 0.5);
  // Kept: the flags by their point (a bad point dropped with its own),
  // the openness; corners alone say neither.
  Json odd = open;
  odd["points"] = Json::array({{0, 1}, "x", {0.5, 0}, {1, 1}});
  odd["smooth"] = Json::array({false, true, true});
  const Json n = media::normalize_objects(Json::array({odd, corners}));
  REQUIRE(n[0]["smooth"].size() == 3);
  CHECK(n[0]["smooth"][0] == false && n[0]["smooth"][1] == true &&
        n[0]["smooth"][2] == false);
  CHECK(jget(n[0], "open", false));
  CHECK(!n[1].contains("smooth") && !n[1].contains("open"));
}

namespace {

// A picture drawn over white into an 8-bit sRGB bitmap `w` x `h`: a
// PDF's page (from 1) at its own size, or a picture file -- what a viewer
// shows of each.
std::vector<std::uint8_t>
drawn_on_white(const fs::path& file, int page, int w, int h)
{
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 8, static_cast<std::size_t>(w) * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGColorSpaceRelease(cs);
  CGContextSetRGBFillColor(ctx, 1, 1, 1, 1);
  CGContextFillRect(ctx, CGRectMake(0, 0, w, h));
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(file.c_str()),
      static_cast<CFIndex>(std::strlen(file.c_str())), false);
  if (page > 0) {
    if (CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url)) {
      if (CGPDFPageRef pg = CGPDFDocumentGetPage(doc, page)) {
        CGContextDrawPDFPage(ctx, pg);
      }
      CGPDFDocumentRelease(doc);
    }
  } else if (CGImageSourceRef src = CGImageSourceCreateWithURL(url,
                                                               nullptr)) {
    if (CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr)) {
      CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
      CGImageRelease(img);
    }
    CFRelease(src);
  }
  CFRelease(url);
  CGContextRelease(ctx);
  return px;
}

// Two drawings' mean difference, a channel's, of 255 -- and the share of
// their pixels some channel of which is more than `far` apart.
std::pair<double, double>
difference(const std::vector<std::uint8_t>& a,
           const std::vector<std::uint8_t>& b, int far = 48)
{
  if (a.size() != b.size() || a.empty()) {
    return {255, 1};
  }
  double sum = 0;
  std::size_t apart = 0;
  for (std::size_t i = 0; i < a.size(); i += 4) {
    int most = 0;
    for (int c = 0; c < 3; ++c) {
      const int d = std::abs(int(a[i + c]) - int(b[i + c]));
      sum += d;
      most = std::max(most, d);
    }
    apart += most > far;
  }
  const double n = static_cast<double>(a.size() / 4);
  return {sum / (n * 3), static_cast<double>(apart) / n};
}

// What a PDF page is made of: its images (its forms' too), fonts and
// shadings, and whether its content shows text (Tj / TJ).
struct PdfInk {
  int  images = 0;
  int  fonts = 0;
  int  shadings = 0;
  bool text = false;
};

void
ink_of_resources(CGPDFDictionaryRef res, PdfInk& ink);

void
ink_of_content(CGPDFStreamRef s, PdfInk& ink)
{
  CGPDFDataFormat format;
  if (CFDataRef d = CGPDFStreamCopyData(s, &format)) {
    const std::string body(
        reinterpret_cast<const char*>(CFDataGetBytePtr(d)),
        static_cast<std::size_t>(CFDataGetLength(d)));
    ink.text = ink.text || body.find("Tj") != std::string::npos ||
               body.find("TJ") != std::string::npos;
    CFRelease(d);
  }
}

void
ink_of_resources(CGPDFDictionaryRef res, PdfInk& ink)
{
  if (!res) {
    return;
  }
  CGPDFDictionaryRef d = nullptr;
  if (CGPDFDictionaryGetDictionary(res, "Font", &d)) {
    ink.fonts += static_cast<int>(CGPDFDictionaryGetCount(d));
  }
  if (CGPDFDictionaryGetDictionary(res, "Shading", &d)) {
    ink.shadings += static_cast<int>(CGPDFDictionaryGetCount(d));
  }
  if (CGPDFDictionaryGetDictionary(res, "XObject", &d)) {
    CGPDFDictionaryApplyFunction(
        d,
        [](const char*, CGPDFObjectRef o, void* info) {
          auto& ink = *static_cast<PdfInk*>(info);
          CGPDFStreamRef s = nullptr;
          if (!CGPDFObjectGetValue(o, kCGPDFObjectTypeStream, &s)) {
            return;
          }
          CGPDFDictionaryRef sd = CGPDFStreamGetDictionary(s);
          const char* sub = nullptr;
          if (CGPDFDictionaryGetName(sd, "Subtype", &sub) &&
              std::strcmp(sub, "Image") == 0) {
            ++ink.images;
          } else if (sub && std::strcmp(sub, "Form") == 0) {
            CGPDFDictionaryRef r = nullptr;
            if (CGPDFDictionaryGetDictionary(sd, "Resources", &r)) {
              ink_of_resources(r, ink);
            }
            ink_of_content(s, ink);
          }
        },
        &ink);
  }
}

PdfInk
pdf_ink(const fs::path& file, int page)
{
  PdfInk ink;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(file.c_str()),
      static_cast<CFIndex>(std::strlen(file.c_str())), false);
  CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url);
  CFRelease(url);
  if (!doc) {
    return ink;
  }
  if (CGPDFPageRef pg = CGPDFDocumentGetPage(doc, page)) {
    CGPDFDictionaryRef pd = CGPDFPageGetDictionary(pg);
    CGPDFDictionaryRef res = nullptr;
    if (CGPDFDictionaryGetDictionary(pd, "Resources", &res)) {
      ink_of_resources(res, ink);
    }
    CGPDFStreamRef content = nullptr;
    CGPDFArrayRef parts = nullptr;
    if (CGPDFDictionaryGetStream(pd, "Contents", &content)) {
      ink_of_content(content, ink);
    } else if (CGPDFDictionaryGetArray(pd, "Contents", &parts)) {
      for (std::size_t i = 0; i < CGPDFArrayGetCount(parts); ++i) {
        if (CGPDFArrayGetStream(parts, i, &content)) {
          ink_of_content(content, ink);
        }
      }
    }
  }
  CGPDFDocumentRelease(doc);
  return ink;
}

// A PDF's pages and the size of each, in points.
std::vector<std::pair<int, int>>
pdf_pages(const fs::path& file)
{
  std::vector<std::pair<int, int>> out;
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(file.c_str()),
      static_cast<CFIndex>(std::strlen(file.c_str())), false);
  CGPDFDocumentRef doc = CGPDFDocumentCreateWithURL(url);
  CFRelease(url);
  if (!doc) {
    return out;
  }
  for (std::size_t i = 1; i <= CGPDFDocumentGetNumberOfPages(doc); ++i) {
    const CGRect r = CGPDFPageGetBoxRect(CGPDFDocumentGetPage(doc, i),
                                         kCGPDFMediaBox);
    out.push_back({static_cast<int>(std::lround(r.size.width)),
                   static_cast<int>(std::lround(r.size.height))});
  }
  CGPDFDocumentRelease(doc);
  return out;
}

}

// A STILL WRITTEN AS A PDF (media/pdf.h): a page a stack, as
// flatten_layers draws it -- its canvas, a layer placed by its crop
// (scaled, turned, moved), a composition a layer shows drawn in its
// place, a mask, effects -- but its words TEXT in a font, its shapes and
// gradients paths and shadings: a page of markup alone holds no image
// but its shadow's. Drawn back, each page is the flattened picture to
// within its edges (anti-aliasing, the blend of a translucent edge).
TEST(media, a_still_is_written_as_a_pdf_its_vectors_kept)
{
  using Kind = media::PdfLayer::Kind;
  const auto root = test::temp_dir("pdf-still");
  const Json words = {
    {"id", "w"}, {"kind", "text"}, {"box", true},
    {"x0", 20}, {"y0", 16}, {"x1", 380}, {"y1", 70},
    {"text", "Vectors stay sharp"}, {"stroke", {0.1, 0.1, 0.1, 1}},
    {"font", {{"family", "Helvetica"}, {"size", 40}, {"bold", true}}}};
  const Json painted = {
    {"id", "g"}, {"kind", "text"}, {"box", true},
    {"x0", 20}, {"y0", 80}, {"x1", 380}, {"y1", 150},
    {"text", "MMMM"}, {"stroke", {0, 0, 0, 1}},
    {"font", {{"family", "Helvetica"}, {"size", 56}, {"bold", true},
              {"underline", true}}},
    {"gradient", {{"kind", "linear"},
                  {"stops", {{{"at", 0}, {"color", {1, 0, 0, 1}}},
                             {{"at", 1}, {"color", {0, 0, 1, 1}}}}}}}};
  const Json band = {
    {"id", "b"}, {"kind", "rect"},
    {"x0", 20}, {"y0", 170}, {"x1", 200}, {"y1", 220}, {"width", 0},
    {"gradient", {{"kind", "linear"},
                  {"stops", {{{"at", 0}, {"color", {1, 0.8, 0, 1}}},
                             {{"at", 1}, {"color", {0, 0.6, 0.2, 1}}}}}}}};
  const Json star = {
    {"id", "p"}, {"kind", "polygon"},
    {"x0", 220}, {"y0", 160}, {"x1", 300}, {"y1", 240},
    {"points", {{0.5, 0}, {0.62, 0.38}, {1, 0.38}, {0.69, 0.62},
                {0.81, 1}, {0.5, 0.76}, {0.19, 1}, {0.31, 0.62},
                {0, 0.38}, {0.38, 0.38}}},
    {"fill", {1, 0.85, 0.1, 1}}, {"stroke", {0.4, 0.2, 0, 1}},
    {"width", 3}};
  const Json shadowed = {
    {"id", "s"}, {"kind", "ellipse"},
    {"x0", 320}, {"y0", 170}, {"x1", 380}, {"y1", 230},
    {"fill", {1, 0, 0, 1}}, {"width", 0},
    {"effects", Json::array({{{"kind", "shadow"},
                              {"color", {0, 0, 0, 0.7}}, {"blur", 6},
                              {"dx", 8}, {"dy", 10}}})}};
  const Json objects = Json::array({words, painted, band, star, shadowed});
  // Page 1: a markup alone.
  media::PdfLayer mk;
  mk.kind = Kind::Markup;
  mk.size = {400, 260};
  mk.objects = objects;
  media::PdfLayer page1;
  page1.kind = Kind::Stack;
  page1.layers = {mk};

  // Page 2: a picture at the bottom; the markup over it placed by a crop;
  // a composition of its own -- a framed canvas, a disc over a picture --
  // shown through a soft MASK above it; the canvas made larger, clear
  // round the frame.
  const auto photo = root / "photo.png";
  REQUIRE_OK(media::render_markup(
      {}, {300, 200},
      Json::array({{{"id", "a"}, {"kind", "rect"}, {"x0", 0}, {"y0", 0},
                    {"x1", 150}, {"y1", 100}, {"width", 0},
                    {"fill", {0.2, 0.4, 0.8, 1}}},
                   {{"id", "b"}, {"kind", "rect"}, {"x0", 150}, {"y0", 0},
                    {"x1", 300}, {"y1", 100}, {"width", 0},
                    {"fill", {0.9, 0.7, 0.2, 1}}},
                   {{"id", "c"}, {"kind", "rect"}, {"x0", 0}, {"y0", 100},
                    {"x1", 300}, {"y1", 200}, {"width", 0},
                    {"fill", {0.3, 0.6, 0.3, 1}}}}),
      {}, photo));
  media::PdfLayer pic;
  pic.kind = Kind::Picture;
  pic.file = photo;
  media::PdfLayer placed = mk;
  placed.crop.scale_x = placed.crop.scale_y = 0.5;
  placed.crop.rotate = 12;
  placed.crop.offset_x = 0.12;
  placed.crop.offset_y = -0.1;
  media::PdfLayer disc;
  disc.kind = Kind::Markup;
  disc.size = {120, 80};
  disc.objects = Json::array({{{"id", "d"}, {"kind", "ellipse"},
                               {"x0", 10}, {"y0", 5}, {"x1", 110},
                               {"y1", 75}, {"width", 4},
                               {"fill", {0.9, 0.1, 0.5, 1}},
                               {"stroke", {1, 1, 1, 1}}}});
  media::PdfLayer inner_pic = pic;
  inner_pic.crop.scale_x = inner_pic.crop.scale_y = 0.3;
  media::PdfLayer inner;
  inner.kind = Kind::Stack;
  inner.canvas.frame_w = 120;
  inner.canvas.frame_h = 80;
  inner.layers = {inner_pic, disc};
  inner.crop.scale_x = inner.crop.scale_y = 0.9;
  inner.crop.offset_x = -0.25;
  inner.crop.offset_y = 0.2;
  media::PdfLayer veil;
  veil.kind = Kind::Markup;
  veil.size = {300, 200};
  veil.mask = true;
  veil.objects = Json::array({{{"id", "v"}, {"kind", "rect"}, {"x0", 0},
                               {"y0", 0}, {"x1", 300}, {"y1", 200},
                               {"width", 0},
                               {"gradient",
                                {{"kind", "linear"},
                                 {"stops", {{{"at", 0},
                                             {"color", {1, 1, 1, 1}}},
                                            {{"at", 1},
                                             {"color", {0, 0, 0, 1}}}}},
                                 {"from", {0, 0.5}},
                                 {"to", {0.6, 0.5}}}}}});
  media::PdfLayer page2;
  page2.kind = Kind::Stack;
  page2.layers = {pic, placed, inner, veil};
  page2.canvas.width = 360;
  page2.canvas.height = 240;
  page2.canvas.x = 30;
  page2.canvas.y = 20;

  // Page 3: the words under a HARD mask -- a shape, all on or off --
  // which a PDF masks mark by mark as a group would be: still text.
  media::PdfLayer cut;
  cut.kind = Kind::Markup;
  cut.size = {300, 200};
  cut.mask = true;
  cut.objects = Json::array({{{"id", "c"}, {"kind", "rect"}, {"x0", 0},
                              {"y0", 0}, {"x1", 300}, {"y1", 110},
                              {"width", 0}, {"fill", {1, 1, 1, 1}}}});
  media::PdfLayer page3;
  page3.kind = Kind::Stack;
  page3.layers = {pic, placed, cut};

  media::PdfDocument doc;
  doc.title = "Poster";
  doc.pages = {page1, page2, page3};
  // As a job carries it: the same again.
  const media::PdfDocument back =
      media::pdf_document_from_json(media::to_json(doc));
  CHECK(media::to_json(back) == media::to_json(doc));
  const auto files = media::files_of(doc);
  CHECK(files.size() == 1 && files[0] == photo);

  const auto out = root / "Poster.pdf";
  int counted = 0;
  REQUIRE_OK(media::write_pdf(back, out, [&](int done, int pages) {
    CHECK(pages == 3);
    counted = done;
    return true;
  }));
  CHECK(counted == 3);
  const auto pages = pdf_pages(out);
  REQUIRE(pages.size() == 3);
  CHECK(pages[0] == std::pair(400, 260));
  CHECK(pages[1] == std::pair(360, 240));
  CHECK(pages[2] == std::pair(300, 200));

  // Page 1: its words text, in a font; its gradients shadings; its one
  // image the shadow beneath the disc.
  const PdfInk ink = pdf_ink(out, 1);
  CHECK(ink.text);
  CHECK(ink.fonts >= 1);
  CHECK(ink.shadings >= 2);
  CHECK(ink.images == 1);

  // Each page drawn back is the picture flattened, near enough.
  auto as_picture = [&](const media::PdfLayer& l)
      -> Result<media::LayerPicture> {
    media::LayerPicture lp;
    lp.adjust = l.adjust;
    lp.crop = l.crop;
    lp.visible = l.visible;
    lp.mask = l.mask;
    if (l.kind == Kind::Picture) {
      lp.file = l.file;
    } else if (l.kind == Kind::Markup) {
      VALTZ_ASSIGN(lp.drawn, media::render_markup_picture(
                                 l.file, l.size, l.objects, {}));
    }
    return lp;
  };
  std::vector<media::LayerPicture> flat1;
  auto m1 = as_picture(mk);
  REQUIRE_OK(m1);
  flat1.push_back(*m1);
  const auto png1 = root / "page-1.png";
  REQUIRE_OK(media::flatten_layers(flat1, png1));
  // The composition within, flattened first, as a stack shows it.
  std::vector<media::LayerPicture> within;
  for (const auto& l : inner.layers) {
    auto lp = as_picture(l);
    REQUIRE_OK(lp);
    within.push_back(*lp);
  }
  const auto inner_png = root / "inner.png";
  REQUIRE_OK(media::flatten_layers(within, inner_png, inner.canvas));
  std::vector<media::LayerPicture> flat2;
  for (const auto& l : page2.layers) {
    auto lp = as_picture(l);
    REQUIRE_OK(lp);
    if (l.kind == Kind::Stack) {
      lp->file = inner_png;
    }
    flat2.push_back(*lp);
  }
  const auto png2 = root / "page-2.png";
  REQUIRE_OK(media::flatten_layers(flat2, png2, page2.canvas));
  std::vector<media::LayerPicture> flat3;
  for (const auto& l : page3.layers) {
    auto lp = as_picture(l);
    REQUIRE_OK(lp);
    flat3.push_back(*lp);
  }
  const auto png3 = root / "page-3.png";
  REQUIRE_OK(media::flatten_layers(flat3, png3));
  // Apart only at edges: the PDF's words set as glyphs (the flattened
  // ones are a picture of them, turned and resampled), a gradient's
  // through their outlines.
  const auto pdf1 = drawn_on_white(out, 1, 400, 260);
  const auto [mean1, apart1] =
      difference(pdf1, drawn_on_white(png1, 0, 400, 260));
  const auto [mean2, apart2] =
      difference(drawn_on_white(out, 2, 360, 240),
                 drawn_on_white(png2, 0, 360, 240));
  const auto [mean3, apart3] =
      difference(drawn_on_white(out, 3, 300, 200),
                 drawn_on_white(png3, 0, 300, 200));
  CHECK(mean1 < 2.0);
  CHECK(apart1 < 0.025);
  CHECK(mean2 < 2.5);
  CHECK(apart2 < 0.025);
  CHECK(mean3 < 2.0);
  CHECK(apart3 < 0.025);
  // Its shadow below and right of the disc, none above it; the painted
  // words red at their left, half way to blue at their right (the box's
  // middle).
  auto at = [&](int x, int y) {
    const std::size_t i = (static_cast<std::size_t>(y) * 400 +
                           static_cast<std::size_t>(x)) * 4;
    return std::array<int, 3>{pdf1[i], pdf1[i + 1], pdf1[i + 2]};
  };
  CHECK(at(360, 236)[0] < 200);
  CHECK(at(326, 172)[0] > 245 && at(326, 172)[2] > 245);
  CHECK(at(350, 200)[0] > 240 && at(350, 200)[1] < 15);
  // Red less blue, on average, over the words' ink in a band.
  auto redder = [&](int x0, int x1) {
    double sum = 0;
    int n = 0;
    for (int y = 90; y < 145; ++y) {
      for (int x = x0; x < x1; ++x) {
        const auto px = at(x, y);
        if (std::min({px[0], px[1], px[2]}) < 120) {
          sum += px[0] - px[2];
          ++n;
        }
      }
    }
    return n > 20 ? sum / n : -999.0;
  };
  CHECK(redder(20, 60) > 150);
  const double right = redder(170, 220);
  CHECK(right > -999 && right < 60);
  // Page 2: under its soft mask the composition is pixels; the words
  // over the picture still text. Page 3: the words under a hard mask
  // still text.
  const PdfInk ink2 = pdf_ink(out, 2);
  CHECK(ink2.images >= 2);
  CHECK(ink2.text);
  CHECK(pdf_ink(out, 3).text);

  // Stopped after its first page: nothing left.
  const auto stopped = root / "Stopped.pdf";
  auto st = media::write_pdf(doc, stopped, [](int, int) { return false; });
  CHECK(st.code() == Code::Cancelled);
  CHECK(!fs::exists(stopped));
  // A page with nothing at its bottom and no frame: refused.
  media::PdfDocument bare;
  bare.pages.push_back(media::PdfLayer{.kind = Kind::Stack});
  CHECK(!media::write_pdf(bare, root / "bare.pdf").ok());
}

// A STILL COMPOSITION EXPORTED AS A PDF (Controller::pdf_document): its
// pages one file; each page's layers -- a picture, a markup's objects as
// vectors, a still composition a layer shows as its own layers -- drawn
// as the page flattens; the export lane's job carrying the document and
// every file it reads. A timeline is not a picture: refused.
TEST(controller, a_still_exports_as_a_pdf)
{
  using Kind = media::PdfLayer::Kind;
  auto root = test::temp_dir("ctl-pdf");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "P.valtz", "pdf");
  REQUIRE_OK(pid);
  REQUIRE(write_png(root / "grey.png", 200, 120, {0x90, 0x90, 0x90, 0xff}));
  REQUIRE(write_png(root / "red.png", 40, 40, {0xff, 0, 0, 0xff}));
  auto grey = import_and_wait(ctl, *pid, root / "grey.png");
  auto red = import_and_wait(ctl, *pid, root / "red.png");
  REQUIRE(grey && red);
  auto still = ctl.create_composition(*pid, project::AssetClass::Still,
                                      {200, 120});
  REQUIRE_OK(still);
  const AssetId c = *still;
  auto l0 = ctl.add_layer(*pid, c);
  REQUIRE_OK(l0);
  REQUIRE_OK(ctl.set_layer_source(*pid, c, *l0, *grey));
  auto ml = ctl.markup_layer(*pid, c, {});
  REQUIRE_OK(ml);
  REQUIRE_OK(ctl.set_markup_objects(
      *pid, c, *ml,
      Json::array({{{"id", "t"}, {"kind", "text"}, {"box", true},
                    {"x0", 10}, {"y0", 10}, {"x1", 190}, {"y1", 50},
                    {"text", "Hello"}, {"stroke", {0, 0, 0, 1}},
                    {"font", {{"family", "Helvetica"}, {"size", 28}}}},
                   {{"id", "r"}, {"kind", "rect"}, {"x0", 120},
                    {"y0", 60}, {"x1", 190}, {"y1", 110}, {"width", 4},
                    {"stroke", {0, 0, 1, 1}}, {"fill", {0, 0, 0, 0}}}})));
  auto inner = ctl.create_composition(*pid, project::AssetClass::Still,
                                      {60, 40});
  REQUIRE_OK(inner);
  auto il = ctl.add_layer(*pid, *inner);
  REQUIRE_OK(il);
  REQUIRE_OK(ctl.set_layer_source(*pid, *inner, *il, *red));
  auto l2 = ctl.add_layer(*pid, c, *ml);
  REQUIRE_OK(l2);
  REQUIRE_OK(ctl.set_layer_source(*pid, c, *l2, *inner));
  REQUIRE_OK(ctl.add_page(*pid, c));

  auto doc = ctl.pdf_document(*pid, c);
  REQUIRE_OK(doc);
  REQUIRE(doc->pages.size() == 2);
  const auto& ls = doc->pages[0].layers;
  REQUIRE(ls.size() == 3);
  CHECK(ls[0].kind == Kind::Picture);
  CHECK(ls[1].kind == Kind::Markup);
  CHECK(ls[1].objects.size() == 2);
  CHECK(ls[2].kind == Kind::Stack);
  REQUIRE(ls[2].layers.size() == 1);
  CHECK(ls[2].layers[0].kind == Kind::Picture);
  const auto drawn = root / "doc.pdf";
  REQUIRE_OK(media::write_pdf(*doc, drawn));
  REQUIRE(pdf_pages(drawn).size() == 2);
  for (int page = 0; page < 2; ++page) {
    const auto flat = root / std::format("page-{}.png", page);
    REQUIRE_OK(ctl.flatten(*pid, c, flat, std::nullopt, std::nullopt, {},
                           page));
    const auto [mean, apart] =
        difference(drawn_on_white(drawn, page + 1, 200, 120),
                   drawn_on_white(flat, 0, 200, 120));
    CHECK(mean < 2.0);
    CHECK(apart < 0.01);
  }
  CHECK(pdf_ink(drawn, 1).text);

  // Exported: the export lane's job, the document and its files in it;
  // written as the lane writes it, saved where asked.
  ExportRequest ex;
  ex.project = *pid;
  ex.asset = c;
  ex.format = "pdf";
  ex.destination = root / "out" / "Booklet.pdf";
  auto xj = ctl.export_asset(ex);
  REQUIRE_OK(xj);
  const engine::JobSpec* spec = engine->spec(*xj);
  REQUIRE(spec != nullptr);
  CHECK(spec->op == engine::kOpExportMedia);
  const auto sent = media::pdf_document_from_json(
      jget(spec->params, "pdf", Json()));
  CHECK(sent.pages.size() == 2);
  CHECK(sent.title == doc->title);
  CHECK(spec->inputs.size() == 2);  // the grey and the red pictures
  for (const auto& in : spec->inputs) {
    CHECK(in.role == "file");
    CHECK(!in.hash.is_zero());
  }
  const auto lane = root / "lane.pdf";
  REQUIRE_OK(media::write_pdf(sent, lane));
  engine->finish(*xj, lane);
  CHECK(pdf_pages(ex.destination).size() == 2);
  auto rec = ctl.jobs().get(*xj);
  REQUIRE(rec);
  CHECK(rec->state == JobState::Finished);
  CHECK(rec->message == ex.destination.string());

  // A timeline is no picture.
  auto clip = ctl.create_composition(*pid, project::AssetClass::Composition,
                                     {200, 120});
  REQUIRE_OK(clip);
  CHECK(!ctl.pdf_document(*pid, *clip).ok());
  ex.asset = *clip;
  CHECK(!ctl.export_asset(ex).ok());
}

// A LONG TEXT read as light Markdown and FLOWED onto pages
// (media/text-flow.h): every word placed once, in order; each box in the
// area; a paragraph cut at a line with two lines at least either side;
// a heading kept with what follows it.
TEST(media, a_long_text_flows_onto_pages)
{
  using K = media::FlowBlock::Kind;
  bool found = false;
  auto marked = media::read_flow_blocks(
      "# The Lighthouse\n\nIt stood on the **rock** for a *hundred* "
      "years,\nits lamp turning.\n\n## The keeper\n\n- [Oil](http://x) "
      "and wicks\n- Bread\n\n> Not tonight.\n\nsnake_case stays; 5 * 3 "
      "too.",
      false, true, &found);
  REQUIRE(marked.size() == 7);
  CHECK(marked[0].kind == K::Title);
  CHECK(marked[1].text ==
        "It stood on the rock for a hundred years, its lamp turning.");
  CHECK(marked[2].kind == K::Heading);
  CHECK(marked[3].kind == K::Item && marked[3].text == "Oil and wicks");
  CHECK(marked[5].kind == K::Quote);
  CHECK(marked[6].text == "snake_case stays; 5 * 3 too.");
  CHECK(!found);
  // No blank line: a paragraph a line; a short line before a longer
  // paragraph, ending no sentence, a heading (the first the title).
  const std::string para =
      "The sea came in grey under a low sky, and the gulls rode the wind "
      "over the harbour wall while the boats came home one by one.";
  auto plain = media::read_flow_blocks(
      "Storm Night\n" + para + "\nThe Morning After\n" + para + "\n" +
          para,
      false, true, &found);
  REQUIRE(plain.size() == 5);
  CHECK(found);
  CHECK(plain[0].kind == K::Title);
  CHECK(plain[2].kind == K::Heading);
  CHECK(plain[4].kind == K::Paragraph);
  // Kept line breaks: a poem's lines stay lines.
  auto poem = media::read_flow_blocks("Roses red,\nviolets blue.\n\nMore.",
                                      true, false);
  REQUIRE(poem.size() == 2);
  CHECK(poem[0].text == "Roses red,\nviolets blue.");

  // A long text: twenty paragraphs under headings, in two columns.
  std::string text = "# A Long Text\n\n";
  std::vector<std::string> words;
  for (int i = 0; i < 20; ++i) {
    if (i % 5 == 0) {
      text += std::format("## Part {}\n\n", i / 5 + 1);
    }
    std::string p;
    for (int w = 0; w < 30 + (i * 13) % 50; ++w) {
      p += std::format("{}w{}x{} ", w == 0 ? "" : "", i, w);
    }
    text += p + "\n\n";
  }
  media::FlowSpec spec;
  spec.blocks = media::read_flow_blocks(text, false, true);
  spec.area = {80, 100, 1040, 560};
  spec.columns = 2;
  spec.gutter = 40;
  spec.first_top = 300;
  spec.body.size = 22;
  spec.body.align = "justified";
  spec.title.size = 44;
  spec.title.bold = true;
  spec.heading.size = 30;
  const auto r = media::flow_text(spec);
  REQUIRE(r.pages.size() >= 3);
  CHECK(r.left == 0);
  CHECK(r.headings.size() == 5);
  CHECK(r.headings[0].title && r.headings[0].page == 0);
  // Every word once, in order -- the markers aside.
  std::string laid;
  int cut = 0;
  for (std::size_t pg = 0; pg < r.pages.size(); ++pg) {
    const auto& page = r.pages[pg];
    CHECK(!page.starts.empty());
    for (const auto& o : page.objects) {
      if (jget<std::string>(o, "kind", "") != "text") {
        continue;
      }
      CHECK(jget(o, "y1", 0.0) <= 660 + 0.5);
      CHECK(jget(o, "y0", 0.0) >= (pg == 0 ? 300 : 100) - 0.5);
      CHECK(jget(o, "x0", 0.0) >= 80 && jget(o, "x1", 0.0) <= 1120.5);
      const auto f = media::text_fit(o);
      CHECK(f.fits);
      if (jget(o, "justify_last", false)) {
        ++cut;
        CHECK(f.lines >= 2);
      }
      laid += jget<std::string>(o, "text", "") + " ";
    }
  }
  CHECK(cut >= 1);
  std::string want;
  for (const auto& b : spec.blocks) {
    want += b.text + " ";
  }
  auto squash = [](std::string s) {
    std::string o;
    for (char c : s) {
      if (c == ' ' || c == '\n') {
        if (!o.empty() && o.back() != ' ') {
          o += ' ';
        }
      } else {
        o += c;
      }
    }
    return o;
  };
  CHECK(squash(laid) == squash(want));
  // A chapter a page: each part on a page of its own, the title kept
  // with the first.
  media::FlowSpec chapters = spec;
  chapters.page_per_heading = true;
  const auto ch = media::flow_text(chapters);
  REQUIRE(ch.headings.size() == 5);
  CHECK(ch.headings[1].page == 0);
  for (std::size_t i = 2; i < ch.headings.size(); ++i) {
    CHECK(ch.headings[i].page > ch.headings[i - 1].page);
  }
  CHECK(ch.left == 0);
  // A page's cap: what is left is told.
  spec.max_pages = 1;
  const auto one = media::flow_text(spec);
  CHECK(one.pages.size() == 1);
  CHECK(one.left > 0);
}

// A long SOFT stroke paints in a moment, and only where it went: each
// dab's gradient is drawn in its own square -- unclipped, Core Graphics
// shaded the whole canvas for every dab, and a 600-point stroke on a
// 1024 x 1024 canvas took 3.6 s (the app hung at the pointer's release).
// Then erased the same way, softly.
TEST(media, a_long_soft_stroke_paints_in_a_moment)
{
  const auto root = test::temp_dir("markup-stroke");
  media::Stroke s;
  s.radius = 30;
  s.softness = 0.5;
  s.color = {1, 0.125, 0, 1};
  for (int i = 0; i < 600; ++i) {
    const double t = i / 599.0;
    s.points.push_back(
        {102 + 819 * std::abs(std::fmod(t * 8, 2.0) - 1), 102 + 819 * t});
  }
  const auto painted = root / "painted.png";
  const auto t0 = std::chrono::steady_clock::now();
  REQUIRE_OK(media::paint_stroke({}, {1024, 1024}, s, painted));
  const double took = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - t0).count();
  CHECK(took < 1.0);
  CHECK(pixel_at(painted, 921, 102)[3] > 200);    // where it starts
  CHECK(pixel_at(painted, 921, 102)[0] > 200);
  CHECK(pixel_at(painted, 50, 50)[3] == 0);       // nowhere near it
  CHECK(pixel_at(painted, 1000, 30)[3] == 0);
  // The soft edge: fading, not cut, past the hard core.
  const int edge = pixel_at(painted, 921, 102 - 25)[3];
  CHECK(edge > 0 && edge < 255);

  media::Stroke e = s;
  e.erase = true;
  const auto erased = root / "erased.png";
  const auto t1 = std::chrono::steady_clock::now();
  REQUIRE_OK(media::paint_stroke(painted, {1024, 1024}, e, erased));
  CHECK(std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      t1).count() < 1.0);
  CHECK(pixel_at(erased, 921, 102)[3] < 10);
}

// An asset's NAME in the list: one line, never empty, undoable.
TEST(controller, an_asset_is_renamed)
{
  Bench b("ctl-rename");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  const std::string was = b.p().asset(*red)->name;
  REQUIRE_OK(ctl.rename_asset(pid, *red, "  Red\nsquare  "));
  CHECK(b.p().asset(*red)->name == "Red square");
  auto empty = ctl.rename_asset(pid, *red, " \n ");
  CHECK(!empty.ok());
  CHECK(b.p().asset(*red)->name == "Red square");
  REQUIRE_OK(ctl.undo(pid));
  CHECK(b.p().asset(*red)->name == was);
  REQUIRE_OK(ctl.redo(pid));
  CHECK(b.p().asset(*red)->name == "Red square");
}

// DECOMPOSE: a layer showing a composition gives way to that
// composition's layers, its placement carried over to each -- the same
// picture -- and it is refused where that cannot be exact.
TEST(composition, decompose_carries_placement)
{
  Bench b("comp-decompose");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 16, 16, {0, 0, 0xff, 0xff});
  REQUIRE(red && blue);
  // An inner still: red, and blue at half size, a quarter to the right.
  auto inner = ctl.create_composition(pid, project::AssetClass::Still,
                                      {16, 16}, {}, "inner");
  REQUIRE_OK(inner);
  REQUIRE_OK(ctl.instantiate(pid, *red, *inner));
  auto bl = ctl.instantiate(pid, *blue, *inner);
  REQUIRE_OK(bl);
  media::Crop half;
  half.scale_x = half.scale_y = 0.5;
  half.offset_x = 0.25;
  REQUIRE_OK(ctl.set_crop(pid, *inner, half, bl->second));
  // The project: 32², the inner still on it, 1.5 times, a little down.
  auto outer = ctl.create_composition(pid, project::AssetClass::Still,
                                      {32, 32}, {}, "outer", true);
  REQUIRE_OK(outer);
  auto inst = ctl.instantiate(pid, *inner, *outer);
  REQUIRE_OK(inst);
  media::Crop place;
  place.scale_x = place.scale_y = 1.5;
  place.offset_y = 0.1;
  REQUIRE_OK(ctl.set_crop(pid, *outer, place, inst->second));
  const auto before = b.root / "before.png";
  const auto after = b.root / "after.png";
  REQUIRE_OK(ctl.flatten(pid, *outer, before));

  // With a look of its own it cannot be carried: refused.
  media::Adjustments warm;
  warm.exposure = 0.5;
  REQUIRE_OK(ctl.set_adjustments(pid, *outer, warm, inst->second));
  CHECK(!ctl.decompose(pid, *outer, inst->second).ok());
  REQUIRE_OK(ctl.set_adjustments(pid, *outer, {}, inst->second));
  // A layer that does not show a composition: nothing to decompose.
  auto plain = ctl.instantiate(pid, *red, *outer, std::string());
  REQUIRE_OK(plain);
  CHECK(!ctl.decompose(pid, *outer, plain->second).ok());
  REQUIRE_OK(ctl.remove_layer(pid, *outer, plain->second));

  REQUIRE_OK(ctl.decompose(pid, *outer, inst->second));
  const auto ls = b.layers(*outer);
  REQUIRE(ls.size() == 2);
  CHECK(ls[0].source == *red && ls[1].source == *blue);
  REQUIRE_OK(ctl.flatten(pid, *outer, after));
  // Off the edges, where one resampling of the drawn still and two of
  // its layers differ by a little.
  for (auto [x, y] : {std::pair{10, 16}, {6, 18}, {22, 20}, {20, 10},
                      {23, 19}, {2, 2}, {30, 30}}) {
    const auto p1 = pixel_at(before, x, y), p2 = pixel_at(after, x, y);
    for (int k = 0; k < 4; ++k) {
      CHECK(std::abs(p1[k] - p2[k]) <= 3);
    }
  }
  // A turned layer under a stretch would skew: refused.
  auto inner2 = ctl.create_composition(pid, project::AssetClass::Still,
                                       {16, 16}, {}, "turned");
  REQUIRE_OK(inner2);
  auto t = ctl.instantiate(pid, *blue, *inner2);
  REQUIRE_OK(t);
  media::Crop turned;
  turned.rotate = 30;
  REQUIRE_OK(ctl.set_crop(pid, *inner2, turned, t->second));
  auto i2 = ctl.instantiate(pid, *inner2, *outer);
  REQUIRE_OK(i2);
  media::Crop stretch;
  stretch.scale_x = 2;
  REQUIRE_OK(ctl.set_crop(pid, *outer, stretch, i2->second));
  CHECK(!ctl.decompose(pid, *outer, i2->second).ok());
}

// An EDITED copy, a CAPTURE, a FLATTEN: the copy is a composition over
// the original, which never changes; a capture is frozen; a flat asset
// is the drawing, made a file, and no longer follows what it came from.
TEST(composition, edited_copy_capture_and_flatten)
{
  Bench b("comp-copies");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 8, 8, {0xff, 0, 0, 0xff});
  auto base = b.png("base.png", 16, 16, {0x80, 0x80, 0x80, 0xff});
  auto blue = b.png("blue.png", 16, 16, {0, 0, 0xff, 0xff});
  REQUIRE(red && base && blue);
  auto mod = ctl.derive_modified(pid, *red, "");
  REQUIRE_OK(mod);
  media::Adjustments dark;
  dark.exposure = -2;
  REQUIRE_OK(ctl.set_adjustments(pid, *mod, dark));
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.flatten(pid, *mod, flat));
  CHECK(pixel_at(flat, 4, 4)[0] < 160);   // darkened
  REQUIRE_OK(ctl.flatten(pid, *red, flat));
  CHECK(pixel_at(flat, 4, 4)[0] > 200);   // the original, as it was
  auto th = ctl.thumbnail(pid, *mod, 64);
  REQUIRE_OK(th);
  CHECK(pixel_at(*th, 2, 2)[0] < 160);
  // Flattened: a flat picture of it as drawn, noting where it came from.
  auto f = ctl.flatten_asset(pid, *mod, "dark red");
  REQUIRE_OK(f);
  auto fa = b.p().asset(*f);
  CHECK(fa->cls == project::AssetClass::Flat);
  CHECK(jget<std::string>(fa->from, "asset", "") == mod->str());
  auto fp = b.p().media_path(*f);
  REQUIRE_OK(fp);
  CHECK(pixel_at(*fp, 4, 4)[0] < 160);
  // A flat copy of a flat picture: its file, shared.
  auto same = ctl.flatten_asset(pid, *red);
  REQUIRE_OK(same);
  CHECK(b.p().version(*same)->blob.hash == b.p().version(*red)->blob.hash);
  // In use: the original stays while the copy shows it.
  CHECK(!ctl.remove_asset(pid, *red).ok());
  REQUIRE_OK(ctl.remove_asset(pid, *mod));
  REQUIRE_OK(ctl.remove_asset(pid, *red));
  CHECK(b.p().blobs().contains(b.p().version(*same)->blob));

  // A capture pins what it shows, and stays as it was.
  auto s = ctl.derive_modified(pid, *base, "stack");
  REQUIRE_OK(s);
  auto l1 = ctl.instantiate(pid, *blue, *s);
  REQUIRE_OK(l1);
  media::Crop small;
  small.scale_x = small.scale_y = 0.5;
  REQUIRE_OK(ctl.set_crop(pid, *s, small, l1->second));
  auto cap = ctl.capture(pid, *s, {}, "whole");
  REQUIRE_OK(cap);
  for (const auto& l : b.layers(*cap)) {
    CHECK(l.source_version > 0);
  }
  REQUIRE_OK(ctl.set_layer_visible(pid, *s, l1->second, false));
  REQUIRE_OK(ctl.flatten(pid, *cap, flat));
  CHECK(pixel_at(flat, 8, 8)[2] > 200);   // blue, as captured
  CHECK(pixel_at(flat, 1, 1)[1] > 100);   // grey round it
  CHECK(!ctl.remove_asset(pid, *blue).ok());
}

// The PROJECT's canvas is its own (DESIGN §10a): the first asset placed
// sizes it; while the project is that one take, a new take sizes it
// again; once the canvas is resized or another layer shows something, it
// stays -- a new take in layer 0 lies on it, centred at its own size --
// and the assets themselves never change. Layer 0 moves and goes as any
// layer does.
TEST(composition, the_project_canvas_is_its_own)
{
  Bench b("comp-project-canvas");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  auto green = b.png("green.png", 24, 12, {0, 0xff, 0, 0xff});
  REQUIRE(red && green);
  auto frame = [&](AssetId id) {
    auto a = b.p().asset(id);
    return a.ok() ? a->canvas.frame() : media::PixelSize{};
  };
  auto placed = ctl.place_in_project(pid, *red);
  REQUIRE_OK(placed);
  REQUIRE(*placed);
  const AssetId view = **placed;
  CHECK((frame(view) == media::PixelSize{16, 16}));
  // Alone, a new take sizes it again.
  REQUIRE_OK(ctl.place_in_project(pid, *green));
  CHECK((frame(view) == media::PixelSize{24, 12}));
  REQUIRE_OK(ctl.place_in_project(pid, *red));
  CHECK((frame(view) == media::PixelSize{16, 16}));
  // Resized (from the top left): the frame is the new size, and stays
  // through a new take, which lies on it centred.
  REQUIRE_OK(ctl.set_canvas(pid, view, {20, 20}, 0, 0));
  REQUIRE_OK(ctl.place_in_project(pid, *green));
  auto v = b.p().asset(view);
  CHECK((v->canvas.frame() == media::PixelSize{20, 20}));
  CHECK(v->canvas.width == 20 && v->canvas.x == 0);
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.flatten(pid, view, flat));
  CHECK(pixel_at(flat, 8, 8)[1] > 200);
  CHECK(pixel_at(flat, 0, 8)[1] > 200);
  CHECK(pixel_at(flat, 18, 8)[1] > 200);
  CHECK(pixel_at(flat, 8, 0)[3] == 0);
  CHECK(pixel_at(flat, 18, 18)[3] == 0);
  REQUIRE_OK(ctl.reset_canvas(pid, view));
  v = b.p().asset(view);
  CHECK(!v->canvas.set() && (v->canvas.frame() == media::PixelSize{20, 20}));
  // Another layer shows something: composed, the canvas stays.
  auto l1 = ctl.add_layer(pid, view);
  REQUIRE_OK(l1);
  REQUIRE_OK(ctl.set_layer_source(pid, view, *l1, *red));
  REQUIRE_OK(ctl.place_in_project(pid, *red));
  REQUIRE_OK(ctl.place_in_project(pid, *green));
  CHECK((frame(view) == media::PixelSize{20, 20}));
  // Layer 0 like any layer: up over layer 1, then gone; the canvas stays.
  REQUIRE_OK(ctl.move_layer(pid, view, "", 1));
  v = b.p().asset(view);
  CHECK(v->layers[1].id.empty() && v->layers[0].id == *l1);
  REQUIRE_OK(ctl.remove_layer(pid, view, ""));
  CHECK(b.layers(view).size() == 1);
  CHECK((frame(view) == media::PixelSize{20, 20}));
  REQUIRE_OK(ctl.flatten(pid, view, flat));
  CHECK(pixel_at(flat, 8, 8)[0] > 200);
  CHECK(!ctl.remove_layer(pid, view, *l1).ok());
  // A new take puts layer 0 back, at the bottom.
  REQUIRE_OK(ctl.place_in_project(pid, *green));
  v = b.p().asset(view);
  REQUIRE(v->layers.size() == 2);
  CHECK(v->layers[0].id.empty() && v->layers[0].source == *green);
}

// A layer is cut where the CANVAS ends: on a project widened to the
// right (its frame the new size, layer 0 kept at the left), a layer moved
// a quarter-width right of the centre sits beside layer 0; three
// quarters, it is off the canvas.
TEST(composition, a_layer_is_cut_at_the_canvas_not_its_frame)
{
  Bench b("comp-canvas-cut");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 16, 16, {0, 0, 0xff, 0xff});
  REQUIRE(red && blue);
  auto placed = ctl.place_in_project(pid, *red);
  REQUIRE_OK(placed);
  const AssetId view = **placed;
  REQUIRE_OK(ctl.set_canvas(pid, view, {32, 16}, 0, 0));
  auto l1 = ctl.add_layer(pid, view);
  REQUIRE_OK(l1);
  REQUIRE_OK(ctl.place_in_project(pid, *blue, *l1));
  media::Crop right;
  right.content = {16, 16};
  right.offset_x = 0.25;
  REQUIRE_OK(ctl.set_crop(pid, view, right, *l1));
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.flatten(pid, view, flat));
  CHECK(pixel_at(flat, 8, 8)[0] > 200);
  CHECK(pixel_at(flat, 24, 8)[2] > 200);
  REQUIRE_OK(ctl.flatten(pid, view, flat, std::nullopt, *l1));
  CHECK(pixel_at(flat, 8, 8)[3] == 0);
  CHECK(pixel_at(flat, 24, 8)[2] > 200);
  right.offset_x = 0.75;
  REQUIRE_OK(ctl.set_crop(pid, view, right, *l1));
  REQUIRE_OK(ctl.flatten(pid, view, flat));
  CHECK(pixel_at(flat, 24, 8)[3] == 0);
}

// CANVAS SIZE resizes a composition's FRAME (DESIGN §10a): the frame is
// its size -- what the project's Information shows, what a generation on
// it is made at -- every layer kept where it lay, so it draws the same.
// One resized before (its frame kept, a canvas around it) is made so when
// it becomes the project.
TEST(composition, canvas_size_resizes_the_frame)
{
  Bench b("comp-frame");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 12, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 4, 4, {0, 0, 0xff, 0xff});
  REQUIRE(red && blue);
  auto comp = ctl.create_composition(pid, project::AssetClass::Still,
                                     {16, 12}, {}, "Doc");
  REQUIRE_OK(comp);
  // Red scaled 2x from the centre (the clip in the report); a small blue
  // one placed right of centre and up.
  auto lr = ctl.instantiate(pid, *red, *comp);
  auto lb = ctl.instantiate(pid, *blue, *comp);
  REQUIRE_OK(lr);
  REQUIRE_OK(lb);
  media::Crop twice;
  twice.content = {16, 12};
  twice.scale_x = twice.scale_y = 2;
  REQUIRE_OK(ctl.set_crop(pid, *comp, twice, lr->second));
  media::Crop off;
  off.content = {4, 4};
  off.offset_x = 0.25;
  off.offset_y = -0.25;
  REQUIRE_OK(ctl.set_crop(pid, *comp, off, lb->second));
  // As one was resized before: its frame kept, a 32 x 24 canvas round it
  // from the top left.
  REQUIRE_OK(b.p().set_canvas(*comp, media::resize_canvas(
      b.p().asset(*comp)->canvas, {16, 12}, {32, 24}, 0, 0)));
  const auto before = b.root / "before.png";
  REQUIRE_OK(ctl.flatten(pid, *comp, before));
  CHECK((b.p().asset(*comp)->canvas.frame() == media::PixelSize{16, 12}));
  // Made the project: the frame is its size, and it draws the same.
  REQUIRE_OK(ctl.set_project_composition(pid, *comp));
  auto own = ctl.own_frame(pid, *comp);
  REQUIRE_OK(own);
  CHECK((*own == media::PixelSize{32, 24}));
  const auto after = b.root / "after.png";
  REQUIRE_OK(ctl.flatten(pid, *comp, after));
  for (int y = 1; y < 24; y += 2) {
    for (int x = 1; x < 32; x += 2) {
      CHECK((pixel_at(before, x, y) == pixel_at(after, x, y)));
    }
  }
  CHECK(pixel_at(after, 4, 4)[0] > 200);    // red, where the frame was
  CHECK(pixel_at(after, 30, 22)[3] == 0);   // clear past it
  // Resized again, from the centre: the frame follows; a new take on
  // layer 0 lies on it, never sizing it again.
  REQUIRE_OK(ctl.set_canvas(pid, *comp, {40, 24}, 0.5, 0.5));
  CHECK((*ctl.own_frame(pid, *comp) == media::PixelSize{40, 24}));
  const auto wider = b.root / "wider.png";
  REQUIRE_OK(ctl.flatten(pid, *comp, wider));
  for (int y = 1; y < 24; y += 2) {
    for (int x = 1; x < 32; x += 2) {
      CHECK((pixel_at(after, x, y) == pixel_at(wider, x + 4, y)));
    }
  }
  REQUIRE_OK(ctl.place_in_project(pid, *blue));
  CHECK((*ctl.own_frame(pid, *comp) == media::PixelSize{40, 24}));
  // Undone, the take and then the resize -- with the places it moved.
  REQUIRE_OK(ctl.undo(pid));
  REQUIRE_OK(ctl.undo(pid));
  CHECK((*ctl.own_frame(pid, *comp) == media::PixelSize{32, 24}));
  const auto back = b.root / "back.png";
  REQUIRE_OK(ctl.flatten(pid, *comp, back));
  CHECK((pixel_at(back, 9, 7) == pixel_at(after, 9, 7)));
}

// MARKUP is an asset (DESIGN §6a): the toolbar draws on the markup a
// layer shows -- an empty selected layer gets a new one, the frame's
// size; else the top layer's; else a new layer -- strokes, objects made
// pixels, a mask, and two layers merged into a flat picture of what they
// showed together.
TEST(composition, markup_masks_and_merge)
{
  Bench b("comp-markup");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto pic = b.png("base.png", 64, 64, {0x80, 0x80, 0x80, 0xff});
  REQUIRE(pic);
  auto comp = ctl.derive_modified(pid, *pic);
  REQUIRE_OK(comp);
  auto markup_of = [&](const std::string& layer) {
    project::Markup out;
    for (const auto& l : b.layers(*comp)) {
      if (l.id == layer && l.source) {
        auto m = b.p().asset(*l.source);
        if (m.ok() && m->markup) {
          out = *m->markup;
        }
      }
    }
    return out;
  };
  auto shows_markup = [&](const std::string& layer) {
    for (const auto& l : b.layers(*comp)) {
      if (l.id == layer && l.source) {
        auto m = b.p().asset(*l.source);
        return m.ok() && m->cls == project::AssetClass::Markup;
      }
    }
    return false;
  };
  // Not on a flat picture.
  CHECK(!ctl.markup_layer(pid, *pic, {}).ok());
  auto t = ctl.markup_layer(pid, *comp, {""});
  REQUIRE_OK(t);
  CHECK(*t == "1" && shows_markup("1"));
  CHECK(markup_of("1").width == 64 && markup_of("1").height == 64);
  CHECK(*ctl.markup_layer(pid, *comp, {}) == "1");   // drawn on again
  auto e = ctl.add_layer(pid, *comp, "");
  REQUIRE_OK(e);
  CHECK(*ctl.markup_layer(pid, *comp, {*e}) == *e);
  CHECK(shows_markup(*e));
  REQUIRE_OK(ctl.remove_layer(pid, *comp, *e));

  media::Stroke s;
  s.points = {{4, 10}, {60, 10}};
  s.radius = 4;
  s.color = {1, 0, 0, 1};
  REQUIRE_OK(ctl.paint_stroke(pid, *comp, "1", s));
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 32, 10)[0] > 200 && pixel_at(flat, 32, 10)[1] < 40);
  CHECK(pixel_at(flat, 32, 40)[1] > 100);
  media::Stroke er = s;
  er.points = {{32, 4}, {32, 16}};
  er.erase = true;
  REQUIRE_OK(ctl.paint_stroke(pid, *comp, "1", er));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 32, 10)[1] > 100);
  CHECK(pixel_at(flat, 10, 10)[0] > 200);
  REQUIRE_OK(ctl.set_markup_objects(pid, *comp, "1", Json::array({
      {{"kind", "rect"}, {"x0", 20}, {"y0", 30}, {"x1", 44}, {"y1", 50},
       {"fill", {0, 0, 1, 1}}, {"width", 0}},
      {{"kind", "nonsense"}}})));
  auto objs = markup_of("1").objects;
  REQUIRE(objs.size() == 1);
  const auto id = objs[0]["id"].get<std::string>();
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 32, 40)[2] > 200);
  REQUIRE_OK(ctl.flatten(pid, *comp, flat, std::nullopt, std::nullopt,
                         {id}));
  CHECK(pixel_at(flat, 32, 40)[2] < 150);
  REQUIRE_OK(ctl.materialize_markup(pid, *comp, "1", {id}));
  CHECK(markup_of("1").objects.empty());
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 32, 40)[2] > 200);
  // The markup is an asset of its own, drawn as it is.
  for (const auto& l : b.layers(*comp)) {
    if (l.id == "1") {
      auto r = ctl.rendered(pid, *l.source);
      REQUIRE_OK(r);
      CHECK(pixel_at(*r, 10, 10)[0] > 200);
    }
  }

  auto m = ctl.add_layer(pid, *comp, "1");
  REQUIRE_OK(m);
  REQUIRE_OK(ctl.markup_layer(pid, *comp, {*m}));
  REQUIRE_OK(ctl.set_markup_objects(pid, *comp, *m, Json::array({
      {{"kind", "rect"}, {"x0", 0}, {"y0", 0}, {"x1", 32}, {"y1", 64},
       {"fill", {1, 1, 1, 1}}, {"width", 0}}})));
  CHECK(!ctl.set_layer_mask(pid, *comp, "", true).ok());
  REQUIRE_OK(ctl.set_layer_mask(pid, *comp, *m, true));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 10, 10)[0] > 200 && pixel_at(flat, 10, 10)[1] < 40);
  CHECK(pixel_at(flat, 54, 10)[1] > 100 && pixel_at(flat, 54, 10)[0] < 200);
  REQUIRE_OK(ctl.set_layer_visible(pid, *comp, *m, false));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 54, 10)[0] > 200);
  REQUIRE_OK(ctl.set_layer_visible(pid, *comp, *m, true));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  // Merging across the mask is refused; the mask with what it masks is
  // the same picture -- a flat one the lower layer shows.
  CHECK(!ctl.merge_layers(pid, *comp, "", *m).ok());
  CHECK(!ctl.merge_layers(pid, *comp, "", "1").ok());
  REQUIRE_OK(ctl.merge_layers(pid, *comp, "1", *m));
  const auto ls = b.layers(*comp);
  REQUIRE(ls.size() == 2);
  CHECK(!ls[1].mask && ls[1].source);
  CHECK(b.p().asset(*ls[1].source)->cls == project::AssetClass::Flat);
  const auto merged = b.root / "merged.png";
  REQUIRE_OK(ctl.flatten(pid, *comp, merged));
  for (auto [x, y] : {std::pair{10, 10}, {54, 10}, {32, 40}}) {
    const auto a1 = pixel_at(flat, x, y), a2 = pixel_at(merged, x, y);
    for (int k = 0; k < 4; ++k) {
      CHECK(std::abs(a1[k] - a2[k]) <= 2);
    }
  }
}

// ARRANGE (media::arrange_objects): objects up and down their layer's
// order -- a run together -- their bounds' edges and middles lined up
// (one alone on the canvas), three spaced evenly; one command each,
// undone whole.
TEST(composition, markup_objects_are_arranged)
{
  const Json objs = Json::array({
      {{"id", "a"}, {"kind", "rect"}, {"x0", 10}, {"y0", 10}, {"x1", 30},
       {"y1", 20}, {"width", 0}},
      {{"id", "b"}, {"kind", "rect"}, {"x0", 50}, {"y0", 40}, {"x1", 90},
       {"y1", 60}, {"width", 4}},
      {{"id", "c"}, {"kind", "polygon"}, {"x0", 120}, {"y0", 5},
       {"x1", 140}, {"y1", 95}, {"width", 0},
       {"points", {{0.5, 0}, {1, 1}, {0, 1}}},
       {"gradient", {{"kind", "radial"}, {"from", {0.2, 0.5}}}}},
      {{"id", "d"}, {"kind", "text"}, {"box", true}, {"x0", 150},
       {"y0", 70}, {"x1", 190}, {"y1", 90}, {"text", "d"}}});
  const media::PixelSize canvas{200, 100};
  auto order = [](const Json& os) {
    std::string s;
    for (const auto& o : os) {
      s += o["id"].get<std::string>();
    }
    return s;
  };
  auto arranged = [&](const Json& os, std::vector<std::string> ids,
                      const std::string& how) -> Json {
    auto r = media::arrange_objects(os, ids, how, canvas);
    CHECK(r.ok());
    return r.ok() ? *r : Json();
  };
  auto by_id = [](const Json& os, const std::string& id) {
    for (const auto& o : os) {
      if (o["id"] == id) {
        return o;
      }
    }
    return Json();
  };
  // The order.
  const Json up = arranged(objs, {"a", "c"}, "up");
  CHECK(order(up) == "badc");
  CHECK(order(arranged(up, {"a", "c"}, "down")) == "abcd");
  CHECK(order(arranged(objs, {"a", "c"}, "to-top")) == "bdac");
  CHECK(order(arranged(objs, {"c"}, "to-bottom")) == "cabd");
  CHECK(order(arranged(objs, {"c", "d"}, "up")) == "abcd");   // on top
  CHECK(order(arranged(objs, {"b", "a"}, "down")) == "abcd");
  // Lined up, by their bounds (b's outline 2 px round its box).
  const Json left = arranged(objs, {"a", "b"}, "left");
  CHECK(by_id(left, "a")["x0"] == 10);
  CHECK(by_id(left, "b")["x0"] == 12 && by_id(left, "b")["x1"] == 52);
  CHECK(by_id(left, "c") == by_id(objs, "c"));
  const Json right = arranged(objs, {"a", "b"}, "right");
  CHECK(by_id(right, "a")["x1"] == 92 && by_id(right, "b")["x0"] == 50);
  const Json mid = arranged(objs, {"a", "b"}, "middle");
  CHECK(by_id(mid, "a")["y0"] == 31 && by_id(mid, "b")["y0"] == 26);
  CHECK(by_id(mid, "a")["x0"] == 10);
  // One alone: on the canvas; its corners and gradient in its box's
  // units, moved with it.
  const Json alone = arranged(objs, {"c"}, "center");
  CHECK(by_id(alone, "c")["x0"] == 90 && by_id(alone, "c")["x1"] == 110);
  CHECK(by_id(alone, "c")["points"] == by_id(objs, "c")["points"]);
  CHECK(by_id(alone, "c")["gradient"] == by_id(objs, "c")["gradient"]);
  CHECK(by_id(arranged(objs, {"d"}, "bottom"), "d")["y1"] == 100);
  // Spaced: a, b, c by their middles; a and c stay, 23 px between.
  const Json spaced = arranged(objs, {"c", "a", "b"}, "distribute-h");
  CHECK(by_id(spaced, "a")["x0"] == 10);
  CHECK(by_id(spaced, "b")["x0"] == 55);
  CHECK(by_id(spaced, "c")["x0"] == 120);
  CHECK(arranged(objs, {"a", "b"}, "distribute-v") == objs);  // two
  CHECK(!media::arrange_objects(objs, {"a"}, "sideways", canvas).ok());
  // A text without a box: its lines as set.
  const auto tb = media::object_bounds(
      {{"kind", "text"}, {"x0", 5}, {"y0", 6}, {"text", "Hi\nthere"},
       {"font", {{"family", "Helvetica"}, {"size", 20}}}});
  CHECK(tb[0] == 5 && tb[1] == 6);
  CHECK(tb[2] > 30 && tb[2] < 80 && tb[3] > 36 && tb[3] < 60);

  // On a project: one command, of its kind; undone whole.
  Bench b("comp-arrange");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto pic = b.png("base.png", 200, 100, {0x80, 0x80, 0x80, 0xff});
  REQUIRE(pic);
  auto comp = ctl.derive_modified(pid, *pic);
  REQUIRE_OK(comp);
  auto layer = ctl.markup_layer(pid, *comp, {""});
  REQUIRE_OK(layer);
  REQUIRE_OK(ctl.set_markup_objects(pid, *comp, *layer, objs));
  auto objects = [&] {
    for (const auto& l : b.layers(*comp)) {
      if (l.id == *layer && l.source) {
        auto m = b.p().asset(*l.source);
        if (m.ok() && m->markup) {
          return m->markup->objects;
        }
      }
    }
    return Json();
  };
  auto undo_kind = [&] {
    return jget<std::string>(
        jget(ctl.project_state(pid), "undo", Json::object()), "kind", "");
  };
  REQUIRE_OK(ctl.arrange_markup(pid, *comp, *layer, {"a", "c"}, "to-top"));
  CHECK(order(objects()) == "bdac" && undo_kind() == "markup.arrange");
  REQUIRE_OK(ctl.arrange_markup(pid, *comp, *layer, {"a", "b", "c"},
                                "top"));
  CHECK(by_id(objects(), "b")["y0"] == 7 && undo_kind() == "markup.align");
  REQUIRE_OK(ctl.arrange_markup(pid, *comp, *layer, {"a", "b", "c"},
                                "distribute-h"));
  CHECK(undo_kind() == "markup.distribute");
  REQUIRE_OK(ctl.undo(pid));
  REQUIRE_OK(ctl.undo(pid));
  CHECK(by_id(objects(), "b")["y0"] == 40);
  REQUIRE_OK(ctl.undo(pid));
  CHECK(order(objects()) == "abcd");
  CHECK(!ctl.arrange_markup(pid, *comp, *layer, {"a"}, "sideways").ok());
}

// TIME (media/timing.h): a layer's source time through its marks and a
// speed ramp, its natural length, the transitions' weights, the
// player's segments and volume ramps.
TEST(composition, layer_time_speed_and_transitions)
{
  using namespace media;
  // 24 fps; marks 24..71 of a 4 s clip at 24: two seconds of source.
  LayerTiming t = resolve_timing(48, 0, 24, 71, {24, 1}, {24, 1}, 4.0, {});
  CHECK(std::abs(t.start - 2.0) < 1e-9 && std::abs(t.in - 1.0) < 1e-9);
  CHECK(std::abs(t.length - 2.0) < 1e-9);
  CHECK(std::abs(t.source_at(3.0) - 2.0) < 1e-9);
  CHECK(t.active(2.0) && !t.active(4.0) && !t.active(1.9));
  // Speed 1 to 3 over its first second: 2 seconds of source in that
  // second; the rest at 3.
  KeyedSpeed ramp;
  ramp.keys = {{0, Speed{1}}, {24, Speed{3}}};
  LayerTiming r = resolve_timing(0, 0, -1, -1, {24, 1}, {24, 1}, 8.0, ramp);
  CHECK(std::abs(r.source_at_local(1.0) - 2.0) < 1e-9);
  CHECK(std::abs(r.source_at_local(2.0) - 5.0) < 1e-9);
  CHECK(std::abs(r.length - 3.0) < 1e-6);      // 8 s: 2 + 3 x 2
  CHECK(std::abs(r.local_at_source(5.0) - 2.0) < 1e-6);
  // A picture runs to the end; a duration of its own ends it.
  LayerTiming pic = resolve_timing(0, 0, -1, -1, {}, {24, 1}, 0, {});
  CHECK(!std::isfinite(pic.length));
  LayerTiming pic2 = resolve_timing(12, 24, -1, -1, {}, {24, 1}, 0, {});
  CHECK(std::abs(pic2.end() - 1.5) < 1e-9);
  CHECK(std::abs(timeline_end({t, pic, pic2}) - 4.0) < 1e-9);

  // Two layers overlapping from 1 to 2 s.
  LayerTiming a = resolve_timing(0, 48, -1, -1, {}, {24, 1}, 0, {});
  LayerTiming bb = resolve_timing(24, 48, -1, -1, {}, {24, 1}, 0, {});
  const std::vector<LayerTiming> two = {a, bb};
  using TK = TransitionSpec::Kind;
  auto w = layer_weights(two, {{0, 1, TK::Cut}}, 1.4);   // cut: a still on
  CHECK(w.opacity[0] == 1 && w.opacity[1] == 0);
  w = layer_weights(two, {{0, 1, TK::Cut}}, 1.6);        // after the middle
  CHECK(w.opacity[0] == 0 && w.opacity[1] == 1);
  w = layer_weights(two, {{0, 1, TK::Dissolve}}, 1.25);  // a dissolve
  CHECK(w.opacity[0] == 1 && std::abs(w.opacity[1] - 0.25) < 1e-9);
  CHECK(std::abs(w.gain[0] - 0.75) < 1e-9 &&
        std::abs(w.gain[1] - 0.25) < 1e-9);
  CHECK(w.dip[0] == 0 && w.dip[1] == 0);
  w = layer_weights(two, {}, 1.5);                     // none: they stack
  CHECK(w.opacity[0] == 1 && w.opacity[1] == 1);
  // A dip: the first to black by the middle, alone, its sound going; the
  // second out of white after it.
  w = layer_weights(two, {{0, 1, TK::DipBlack}}, 1.25);
  CHECK(w.opacity[0] == 1 && w.opacity[1] == 0);
  CHECK(std::abs(w.dip[0] - 0.5) < 1e-9 && w.dip_level[0] == 0);
  CHECK(std::abs(w.gain[0] - 0.5) < 1e-9 && w.gain[1] == 0);
  w = layer_weights(two, {{0, 1, TK::DipWhite}}, 1.75);
  CHECK(w.opacity[0] == 0 && w.opacity[1] == 1);
  CHECK(std::abs(w.dip[1] - 0.5) < 1e-9 && w.dip_level[1] == 1);
  CHECK(w.gain[0] == 0 && std::abs(w.gain[1] - 0.5) < 1e-9);
  CHECK(transition_kind("dip-white") == TK::DipWhite);
  CHECK(!transition_kind("wipe"));

  // The player's pieces: one at a constant rate; a ramp, a frame each.
  auto segs = time_segments(t, 10, 1.0 / 24);
  REQUIRE(segs.size() == 1);
  CHECK(std::abs(segs[0].at - 2) < 1e-9 && std::abs(segs[0].source - 1) <
        1e-9 && std::abs(segs[0].source_length - 2) < 1e-9);
  CHECK(time_segments(r, 10, 1.0 / 24).size() > 20);
  // Volume ramps: keys times the transitions' weights.
  SoundPlan plan;
  plan.seconds = 3;
  plan.layers = {{"a.wav", a, {}}, {"b.wav", bb, {}}};
  plan.layers[1].sound.keys = {{0, Sound{0.5, 0}}};
  plan.transitions = {{0, 1, TK::Dissolve}};
  auto pts = volume_points(plan, 1);
  REQUIRE(pts.size() >= 3);
  CHECK(std::abs(pts.front().second) < 1e-9);         // faded in from 0
  CHECK(std::abs(pts.back().second - 0.5) < 1e-6);    // its own volume
  CHECK(!needs_render(plan));
  plan.layers[1].sound.keys = {{0, Sound{1, 3}}};
  CHECK(needs_render(plan));
}

// A TIMELINE drawn in time: pictures with their own spans, a dissolve
// and a cut between them, frame by frame; its SOUND, the layers' spans
// at their volumes, mixed.
TEST(composition, a_timeline_draws_and_sounds_in_time)
{
  Bench b("comp-timeline");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 16, 16, {0, 0, 0xff, 0xff});
  REQUIRE(red && blue);
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {16, 16}, {24, 1}, "timeline");
  REQUIRE_OK(tl);
  auto lr = ctl.instantiate(pid, *red, *tl);
  auto lb = ctl.instantiate(pid, *blue, *tl, std::nullopt, 12);
  REQUIRE_OK(lr);
  REQUIRE_OK(lb);
  project::LayerTime tr;
  tr.duration = 24;
  REQUIRE_OK(ctl.set_layer_time(pid, *tl, lr->second, tr));
  project::LayerTime tb;
  tb.offset = 12;
  tb.duration = 24;
  REQUIRE_OK(ctl.set_layer_time(pid, *tl, lb->second, tb));
  auto n = ctl.composition_length(pid, *tl);
  REQUIRE_OK(n);
  CHECK(*n == 36);
  auto frame_at = [&](std::int64_t f) {
    auto m = ctl.movie_stack(pid, *tl, true);
    const auto out = b.root / std::format("f{}.png", f);
    if (!m.ok() || !media::write_stack_frame(*m, {24, 1}, f, out).ok()) {
      return std::array<int, 4>{-1, -1, -1, -1};
    }
    return pixel_at(out, 8, 8);
  };
  // No transition: blue simply over red where they overlap.
  CHECK(frame_at(6)[0] > 200);
  CHECK(frame_at(18)[2] > 200 && frame_at(18)[0] < 40);
  REQUIRE_OK(ctl.set_transition(pid, *tl, lr->second, lb->second,
                                "dissolve"));
  // Half way: half and half -- mixed in linear light, so each is 0.5
  // linear, 188 in sRGB.
  const auto mid = frame_at(18);
  CHECK(std::abs(mid[0] - 188) <= 6 && std::abs(mid[2] - 188) <= 6);
  CHECK(mid[3] == 255);
  CHECK(frame_at(30)[2] > 200);
  REQUIRE_OK(ctl.set_transition(pid, *tl, lr->second, lb->second, "cut"));
  CHECK(frame_at(17)[0] > 200);
  CHECK(frame_at(18)[2] > 200);
  // Undo takes the transition back.
  REQUIRE_OK(ctl.undo(pid));
  CHECK(b.p().asset(*tl)->transitions.front().kind == "dissolve");

  // Sound: a tone, and another from half a second at half volume.
  REQUIRE(write_tone(b.root / "a.wav", 1, 440, 0.5));
  REQUIRE(write_tone(b.root / "b.wav", 1, 440, 0.5));
  auto ta = import_and_wait(ctl, pid, b.root / "a.wav");
  auto tb2 = import_and_wait(ctl, pid, b.root / "b.wav");
  REQUIRE(ta && tb2);
  auto snd = ctl.create_composition(pid, project::AssetClass::Composition,
                                    {0, 0}, {}, "sound");
  REQUIRE_OK(snd);
  REQUIRE_OK(ctl.instantiate(pid, *ta, *snd));
  auto sb = ctl.instantiate(pid, *tb2, *snd, std::nullopt, 500);
  REQUIRE_OK(sb);
  media::KeyedSound half;
  half.keys = {{0, media::Sound{0.5, 0}}};
  REQUIRE_OK(ctl.set_sound_keys(pid, *snd, half, sb->second));
  auto mix = ctl.sound_mix(pid, *snd);
  REQUIRE_OK(mix);
  REQUIRE(!mix->empty());
  const auto s = read_sound(*mix);
  CHECK(std::abs(static_cast<double>(s.size()) / 48000 - 1.5) < 0.01);
  CHECK(std::abs(rms(s, 0.1, 0.4) - 0.354) < 0.03);   // a alone
  CHECK(std::abs(rms(s, 0.6, 0.9) - 0.530) < 0.05);   // a + b / 2
  CHECK(std::abs(rms(s, 1.1, 1.4) - 0.177) < 0.03);   // b / 2
  // Drawn as a file: the sound composition is its mix.
  auto file = ctl.rendered(pid, *snd);
  REQUIRE_OK(file);
  CHECK(*file == *mix);
  // Half speed on a: 2 s of it.
  media::KeyedSpeed slow;
  slow.keys = {{0, media::Speed{0.5}}};
  REQUIRE_OK(ctl.set_speed_keys(pid, *snd, slow, ""));
  auto longer = ctl.composition_length(pid, *snd);
  REQUIRE_OK(longer);
  CHECK(*longer == 2000);
}

// A DIP (DESIGN §6a): where two layers overlap, the first goes to black
// (or white) by the middle and the second comes out of it -- each alone,
// mixed in linear light; a layer's transparent part stays so.
TEST(composition, a_dip_goes_through_black_or_white)
{
  Bench b("comp-dip");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 16, 16, {0, 0, 0xff, 0xff});
  auto glass = b.png("glass.png", 16, 16, {0, 0xff, 0, 0x80});
  REQUIRE(red && blue && glass);
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {16, 16}, {24, 1}, "timeline");
  REQUIRE_OK(tl);
  // Red 0-24, blue 12-36: the overlap 12-24, its middle 18.
  auto lr = ctl.instantiate(pid, *red, *tl);
  auto lb = ctl.instantiate(pid, *blue, *tl, std::nullopt, 12);
  REQUIRE_OK(lr);
  REQUIRE_OK(lb);
  project::LayerTime tr;
  tr.duration = 24;
  REQUIRE_OK(ctl.set_layer_time(pid, *tl, lr->second, tr));
  project::LayerTime tb;
  tb.offset = 12;
  tb.duration = 24;
  REQUIRE_OK(ctl.set_layer_time(pid, *tl, lb->second, tb));
  auto frame_at = [&](std::int64_t f) {
    auto m = ctl.movie_stack(pid, *tl, true);
    const auto out = b.root / std::format("f{}.png", f);
    if (!m.ok() || !media::write_stack_frame(*m, {24, 1}, f, out).ok()) {
      return std::array<int, 4>{-1, -1, -1, -1};
    }
    return pixel_at(out, 8, 8);
  };
  auto near = [](std::array<int, 4> p, int r, int g, int bl) {
    return std::abs(p[0] - r) <= 6 && std::abs(p[1] - g) <= 6 &&
           std::abs(p[2] - bl) <= 6;
  };
  REQUIRE_OK(ctl.set_transition(pid, *tl, lr->second, lb->second,
                                "dip-black"));
  CHECK(near(frame_at(6), 255, 0, 0));
  CHECK(near(frame_at(15), 188, 0, 0));    // half way to black (linear)
  CHECK(near(frame_at(18), 0, 0, 0));      // black: blue out of it
  CHECK(near(frame_at(21), 0, 0, 188));
  CHECK(near(frame_at(30), 0, 0, 255));
  REQUIRE_OK(ctl.set_transition(pid, *tl, lr->second, lb->second,
                                "dip-white"));
  CHECK(near(frame_at(15), 255, 188, 188));
  CHECK(near(frame_at(18), 255, 255, 255));
  CHECK(near(frame_at(21), 188, 188, 255));
  CHECK(b.p().asset(*tl)->transitions.front().kind == "dip-white");
  CHECK(!ctl.set_transition(pid, *tl, lr->second, lb->second, "wipe")
             .ok());
  // Changed, a transition keeps its place among the others (the
  // timeline's arrows); "" takes it away.
  auto lg = ctl.instantiate(pid, *red, *tl, std::nullopt, 30);
  REQUIRE_OK(lg);
  REQUIRE_OK(ctl.set_transition(pid, *tl, lb->second, lg->second,
                                "dissolve"));
  REQUIRE_OK(ctl.set_transition(pid, *tl, lb->second, lr->second,
                                "dip-black"));
  {
    const auto ts = b.p().asset(*tl)->transitions;
    REQUIRE(ts.size() == 2);
    CHECK(ts[0].kind == "dip-black" && ts[0].from == lb->second);
    CHECK(ts[1].kind == "dissolve");
  }
  REQUIRE_OK(ctl.set_transition(pid, *tl, lb->second, lr->second,
                                "dip-white"));
  REQUIRE_OK(ctl.set_transition(pid, *tl, lb->second, lg->second, ""));
  {
    const auto ts = b.p().asset(*tl)->transitions;
    REQUIRE(ts.size() == 1);
    CHECK(ts[0].kind == "dip-white");
  }
  REQUIRE_OK(ctl.set_transition(pid, *tl, lr->second, lb->second,
                                "dip-white"));
  // Through white, a half-clear layer stays half clear: white at half,
  // over the black a frame is drawn on.
  REQUIRE_OK(ctl.set_layer_source(pid, *tl, lb->second, *glass));
  CHECK(near(frame_at(18), 188, 188, 188));
}

// The timeline's SCISSORS (DESIGN §10a Timeline): a raw clip cut in two
// PARTS, each a composition of one layer showing its span of the source,
// on the layer and one right above it -- the same frames where they
// were, its look on both; a part cut again from its source; refused
// where it cannot be cut. And a layer SLID along the timeline, a length
// set grown to hold it.
TEST(composition, a_clip_is_cut_in_two_parts)
{
  Bench b("comp-split");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  // A raw clip: two seconds at 24, a movie imported.
  auto shot = ctl.create_composition(pid, project::AssetClass::Composition,
                                     {16, 16}, {24, 1}, "shot");
  REQUIRE_OK(shot);
  auto in_shot = ctl.instantiate(pid, *red, *shot);
  REQUIRE_OK(in_shot);
  project::LayerTime two;
  two.duration = 48;
  REQUIRE_OK(ctl.set_layer_time(pid, *shot, in_shot->second, two));
  auto movie = ctl.rendered(pid, *shot);
  REQUIRE_OK(movie);
  fs::copy_file(*movie, b.root / "shot.mov");
  auto clip = import_and_wait(ctl, pid, b.root / "shot.mov");
  REQUIRE(clip);
  REQUIRE(b.p().asset(*clip)->kind == project::AssetKind::Video);
  // On a timeline from frame 12, marked in at its frame 6: frames 12-53.
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {16, 16}, {24, 1}, "edit");
  REQUIRE_OK(tl);
  auto placed = ctl.instantiate(pid, *clip, *tl, std::nullopt, 12);
  REQUIRE_OK(placed);
  const std::string l = placed->second;
  project::LayerTime marked;
  marked.in = 6;
  marked.rate = {24, 1};
  marked.offset = 12;
  REQUIRE_OK(ctl.set_layer_time(pid, *tl, l, marked));
  media::KeyedAdjustments look;
  look.keys = {{0, media::Adjustments{}}};
  look.keys[0].value.exposure = 0.5;
  REQUIRE_OK(ctl.set_adjustment_keys(pid, *tl, look, l));
  auto len = ctl.composition_length(pid, *tl);
  REQUIRE_OK(len);
  CHECK(*len == 54);
  // A frame from either end, and outside it: refused.
  auto edge = ctl.split_layer(pid, *tl, l, 12);
  CHECK(!edge.ok() && edge.error().key == msg::kSplitOutside.key);
  CHECK(!ctl.split_layer(pid, *tl, l, 54).ok());
  CHECK(!ctl.split_layer(pid, *tl, l, 70).ok());
  // Cut at 30: its source's frame 24.
  auto r = ctl.split_layer(pid, *tl, l, 30);
  REQUIRE_OK(r);
  auto ls = b.layers(*tl);
  REQUIRE(ls.size() == 2);
  CHECK(ls[0].id == l && ls[0].source == r->first);
  CHECK(ls[0].time.offset == 12 && ls[0].time.in < 0);
  CHECK(ls[1].id == r->layer && ls[1].source == r->second);
  CHECK(ls[1].time.offset == 30);
  auto p1 = b.layers(r->first);
  auto p2 = b.layers(r->second);
  REQUIRE(p1.size() == 1 && p2.size() == 1);
  CHECK(p1[0].source == *clip && p1[0].time.in == 6 &&
        p1[0].time.out == 23);
  CHECK(p2[0].source == *clip && p2[0].time.in == 24 &&
        p2[0].time.out < 0);
  CHECK(b.p().asset(r->first)->name.ends_with(" · 1"));
  CHECK(b.p().asset(r->second)->kind == project::AssetKind::Video);
  // Where they were: 12-29 and 30-53; the look on both.
  auto m = ctl.movie_stack(pid, *tl, true);
  REQUIRE_OK(m);
  REQUIRE(m->layers.size() == 2);
  CHECK(std::abs(m->layers[0].timing.end() - 30.0 / 24) < 1e-6);
  CHECK(std::abs(m->layers[1].timing.start - 30.0 / 24) < 1e-6);
  CHECK(std::abs(m->layers[1].timing.end() - 54.0 / 24) < 1e-6);
  CHECK(std::abs(m->layers[1].adjust.at(0).exposure - 0.5) < 1e-9);
  CHECK(m->layers[1].id == r->layer);
  len = ctl.composition_length(pid, *tl);
  REQUIRE_OK(len);
  CHECK(*len == 54);
  // The second part cut again, at 40: from the clip itself, 24-33, 34-.
  auto again = ctl.split_layer(pid, *tl, r->layer, 40);
  REQUIRE_OK(again);
  auto q1 = b.layers(again->first);
  auto q2 = b.layers(again->second);
  REQUIRE(q1.size() == 1 && q2.size() == 1);
  CHECK(q1[0].source == *clip && q1[0].time.in == 24 &&
        q1[0].time.out == 33);
  CHECK(q2[0].source == *clip && q2[0].time.in == 34);
  CHECK(b.layers(*tl).size() == 3);
  CHECK(b.layers(*tl)[2].time.offset == 40);
  // Keyed along it: refused. A picture: refused.
  media::KeyedAdjustments ramp = look;
  ramp.keys.push_back({10, media::Adjustments{}});
  REQUIRE_OK(ctl.set_adjustment_keys(pid, *tl, ramp, l));
  auto keyed = ctl.split_layer(pid, *tl, l, 20);
  CHECK(!keyed.ok() && keyed.error().key == msg::kSplitKeyed.key);
  auto pic = ctl.instantiate(pid, *red, *tl);
  REQUIRE_OK(pic);
  auto still = ctl.split_layer(pid, *tl, pic->second, 20);
  CHECK(!still.ok() && still.error().key == msg::kSplitNeedsClip.key);
  // Undone a command at a time: one layer showing the clip, marked.
  for (int i = 0; i < 4; ++i) {
    REQUIRE_OK(ctl.undo(pid));
  }
  ls = b.layers(*tl);
  REQUIRE(ls.size() == 1);
  CHECK(ls[0].source == *clip && ls[0].time.in == 6);
  // Slid along: from 60, the length following its content (to 102); a
  // length set grows to hold it.
  REQUIRE_OK(ctl.slide_layer(pid, *tl, l, 60));
  CHECK(b.layers(*tl)[0].time.offset == 60);
  len = ctl.composition_length(pid, *tl);
  REQUIRE_OK(len);
  CHECK(*len == 102);
  REQUIRE_OK(ctl.set_timeline(pid, *tl, 110));
  REQUIRE_OK(ctl.slide_layer(pid, *tl, l, 80));
  CHECK(b.p().asset(*tl)->timeline_frames == 122);
  REQUIRE_OK(ctl.undo(pid));
  CHECK(b.p().asset(*tl)->timeline_frames == 110);
  CHECK(b.layers(*tl)[0].time.offset == 60);

  // A sound cut at 400 ms: two sounds, the mix as long as before.
  REQUIRE(write_tone(b.root / "tone.wav", 1, 440, 0.5));
  auto tone = import_and_wait(ctl, pid, b.root / "tone.wav");
  REQUIRE(tone);
  auto snd = ctl.create_composition(pid, project::AssetClass::Composition,
                                    {0, 0}, {}, "sound");
  REQUIRE_OK(snd);
  auto sl = ctl.instantiate(pid, *tone, *snd);
  REQUIRE_OK(sl);
  auto sr = ctl.split_layer(pid, *snd, sl->second, 400);
  REQUIRE_OK(sr);
  CHECK(b.p().asset(sr->first)->kind == project::AssetKind::Audio);
  auto s1 = b.layers(sr->first);
  auto s2 = b.layers(sr->second);
  REQUIRE(s1.size() == 1 && s2.size() == 1);
  CHECK(s1[0].time.in < 0 && s1[0].time.out == 399);
  CHECK(s2[0].time.in == 400 && s2[0].time.out < 0);
  auto slen = ctl.composition_length(pid, *snd);
  REQUIRE_OK(slen);
  CHECK(*slen == 1000);
  auto mix = ctl.sound_mix(pid, *snd);
  REQUIRE_OK(mix);
  const auto samples = read_sound(*mix);
  CHECK(std::abs(static_cast<double>(samples.size()) / 48000 - 1.0) < 0.01);
  CHECK(std::abs(rms(samples, 0.3, 0.6) - 0.354) < 0.03);  // across it
}

// A clip SUMMARIZED (DESIGN §4i): read sparse -- the frame at each mark,
// upright, sRGB, 8-bit planar, at a size within 576 x 320 on a 32-pixel
// grid, with its time -- and asked of a clip only, by a model that watches
// video (none here: no engine). The frame interval: one a second where
// memory moves 200 GB/s or more, one every two below, or as set.
TEST(composition, a_clip_is_read_sparse_for_a_summary)
{
  ::setenv("VALTZ_MEMORY_BANDWIDTH_GBS", "150", 1);
  Bench b("comp-summary");
  ::unsetenv("VALTZ_MEMORY_BANDWIDTH_GBS");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 640, 360, {0xff, 0, 0, 0xff});
  auto blue = b.png("blue.png", 640, 360, {0, 0, 0xff, 0xff});
  REQUIRE(red && blue);
  // Red for a second, blue for the next.
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {640, 360}, {24, 1}, "two");
  REQUIRE_OK(tl);
  auto lr = ctl.instantiate(pid, *red, *tl);
  auto lb = ctl.instantiate(pid, *blue, *tl);
  REQUIRE_OK(lr);
  REQUIRE_OK(lb);
  project::LayerTime tr;
  tr.duration = 24;
  REQUIRE_OK(ctl.set_layer_time(pid, *tl, lr->second, tr));
  project::LayerTime tb;
  tb.offset = 24;
  tb.duration = 24;
  REQUIRE_OK(ctl.set_layer_time(pid, *tl, lb->second, tb));
  auto movie = ctl.rendered(pid, *tl);
  REQUIRE_OK(movie);

  auto size = media::sampled_size(*movie, {576, 320}, 32);
  REQUIRE_OK(size);
  CHECK(size->width == 576);
  CHECK(size->height == 320);
  // The most is a long edge and a short, in either order.
  {
    auto upright = media::sampled_size(*movie, {320, 576}, 32);
    REQUIRE_OK(upright);
    CHECK(upright->width == 576);
  }
  std::vector<double> times;
  std::vector<std::array<int, 3>> colours;
  std::vector<std::uint8_t> buf(
      static_cast<std::size_t>(size->width) * size->height * 3);
  const std::size_t plane = static_cast<std::size_t>(size->width) *
                            size->height;
  auto st = media::sample_movie(
      *movie, 0.5, *size,
      [&](std::int64_t, double t) -> Result<media::FrameBuffer> {
        times.push_back(t);
        return media::FrameBuffer{buf.data(), buf.size(), {}};
      },
      [&](std::int64_t) -> Status {
        const std::size_t mid = plane / 2 + size->width / 2;
        colours.push_back({buf[mid], buf[plane + mid], buf[2 * plane + mid]});
        return ok_status();
      });
  REQUIRE_OK(st);
  REQUIRE(times.size() == 4);
  for (std::size_t i = 0; i < times.size(); ++i) {
    CHECK(std::abs(times[i] - 0.5 * static_cast<double>(i)) < 0.05);
  }
  CHECK(colours[0][0] > 200 && colours[0][2] < 50);  // red
  CHECK(colours[3][2] > 200 && colours[3][0] < 50);  // blue

  // A picture is not summarized; a clip is, by a model that watches.
  auto summarize = [&](AssetId a) {
    Controller::AssetOpRequest req;
    req.project = pid;
    req.asset = a;
    req.operation = "summarize";
    return ctl.run_asset_operation(std::move(req));
  };
  auto pic = summarize(*red);
  REQUIRE(!pic.ok());
  CHECK(pic.error().key == msg::kSummarizeNeedsPicture.key);
  auto none = summarize(*tl);
  REQUIRE(!none.ok());
  CHECK(none.error().key == msg::kNoVideoWatcher.key);

  // 150 GB/s: a frame every two seconds; as set, then Auto again --
  // and kept for the next run.
  CHECK(ctl.hardware().memory_bandwidth_gbs == 150);
  CHECK(ctl.video_every() == 2.0);
  REQUIRE_OK(ctl.set_video_every(3));
  CHECK(ctl.video_every() == 3.0);
  CHECK(jget(ctl.assistants(), "video_every_now", 0.0) == 3.0);
  {
    ControllerConfig cfg;
    cfg.support_root = b.root / "support";
    cfg.model_roots = {b.root / "models"};
    cfg.with_engine = false;
    auto again = Controller::create(cfg);
    REQUIRE_OK(again);
    CHECK((*again)->video_every_setting() == 3.0);
  }
  REQUIRE_OK(ctl.set_video_every(0));
  CHECK(ctl.video_every() == 2.0);
}

// A CLIP'S SOUND (DESIGN §6a): a composition of sound takes a clip with
// sound -- its sound alone, marked in milliseconds -- and refuses one
// without; put in a timeline with a frame, it is sound there, and its
// clips' pictures stay out (decompose refused).
TEST(composition, a_sound_composition_takes_a_clips_sound)
{
  Bench b("comp-clip-sound");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  REQUIRE(write_tone(b.root / "tone.wav", 2, 440, 0.5));
  auto tone = import_and_wait(ctl, pid, b.root / "tone.wav");
  REQUIRE(tone);
  // A clip with sound, and one without: two seconds each, imported.
  const auto movie = [&](bool sound, const char* name)
      -> std::optional<AssetId> {
    auto shot = ctl.create_composition(
        pid, project::AssetClass::Composition, {16, 16}, {24, 1}, name);
    if (!shot.ok()) {
      return std::nullopt;
    }
    auto pic = ctl.instantiate(pid, *red, *shot);
    project::LayerTime two;
    two.duration = 48;
    if (!pic.ok() ||
        !ctl.set_layer_time(pid, *shot, pic->second, two).ok() ||
        (sound && !ctl.instantiate(pid, *tone, *shot).ok())) {
      return std::nullopt;
    }
    auto file = ctl.rendered(pid, *shot);
    if (!file.ok()) {
      return std::nullopt;
    }
    const auto to = b.root / (std::string(name) + ".mov");
    fs::copy_file(*file, to);
    return import_and_wait(ctl, pid, to);
  };
  auto loud = movie(true, "loud");
  auto mute = movie(false, "mute");
  REQUIRE(loud && mute);
  auto snd = ctl.create_composition(pid, project::AssetClass::Composition,
                                    {0, 0}, {}, "sound");
  REQUIRE_OK(snd);
  auto none = ctl.instantiate(pid, *mute, *snd, std::string());
  CHECK(!none.ok() && none.error().key == msg::kClipHasNoSound.key);
  CHECK(!ctl.instantiate(pid, *red, *snd).ok());
  // Dropped on the stage of the blank composition, which names its layer
  // 0 though it has none yet: its first layer.
  auto in = ctl.instantiate(pid, *loud, *snd, std::string());
  REQUIRE_OK(in);
  // Its sound alone: no picture to draw, two seconds of the tone.
  auto m = ctl.movie_stack(pid, *snd, true);
  REQUIRE_OK(m);
  REQUIRE(m->layers.size() == 1);
  CHECK(!m->layers[0].video && m->layers[0].audio_only);
  auto len = ctl.composition_length(pid, *snd);
  REQUIRE_OK(len);
  CHECK(std::abs(*len - 2000) <= 1);
  auto mix = ctl.sound_mix(pid, *snd);
  REQUIRE_OK(mix);
  REQUIRE(!mix->empty());
  const auto samples = read_sound(*mix);
  CHECK(std::abs(static_cast<double>(samples.size()) / 48000 - 2) < 0.02);
  CHECK(std::abs(rms(samples, 0.5, 1.5) - 0.354) < 0.03);
  // Marked in milliseconds: from 500 to its end, 1.5 s.
  project::LayerTime half;
  half.in = 500;
  REQUIRE_OK(ctl.set_layer_time(pid, *snd, in->second, half));
  len = ctl.composition_length(pid, *snd);
  REQUIRE_OK(len);
  CHECK(std::abs(*len - 1500) <= 1);
  // Cut there: parts of sound, marked in milliseconds.
  auto cut = ctl.split_layer(pid, *snd, in->second, 700);
  REQUIRE_OK(cut);
  CHECK(b.p().asset(cut->first)->kind == project::AssetKind::Audio);
  auto p2 = b.layers(cut->second);
  REQUIRE(p2.size() == 1);
  CHECK(p2[0].source == *loud && p2[0].time.in == 1200 &&
        p2[0].time.rate == Rational(1000, 1));
  REQUIRE_OK(ctl.undo(pid));
  // On a timeline with a frame: sound there, not a picture.
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {16, 16}, {24, 1}, "edit");
  REQUIRE_OK(tl);
  auto put = ctl.instantiate(pid, *snd, *tl);
  REQUIRE_OK(put);
  auto tm = ctl.movie_stack(pid, *tl, true);
  REQUIRE_OK(tm);
  REQUIRE(tm->layers.size() == 1);
  CHECK(!tm->layers[0].video && tm->layers[0].audio_only);
  auto tmix = ctl.sound_mix(pid, *tl);
  REQUIRE_OK(tmix);
  CHECK(!tmix->empty());
  auto dec = ctl.decompose(pid, *tl, put->second);
  CHECK(!dec.ok() && dec.error().key == msg::kDecomposeClipSound.key);
}

// A clip whose soundtrack runs on past its last frame (an H3 clip's AAC
// does, by a frame or so) runs on a timeline for its PICTURE's frames: a
// 24-frame clip ends at frame 24, where the next one starts -- its
// movie's duration, the longest track's, held it into frame 24. In a
// composition of sound it is its sound, whole.
TEST(composition, a_clip_runs_its_frames_not_its_sound)
{
  Bench b("comp-clip-frames");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  // A second of picture, then a second and a half of sound under it.
  auto shot = ctl.create_composition(
      pid, project::AssetClass::Composition, {16, 16}, {24, 1}, "shot");
  REQUIRE_OK(shot);
  auto pic = ctl.instantiate(pid, *red, *shot);
  REQUIRE_OK(pic);
  project::LayerTime one;
  one.duration = 24;
  REQUIRE_OK(ctl.set_layer_time(pid, *shot, pic->second, one));
  auto file = ctl.rendered(pid, *shot);
  REQUIRE_OK(file);
  REQUIRE(write_tone(b.root / "tone.wav", 1.5, 440, 0.5));
  const auto with_sound = b.root / "long-sound.mov";
  REQUIRE_OK(media::add_soundtrack(*file, b.root / "tone.wav", with_sound));
  auto clip = import_and_wait(ctl, pid, with_sound);
  REQUIRE(clip);
  auto v = b.p().version(*clip);
  REQUIRE_OK(v);
  REQUIRE(v->info.frame_count == 24);
  REQUIRE(v->info.duration.seconds() > 1.4);
  // On a timeline: its 24 frames.
  auto tl = ctl.create_composition(
      pid, project::AssetClass::Composition, {16, 16}, {24, 1}, "tl");
  REQUIRE_OK(tl);
  auto in = ctl.instantiate(pid, *clip, *tl);
  REQUIRE_OK(in);
  auto len = ctl.composition_length(pid, *tl);
  REQUIRE_OK(len);
  CHECK(*len == 24);
  auto m = ctl.movie_stack(pid, *tl);
  REQUIRE_OK(m);
  REQUIRE(m->layers.size() == 1);
  CHECK(std::abs(m->layers[0].timing.length - 1.0) < 1e-9);
  CHECK(m->layers[0].timing.active(23.0 / 24));
  CHECK(!m->layers[0].timing.active(24.0 / 24));
  // Its sound alone: a second and a half.
  auto snd = ctl.create_composition(pid, project::AssetClass::Composition,
                                    {0, 0}, {}, "sound");
  REQUIRE_OK(snd);
  REQUIRE_OK(ctl.instantiate(pid, *clip, *snd, std::string()));
  len = ctl.composition_length(pid, *snd);
  REQUIRE_OK(len);
  CHECK(std::abs(*len - 1500) <= 25);
}

// Layer FOLDERS (DESIGN §6a): layers gathered under a folder lie together
// in the stack -- a new layer above one of them joins it, layers placed
// in or out by a drag, one moved between two of them joins it -- shown or
// hidden together, renamed, ungrouped; a folder left empty goes.
// A still put on a timeline runs a second from where it was put, and its
// block's end stretches it (a clip's length is its marks'); markup is
// drawn at the player's frame -- on a markup showing there, else on a new
// one from there, a second long (DESIGN §6a).
// A markup's DRAWING -- its painted pixels -- copied onto another
// layer (pasted where asked -- floating until placed -- cleared), and a
// layer DUPLICATED: a markup's copy its own (drawn on apart), its looks
// with it.
TEST(composition, a_drawing_is_copied_and_a_layer_duplicated)
{
  Bench b("comp-drawing");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto comp = ctl.create_composition(pid, project::AssetClass::Still,
                                     {64, 64}, {0, 1}, "sheet");
  REQUIRE_OK(comp);
  auto m0 = ctl.markup_layer(pid, *comp, {});
  REQUIRE_OK(m0);
  media::Stroke s;
  s.points = {{4, 10}, {20, 10}};
  s.radius = 3;
  s.color = {1, 0, 0, 1};
  REQUIRE_OK(ctl.paint_stroke(pid, *comp, *m0, s));
  auto drawing = ctl.markup_drawing(pid, *comp, *m0);
  REQUIRE_OK(drawing);
  REQUIRE(!drawing->empty());
  // Copied: a copy of the file, as the app's pasteboard holds it.
  const auto held = b.root / "held.png";
  std::filesystem::copy_file(*drawing, held);
  // Onto a blank layer: it becomes a markup, the drawing 30 px down.
  auto blank = ctl.add_layer(pid, *comp, *m0);
  REQUIRE_OK(blank);
  REQUIRE_OK(ctl.markup_layer(pid, *comp, {*blank}));
  REQUIRE_OK(ctl.paste_drawing(pid, *comp, *blank, held, 0, 30));
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.set_layer_visible(pid, *comp, *m0, false));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 12, 40)[0] > 200);   // pasted, down
  CHECK(pixel_at(flat, 12, 10)[3] < 40);    // not where it was
  // As bytes too (the app's way), onto the same layer: both there.
  std::ifstream f(held, std::ios::binary);
  std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
  REQUIRE_OK(ctl.paste_drawing_data(pid, *comp, *blank, bytes, 0, 0));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 12, 10)[0] > 200 && pixel_at(flat, 12, 40)[0] > 200);
  // Floating, both: laid into its raster when placed.
  CHECK(ctl.markup_drawing(pid, *comp, *blank)->empty());
  REQUIRE_OK(ctl.place_drawings(pid, *comp, *blank));
  REQUIRE(!ctl.markup_drawing(pid, *comp, *blank)->empty());
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 12, 10)[0] > 200 && pixel_at(flat, 12, 40)[0] > 200);
  // Cleared; undone.
  REQUIRE_OK(ctl.clear_drawing(pid, *comp, *blank));
  CHECK(ctl.markup_drawing(pid, *comp, *blank)->empty());
  REQUIRE_OK(ctl.undo(pid));
  CHECK(!ctl.markup_drawing(pid, *comp, *blank)->empty());

  // Duplicated: right above it, a markup of its own, its crop with it.
  media::Crop c;
  c.offset_x = 0.25;
  REQUIRE_OK(ctl.set_crop(pid, *comp, c, *m0));
  auto dup = ctl.duplicate_layer(pid, *comp, *m0, "Copy");
  REQUIRE_OK(dup);
  const auto ls = b.layers(*comp);
  auto at = [&](const std::string& id) {
    for (std::size_t i = 0; i < ls.size(); ++i) {
      if (ls[i].id == id) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };
  CHECK(at(*dup) == at(*m0) + 1);
  CHECK(ls[at(*dup)].name == "Copy");
  CHECK(ls[at(*dup)].source != ls[at(*m0)].source);
  auto ca = b.p().asset(*comp);
  REQUIRE_OK(ca);
  CHECK(Controller::crop_of(*ca, *dup).offset_x == 0.25);
  // Painted on apart: the original's drawing is as it was.
  const auto before = ctl.markup_drawing(pid, *comp, *m0);
  REQUIRE_OK(before);
  media::Stroke s2 = s;
  s2.points = {{40, 50}, {60, 50}};
  REQUIRE_OK(ctl.paint_stroke(pid, *comp, *dup, s2));
  CHECK(*ctl.markup_drawing(pid, *comp, *m0) == *before);
  CHECK(*ctl.markup_drawing(pid, *comp, *dup) != *before);
  // Undone whole.
  REQUIRE_OK(ctl.undo(pid));
  REQUIRE_OK(ctl.undo(pid));
  CHECK(b.layers(*comp).size() == ls.size() - 1);
}

// DRAWINGS (media/markup.h): strokes apart are two, each selected alone;
// one LIFTED floats as an object -- drawn where it is moved, the raster
// without it -- and PLACED is pixels again: over another, the two are
// one. Lifted and moved at once is one command, placed another; a
// drawing alone is cleared or copied alone.
TEST(composition, a_drawing_lifts_moves_and_fuses)
{
  Bench b("comp-drawing-lift");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto comp = ctl.create_composition(pid, project::AssetClass::Still,
                                     {64, 64}, {0, 1}, "sheet");
  REQUIRE_OK(comp);
  auto m0 = ctl.markup_layer(pid, *comp, {});
  REQUIRE_OK(m0);
  media::Stroke red;
  red.points = {{4, 10}, {20, 10}};
  red.radius = 3;
  red.color = {1, 0, 0, 1};
  REQUIRE_OK(ctl.paint_stroke(pid, *comp, *m0, red));
  media::Stroke blue = red;
  blue.points = {{40, 40}, {56, 40}};
  blue.color = {0, 0, 1, 1};
  REQUIRE_OK(ctl.paint_stroke(pid, *comp, *m0, blue));
  const auto markup = [&] {
    for (const auto& l : b.layers(*comp)) {
      if (l.id == *m0 && l.source) {
        return b.p().asset(*l.source)->markup.value_or(project::Markup{});
      }
    }
    return project::Markup{};
  };
  // Two drawings: each its own box.
  auto ra = ctl.drawing_at(pid, *comp, *m0, 12, 10, 4);
  REQUIRE_OK(ra);
  CHECK(ra->x <= 2 && ra->x + ra->w >= 22 && ra->y + ra->h <= 16);
  auto rb = ctl.drawing_at(pid, *comp, *m0, 48, 40, 4);
  REQUIRE_OK(rb);
  CHECK(rb->x >= 34 && rb->y >= 34);
  CHECK(ctl.drawing_at(pid, *comp, *m0, 30, 30, 2)->empty());
  // Lifted: an object; the raster without it, the picture as it was.
  auto id = ctl.lift_drawing(pid, *comp, *m0, 12, 10, 4);
  REQUIRE_OK(id);
  REQUIRE(markup().objects.size() == 1);
  CHECK(jget<std::string>(markup().objects[0], "kind", "") == "drawing");
  CHECK(ctl.drawing_at(pid, *comp, *m0, 12, 10, 2)->empty());
  const auto flat = b.root / "flat.png";
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 12, 10)[0] > 200);
  // Its picture is the project's to keep while it floats.
  const auto image = jget(markup().objects[0], "image", project::BlobRef{});
  REQUIRE(!image.empty());
  CHECK(b.p().held_blobs()->contains(image.hash.hex()));
  // Moved onto the blue one, overlapping it: drawn there, the two still
  // apart.
  Json moved = markup().objects;
  for (const char* k : {"x0", "x1"}) {
    moved[0][k] = moved[0][k].get<double>() + 30;
  }
  for (const char* k : {"y0", "y1"}) {
    moved[0][k] = moved[0][k].get<double>() + 30;
  }
  REQUIRE_OK(ctl.set_markup_objects(pid, *comp, *m0, moved));
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 12, 10)[3] < 40);
  CHECK(pixel_at(flat, 46, 40)[0] > 200);
  const auto apart = ctl.drawing_at(pid, *comp, *m0, 48, 40, 4);
  CHECK(apart->w == rb->w && apart->h == rb->h);
  // Placed: one drawing, wider than either; no object left.
  REQUIRE_OK(ctl.place_drawings(pid, *comp, *m0));
  CHECK(markup().objects.empty());
  const auto one = ctl.drawing_at(pid, *comp, *m0, 48, 40, 4);
  REQUIRE_OK(one);
  CHECK(one->x < rb->x && one->w > rb->w);
  REQUIRE_OK(ctl.flatten(pid, *comp, flat));
  CHECK(pixel_at(flat, 46, 40)[0] > 200);
  // Undone: floating again where it was moved; then the lift and the
  // move together -- back in the raster where it was painted.
  REQUIRE_OK(ctl.undo(pid));
  CHECK(markup().objects.size() == 1);
  REQUIRE_OK(ctl.undo(pid));
  CHECK(markup().objects.empty());
  CHECK(!ctl.drawing_at(pid, *comp, *m0, 12, 10, 2)->empty());
  // Moved apart and placed: still two.
  REQUIRE_OK(ctl.lift_drawing(pid, *comp, *m0, 12, 10, 4));
  moved = markup().objects;
  for (const char* k : {"y0", "y1"}) {
    moved[0][k] = moved[0][k].get<double>() + 10;
  }
  REQUIRE_OK(ctl.set_markup_objects(pid, *comp, *m0, moved));
  REQUIRE_OK(ctl.place_drawings(pid, *comp, *m0));
  CHECK(ctl.drawing_at(pid, *comp, *m0, 12, 20, 2)->w == ra->w);
  CHECK(ctl.drawing_at(pid, *comp, *m0, 48, 40, 4)->w == rb->w);
  // Copied alone: the blue one's picture, nothing of the red.
  auto copied = ctl.copy_drawings(pid, *comp, *m0,
                                  std::array<double, 2>{48, 40}, 4, {});
  REQUIRE_OK(copied);
  CHECK(pixel_at(*copied, 48, 40)[2] > 200);
  CHECK(pixel_at(*copied, 12, 20)[3] == 0);
  // Cleared alone: the red one stays.
  REQUIRE_OK(ctl.clear_drawing(pid, *comp, *m0,
                               std::array<double, 2>{48, 40}, 4));
  CHECK(ctl.drawing_at(pid, *comp, *m0, 48, 40, 4)->empty());
  CHECK(!ctl.drawing_at(pid, *comp, *m0, 12, 20, 2)->empty());
}

// A LOCKED layer (DESIGN §6a): what it shows, its look, time, mask and
// markup refused, and its removal -- each kLayerLocked; shown, hidden,
// renamed, moved and duplicated all the same, the copy unlocked. A
// drawing floating on it is laid down as it is locked; the markup
// toolbar passes a locked top markup over. Lock and unlock are undone.
TEST(composition, a_locked_layer_refuses_changes)
{
  Bench b("comp-lock");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto comp = ctl.create_composition(pid, project::AssetClass::Still,
                                     {64, 64}, {0, 1}, "sheet");
  REQUIRE_OK(comp);
  auto m0 = ctl.markup_layer(pid, *comp, {});
  REQUIRE_OK(m0);
  media::Stroke red;
  red.points = {{4, 10}, {20, 10}};
  red.radius = 3;
  red.color = {1, 0, 0, 1};
  REQUIRE_OK(ctl.paint_stroke(pid, *comp, *m0, red));
  const auto layer = [&](const std::string& id) {
    for (const auto& l : b.layers(*comp)) {
      if (l.id == id) {
        return l;
      }
    }
    return project::Layer{};
  };
  const auto objects = [&] {
    auto l = layer(*m0);
    return l.source ? b.p().asset(*l.source)->markup->objects
                    : Json::array();
  };
  const auto locked = [&](const auto& st) {
    return !st.ok() && st.error().key == msg::kLayerLocked.key;
  };
  // Floating when it is locked: laid down first.
  REQUIRE_OK(ctl.lift_drawing(pid, *comp, *m0, 12, 10, 4));
  REQUIRE(objects().size() == 1);
  REQUIRE_OK(ctl.set_layer_locked(pid, *comp, *m0, true));
  CHECK(layer(*m0).locked);
  CHECK(objects().empty());
  CHECK(!ctl.drawing_at(pid, *comp, *m0, 12, 10, 2)->empty());

  // Refused, each.
  CHECK(locked(ctl.paint_stroke(pid, *comp, *m0, red)));
  CHECK(locked(ctl.set_markup_objects(pid, *comp, *m0, Json::array())));
  CHECK(locked(ctl.clear_drawing(pid, *comp, *m0)));
  CHECK(locked(ctl.lift_drawing(pid, *comp, *m0, 12, 10, 4)));
  media::Adjustments adj;
  adj.exposure = 0.5;
  CHECK(locked(ctl.set_adjustments(pid, *comp, adj, *m0)));
  media::Crop crop;
  crop.rotate = 10;
  CHECK(locked(ctl.set_crop(pid, *comp, crop, *m0)));
  project::LayerTime t;
  t.in = 0;
  CHECK(locked(ctl.set_layer_time(pid, *comp, *m0, t)));
  CHECK(locked(ctl.set_layer_mask(pid, *comp, *m0, true)));
  auto above = ctl.add_layer(pid, *comp, *m0);
  REQUIRE_OK(above);
  CHECK(locked(ctl.merge_layers(pid, *comp, *m0, *above)));
  CHECK(locked(ctl.remove_layer(pid, *comp, *m0)));
  CHECK(locked(ctl.markup_layer(pid, *comp, {*m0})));
  // Unselected, the toolbar draws on a new markup, not the locked one.
  REQUIRE_OK(ctl.remove_layer(pid, *comp, *above));
  auto fresh = ctl.markup_layer(pid, *comp, {});
  REQUIRE_OK(fresh);
  CHECK(*fresh != *m0);
  REQUIRE_OK(ctl.remove_layer(pid, *comp, *fresh));

  // Allowed all the same.
  REQUIRE_OK(ctl.set_layer_visible(pid, *comp, *m0, false));
  REQUIRE_OK(ctl.set_layer_visible(pid, *comp, *m0, true));
  REQUIRE_OK(ctl.rename_layer(pid, *comp, *m0, "Sketch"));
  auto copy = ctl.duplicate_layer(pid, *comp, *m0);
  REQUIRE_OK(copy);
  CHECK(!layer(*copy).locked);
  REQUIRE_OK(ctl.paint_stroke(pid, *comp, *copy, red));
  REQUIRE_OK(ctl.remove_layer(pid, *comp, *copy));

  // Unlocked: changed again. The unlock undone: locked again.
  REQUIRE_OK(ctl.set_layer_locked(pid, *comp, *m0, false));
  CHECK(!layer(*m0).locked);
  REQUIRE_OK(ctl.set_adjustments(pid, *comp, adj, *m0));
  REQUIRE_OK(ctl.undo(pid));
  REQUIRE_OK(ctl.undo(pid));
  CHECK(layer(*m0).locked);
  REQUIRE_OK(ctl.redo(pid));
  CHECK(!layer(*m0).locked);
}

// A timeline of markup drawn for a model (rendered_, a reference's
// movie) frame by frame as the player draws it: its keys between them.
TEST(composition, a_markup_timeline_moves_between_its_keys)
{
  Bench b("comp-markup-keys");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {64, 36}, {24, 1}, "motion");
  REQUIRE_OK(tl);
  auto m = ctl.markup_layer(pid, *tl, {}, 0);
  REQUIRE_OK(m);
  REQUIRE_OK(ctl.set_markup_objects(pid, *tl, *m, Json::array({
      {{"kind", "rect"}, {"x0", 0}, {"y0", 12}, {"x1", 8}, {"y1", 24},
       {"fill", {1, 0, 0, 1}}, {"width", 0}}})));
  REQUIRE_OK(ctl.stretch_layer(pid, *tl, *m, 24));
  // Across half the frame over its 24 frames.
  media::KeyedCrop k;
  media::Crop c0, c1;
  c1.offset_x = 0.5;
  k.place.keys = {{0, c0}, {23, c1}};
  k.place.rate = {24, 1};
  REQUIRE_OK(ctl.set_crop_keys(pid, *tl, k, *m));
  auto stack = ctl.movie_stack(pid, *tl, true);
  REQUIRE_OK(stack);
  auto red_x = [&](std::int64_t f) {
    const auto out = b.root / std::format("k{}.png", f);
    if (!media::write_stack_frame(*stack, {24, 1}, f, out).ok()) {
      return -1;
    }
    for (int x = 63; x >= 0; --x) {
      if (pixel_at(out, x, 18)[0] > 200) {
        return x;   // the right edge
      }
    }
    return -1;
  };
  CHECK(std::abs(red_x(0) - 7) <= 1);
  CHECK(std::abs(red_x(23) - (7 + 32)) <= 1);
  // Half way: half way (linear between the keys).
  CHECK(std::abs(red_x(12) - (7 + 17)) <= 2);
}

// A timeline of markup alone, its frame drawn as it plays: on black
// where nothing is drawn -- as its movie (a reference's) and the player
// have it. The frame (a thumbnail's, a still's layer showing it, the
// agent's look) was drawn clear, and the JPEG made of it -- the prompt
// row's thumbnail of a captured timeline -- came out white.
TEST(composition, a_timelines_frame_is_on_black_as_it_plays)
{
  Bench b("comp-frame-black");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {64, 36}, {24, 1}, "drawn");
  REQUIRE_OK(tl);
  auto m = ctl.markup_layer(pid, *tl, {}, 0);
  REQUIRE_OK(m);
  REQUIRE_OK(ctl.set_markup_objects(pid, *tl, *m, Json::array({
      {{"kind", "rect"}, {"x0", 0}, {"y0", 12}, {"x1", 8}, {"y1", 24},
       {"fill", {1, 0, 0, 1}}, {"width", 0}}})));
  auto stack = ctl.movie_stack(pid, *tl, true);
  REQUIRE_OK(stack);
  const auto out = b.root / "f0.png";
  REQUIRE_OK(media::write_stack_frame(*stack, {24, 1}, 0, out));
  const auto bg = pixel_at(out, 40, 4);
  CHECK(bg[0] < 4 && bg[1] < 4 && bg[2] < 4 && bg[3] == 255);
  CHECK(pixel_at(out, 4, 18)[0] > 200);
  // Its thumbnail, a JPEG: black there too.
  auto thumb = ctl.thumbnail(pid, *tl, 64, false);
  REQUIRE_OK(thumb);
  const auto t = pixel_at(*thumb, 40, 4);
  CHECK(t[0] < 24 && t[1] < 24 && t[2] < 24);
}

TEST(composition, a_still_on_a_timeline_runs_a_second)
{
  Bench b("comp-still-second");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {16, 16}, {24, 1}, "edit");
  REQUIRE_OK(tl);
  const auto time_of = [&](const std::string& id) {
    for (const auto& l : b.layers(*tl)) {
      if (l.id == id) {
        return l.time;
      }
    }
    return project::LayerTime{};
  };
  auto in = ctl.instantiate(pid, *red, *tl, std::nullopt, 12);
  REQUIRE_OK(in);
  CHECK(time_of(in->second).offset == 12);
  CHECK(time_of(in->second).duration == 24);
  auto len = ctl.composition_length(pid, *tl);
  REQUIRE_OK(len);
  CHECK(*len == 36);
  // Stretched by its end: two seconds; a frame at least; undone.
  REQUIRE_OK(ctl.stretch_layer(pid, *tl, in->second, 48));
  CHECK(time_of(in->second).duration == 48);
  REQUIRE_OK(ctl.stretch_layer(pid, *tl, in->second, 0));
  CHECK(time_of(in->second).duration == 1);
  REQUIRE_OK(ctl.undo(pid));
  CHECK(time_of(in->second).duration == 48);
  // A blank layer given a still: a second, from where it starts.
  auto blank = ctl.add_layer(pid, *tl, in->second);
  REQUIRE_OK(blank);
  REQUIRE_OK(ctl.set_layer_source(pid, *tl, *blank, *red));
  CHECK(time_of(*blank).duration == 24);
  // A clip -- a timeline -- is not stretched: its marks are its length.
  auto shot = ctl.create_composition(pid, project::AssetClass::Composition,
                                     {16, 16}, {24, 1}, "shot");
  REQUIRE_OK(shot);
  REQUIRE_OK(ctl.instantiate(pid, *red, *shot));
  auto clip = ctl.instantiate(pid, *shot, *tl);
  REQUIRE_OK(clip);
  CHECK(time_of(clip->second).duration == 0);
  CHECK(!ctl.stretch_layer(pid, *tl, clip->second, 10).ok());
  // Markup at frame 30: a new layer from there, a second long; at 40 the
  // same one; at 60, past it, another.
  auto m1 = ctl.markup_layer(pid, *tl, {}, 30);
  REQUIRE_OK(m1);
  CHECK(time_of(*m1).offset == 30);
  CHECK(time_of(*m1).duration == 24);
  auto again = ctl.markup_layer(pid, *tl, {}, 40);
  REQUIRE_OK(again);
  CHECK(*again == *m1);
  auto m2 = ctl.markup_layer(pid, *tl, {}, 60);
  REQUIRE_OK(m2);
  CHECK(*m2 != *m1);
  CHECK(time_of(*m2).offset == 60);
  // Selected, a markup elsewhere in time is not drawn on either.
  auto m3 = ctl.markup_layer(pid, *tl, {*m1}, 90);
  REQUIRE_OK(m3);
  CHECK(*m3 != *m1);
  CHECK(time_of(*m3).offset == 90);
}

TEST(composition, a_layers_key_moves_on_every_track)
{
  Bench b("comp-key-move");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {16, 16}, {24, 1}, "edit");
  REQUIRE_OK(tl);
  auto in = ctl.instantiate(pid, *red, *tl, std::nullopt, 12);
  REQUIRE_OK(in);
  const std::string l = in->second;
  // Its look from 0 to 10, eased; its placement too; its turn at its
  // default, one key at the start (as the panels keep it).
  media::KeyedAdjustments look;
  look.keys = {{0, media::Adjustments{}}, {10, media::Adjustments{}}};
  look.keys[0].interp = media::Interp::Spline;
  look.keys[1].value.exposure = 1;
  REQUIRE_OK(ctl.set_adjustment_keys(pid, *tl, look, l));
  media::KeyedCrop crop;
  crop.place.keys = {{0, media::Crop{}}, {10, media::Crop{}}};
  crop.place.keys[1].value.scale_x = 0.5;
  crop.turn.keys = {{0, media::Rotation{}}};
  REQUIRE_OK(ctl.set_crop_keys(pid, *tl, crop, l));
  const auto frames = [](const auto& k) {
    std::vector<std::int64_t> f;
    for (const auto& key : k.keys) {
      f.push_back(key.frame);
    }
    return f;
  };
  const auto now = [&] { return *b.p().asset(*tl); };
  using F = std::vector<std::int64_t>;
  // The key at 10 to 16: both tracks, its value and its way kept.
  REQUIRE_OK(ctl.move_layer_key(pid, *tl, l, 10, 16));
  CHECK(frames(Controller::adjustment_keys_of(now(), l)) == F({0, 16}));
  CHECK(frames(Controller::crop_keys_of(now(), l).place) == F({0, 16}));
  CHECK(Controller::adjustment_keys_of(now(), l).at(16).exposure == 1);
  CHECK(Controller::adjustment_keys_of(now(), l).keys[0].interp ==
        media::Interp::Spline);
  // The first to 4: the turn at its default stays where it was.
  REQUIRE_OK(ctl.move_layer_key(pid, *tl, l, 0, 4));
  CHECK(frames(Controller::adjustment_keys_of(now(), l)) == F({4, 16}));
  CHECK(frames(Controller::crop_keys_of(now(), l).place) == F({4, 16}));
  CHECK(frames(Controller::crop_keys_of(now(), l).turn) == F({0}));
  // None there; another in the way on a track it is on: refused.
  auto none = ctl.move_layer_key(pid, *tl, l, 7, 9);
  CHECK(!none.ok() && none.error().key == msg::kNoKeyToMove.key);
  look = Controller::adjustment_keys_of(now(), l);
  look.keys.push_back({20, look.keys.back().value});
  REQUIRE_OK(ctl.set_adjustment_keys(pid, *tl, look, l));
  auto way = ctl.move_layer_key(pid, *tl, l, 16, 20);
  CHECK(!way.ok() && way.error().key == msg::kKeyInTheWay.key);
  // A key on one track alone moves alone.
  REQUIRE_OK(ctl.move_layer_key(pid, *tl, l, 20, 18));
  CHECK(frames(Controller::adjustment_keys_of(now(), l)) ==
        F({4, 16, 18}));
  CHECK(frames(Controller::crop_keys_of(now(), l).place) == F({4, 16}));
  // Each move one command, its tracks undone together.
  REQUIRE_OK(ctl.undo(pid));
  REQUIRE_OK(ctl.undo(pid));
  REQUIRE_OK(ctl.undo(pid));
  CHECK(frames(Controller::adjustment_keys_of(now(), l)) == F({0, 16}));
  CHECK(frames(Controller::crop_keys_of(now(), l).place) == F({0, 16}));
}

TEST(composition, layers_go_in_folders)
{
  Bench b("comp-folders");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto red = b.png("red.png", 16, 16, {0xff, 0, 0, 0xff});
  REQUIRE(red);
  auto tl = ctl.create_composition(pid, project::AssetClass::Composition,
                                   {16, 16}, {24, 1}, "edit");
  REQUIRE_OK(tl);
  for (int i = 0; i < 4; ++i) {
    REQUIRE_OK(ctl.instantiate(pid, *red, *tl));
  }
  const auto order = [&] {
    std::string o;
    for (const auto& l : b.layers(*tl)) {
      o += (l.id.empty() ? "0" : l.id) + (l.folder.empty() ? "" : "*");
    }
    return o;
  };
  CHECK(order() == "0123");
  auto f = ctl.group_layers(pid, *tl, {"1", "2"});
  REQUIRE_OK(f);
  CHECK(order() == "01*2*3");
  REQUIRE(b.p().asset(*tl)->layer_folders.size() == 1);
  CHECK(b.p().asset(*tl)->layer_folders[0].name == "Folder 1");
  // Gathered where the topmost was: 0 and 3 around them.
  auto g = ctl.group_layers(pid, *tl, {"", "3"});
  REQUIRE_OK(g);
  CHECK(order() == "1*2*0*3*");
  REQUIRE_OK(ctl.undo(pid));
  CHECK(order() == "01*2*3");
  // A new layer above one in it: in it.
  auto added = ctl.add_layer(pid, *tl, "1");
  REQUIRE_OK(added);
  CHECK(*added == "4");
  CHECK(order() == "01*4*2*3");
  // Placed into it, and out of it above it.
  REQUIRE_OK(ctl.place_layers(pid, *tl, {"3"}, std::string("1"), *f));
  CHECK(order() == "01*3*4*2*");
  REQUIRE_OK(ctl.place_layers(pid, *tl, {"4"}, std::string("2"), ""));
  CHECK(order() == "01*3*2*4");
  // Its TOPMOST taken out as the app takes a layer out -- above the
  // folder's topmost layer, itself: where it is, in none (it kept its
  // folder). Undone, back in.
  REQUIRE_OK(ctl.place_layers(pid, *tl, {"2"}, std::string("2"), ""));
  CHECK(order() == "01*3*24");
  REQUIRE_OK(ctl.undo(pid));
  CHECK(order() == "01*3*2*4");
  // Moved up between two of its layers: in it.
  REQUIRE_OK(ctl.move_layer(pid, *tl, "", 2));
  CHECK(order() == "1*3*0*2*4");
  // To the bottom, out of it.
  REQUIRE_OK(ctl.place_layers(pid, *tl, {"1"}, std::nullopt, ""));
  CHECK(order() == "13*0*2*4");
  // Shown and hidden at once; renamed.
  REQUIRE_OK(ctl.set_folder_visible(pid, *tl, *f, false));
  for (const auto& l : b.layers(*tl)) {
    CHECK(l.visible == l.folder.empty());
  }
  REQUIRE_OK(ctl.rename_layer_folder(pid, *tl, *f, "Shots"));
  CHECK(b.p().asset(*tl)->layer_folders[0].name == "Shots");
  // Ungrouped: the layers where they are, the folder gone; undone, back.
  REQUIRE_OK(ctl.ungroup_layers(pid, *tl, *f));
  CHECK(order() == "13024");
  CHECK(b.p().asset(*tl)->layer_folders.empty());
  REQUIRE_OK(ctl.undo(pid));
  CHECK(order() == "13*0*2*4");
  // Emptied, it goes.
  for (const char* id : {"3", "", "2"}) {
    REQUIRE_OK(ctl.remove_layer(pid, *tl, id));
  }
  CHECK(order() == "14");
  CHECK(b.p().asset(*tl)->layer_folders.empty());
}

// A layer's PITCH follows its speed, as a tape's, or is held (DESIGN
// §6a): a 440 Hz tone at twice its speed sounds at 880 following, at 440
// held -- an hour's worth of sound at either, through the mixer.
TEST(composition, a_pitch_follows_the_speed_or_is_held)
{
  Bench b("comp-pitch");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  REQUIRE(write_tone(b.root / "tone.wav", 2, 440, 0.5));
  auto tone = import_and_wait(ctl, pid, b.root / "tone.wav");
  REQUIRE(tone);
  auto snd = ctl.create_composition(pid, project::AssetClass::Composition,
                                    {0, 0}, {}, "sound");
  REQUIRE_OK(snd);
  REQUIRE_OK(ctl.instantiate(pid, *tone, *snd));
  media::KeyedSpeed twice;
  twice.keys = {{0, media::Speed{2}}};
  REQUIRE_OK(ctl.set_speed_keys(pid, *snd, twice, ""));
  auto pitch = [&] {
    auto mix = ctl.sound_mix(pid, *snd);
    if (!mix.ok() || mix->empty()) {
      return -1.0;
    }
    const auto s = read_sound(*mix);
    CHECK(std::abs(static_cast<double>(s.size()) / 48000 - 1.0) < 0.01);
    return hertz(s, 0.2, 0.8);
  };
  // Held: stretched, its pitch kept.
  auto plan = ctl.sound_plan(pid, *snd);
  REQUIRE_OK(plan);
  CHECK(!media::needs_render(*plan));
  CHECK(std::abs(pitch() - 440) < 15);
  // Following: an octave up -- kept on with every key the identity.
  REQUIRE_OK(ctl.set_sound_keys(pid, *snd, {}, "", true));
  CHECK(Controller::pitch_follows_speed(*b.p().asset(*snd), ""));
  plan = ctl.sound_plan(pid, *snd);
  REQUIRE_OK(plan);
  CHECK(media::needs_render(*plan) && plan->layers[0].follow_speed);
  CHECK(std::abs(pitch() - 880) < 30);
  // Keys given without a word on it keep it; a semitone down on top.
  media::KeyedSound down;
  down.keys = {{0, media::Sound{1, -12}}};
  REQUIRE_OK(ctl.set_sound_keys(pid, *snd, down, ""));
  CHECK(std::abs(pitch() - 440) < 15);
  // The plan as JSON keeps it (the app's player, a nested mix).
  plan = ctl.sound_plan(pid, *snd);
  REQUIRE_OK(plan);
  CHECK(media::sound_plan_from_json(media::to_json(*plan))
            .layers[0].follow_speed);
  // Held again.
  REQUIRE_OK(ctl.set_sound_keys(pid, *snd, {}, "", false));
  CHECK(!Controller::pitch_follows_speed(*b.p().asset(*snd), ""));
  CHECK(std::abs(pitch() - 440) < 15);
}

// MIGRATION (project/migrate.h): a schema-1 project brought over -- its
// classes, the look on a flat picture moved to a composition of its own,
// a markup layer's content made an asset, the project's composition
// named, a clip stack's upper clips muted, a trim made marks.
TEST(composition, a_schema_1_project_is_migrated)
{
  using namespace project;
  auto id = [] { return AssetId::make(); };
  const AssetId pic = id(), clip = id(), clip2 = id(), gen = id(),
                view = id(), mod = id();
  const RecipeId r_gen = RecipeId::make(), r_view = RecipeId::make(),
                 r_mod = RecipeId::make();
  auto asset = [](AssetId i, const char* name, const char* kind,
                  const char* origin, RecipeId r = {}) {
    return Json{{"id", i.str()}, {"name", name}, {"kind", kind},
                {"origin", origin}, {"head", 1}, {"recipe", r.str()},
                {"created", 1}, {"modified", 1}};
  };
  Json a_pic = asset(pic, "photo", "image", "source");
  a_pic["modifiers"] = Json::array({{{"kind", "adjust"}, {"layer", ""},
                                     {"params", {{"exposure", 0.5}}}}});
  a_pic["layers"] = Json::array({
      {{"id", ""}, {"name", ""}, {"visible", true}, {"own", true}},
      {{"id", "1"}, {"name", "notes"}, {"visible", true},
       {"markup", {{"objects", Json::array()}}}}});
  Json a_clip = asset(clip, "clip", "video", "source");
  a_clip["modifiers"] = Json::array({{{"kind", "trim"}, {"layer", ""},
      {"params", {{"in", 3}, {"out", 30}, {"rate_num", 24},
                  {"rate_den", 1}}}}});
  Json a_clip2 = asset(clip2, "clip 2", "video", "source");
  Json a_gen = asset(gen, "a fox", "image", "derived", r_gen);
  Json a_view = asset(view, "Project", "video", "derived", r_view);
  a_view["modified"] = 5;
  a_view["layers"] = Json::array({
      {{"id", ""}, {"name", ""}, {"visible", true},
       {"source", clip2.str()}},
      {{"id", "1"}, {"name", ""}, {"visible", true},
       {"source", clip2.str()}}});
  Json a_mod = asset(mod, "fox, edited", "image", "derived", r_mod);
  a_mod["modifiers"] = Json::array({{{"kind", "crop"}, {"layer", ""},
                                     {"params", {{"scale", 0.5}}}}});
  auto recipe = [](RecipeId r, const char* op, Json inputs) {
    return Json{{"id", r.str()}, {"op", op}, {"inputs", inputs},
                {"params", Json::object()}};
  };
  Json doc = {
      {"meta", {{"project", {{"schema", 1}, {"name", "old"}}}}},
      {"tables",
       {{"assets", Json::array({a_pic, a_clip, a_clip2, a_gen, a_view,
                                a_mod})},
        {"recipes",
         Json::array({recipe(r_gen, "generate-image", Json::array()),
                      recipe(r_view, "project", Json::array()),
                      recipe(r_mod, "modify",
                             Json::array({{{"role", "base"},
                                           {"asset", gen.str()},
                                           {"version", 1}}}))})},
        {"versions", Json::array()}}}};
  CHECK(document_schema(doc) == 1);
  REQUIRE_OK(migrate_document(doc));
  CHECK(document_schema(doc) == kSchemaVersion);
  std::map<std::string, Asset> by_name;
  for (const auto& j : doc["tables"]["assets"]) {
    const auto a = j.get<Asset>();
    by_name[a.name] = a;
  }
  CHECK(by_name["photo"].cls == AssetClass::Flat);
  CHECK(by_name["photo"].layers.empty() &&
        by_name["photo"].modifiers.empty());
  CHECK(by_name["a fox"].cls == AssetClass::Generated);
  CHECK(by_name["Project"].cls == AssetClass::Composition);
  CHECK(by_name["fox, edited"].cls == AssetClass::Still);
  // The modified copy shows what it was made from.
  REQUIRE(by_name["fox, edited"].layers.size() == 1);
  CHECK(by_name["fox, edited"].layers[0].source == gen);
  // The photo's look: on a composition of its own, layer 0 showing it.
  const Asset& pe = by_name["photo, edited"];
  CHECK(pe.cls == AssetClass::Still);
  REQUIRE(pe.layers.size() == 2);
  CHECK(pe.layers[0].source == pic && !pe.layers[0].own);
  CHECK(Controller::adjustments_of(pe, "").exposure == 0.5);
  // Its markup layer shows a markup asset now.
  REQUIRE(pe.layers[1].source);
  bool markup = false;
  for (const auto& [name, a] : by_name) {
    markup = markup || (a.id == *pe.layers[1].source &&
                        a.cls == AssetClass::Markup && a.markup);
  }
  CHECK(markup && !pe.layers[1].markup);
  // The clip's trim: its composition's layer 0 marks.
  const Asset& ce = by_name["clip, edited"];
  REQUIRE(ce.layers.size() == 1);
  CHECK(ce.layers[0].time.in == 3 && ce.layers[0].time.out == 30);
  CHECK((ce.layers[0].time.rate == Rational{24, 1}));
  // The project: its composition named; its upper clip muted.
  CHECK(jget<std::string>(doc["meta"], "composition", "") == view.str());
  CHECK(Controller::sound_keys_of(by_name["Project"], "1").at(0).volume ==
        0);
  CHECK(Controller::sound_keys_of(by_name["Project"], "").identity());
  // Once is enough.
  const Json again = doc;
  REQUIRE_OK(migrate_document(doc));
  CHECK(doc == again);
}

// A project as a DOCUMENT (DESIGN §5b): every action a command to undo
// and redo; Save writes the working copy back, Revert reads the save
// again, a working copy left dirty is resumed at the next open.
TEST(controller, undo_save_revert_and_resume)
{
  auto root = test::temp_dir("ctl-document");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  const fs::path pkg = root / "Doc.valtz";
  REQUIRE(write_png(root / "red.png", 16, 16, {0xff, 0, 0, 0xff}));
  ProjectId pid;
  AssetId red, view;
  {
    auto made = Controller::create(cfg);
    REQUIRE_OK(made);
    Controller& ctl = **made;
    auto p = ctl.create_project(pkg, "Doc");
    REQUIRE_OK(p);
    pid = *p;
    CHECK(!jget(ctl.project_state(pid), "dirty", true));
    auto a = import_and_wait(ctl, pid, root / "red.png");
    REQUIRE(a);
    red = *a;
    CHECK(jget(ctl.project_state(pid), "dirty", false));
    CHECK(ctl.project_state(pid)["undo"]["kind"] == "import");
    auto v = ctl.place_in_project(pid, red);
    REQUIRE_OK(v);
    REQUIRE(*v);
    view = **v;
    auto l1 = ctl.add_layer(pid, view);
    REQUIRE_OK(l1);
    // A slider's steps: one command.
    for (double e : {0.2, 0.4, 0.6}) {
      media::Adjustments adj;
      adj.exposure = e;
      REQUIRE_OK(ctl.set_adjustments(pid, view, adj, *l1));
    }
    project::Project& pr = *ctl.project(pid);
    auto exposure = [&] {
      auto x = pr.asset(view);
      return x.ok() ? Controller::adjustments_of(*x, *l1).exposure : -1.0;
    };
    CHECK(exposure() == 0.6);
    REQUIRE_OK(ctl.undo(pid));
    CHECK(exposure() == 0.0);
    CHECK(ctl.project_state(pid)["redo"]["kind"] == "adjust");
    REQUIRE_OK(ctl.undo(pid));          // the layer
    CHECK(pr.asset(view)->layers.size() == 1);
    REQUIRE_OK(ctl.redo(pid));
    CHECK(pr.asset(view)->layers.size() == 2);
    REQUIRE_OK(ctl.save_project(pid));
    CHECK(!jget(ctl.project_state(pid), "dirty", true));
    CHECK(fs::exists(pkg / project::kProjectFile));
    // Revert: the removal goes, the history with it.
    REQUIRE_OK(ctl.remove_layer(pid, view, *l1));
    CHECK(pr.asset(view)->layers.size() == 1);
    REQUIRE_OK(ctl.revert_project(pid));
    CHECK(pr.asset(view)->layers.size() == 2);
    CHECK(ctl.project_state(pid)["undo"].is_null());
    // Left dirty at quit: kept.
    REQUIRE_OK(ctl.set_layer_visible(pid, view, *l1, false));
    ctl.shutdown();
  }
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto again = ctl.open_project(pkg);
  REQUIRE_OK(again);
  CHECK(jget(ctl.open_report(*again), "recovered", false));
  CHECK(jget(ctl.project_state(*again), "dirty", false));
  project::Project& pr = *ctl.project(*again);
  CHECK(!pr.asset(view)->layers[1].visible);
  CHECK(ctl.project_state(*again)["undo"]["kind"] == "layer.hide");
  REQUIRE_OK(ctl.undo(*again));
  CHECK(pr.asset(view)->layers[1].visible);
  CHECK(!jget(ctl.project_state(*again), "dirty", true));
  // Discarded: the working copy goes; the package is as saved.
  REQUIRE_OK(ctl.close_project(*again, true));
  auto third = ctl.open_project(pkg);
  REQUIRE_OK(third);
  CHECK(!jget(ctl.open_report(*third), "recovered", true));
}

// A CLIP's stack as the player draws it (media::StackRenderer, which the
// app's compositor calls each frame): a picture over the clip, the clip's
// frame on a wider canvas, a timeline past the clip's end -- where the
// clip shows nothing and the picture stays.
TEST(controller, a_clip_stack_draws_its_layers)
{
  const char* mov = std::getenv("VALTZ_TEST_MOVIE");
  if (!mov) {
    SKIP("set VALTZ_TEST_MOVIE to a video file");
  }
  auto info = media::probe_file(mov);
  REQUIRE_OK(info);
  const int w = info->frame.width, h = info->frame.height;
  const Rational rate = info->frame_rate;
  REQUIRE(w > 16 && h > 16 && rate.num > 0 && info->frame_count > 0);
  auto root = test::temp_dir("ctl-clip-stack");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  REQUIRE(write_png(root / "red.png", 8, 8, {0xff, 0, 0, 0xff}));
  auto pid = ctl.create_project(root / "S.valtz", "stack");
  REQUIRE_OK(pid);
  auto movie = import_and_wait(ctl, *pid, mov);
  auto red = import_and_wait(ctl, *pid, root / "red.png");
  REQUIRE(movie && red);
  // The clip in a composition of its own: its edited copy.
  auto clip = ctl.derive_modified(*pid, *movie);
  REQUIRE_OK(clip);
  CHECK((ctl.project(*pid)->asset(*clip)->rate == rate));
  auto layer = ctl.add_layer(*pid, *clip);
  REQUIRE_OK(layer);
  REQUIRE_OK(ctl.set_layer_source(*pid, *clip, *layer, *red));
  // 40 columns more, on the right; 5 frames longer than the clip.
  REQUIRE_OK(ctl.set_canvas(*pid, *clip, {w + 40, h}, 0, 0));
  const std::int64_t frames = info->frame_count + 5;
  REQUIRE_OK(ctl.set_timeline(*pid, *clip, frames));
  auto stack = ctl.movie_stack(*pid, *clip, /*for_job=*/false);
  REQUIRE_OK(stack);
  CHECK(stack->layers.size() == 2 && stack->layers[0].video
        && !stack->layers[1].video);
  auto r = media::StackRenderer::make(std::move(*stack), rate);
  REQUIRE_OK(r);
  CHECK((*r)->size().width == w + 40 && (*r)->size().height == h);
  CHECK((*r)->frames() == frames);
  CHECK((*r)->clips().size() == 1);
  // The first frame: the clip, the red picture at its frame's centre,
  // nothing on the added columns.
  auto first = (*r)->still(0);
  REQUIRE_OK(first);
  const auto mid = pixel_of(*first, w / 2, h / 2);
  CHECK(mid[0] > 200 && mid[1] < 40 && mid[2] < 40);
  CHECK(pixel_of(*first, w + 20, h / 2)[3] == 0);
  CHECK(pixel_of(*first, 2, 2)[3] == 255);
  CGImageRelease(*first);
  // Past the clip's end: the clip shows nothing; the picture stays.
  auto last = (*r)->still(frames - 1);
  REQUIRE_OK(last);
  CHECK(pixel_of(*last, w / 2, h / 2)[0] > 200);
  CHECK(pixel_of(*last, 2, 2)[3] == 0);
  CGImageRelease(*last);
  // A layer's tracks as the panels hold them, in place of the record:
  // the picture hidden by its own exposure at -10 is near black.
  Controller::LiveTracks live;
  live.layer = *layer;
  media::Adjustments dark;
  dark.exposure = -10;
  live.adjust.keys = {{0, dark}};
  auto lit = ctl.movie_stack(*pid, *clip, false, live);
  REQUIRE_OK(lit);
  auto r2 = media::StackRenderer::make(std::move(*lit), rate);
  REQUIRE_OK(r2);
  auto darkened = (*r2)->still(0);
  REQUIRE_OK(darkened);
  CHECK(pixel_of(*darkened, w / 2, h / 2)[0] < 20);
  CGImageRelease(*darkened);
  // For the screen, at the player's size -- half the canvas -- the
  // picture where it is, scaled: the red at the frame's centre.
  CVPixelBufferRef half = nullptr;
  REQUIRE(CVPixelBufferCreate(kCFAllocatorDefault,
                              static_cast<std::size_t>((w + 40) / 2),
                              static_cast<std::size_t>(h / 2),
                              kCVPixelFormatType_32BGRA, nullptr,
                              &half) == kCVReturnSuccess);
  REQUIRE_OK((*r)->render(0, {nullptr}, half));
  CVPixelBufferLockBaseAddress(half, kCVPixelBufferLock_ReadOnly);
  const auto* row = static_cast<const std::uint8_t*>(
      CVPixelBufferGetBaseAddress(half)) +
      CVPixelBufferGetBytesPerRow(half) * static_cast<std::size_t>(h / 4);
  const std::uint8_t* bgra = row + 4 * static_cast<std::size_t>(w / 4);
  CHECK(bgra[2] > 200 && bgra[1] < 40 && bgra[0] < 40);  // red
  CVPixelBufferUnlockBaseAddress(half, kCVPixelBufferLock_ReadOnly);
  CVPixelBufferRelease(half);
}

TEST(controller, job_timing_estimates_and_records)
{
  const auto near = [](std::optional<double> v, double want) {
    return v && std::abs(*v - want) < 1e-6;
  };
  const double clip = 832.0 * 480.0;
  JobTiming t(0, Json(), clip * 39, 39);
  t.note({{"phase", "prepare"}}, 0);
  CHECK(!t.left(5));
  t.note({{"phase", "denoise"}, {"estimate", -1.0}}, 10);
  CHECK(!t.left(12));  // loading: nothing counted
  t.note({{"phase", "denoise"}, {"estimate", 0.04}, {"rate", 0.01}}, 14);
  CHECK(!t.left(14));  // under 5%
  t.note({{"phase", "denoise"}, {"estimate", 0.1}, {"rate", 0.01}}, 20);
  CHECK(near(t.left(20), 90));  // 0.9 of it at 1% a second
  // A decode with no prior: no estimate of it.
  t.note({{"phase", "decode"}}, 110);
  CHECK(!t.left(112));
  const Json r = t.record(120);
  CHECK(r["seconds"] == 120.0);
  CHECK(r["phases"]["prepare"] == 10.0 && r["phases"]["denoise"] == 100.0);
  CHECK(r["phases"]["decode"] == 10.0);

  // The next, twice as long: its decode is expected to take twice the
  // last one's, and is added to the denoise left.
  JobTiming u(0, t.prior(120), clip * 78, 78);
  u.note({{"phase", "denoise"}, {"estimate", 0.5}, {"rate", 0.005}}, 50);
  CHECK(near(u.left(50), 100 + 20));
  u.note({{"phase", "decode"}}, 150);  // not counted: as the last went
  CHECK(near(u.left(150), 20) && near(u.left(160), 10));
  CHECK(near(u.left(175), 0));

  // A clip "finishes" its denoise before its decode opens: that first
  // finish is followed by the decode and the last finish, as the prior's
  // sequence had them.
  JobTiming a(0, Json(), clip * 39, 39);
  a.note({{"phase", "denoise"}, {"estimate", 0.5}, {"rate", 0.01}}, 10);
  a.note({{"phase", "finish"}}, 60);
  a.note({{"phase", "decode"}, {"estimate", 0.5}, {"rate", 0.1}}, 61);
  a.note({{"phase", "finish"}}, 71);
  const Json ar = a.record(73);
  CHECK(ar["sequence"].size() == 5);  // prepare, denoise, finish x2
  CHECK(ar["phases"]["finish"] == 3.0);
  JobTiming b(0, a.prior(73), clip * 39, 39);
  b.note({{"phase", "denoise"}, {"estimate", 0.5}, {"rate", 0.01}}, 10);
  b.note({{"phase", "finish"}}, 60);
  CHECK(near(b.left(60), 1 + 10 + 2));  // the gap, the decode, the end
  b.note({{"phase", "decode"}}, 61);
  b.note({{"phase", "finish"}}, 71);
  CHECK(near(b.left(71), 2));

  // No pace reported: the phase's own average since its count began.
  JobTiming w(0, Json(), 1, 1);
  w.note({{"phase", "denoise"}, {"estimate", 0.0}}, 10);
  w.note({{"phase", "denoise"}, {"estimate", 0.2}}, 20);
  CHECK(near(w.left(20), 40));

  // Kept with the version it made.
  project::AssetVersion v;
  v.timing = r;
  const Json j = v;
  CHECK(j.get<project::AssetVersion>().timing == r);
}

// A generation refused for memory, as job.failed carries it: the message
// from the budget that was short, vpipe's numbers as they came, and what
// to change from the request -- a clip's length, the size, the
// references; a picture has no length.
TEST(controller, out_of_memory_names_what_to_change)
{
  const std::uint64_t gb = 1ull << 30;
  const Json memory = {
    {"refusal",
     {{"step", "denoise"},
      {"need", 20 * gb},
      {"parts", Json::array({{{"name", "transformer"}, {"bytes", 18 * gb}},
                             {{"name", "sage_attn"}, {"bytes", 2 * gb}}})},
      {"gates", Json::array({{{"name", "gpu_working_set"},
                              {"need", 22 * gb},
                              {"have", 16 * gb},
                              {"ok", false}},
                             {{"name", "reclaimable_ram"},
                              {"need", 22 * gb},
                              {"have", 30 * gb},
                              {"ok", true}}})}}},
    {"plan", {{"peak", 15 * gb}, {"phase", "decode"}}},
  };
  project::Recipe clip;
  clip.op = "generate-video";
  clip.params = {{"width", 1920}, {"height", 1088}, {"frames", 241},
                 {"fps", 24.0}};
  clip.inputs = {{"reference", AssetId::make(), 0},
                 {"continue", AssetId::make(), 0},
                 {"prompt", AssetId::make(), 0}};
  const Json r = out_of_memory_report(memory, &clip);
  CHECK(jget<std::string>(r, "key", "") == "core.out_of_memory");
  // The short budget's figures, as text.
  CHECK(jget<std::string>(r["args"], "need", "") == "22.0 GB");
  CHECK(jget<std::string>(r["args"], "have", "") == "16.0 GB");
  CHECK(jget<std::string>(r, "message", "").find("22.0 GB") !=
        std::string::npos);
  CHECK(r["memory"] == memory);
  const Json s = r["suggest"];
  CHECK(jget(s["length"], "frames", 0) == 241);
  CHECK(std::abs(jget(s["length"], "seconds", 0.0) - 241 / 24.0) < 1e-9);
  CHECK(jget(s["resolution"], "width", 0) == 1920);
  CHECK(jget(s["resolution"], "height", 0) == 1088);
  CHECK(jget(s["references"], "count", 0) == 2);  // the prompt is not one

  project::Recipe picture;
  picture.op = "generate-image";
  picture.params = {{"width", 1024}, {"height", 1024}};
  const Json p = out_of_memory_report(memory, &picture);
  CHECK(!p["suggest"].contains("length"));
  CHECK(!p["suggest"].contains("references"));
  CHECK(p["suggest"].contains("resolution"));
  // No recipe (the job was gone): the numbers, nothing to suggest.
  CHECK(out_of_memory_report(memory, nullptr)["suggest"].empty());
}

// Blank assets of the list and SOUND CAPTURED into them (DESIGN §7b): a
// composition made with `claim` off never takes the project's place;
// sound records only into a composition of sound alone; the sources list
// the system's audio beside the microphones; with nothing running, stop
// and cancel stop nothing.
TEST(capture, a_blank_sound_records_into_itself)
{
  Bench b("capture");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto sound = ctl.create_composition(pid, project::AssetClass::Composition,
                                      {0, 0}, {0, 1}, "Audio", false, false);
  REQUIRE_OK(sound);
  CHECK(!ctl.project_composition(pid));  // it claimed nothing
  CHECK(b.p().asset(*sound)->kind == project::AssetKind::Audio);
  auto still = ctl.create_composition(pid, project::AssetClass::Still,
                                      {64, 64}, {0, 1}, "Still", false,
                                      false);
  REQUIRE_OK(still);
  CHECK(!ctl.project_composition(pid));
  CHECK(!ctl.start_capture(pid, *still, "system").ok());
  CHECK(!ctl.start_capture(pid, *sound, "no-such-source").ok());
  CHECK(!jget(ctl.capture_state(), "recording", true));
  CHECK(!ctl.stop_capture().ok());
  CHECK(!ctl.cancel_capture().ok());
  bool system = false;
  for (const auto& s : jget(ctl.capture_sources(), "sources",
                            Json::array())) {
    system = system || jget<std::string>(s, "kind", "") == "system";
  }
  CHECK(system);
}

// A real recording (VALTZ_TEST_CAPTURE=system, or a microphone's id --
// macOS asks leave of the terminal first): a second and a half, stopped,
// a sound of the project's on the composition's layer 0.
TEST(capture, a_recording_lands_on_its_composition)
{
  const char* source = std::getenv("VALTZ_TEST_CAPTURE");
  if (!source) {
    SKIP("set VALTZ_TEST_CAPTURE to system or a microphone's id");
  }
  Bench b("capture-real");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  auto sound = ctl.create_composition(b.pid,
                                      project::AssetClass::Composition,
                                      {0, 0}, {0, 1}, "Audio", false, false);
  REQUIRE_OK(sound);
  REQUIRE_OK(ctl.start_capture(b.pid, *sound, source));
  CHECK(jget(ctl.capture_state(), "recording", false));
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  auto made = ctl.stop_capture();
  REQUIRE_OK(made);
  const auto ls = b.layers(*sound);
  REQUIRE(ls.size() == 1);
  CHECK(ls[0].source == *made);
  auto v = b.p().version(*made);
  REQUIRE_OK(v);
  CHECK(v->info.duration.seconds() > 1.0);
  // Into no composition (the attach menu's): a flat sound alone.
  REQUIRE_OK(ctl.start_capture(b.pid, std::nullopt, source));
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  auto alone = ctl.stop_capture();
  REQUIRE_OK(alone);
  CHECK(b.p().asset(*alone)->cls == project::AssetClass::Flat);
  CHECK(b.layers(*sound).size() == 1);
}

// The camera (VALTZ_TEST_CAMERA=1 -- macOS asks leave of the terminal
// first): shown, a still taken, a clip recorded -- flat assets.
TEST(capture, the_camera_takes_a_still_and_a_clip)
{
  if (!std::getenv("VALTZ_TEST_CAMERA")) {
    SKIP("set VALTZ_TEST_CAMERA=1 (the default camera)");
  }
  if (media::camera_permission() == "undetermined") {
    media::request_camera_access();
  }
  Bench b("camera-real");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  REQUIRE_OK(ctl.start_camera(b.pid, "", true));
  for (int i = 0; i < 100 && jget(ctl.camera_state(), "frames", 0) < 15;
       ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const auto f = ctl.camera_frame();
  REQUIRE(f.surface != nullptr);
  CHECK(f.width > 0 && f.height > 0);
  CFRelease(f.surface);
  auto still = ctl.camera_snap();
  REQUIRE_OK(still);
  CHECK(b.p().asset(*still)->kind == project::AssetKind::Image);
  REQUIRE_OK(ctl.camera_record());
  std::this_thread::sleep_for(std::chrono::milliseconds(2000));
  auto clip = ctl.camera_stop_recording();
  REQUIRE_OK(clip);
  CHECK(b.p().asset(*clip)->kind == project::AssetKind::Video);
  REQUIRE_OK(ctl.stop_camera());
  CHECK(!jget(ctl.camera_state(), "on", true));
}

// Nothing is cleaned up behind the person's back: a generation nothing
// uses any more -- marked stale, removed only by Remove -- and a markup's
// painting keep their media through a save, a close and a reopen.
TEST(controller, stale_and_markup_media_survive_save_and_reopen)
{
  const fs::path root = test::temp_dir("keep-media");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  const fs::path pkg = root / "Keep.valtz";
  auto pid = ctl.create_project(pkg, "Keep");
  REQUIRE_OK(pid);
  project::Project& p = *ctl.project(*pid);
  // A generation, as a build commits one; nothing shows it.
  project::Recipe r;
  r.op = "generate-image";
  r.model = "krea2-turbo";
  auto gen = p.define_derived("Fox", project::AssetKind::Image, r, 48);
  REQUIRE_OK(gen);
  REQUIRE(write_png(root / "fox.png", 8, 8, {0xff, 0x80, 0, 0xff}));
  const fs::path out = p.blobs().tmp_dir() / "out.png";
  fs::copy_file(root / "fox.png", out);
  project::Project::BuildRecord b;
  b.recipe = r;
  b.output = out;
  auto info = media::probe_file(out);
  REQUIRE_OK(info);
  b.info = *info;
  REQUIRE_OK(p.commit_build(gen->id, std::move(b)));
  // A markup, painted.
  auto still = ctl.create_composition(*pid, project::AssetClass::Still,
                                      {32, 32});
  REQUIRE_OK(still);
  auto layer = ctl.markup_layer(*pid, *still, {});
  REQUIRE_OK(layer);
  media::Stroke s;
  s.points = {{4, 4}, {28, 28}};
  s.radius = 4;
  s.color = {1, 0, 0, 1};
  REQUIRE_OK(ctl.paint_stroke(*pid, *still, *layer, s));
  REQUIRE_OK(ctl.save_project(*pid));
  REQUIRE_OK(ctl.close_project(*pid));

  auto again = ctl.open_project(pkg);
  REQUIRE_OK(again);
  project::Project& q = *ctl.project(*again);
  auto path = q.media_path(gen->id);
  REQUIRE_OK(path);
  CHECK(fs::exists(*path));
  bool painted = false;
  const auto all = q.assets();
  for (const auto& a : all.ok() ? *all : std::vector<project::Asset>{}) {
    if (a.cls == project::AssetClass::Markup && a.markup &&
        !a.markup->raster.empty()) {
      painted = fs::exists(q.blobs().path_of(a.markup->raster));
    }
  }
  CHECK(painted);
}

// Settings > Agentic Helper: the helper chosen by name is kept in the
// support folder -- a later run (the app, valtzctl) takes it -- and one
// not installed gives way to Auto's pick rather than failing Enhance.
// Only an assistant may be chosen; "" is Auto again.
TEST(controller, the_chosen_helper_is_kept_and_falls_back_to_auto)
{
  Bench b("helper");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  Json a = ctl.assistants();
  CHECK(a["choice"] == "");
  CHECK(a["using"] == a["auto"]);
  CHECK(a["models"].size() >= 3);
  REQUIRE_OK(ctl.choose_assistant("qwen3.8-27b-4bit"));
  a = ctl.assistants();
  CHECK(a["choice"] == "qwen3.8-27b-4bit");
  // Nothing is installed under the bench's model root.
  CHECK(a["using"] == a["auto"]);
  for (const auto& m : a["models"]) {
    if (m["id"] == "qwen3.8-27b-4bit") {
      CHECK(m["drafter"]["id"] == "qwen3.8-27b-mtp-4bit");
      CHECK(m["sampling"]["top_k"] == 20);
    }
  }
  CHECK(ctl.choose_assistant("no-such-model").code() == Code::NotFound);
  CHECK(ctl.choose_assistant("z-image-turbo").code() ==
        Code::InvalidArgument);
  CHECK(ctl.assistants()["choice"] == "qwen3.8-27b-4bit");
  // How long it stays loaded after a request: 10 minutes until set.
  CHECK(ctl.assistant_keep_loaded() == Controller::kDefaultKeepLoaded);
  REQUIRE_OK(ctl.set_assistant_keep_loaded(1800));
  CHECK(ctl.assistants()["keep_loaded"] == 1800.0);
  // What it drafts with: its MTP head until DFlash 2 is chosen; both
  // 27Bs -- the 4-bit, the OptiQ pack -- name the DFlash 2 drafter.
  CHECK(ctl.assistants()["drafter"] == "mtp");
  REQUIRE_OK(ctl.set_assistant_drafter("dflash", 4));
  CHECK(ctl.assistants()["drafter"] == "dflash");
  CHECK(ctl.assistants()["drafter_bits"] == 4);
  CHECK(ctl.set_assistant_drafter("eagle", 8).code() ==
        Code::InvalidArgument);
  // Bound first: a range-for over a temporary's member dangles.
  const Json helpers = ctl.assistants();
  int dflash_27b = 0;
  for (const auto& m : helpers["models"]) {
    if (m["id"] == "qwen3.8-27b-4bit" || m["id"] == "qwen3.8-27b") {
      CHECK(m["dflash"]["id"] == "qwen3.8-27b-dflash2");
      ++dflash_27b;
    }
  }
  CHECK(dflash_27b == 2);
  // A later run over the same support folder.
  ControllerConfig cfg;
  cfg.support_root = b.root / "support";
  cfg.model_roots = {b.root / "models"};
  cfg.with_engine = false;
  {
    auto again = Controller::create(cfg);
    REQUIRE_OK(again);
    CHECK((*again)->assistants()["choice"] == "qwen3.8-27b-4bit");
    CHECK((*again)->assistant_keep_loaded() == 1800);
    CHECK((*again)->assistants()["drafter"] == "dflash");
    CHECK((*again)->assistants()["drafter_bits"] == 4);
    REQUIRE_OK((*again)->choose_assistant(""));
  }
  auto third = Controller::create(cfg);
  REQUIRE_OK(third);
  CHECK((*third)->assistants()["choice"] == "");
}

// THE UPSCALERS' OPTIONS (Settings › Upscaling, DESIGN §4f): an upscale
// has no generation card, so each upscaler's arithmetic -- int8 GEMMs,
// Sage, Sol where it takes it -- is set once, kept in the support folder
// for later runs, and every upscale's recipe carries it as settled. A
// value as the default is not kept; a reset brings the defaults back.
TEST(controller, upscalers_run_as_settings_have_them)
{
  auto root = test::temp_dir("ctl-upscale-settings");
  // VOSR and the DINOv2 it reads with, "installed".
  for (const char* repo : {"CSWRY/VOSR", "facebook/dinov2-large"}) {
    const fs::path d = root / "models" / repo;
    fs::create_directories(d);
    std::ofstream(d / "model.safetensors") << "w";
  }
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  const bool m5 = ctl.hardware().gpu_matrix_cores;
  // Both listed, each with only what it takes: FlashVSR no Sol (its
  // attention is routed by its own weights), neither steps nor LoRAs.
  auto find = [&](const char* id) {
    const Json all = ctl.upscale_settings();
    for (const auto& u : all["upscalers"]) {
      if (u["id"] == id) {
        return u;
      }
    }
    return Json();
  };
  const Json fv = find("flashvsr");
  const Json vo = find("vosr");
  REQUIRE(fv.is_object() && vo.is_object());
  CHECK(fv["kind"] == "video" && vo["kind"] == "image");
  CHECK(vo["state"] == "installed");
  CHECK(!jget(vo, "custom", true));
  auto offered = [](const Json& u) {
    std::vector<std::string> keys;
    for (const auto& o : u["tuning"]["options"]) {
      if (jget<std::string>(o, "why", "") != "not-for-model") {
        keys.push_back(o["key"].get<std::string>());
      }
    }
    return keys;
  };
  CHECK((offered(fv) == std::vector<std::string>{"sage_attn", "i8_gemm",
                                                 "color_fix"}));
  CHECK((offered(vo) == std::vector<std::string>{"sol_attn", "sol_tau",
                                                 "sage_attn", "i8_gemm"}));
  // Med's: int8 GEMMs where the GPU has matrix cores, the rest off.
  CHECK(vo["tuning"]["values"]["i8_gemm"] == m5);
  CHECK(vo["tuning"]["values"]["sol_attn"] == false);
  // Set: Sol on at a threshold of its own, int8 off. The threshold's
  // default is not kept.
  REQUIRE_OK(ctl.set_upscale_tuning("vosr", {{"sol_attn", true},
                                             {"sol_tau", 1.0}}));
  REQUIRE_OK(ctl.set_upscale_tuning("vosr", {{"i8_gemm", !m5}}));
  {
    const Json v = find("vosr");
    CHECK(jget(v, "custom", false));
    CHECK(v["tuning"]["values"]["sol_attn"] == true);
    CHECK(v["tuning"]["values"]["i8_gemm"] == !m5);
  }
  CHECK(ctl.set_upscale_tuning("krea2-turbo", Json::object()).code() ==
        Code::InvalidArgument);
  CHECK(ctl.set_upscale_tuning("no-such", Json::object()).code() ==
        Code::NotFound);
  // A picture on a still, scaled 2x: its upscale carries them.
  auto pid = ctl.create_project(root / "U.valtz", "upscale");
  REQUIRE_OK(pid);
  REQUIRE(write_png(root / "small.png", 32, 24, {0x40, 0x80, 0xc0, 0xff}));
  auto pic = import_and_wait(ctl, *pid, root / "small.png");
  REQUIRE(pic);
  auto still = ctl.create_composition(*pid, project::AssetClass::Still,
                                      {64, 48});
  REQUIRE_OK(still);
  auto placed = ctl.instantiate(*pid, *pic, *still);
  REQUIRE_OK(placed);
  media::Crop twice;
  twice.scale_x = twice.scale_y = 2;
  REQUIRE_OK(ctl.set_crop(*pid, *still, twice, placed->second));
  Controller::AssetOpRequest up;
  up.project = *pid;
  up.asset = *still;
  up.layer = placed->second;
  up.operation = "upscale";
  auto upscaled = ctl.run_asset_operation(up);
  REQUIRE_OK(upscaled);
  const engine::JobSpec* spec = engine->spec(upscaled->job);
  REQUIRE(spec != nullptr);
  CHECK(spec->op == engine::kOpUpscaleImage);
  const Json t = jget(spec->params, "tuning", Json());
  CHECK(t["sol_attn"] == true);
  CHECK(t["i8_gemm"] == !m5);
  CHECK(!t.contains("loras") && !t.contains("steps"));
  // Kept for a later run over the same support folder.
  ControllerConfig again = cfg;
  again.engine_factory = nullptr;
  {
    auto later = Controller::create(again);
    REQUIRE_OK(later);
    const Json v = (*later)->upscale_settings()["upscalers"];
    bool seen = false;
    for (const auto& u : v) {
      if (u["id"] == "vosr") {
        seen = true;
        CHECK(u["tuning"]["values"]["sol_attn"] == true);
      }
    }
    CHECK(seen);
  }
  // Reset: the defaults, nothing kept.
  REQUIRE_OK(ctl.set_upscale_tuning("vosr", Json::object(), true));
  CHECK(!jget(find("vosr"), "custom", true));
  CHECK(find("vosr")["tuning"]["values"]["sol_attn"] == false);
}

// WHICH UPSCALER a kind runs (Settings › Upscaling): Auto's -- the best
// installed by rank: FlashVSR+ Turbo, then Base, then 1.1 -- or the
// person's pick, kept beside the upscalers' values; a pick not installed
// gives way to Auto's until it is. What the asset menu says Render
// Upscaled runs follows it.
TEST(controller, a_video_upscaler_is_auto_or_picked)
{
  auto root = test::temp_dir("ctl-upscaler-pick");
  const fs::path models = root / "models";
  auto put = [&](const char* file) {
    const fs::path f = models / file;
    fs::create_directories(f.parent_path());
    std::ofstream(f) << "w";
  };
  put("JunhaoZhuang/FlashVSR-v1.1/"
      "diffusion_pytorch_model_streaming_dmd.safetensors");
  put("JunhaoZhuang/FlashVSR-v1.1/model_index.json");
  put("ruchangyao/FlashVSR_plus/base/dit.safetensors");
  put("ruchangyao/FlashVSR_plus/base/lq_proj_in.safetensors");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {models};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.engine_factory = [] {
    return std::unique_ptr<engine::Engine>(std::make_unique<HeldEngine>());
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto video = [](const Json& all) {
    for (const auto& k : all["kinds"]) {
      if (k["kind"] == "video") {
        return k;
      }
    }
    return Json();
  };
  auto menu = [&] {
    for (const auto& op : ctl.asset_operations_json()) {
      if (op["name"] != "upscale") {
        continue;
      }
      for (const auto& t : op["takes"]) {
        if (t["kind"] == "video") {
          return t["model"].get<std::string>();
        }
      }
    }
    return std::string();
  };
  // Turbo not here: Auto's is Base, ahead of 1.1.
  Json k = video(ctl.upscale_settings());
  CHECK(k["chosen"] == "");
  CHECK(k["auto"] == "flashvsr-plus-base");
  CHECK(k["runs"] == "flashvsr-plus-base");
  CHECK(menu() == "flashvsr-plus-base");
  for (const auto& u : ctl.upscale_settings()["upscalers"]) {
    CHECK(jget(u, "runs", false) == (u["id"] == "flashvsr-plus-base"));
  }
  // Picked: 1.1.
  REQUIRE_OK(ctl.use_upscaler("video", "flashvsr"));
  k = video(ctl.upscale_settings());
  CHECK(k["chosen"] == "flashvsr");
  CHECK(k["runs"] == "flashvsr");
  CHECK(menu() == "flashvsr");
  // Turbo picked, not downloaded: Auto's runs meanwhile.
  REQUIRE_OK(ctl.use_upscaler("video", "flashvsr-plus-turbo"));
  k = video(ctl.upscale_settings());
  CHECK(k["chosen"] == "flashvsr-plus-turbo");
  CHECK(k["runs"] == "flashvsr-plus-base");
  // Not an upscaler of clips, not a kind, not a model: refused, the
  // pick kept.
  CHECK(ctl.use_upscaler("video", "vosr").code() == Code::InvalidArgument);
  CHECK(ctl.use_upscaler("sound", "flashvsr").code() ==
        Code::InvalidArgument);
  CHECK(ctl.use_upscaler("video", "no-such").code() == Code::NotFound);
  // The values beside it untouched by it, and it by them.
  REQUIRE_OK(ctl.set_upscale_tuning("flashvsr", {{"sage_attn", true}}));
  CHECK(video(ctl.upscale_settings())["chosen"] == "flashvsr-plus-turbo");
  // Kept for a later run.
  ControllerConfig again = cfg;
  again.engine_factory = nullptr;
  {
    auto later = Controller::create(again);
    REQUIRE_OK(later);
    CHECK(video((*later)->upscale_settings())["chosen"] ==
          "flashvsr-plus-turbo");
  }
  // Auto again.
  REQUIRE_OK(ctl.use_upscaler("video", "auto"));
  CHECK(video(ctl.upscale_settings())["chosen"] == "");
  CHECK(menu() == "flashvsr-plus-base");
}

// FL2VA's two keyframes: a clip that opens on one picture and ENDS on
// another -- given to the job as inputs "first" and "last", in that
// order whatever the row's, and named in the prompt as the guide's FL2VA
// case names them ("Picture 1", "Picture 2": by asset, not by place),
// on FL2VA's own Turbo LoRA, steps and shift. A last frame needs a
// first, and never comes with references.
TEST(controller, a_clip_opens_and_ends_on_pictures)
{
  auto root = test::temp_dir("first-last");
  for (const char* f :
       {"MiniMaxAI/MiniMax-H3/FL2VA/model.safetensors",
        "MiniMaxAI/MiniMax-H3/Ref2VA/model.safetensors",
        "lightx2v/Minimax-h3-Turbo/"
        "minimax_h3_fl2v_turbo_8step_v1.0_768p_bf16.safetensors",
        "lightx2v/Minimax-h3-Turbo/"
        "minimax_h3_ref2v_turbo_8step_v1.0_768p_bf16.safetensors"}) {
    const fs::path file = root / "models" / f;
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "w";
  }
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "F.valtz", "first-last");
  REQUIRE_OK(pid);
  REQUIRE(write_png(root / "closed.png", 32, 32, {0x20, 0x40, 0x80, 0xff}));
  REQUIRE(write_png(root / "open.png", 32, 32, {0x80, 0x40, 0x20, 0xff}));
  auto closed = import_and_wait(ctl, *pid, root / "closed.png");
  auto open = import_and_wait(ctl, *pid, root / "open.png");
  REQUIRE(closed && open && *closed != *open);

  GenerateVideoRequest req;
  req.project = *pid;
  req.model = "minimax-h3-fl2va";
  // The row holds the end first: the tags name by place, the model by
  // what each picture is to the clip.
  req.row = {*open, *closed};
  req.prompt = "<valtz_ref_img_1> opens her umbrella and ends as in "
               "<valtz_ref_img_0>.";
  req.first = *closed;
  req.last = *open;
  req.width = 832;
  req.height = 480;
  req.frames = 124;
  auto job = ctl.generate_video(req);
  REQUIRE_OK(job);
  REQUIRE(eventually([&] {
    return engine->held_of(engine::kOpGenerateVideo).size() == 1;
  }, 10));
  const auto* spec =
      engine->spec(engine->held_of(engine::kOpGenerateVideo).front());
  REQUIRE(spec != nullptr);
  std::vector<std::string> roles;
  for (const auto& in : spec->inputs) {
    roles.push_back(in.role);
  }
  CHECK((roles == std::vector<std::string>{"first", "last", "prompt"}) ||
        (roles == std::vector<std::string>{"first", "last"}));
  REQUIRE(spec->inputs.size() >= 2);
  CHECK(spec->inputs[0].path != spec->inputs[1].path);
  CHECK(jget<std::string>(spec->params, "prompt", "") ==
        "Picture 1 opens her umbrella and ends as in Picture 2.");
  // FL2VA's own Med: its 8-step Turbo LoRA at video shift 6.
  CHECK(jget<std::string>(spec->params, "lora", "") ==
        "minimax-h3-turbo-8step");
  CHECK(jget(spec->params, "steps", 0) == 8);
  CHECK(jget(jget(spec->params, "tuning", Json::object()), "video_shift",
             0.0) == 6.0);
  // Kept as the project keeps its clips: ProRes 422 HQ unless set.
  CHECK(jget<std::string>(spec->params, "intermediate", "") ==
        "prores422hq");

  // One picture: <Picture 1>, as I2VA names it.
  GenerateVideoRequest one = req;
  one.last.reset();
  one.row = {*closed};
  one.prompt = "<valtz_ref_img_0> waves.";
  REQUIRE_OK(ctl.generate_video(one));
  REQUIRE(eventually([&] {
    return engine->held_of(engine::kOpGenerateVideo).size() == 2;
  }, 10));
  const auto* i2va =
      engine->spec(engine->held_of(engine::kOpGenerateVideo).back());
  REQUIRE(i2va != nullptr);
  CHECK(jget<std::string>(i2va->params, "prompt", "") ==
        "<Picture 1> waves.");

  // A last frame needs a first, and no references with it.
  GenerateVideoRequest alone = req;
  alone.first.reset();
  auto r1 = ctl.generate_video(alone);
  CHECK(!r1.ok() && r1.error().code == Code::InvalidArgument);
  GenerateVideoRequest both = req;
  both.references = {*closed};
  CHECK(!ctl.generate_video(both).ok());

  // A continuation as Continue writes it in the prompt box: the clip
  // continued by its tag for its place in the row -- what the box draws
  // as the clip -- told to the model as its <Video 1>.
  auto shot = ctl.create_composition(*pid, project::AssetClass::Composition,
                                     {32, 32}, {24, 1}, "shot");
  REQUIRE_OK(shot);
  auto layer = ctl.instantiate(*pid, *closed, *shot);
  REQUIRE_OK(layer);
  project::LayerTime held;
  held.duration = 56;
  REQUIRE_OK(ctl.set_layer_time(*pid, *shot, layer->second, held));
  GenerateVideoRequest cont;
  cont.project = *pid;
  cont.model = "minimax-h3-ref2va";
  cont.row = {*shot};
  cont.continue_from = *shot;
  cont.prompt = "[video continuation + audio reference] The video "
                "continues from the end of <valtz_ref_vid_0>.";
  cont.width = 832;
  cont.height = 480;
  cont.frames = 124;
  REQUIRE_OK(ctl.generate_video(cont));
  REQUIRE(eventually([&] {
    return engine->held_of(engine::kOpGenerateVideo).size() == 3;
  }, 20));
  const auto* ref2va =
      engine->spec(engine->held_of(engine::kOpGenerateVideo).back());
  REQUIRE(ref2va != nullptr);
  CHECK(jget<std::string>(ref2va->params, "prompt", "") ==
        "[video continuation + audio reference] The video continues "
        "from the end of <Video 1>.");
}

// A continuation's GUIDE (continuation_guide): the clip's last seconds,
// rounded UP to the model's grid -- 17n + 5 frames for H3 at 24 fps, so
// 2 s is 56 frames -- and down again to what the clip holds, on a
// timeline of their own at the model's rate, its layer marking the
// clip's tail. A FRAME grabbed (grab_frame): a still at the clip's size
// showing it from that frame. Each one command. Gated on VALTZ_TEST_MOVIE.
TEST(controller, a_guide_and_a_grabbed_frame)
{
  const char* mov = std::getenv("VALTZ_TEST_MOVIE");
  if (!mov) {
    SKIP("set VALTZ_TEST_MOVIE to a video file");
  }
  Bench b("guide");
  REQUIRE(b.ok());
  Controller& ctl = *b.ctl;
  const ProjectId pid = b.pid;
  auto clip = import_and_wait(ctl, pid, mov);
  REQUIRE(clip);
  auto v = b.p().version(*clip);
  REQUIRE_OK(v);
  const std::int64_t have = v->info.frame_count;
  const double rate = v->info.frame_rate.to_double();
  REQUIRE(have > 0 && rate > 0);
  // What the model takes of 2 s, held to the clip: its 24 fps grid.
  const auto fit = static_cast<std::int64_t>(
      std::floor(static_cast<double>(have) * 24.0 / rate + 1e-6));
  std::int64_t want = 56;
  if (want > fit) {
    want = fit < 22 ? fit : 5 + (fit - 5) / 17 * 17;
  }
  auto g = ctl.continuation_guide(pid, *clip, 2.0, "minimax-h3-ref2va",
                                  "guide");
  REQUIRE_OK(g);
  CHECK(jget<std::int64_t>(*g, "frames", 0) == want);
  CHECK(std::abs(jget(*g, "seconds", 0.0) - want / 24.0) < 1e-9);
  auto gid = AssetId::parse(jget<std::string>(*g, "asset", ""));
  REQUIRE(gid);
  auto guide = b.p().asset(*gid);
  REQUIRE_OK(guide);
  CHECK(guide->cls == project::AssetClass::Composition);
  CHECK((guide->rate == Rational{24, 1}));
  CHECK(*ctl.composition_length(pid, *gid) == want);
  REQUIRE(guide->layers.size() == 1);
  CHECK(guide->layers[0].source == *clip);
  const auto span = std::min<std::int64_t>(
      have, std::llround(want / 24.0 * rate));
  CHECK(guide->layers[0].time.in == have - span);
  // A continuation takes it, as <Video 1>.
  auto tags = ctl.video_reference_tags(pid, "minimax-h3-ref2va", {},
                                       *gid);
  REQUIRE_OK(tags);
  CHECK(jget<std::string>(*tags, gid->str().c_str(), "") == "<Video 1>");
  // A still of frame 5, the clip's size.
  auto s = ctl.grab_frame(pid, *clip, 5, "frame 5");
  REQUIRE_OK(s);
  auto still = b.p().asset(*s);
  REQUIRE_OK(still);
  CHECK(still->cls == project::AssetClass::Still);
  REQUIRE(still->layers.size() == 1);
  CHECK(still->layers[0].time.in == 5);
  const auto flat = b.root / "frame5.png";
  REQUIRE_OK(ctl.flatten(pid, *s, flat));
  auto info = media::probe_file(flat);
  REQUIRE_OK(info);
  CHECK(info->frame.width == v->info.frame.width);
  CHECK(info->frame.height == v->info.frame.height);
  // Each undone whole: the still, then the guide.
  REQUIRE_OK(ctl.undo(pid));
  CHECK(!b.p().asset(*s).ok());
  REQUIRE_OK(ctl.undo(pid));
  CHECK(!b.p().asset(*gid).ok());
  CHECK(ctl.grab_frame(pid, *s, 0).code() == Code::NotFound);
}

// QwenLM's own Qwen-Image rewriters are non-commercial, so they are a
// download (Settings > Capabilities), not part of Valtz: once there, the
// enhancer follows them; gone again, Valtz's own guide.
TEST(controller, a_downloaded_rewriter_takes_the_guides_place)
{
  const auto root = test::temp_dir("rewriters");
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  auto c = Controller::create(cfg);
  REQUIRE_OK(c);
  const std::string ours = (*c)->skill_text("qwen-image-2.1-edit");
  CHECK(ours.find("<image1> is the picture being edited") !=
        std::string::npos);
  const auto* m = (*c)->catalog().find("qwen-image-21-rewriters");
  REQUIRE(m);
  CHECK(m->role == "prompt" && m->family == "qwen-image-21");

  const fs::path dir = root / "models" / "QwenLM" / "Qwen-Image-2.1";
  fs::create_directories(dir);
  std::ofstream(dir / "system_prompt_t2i.txt") << "QwenLM t2i rules.";
  std::ofstream(dir / "system_prompt_edit.txt") << "QwenLM edit rules:";
  std::ofstream(dir / "LICENSE") << "Qwen RESEARCH LICENSE AGREEMENT";
  (*c)->rescan_models();   // as a finished download does
  CHECK((*c)->skill_text("qwen-image-2.1-t2i") == "QwenLM t2i rules.");
  CHECK((*c)->skill_text("qwen-image-2.1-edit") == "QwenLM edit rules:");
  CHECK((*c)->skill_text("yue2-song").find("[Verse]") != std::string::npos);

  fs::remove_all(dir);
  (*c)->rescan_models();   // as Remove in Capabilities does
  CHECK((*c)->skill_text("qwen-image-2.1-edit") == ours);
}

// ---- THE AGENT (DESIGN §9b) ---------------------------------------------
//
// The loop, the tools, the review and a picture landing, with a model
// SCRIPTED to make the calls a real one would: what each tool did is
// read back from the document -- its layers, and its pixels.

#include "valtz/agent/agent.h"

namespace {

// A model that answers as it is scripted: each turn the next reply (its
// calls), then "Done." with none. What it was handed each turn is kept.
class ScriptedBackend final : public agent::Backend {
public:
  struct Shared {
    std::mutex                                    mu;
    std::deque<std::vector<agent::ToolCall>>      replies;
    std::vector<std::vector<agent::Message>>      seen;
    // A model that sees pictures (the view tools then show them).
    bool                                          sees = false;
    // COMPACTION: its context (0: not said) -- set at turn `shrink_at`
    // so that turn's conversation is 88% of it; the notes it writes when
    // asked for them; turn `overflow_at` refused as too long, once.
    std::int64_t                                  context = 0;
    int                                           shrink_at = -1;
    int                                           overflow_at = -1;
    std::string                                   notes = "Notes.";
  };
  explicit ScriptedBackend(std::shared_ptr<Shared> s) : _s(std::move(s)) {}

  Json describe() const override { return {{"kind", "test"}}; }
  bool sees() const override { return _s->sees; }
  std::int64_t
  context_tokens() const override
  {
    std::lock_guard lk(_s->mu);
    return _s->context;
  }

  Result<agent::Reply>
  turn(const std::vector<agent::Message>& messages,
       const std::vector<agent::ToolSpec>& tools,
       const std::function<void(std::string_view)>& on_text,
       const std::atomic<bool>&) override
  {
    std::lock_guard lk(_s->mu);
    _s->seen.push_back(messages);
    agent::Reply r;
    const std::size_t n = _s->seen.size() - 1;
    if (static_cast<int>(n) == _s->overflow_at) {
      _s->overflow_at = -1;
      return make_error(Code::OutOfMemory, msg::kAgentContextFull,
                        {{"tokens", std::to_string(_s->context)}});
    }
    if (!messages.empty() &&
        messages.back().content.starts_with(
            "[Valtz] Your context is nearly full")) {
      r.content = r.raw = _s->notes;
      r.finish = "stop";
      return r;
    }
    if (!_s->replies.empty()) {
      r.calls = _s->replies.front();
      _s->replies.pop_front();
      for (std::size_t i = 0; i < r.calls.size(); ++i) {
        r.calls[i].id = std::format("c{}-{}", n, i);
      }
      r.content = "Working on it.";
      r.finish = "tool_calls";
    } else {
      r.content = "Done.";
      r.finish = "stop";
    }
    r.raw = r.content;
    if (on_text) {
      on_text(r.content);
    }
    // Counted as a helper counts it: the conversation and its reply.
    if (_s->context > 0 || _s->shrink_at >= 0) {
      agent::Message mine{.role = "assistant", .content = r.content,
                          .raw = r.raw, .calls = r.calls};
      const std::int64_t pos = agent::estimate_tokens(messages) +
                               agent::estimate_tokens(tools) +
                               agent::estimate_tokens(mine);
      r.usage["ctx_pos"] = pos;
      if (static_cast<int>(n) == _s->shrink_at) {
        _s->context = pos * 100 / 88;
      }
    }
    return r;
  }

private:
  std::shared_ptr<Shared> _s;
};

agent::ToolCall
call(std::string name, Json args)
{
  return {"", std::move(name), std::move(args)};
}

// The session's state once it is `state` (or anything after it ends),
// waited for up to `seconds`.
Json
agent_wait(Controller& ctl, const std::string& id, const std::string& state,
           double seconds = 20)
{
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(
                         static_cast<int>(seconds * 1000));
  Json st;
  while (std::chrono::steady_clock::now() < until) {
    st = ctl.agent_state(id);
    const std::string now = jget<std::string>(st, "state", "");
    if (now == state || now == "stopped" || now == "failed") {
      return st;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return st;
}

}  // namespace

// The tools build a two-page still: a composition, words on page 1 and a
// shape on page 2, a picture placed in a box -- each as its tool says,
// to the pixel; each one undoable command.
TEST(agent, the_tools_build_a_deck)
{
  auto root = test::temp_dir("agent-deck");
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "D.valtz", "deck");
  REQUIRE_OK(pid);
  REQUIRE(write_png(root / "blue.png", 32, 18, {0, 0, 0xff, 0xff}));
  auto blue = import_and_wait(ctl, *pid, root / "blue.png");
  REQUIRE(blue.has_value());

  script->replies = {
      {call("create_composition",
            {{"name", "Deck"}, {"width", 320}, {"height", 180},
             {"pages", 2}})},
      {call("add_text", {{"page", 1}, {"text", "Hello"}, {"x", 20},
                         {"y", 20}, {"width", 200}, {"height", 60},
                         {"size", 30}, {"color", "#ff0000"}}),
       call("add_shape", {{"page", 2}, {"shape", "rect"}, {"x", 0},
                          {"y", 0}, {"width", 160}, {"height", 180},
                          {"fill", "#00ff00"}})},
      {call("place_asset", {{"asset", blue->str()}, {"page", 1},
                            {"x", 160}, {"y", 90}, {"width", 160},
                            {"height", 90}})},
      {call("get_composition", Json::object()),
       call("no_such_tool", Json::object())}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "A two-page deck";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  const Json st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  const AssetId comp =
      *AssetId::parse(jget<std::string>(st, "composition", ""));
  auto a = ctl.project(*pid)->asset(comp);
  REQUIRE_OK(a);
  CHECK(a->pages == 2);
  CHECK(a->name == "Deck");
  // Seven steps: the unknown tool is a step that failed, told so.
  const Json steps = jget(st, "steps", Json::array());
  REQUIRE(steps.size() == 6);
  CHECK(jget<std::string>(steps[5], "state", "") == "failed");
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(jget<std::string>(steps[i], "state", "") == "done");
  }
  // The words: on page 1's markup, read back by get_composition.
  const std::string described = to_text(jget(steps[4], "result", Json()));
  CHECK(described.find("Hello") != std::string::npos);
  CHECK(described.find("\"markup\"") != std::string::npos);
  // Page 2: the green half; page 1: the picture in its box.
  const fs::path p1 = root / "p1.png", p2 = root / "p2.png";
  REQUIRE_OK(ctl.flatten(*pid, comp, p2, std::nullopt, std::nullopt, {}, 1));
  REQUIRE_OK(ctl.flatten(*pid, comp, p1, std::nullopt, std::nullopt, {}, 0));
  CHECK(pixel_at(p2, 40, 90)[1] > 200);
  CHECK(pixel_at(p2, 240, 90)[3] == 0);
  CHECK(pixel_at(p1, 240, 135)[2] > 200);
  CHECK(pixel_at(p1, 170, 100)[2] > 200);
  CHECK(pixel_at(p1, 150, 135)[2] < 50);
  CHECK(pixel_at(p1, 240, 80)[2] < 50);
  // Its box, as the composition says it.
  CHECK(described.find("\"x\":160") != std::string::npos);
  CHECK(described.find("\"width\":160") != std::string::npos);
  // Each edit one command, named by its tool.
  const Json hist = ctl.undo_history(*pid);
  int agent_steps = 0;
  for (const auto& h : hist) {
    if (jget<std::string>(h, "kind", "") == "agent") {
      ++agent_steps;
    }
  }
  CHECK(agent_steps == 4);
  // The model was told each result, the unknown tool's error too.
  std::lock_guard lk(script->mu);
  const auto& last = script->seen.back();
  int tools = 0;
  for (const auto& m : last) {
    tools += m.role == "tool";
  }
  CHECK(tools == 6);
  CHECK(last.back().content.find("no tool") != std::string::npos);
  REQUIRE_OK(ctl.agent_stop(*id));
}

// A CONVERSATION COMPACTED (DESIGN §9b): one nearing its model's context
// is compacted before the turn that would not fit -- the model asked for
// its notes on the conversation as it is, then handed its head and those
// notes with Valtz's record of the steps -- and goes on; a turn that
// does not fit at all is compacted on Valtz's notes alone and tried
// again. The app still shows the session whole, each compaction a line.
TEST(agent, a_long_conversation_is_compacted_and_goes_on)
{
  auto root = test::temp_dir("agent-compact");
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "C.valtz", "compact");
  REQUIRE_OK(pid);

  const std::string long_words(6000, 'w');
  auto text = [&](int page, const std::string& t) {
    return call("add_text", {{"page", page}, {"text", t}, {"x", 10},
                             {"y", 10}, {"width", 300}, {"height", 160},
                             {"size", 12}});
  };
  script->notes = "Notes: a deck Deck of two pages, three texts on page 1.";
  script->shrink_at = 3;
  script->overflow_at = 7;
  script->replies = {
      {call("create_composition", {{"name", "Deck"}, {"width", 320},
                                   {"height", 180}, {"pages", 2}})},
      {text(1, long_words)},
      {text(1, long_words)},
      {text(1, long_words)},
      // (turn 4: its notes asked for)
      {call("add_shape", {{"page", 2}, {"shape", "rect"}, {"x", 0},
                          {"y", 0}, {"width", 160}, {"height", 180},
                          {"fill", "#00ff00"}})},
      {text(2, "Page two")},
      // (turn 7: too long -- compacted on Valtz's notes, tried again)
      {text(2, "The end")}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "A two-page deck";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  const Json st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  CHECK(jget(st, "compactions", 0) == 2);
  const Json steps = jget(st, "steps", Json::array());
  REQUIRE(steps.size() == 7);
  for (const auto& x : steps) {
    CHECK(jget<std::string>(x, "state", "") == "done");
  }
  {
    std::lock_guard lk(script->mu);
    const auto& seen = script->seen;
    REQUIRE(seen.size() == 10);
    // Turn 4: the conversation as it was, then the question.
    CHECK(seen[4].size() == seen[3].size() + 3);
    CHECK(seen[4].back().content.starts_with(
        "[Valtz] Your context is nearly full"));
    CHECK(seen[4][seen[4].size() - 2].role == "tool");
    // Turn 5: its head, its notes with Valtz's record of the four steps.
    REQUIRE(seen[5].size() >= 3);
    CHECK(seen[5][0].role == "system");
    CHECK(seen[5][1].content == "A two-page deck");
    CHECK(agent::is_compaction(seen[5][2]));
    CHECK(seen[5][2].content.find(script->notes) != std::string::npos);
    CHECK(seen[5][2].content.find("(4 in all)") != std::string::npos);
    CHECK(seen[5][2].content.find("create_composition") !=
          std::string::npos);
    if (seen[5].size() > 3) {
      CHECK(seen[5][3].role != "tool");
    }
    CHECK(agent::estimate_tokens(seen[5]) <
          agent::estimate_tokens(seen[3]));
    // Turn 8, after the refusal: not asked, Valtz's notes alone, the
    // earlier notes not kept as they were.
    for (const auto& m : seen[8]) {
      CHECK(!m.content.starts_with("[Valtz] Your context is nearly full"));
    }
    CHECK(agent::is_compaction(seen[8][2]));
    CHECK(seen[8][2].content.find("could not be asked") !=
          std::string::npos);
    CHECK(std::ranges::count_if(seen[8], agent::is_compaction) == 1);
    CHECK(seen[8][2].content.find("(6 in all)") != std::string::npos);
  }
  // Its work went on: the shape on page 2, the last words on it.
  const AssetId comp =
      *AssetId::parse(jget<std::string>(st, "composition", ""));
  auto a = ctl.project(*pid)->asset(comp);
  REQUIRE_OK(a);
  CHECK(a->pages == 2);
  // Shown whole: every message, each compaction a line, every step
  // under the reply that called it.
  int calls = 0, lines = 0, people = 0;
  for (const auto& m : jget(st, "messages", Json::array())) {
    const std::string role = jget<std::string>(m, "role", "");
    calls += static_cast<int>(jget(m, "calls", Json::array()).size());
    lines += role == "compaction";
    people += role == "user" &&
              !jget<std::string>(m, "content", "").starts_with("[Valtz]");
  }
  CHECK(calls == 7);
  CHECK(lines == 2);
  CHECK(people == 1);
  REQUIRE_OK(ctl.agent_stop(*id));
}

// Settings › Agentic Helper's LONG CONVERSATIONS: the helper's context
// at most kept (Auto 0; never under 16,384), and compaction switched off
// -- a turn that does not fit then ends the session, said so.
TEST(agent, compaction_switched_off_stops_at_the_context)
{
  auto root = test::temp_dir("agent-nocompact");
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  CHECK(jget(ctl.assistants(), "context", -1) == 0);
  REQUIRE_OK(ctl.set_assistant_context(100000));
  CHECK(jget(ctl.assistants(), "context", -1) == 100000);
  REQUIRE_OK(ctl.set_assistant_context(1000));
  CHECK(jget(ctl.assistants(), "context", -1) == 16384);
  REQUIRE_OK(ctl.set_assistant_context(0));
  CHECK(jget(ctl.assistants(), "context", -1) == 0);

  CHECK(jget(ctl.agent_settings(), "compact", false));
  REQUIRE_OK(ctl.set_agent_settings({{"compact", false}}));
  CHECK(!jget(ctl.agent_settings(), "compact", true));
  auto pid = ctl.create_project(root / "N.valtz", "no-compact");
  REQUIRE_OK(pid);
  script->overflow_at = 1;
  script->replies = {{call("create_composition",
                           {{"name", "Deck"}, {"width", 320},
                            {"height", 180}})}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "A deck";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  const Json st = agent_wait(ctl, *id, "failed");
  CHECK(jget<std::string>(st, "state", "") == "failed");
  CHECK(jget<std::string>(jget(st, "error_fields", Json::object()), "key",
                          "") == "core.agent_context_full");
  CHECK(jget(st, "compactions", -1) == 0);
  {
    std::lock_guard lk(script->mu);
    CHECK(script->seen.size() == 2);   // no notes asked, no retry
  }
  REQUIRE_OK(ctl.agent_stop(*id));
}

// A PICTURE IN ASSETS IS SEEN (view_asset): an import is what "draw the
// picture I imported" is about, and the view tools showed only pages
// and clips' frames -- view_frames refused it as no clip, view_page as
// no composition, and a model drew from its name and size. Each of the
// three shows it now, the picture handed over as drawn; a model that
// cannot see is told its name and size.
TEST(agent, a_picture_in_assets_is_seen)
{
  auto root = test::temp_dir("agent-view");
  auto script = std::make_shared<ScriptedBackend::Shared>();
  script->sees = true;
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "V.valtz", "view");
  REQUIRE_OK(pid);
  REQUIRE(write_png(root / "red.png", 48, 32, {0xff, 0, 0, 0xff}));
  auto red = import_and_wait(ctl, *pid, root / "red.png");
  REQUIRE(red.has_value());
  const std::string rid = red->str();
  script->replies = {{call("view_asset", {{"asset", rid}})},
                     {call("view_frames", {{"asset", rid}})},
                     {call("view_page", {{"composition", rid}})}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "Draw the picture I imported";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  const Json st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  const Json steps = jget(st, "steps", Json::array());
  REQUIRE(steps.size() == 3);
  for (const auto& step : steps) {
    CHECK(jget<std::string>(step, "state", "") == "done");
    const Json r = jget(step, "result", Json());
    CHECK(jget(r, "shown", false));
    CHECK(jget(r, "width", 0) == 48 && jget(r, "height", 0) == 32);
  }
  {
    // Each look a message of its own, its picture the import's red.
    std::lock_guard lk(script->mu);
    int looks = 0;
    for (const auto& m : script->seen.back()) {
      if (m.role != "user" || m.images.empty()) {
        continue;
      }
      ++looks;
      REQUIRE(m.images.size() == 1);
      const auto px = pixel_at(m.images[0], 24, 16);
      CHECK(px[0] > 200 && px[1] < 60 && px[2] < 60);
    }
    CHECK(looks == 3);
  }
  REQUIRE_OK(ctl.agent_stop(*id));

  // A model that cannot see: its name and size, said so.
  script->sees = false;
  script->replies = {{call("view_asset", {{"asset", rid}})}};
  auto blind = ctl.agent_start(req);
  REQUIRE_OK(blind);
  const Json bs = agent_wait(ctl, *blind, "done");
  REQUIRE(jget<std::string>(bs, "state", "") == "done");
  const Json r = jget(jget(bs, "steps", Json::array())[0], "result", Json());
  CHECK(!jget(r, "shown", true));
  CHECK(jget<std::string>(r, "name", "") == "red.png");
  CHECK(jget(r, "width", 0) == 48);
  REQUIRE_OK(ctl.agent_stop(*blind));
}

// DRAWING (DESIGN §9b): a polygon smooth through its points is a curve
// -- four round a box an ellipse, where the corners would have cut it --
// an open one a line with no inside; brush strokes painted over the
// shapes, on a layer of their own that says it holds them; a shape sent
// under another; and a layer the person LOCKED is said to be, and
// refuses a mark.
TEST(agent, the_tools_draw_curves_strokes_and_arrange)
{
  auto root = test::temp_dir("agent-draw");
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "D.valtz", "draw");
  REQUIRE_OK(pid);
  using J = Json;
  script->replies = {
      {call("create_composition",
            {{"name", "Sketch"}, {"width", 200}, {"height", 100}})},
      {call("add_shape",
            {{"shape", "polygon"},
             {"points", J::array({J::array({100, 10}), J::array({150, 50}),
                                  J::array({100, 90}), J::array({50, 50})})},
             {"smooth", true}, {"fill", "#ff0000"}}),
       call("add_shape",
            {{"shape", "polygon"},
             {"points", J::array({J::array({10, 95}), J::array({30, 70}),
                                  J::array({50, 95})})},
             {"smooth", true}, {"open", true}, {"fill", "#ffff00"},
             {"stroke", "#00ff00"}, {"line_width", 4}}),
       call("add_shape", {{"shape", "rect"}, {"x", 90}, {"y", 40},
                          {"width", 20}, {"height", 20},
                          {"fill", "#0000ff"}}),
       call("draw_strokes",
            {{"strokes",
              J::array({{{"points", J::array({J::array({150, 10}),
                                              J::array({190, 10})})},
                         {"size", 6}, {"color", "#000000"}},
                        {{"points", J::array({J::array({60, 70}),
                                              J::array({80, 70})})},
                         {"size", 6}, {"color", "#000000"}}})}})},
      {call("get_composition", J::object())}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "Sketch a red sun";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  Json st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  Json steps = jget(st, "steps", J::array());
  REQUIRE(steps.size() == 6);
  for (std::size_t i = 0; i < 6; ++i) {
    CHECK(jget<std::string>(steps[i], "state", "") == "done");
  }
  // Read back: the curve, the open line -- and the strokes on a markup
  // of their own over them (a markup's strokes lie under its objects).
  const Json comp_r = jget(steps[5], "result", J());
  const Json all = jget(comp_r, "layers", J::array());
  REQUIRE(all.size() == 2);
  const Json layer = all[0];
  CHECK(!jget(layer, "painted", false));
  CHECK(jget(all[1], "painted", false));
  CHECK(jget(all[1], "objects", J::array()).empty());
  const Json objs = jget(layer, "objects", J::array());
  REQUIRE(objs.size() == 3);
  CHECK(jget(objs[0], "smooth", false));
  CHECK(!objs[0].contains("open"));
  CHECK(jget(objs[1], "open", false));
  const std::string rect = jget<std::string>(objs[2], "id", "");
  const std::string layer_id = jget<std::string>(layer, "id", "");
  const AssetId comp =
      *AssetId::parse(jget<std::string>(st, "composition", ""));
  const fs::path before = root / "before.png";
  REQUIRE_OK(ctl.flatten(*pid, comp, before, std::nullopt, std::nullopt,
                         {}, 0));
  CHECK(pixel_at(before, 100, 50)[2] > 200);   // the square, over the sun
  CHECK(pixel_at(before, 128, 28)[0] > 200);   // inside the curve, past
                                                // the corners' line
  CHECK(pixel_at(before, 140, 15)[3] == 0);    // outside it
  CHECK(pixel_at(before, 30, 70)[1] > 200);    // on the open line
  CHECK(pixel_at(before, 30, 90)[3] == 0);     // under it: no inside
  CHECK(pixel_at(before, 170, 10)[3] > 200);   // the stroke
  CHECK(pixel_at(before, 170, 30)[3] == 0);
  CHECK(pixel_at(before, 70, 70)[0] < 60);     // one over the sun
  CHECK(pixel_at(before, 70, 70)[3] > 200);

  // The square sent to the bottom: the sun over it.
  script->replies = {
      {call("arrange_objects", {{"composition", comp.str()},
                                {"layer", layer_id},
                                {"objects", J::array({rect})},
                                {"how", "to-bottom"}})}};
  auto again = ctl.agent_start(req);
  REQUIRE_OK(again);
  st = agent_wait(ctl, *again, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  const Json ar = jget(jget(st, "steps", J::array())[0], "result", J());
  CHECK(jget(ar, "order", J::array())[0].get<std::string>() == rect);
  const fs::path after = root / "after.png";
  REQUIRE_OK(ctl.flatten(*pid, comp, after, std::nullopt, std::nullopt,
                         {}, 0));
  CHECK(pixel_at(after, 100, 50)[0] > 200);
  CHECK(pixel_at(after, 100, 50)[2] < 60);
  REQUIRE_OK(ctl.agent_stop(*again));

  // Locked by the person -- the topmost, which marks go on: said so, and
  // a mark on it refused.
  const std::string top = jget<std::string>(all[1], "id", "");
  REQUIRE_OK(ctl.set_layer_locked(*pid, comp, top == "0" ? "" : top, true));
  script->replies = {
      {call("get_composition", {{"composition", comp.str()}}),
       call("add_shape", {{"composition", comp.str()}, {"shape", "rect"},
                          {"x", 0}, {"y", 0}, {"width", 10}, {"height", 10},
                          {"fill", "#00ff00"}})}};
  auto locked = ctl.agent_start(req);
  REQUIRE_OK(locked);
  st = agent_wait(ctl, *locked, "done");
  steps = jget(st, "steps", J::array());
  REQUIRE(steps.size() == 2);
  const Json lr = jget(steps[0], "result", J());
  CHECK(jget(jget(lr, "layers", J::array())[1], "locked", false));
  CHECK(!jget(lr, "layers", J::array())[0].contains("locked"));
  CHECK(jget<std::string>(steps[1], "state", "") == "failed");
  REQUIRE_OK(ctl.agent_stop(*locked));
  (void)ctl.agent_stop(*id);
}

// THE PROMPT ENHANCER for the agent (enhance_prompt, DESIGN §9b): the
// agent's idea goes to the helper as the person's Enhance sends it -- an
// edit's pictures by the model's own names -- and comes back in the
// fields of the tool that makes it, in the agent's names, with what to
// look at said: words it would write in the picture, the look it was
// told to keep and dropped, a line it has someone speak. An edit's prompt
// reaches the model by its names; a shot opening on one picture and
// ending on another reaches it in its guide's FL2VA form.
TEST(agent, the_enhancer_writes_and_the_agent_reads_it)
{
  auto root = test::temp_dir("agent-enhance");
  const fs::path models = root / "models";
  {
    const fs::path krea = models / "krea" / "Krea-2-Turbo";
    fs::create_directories(krea);
    std::ofstream(krea / "model.safetensors") << "w";
    const fs::path helper = models / "mlx-community" / "Qwen3.5-9B-OptiQ-4bit";
    fs::create_directories(helper);
    std::ofstream(helper / "config.json") << "{}";
    std::ofstream(helper / "model.safetensors") << "w";
    fake_pipeline(models / "Qwen" / "Qwen-Image-2.1", "QwenImage21Pipeline",
                  "QwenImage21Transformer2DModel");
    for (const char* f :
         {"MiniMaxAI/MiniMax-H3/FL2VA/model.safetensors",
          "lightx2v/Minimax-h3-Turbo/"
          "minimax_h3_fl2v_turbo_8step_v1.0_768p_bf16.safetensors"}) {
      fs::create_directories((models / f).parent_path());
      std::ofstream(models / f) << "w";
    }
  }
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {models};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "E.valtz", "enhance");
  REQUIRE_OK(pid);
  REQUIRE(write_png(root / "day.png", 64, 48, {0x80, 0xb0, 0xe0, 0xff}));
  REQUIRE(write_png(root / "dusk.png", 64, 48, {0x40, 0x20, 0x40, 0xff}));
  auto day = import_and_wait(ctl, *pid, root / "day.png");
  auto dusk = import_and_wait(ctl, *pid, root / "dusk.png");
  REQUIRE(day && dusk);
  script->replies = {
      {call("get_project", Json::object())},
      {call("enhance_prompt",
            {{"idea", "a harbour at dawn"},
             {"style", "soft watercolor illustration"}})},
      {call("enhance_prompt",
            {{"idea", "<Picture 1> at night"}, {"base", day->str()}})},
      {call("enhance_prompt",
            {{"for", "shot"}, {"idea", "a fox trots through snow"},
             {"seconds", 3}})},
      {call("generate_image",
            {{"prompt", "Show <Picture 1> at night, lit windows"},
             {"base", day->str()}}),
       call("generate_video",
            {{"description", "[Shot 1] The street from <Picture 1> to "
                             "<Picture 2>."},
             {"first", day->str()}, {"last", dusk->str()},
             {"seconds", 3}})}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "A harbour poster";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  // Each enhance is a chat on the helper: answered as the helper would.
  auto answer = [&](const std::string& reply) -> std::string {
    std::vector<JobId> chats;
    for (int i = 0; i < 500 && chats.empty(); ++i) {
      chats = engine->held_of(engine::kOpChat);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(chats.size() == 1);
    if (chats.empty()) {
      return {};
    }
    const std::string text =
        jget<std::string>(engine->spec(chats[0])->params, "text", "");
    engine->turn(chats[0], {{"text", reply}});
    return text;
  };
  CHECK(answer(Json{{"prompt", "A harbour at dawn, fishing boats under a "
                               "sign that reads \"FISH\", oil painting"},
                    {"title", "Harbour"}}.dump())
            .find("harbour at dawn") != std::string::npos);
  // The edit's picture by the model's own name, and back in the agent's.
  CHECK(answer(Json{{"prompt", "Turn <image1> into a night scene, its "
                               "windows lit"}}.dump())
            .find("<image1>") != std::string::npos);
  answer(Json{{"integrated_multimodal_description",
               "[Shot 1] A red fox trots through deep snow; the fox (S1) "
               "says: <d>[English] Cold.</d>"},
              {"overall_soundscape", "Wind, snow crunching."},
              {"non_diegetic_music", "N/A"}}.dump());
  Json st = agent_wait(ctl, *id, "waiting");
  REQUIRE(jget<std::string>(st, "state", "") == "waiting");
  const Json steps = jget(st, "steps", Json::array());
  REQUIRE(steps.size() == 6);
  const Json gp = jget(steps[0], "result", Json());
  CHECK(jget(gp, "can_enhance_prompts", false));
  CHECK(jget<std::string>(gp, "auto_edit_model", "") == "qwen-image-2.1");
  // A picture's prompt, and what to look at in it.
  const Json e1 = jget(steps[1], "result", Json());
  CHECK(jget<std::string>(e1, "prompt", "").starts_with("A harbour"));
  const std::string n1 = to_text(jget(e1, "notes", Json()));
  CHECK(n1.find("FISH") != std::string::npos);
  CHECK(n1.find("watercolor") != std::string::npos);
  CHECK(jget<std::string>(e1, "review", "").find("generate_image") !=
        std::string::npos);
  const Json e2 = jget(steps[2], "result", Json());
  CHECK(jget<std::string>(e2, "prompt", "") ==
        "Turn <Picture 1> into a night scene, its windows lit");
  CHECK(jget<std::string>(e2, "written_for", "") == "Qwen-Image 2.1");
  // A shot's parts, as generate_video takes them.
  const Json e3 = jget(steps[3], "result", Json());
  CHECK(jget<std::string>(e3, "description", "").starts_with("[Shot 1]"));
  CHECK(jget<std::string>(e3, "soundscape", "") == "Wind, snow crunching.");
  CHECK(to_text(jget(e3, "notes", Json())).find("Cold.") !=
        std::string::npos);
  // The edit as the model reads it; the shot from one picture to the
  // other in FL2VA's form.
  const auto edits = engine->held_of(engine::kOpEditImage);
  REQUIRE(edits.size() == 1);
  CHECK(jget<std::string>(engine->spec(edits[0])->params, "prompt", "")
            .find("Show <image1> at night") != std::string::npos);
  const auto clips = engine->held_of(engine::kOpGenerateVideo);
  REQUIRE(clips.size() == 1);
  const auto* clip = engine->spec(clips[0]);
  const auto shot = jget<std::string>(clip->params, "prompt", "");
  CHECK(shot.find("Picture 1 (from Shot 1) aligns with the 0.00-second") !=
        std::string::npos);
  CHECK(shot.find("from Picture 1 to Picture 2") != std::string::npos);
  bool ends = false;
  for (const auto& in : clip->inputs) {
    ends = ends || in.role == "last";
  }
  CHECK(ends);
  (void)engine->cancel(edits[0]);
  (void)engine->cancel(clips[0]);
  st = agent_wait(ctl, *id, "done");
  CHECK(jget<std::string>(st, "state", "") == "done");
  REQUIRE_OK(ctl.agent_stop(*id));
}

// A SONG SEPARATED for the agent (separate_sound): one task in the
// background, its two sounds named at once, waited for as its pictures
// are; the wait tells the model they are made.
TEST(agent, a_song_is_separated_in_the_background)
{
  auto root = test::temp_dir("agent-separate");
  const fs::path repo = root / "models" / "pcunwa" /
                        "BS-Roformer-Resurrection";
  fs::create_directories(repo);
  std::ofstream(repo / "BS-Roformer-Resurrection.ckpt") << "w";
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "S.valtz", "separate");
  REQUIRE_OK(pid);
  REQUIRE(write_tone(root / "song.wav", 1, 440, 0.5));
  auto song = import_and_wait(ctl, *pid, root / "song.wav");
  REQUIRE(song);
  script->replies = {{call("get_project", Json::object())},
                     {call("separate_sound", {{"asset", song->str()}})},
                     {call("wait_for_generations", Json::object())}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "The song without its singing";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  Json st = agent_wait(ctl, *id, "waiting");
  REQUIRE(jget<std::string>(st, "state", "") == "waiting");
  Json steps = jget(st, "steps", Json::array());
  CHECK(jget(jget(steps[0], "result", Json()), "can_separate_songs", false));
  const Json r = jget(steps[1], "result", Json());
  const auto voice = AssetId::parse(jget<std::string>(r, "vocals", ""));
  const auto rest =
      AssetId::parse(jget<std::string>(r, "accompaniment", ""));
  REQUIRE(voice && rest);
  const auto jobs = engine->held_of(engine::kOpSeparateAudio);
  REQUIRE(jobs.size() == 1);
  REQUIRE(write_tone(root / "voice.wav", 1, 880, 0.3));
  REQUIRE(write_tone(root / "band.wav", 1, 220, 0.3));
  engine->finish_outputs(jobs[0], {{"vocals", root / "voice.wav"},
                                   {"accompaniment", root / "band.wav"}});
  st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  steps = jget(st, "steps", Json::array());
  CHECK(jget<std::string>(steps[1], "state", "") == "done");
  auto rv = ctl.project(*pid)->version(*rest);
  REQUIRE_OK(rv);
  CHECK(rv->number == 1);
  // Told by the wait: the step it was made by.
  const Json landed = jget(jget(steps[2], "result", Json()), "landed",
                           Json::array());
  REQUIRE(landed.size() == 1);
  CHECK(jget(landed[0], "step", 0) == 2);
  REQUIRE_OK(ctl.agent_stop(*id));
}

// A LONG TEXT laid on pages (flow_text, DESIGN §9b): a cover made, a
// background and a header on every page, the person's passage -- named
// by its first and last words, loosely -- flowed from page 2 onto pages
// added as it needs them, every word once and in order; then a page
// progress bar, made again (it replaces the first). The cover's words
// stay on the cover: a one-page still's layers are its page's alone.
TEST(agent, a_long_text_is_laid_on_pages)
{
  auto root = test::temp_dir("agent-flow");
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "F.valtz", "flow");
  REQUIRE_OK(pid);

  std::string passage = "It was the first, of everything.\n\n";
  for (int i = 0; i < 18; ++i) {
    if (i % 6 == 0) {
      passage += std::format("## Part {}\n\n", i / 6 + 1);
    }
    for (int w = 0; w < 40 + (i * 17) % 45; ++w) {
      passage += std::format("{}w{}x{}", w ? " " : "", i, w);
    }
    passage += ".\n\n";
  }
  passage += "These are the last words.";
  script->replies = {
      {call("create_composition",
            {{"name", "Booklet"}, {"width", 600}, {"height", 800}})},
      {call("add_text", {{"page", 1}, {"text", "The Title"}, {"x", 40},
                         {"y", 300}, {"width", 520}, {"height", 80},
                         {"size", 48}, {"color", "#000000"}}),
       call("add_shape", {{"every_page", true}, {"below_all", true},
                          {"shape", "rect"}, {"x", 0}, {"y", 0},
                          {"width", 600}, {"height", 800},
                          {"fill", "#f4efe6"}}),
       call("add_text", {{"every_page", true}, {"text", "BOOKLET"},
                         {"x", 40}, {"y", 16}, {"width", 520},
                         {"height", 30}, {"size", 18}, {"align", "right"},
                         {"color", "#202020"}}),
       // A rule on every page, inside the text's area: kept clear of.
       call("add_shape", {{"every_page", true}, {"shape", "line"},
                          {"x", 40}, {"y", 80}, {"width", 520},
                          {"height", 2}, {"stroke", "#aa3300"}})},
      {call("flow_text", {{"from_words", "it was the first of"},
                          {"to_words", "the last words"},
                          {"first_page", 2}, {"size", 18},
                          {"align", "justified"},
                          {"color", "#111111"}})},
      {call("add_page_progress", {{"from_page", 2}, {"numbers", true},
                                  {"fill", "#aa3300"},
                                  {"track", "#aa330033"}})},
      {call("add_page_progress", {{"from_page", 2}, {"style", "segments"},
                                  {"fill", "#aa3300"},
                                  {"track", "#aa330033"}}),
       call("get_composition", {{"page", 2}}),
       call("flow_text", {{"from_words", "words nobody wrote here"}})}};
  // The passage RETYPED by the model (a word changed on the way): laid
  // as the person wrote it. And words over the cover's: told.
  std::string retyped =
      passage.substr(0, passage.find("w3x0 "));
  retyped.replace(retyped.find("w1x5 "), 4, "CHANGED");
  script->replies.push_back(
      {call("add_text", {{"page", 1}, {"text", "Over"}, {"x", 60},
                         {"y", 310}, {"width", 200}, {"height", 40},
                         {"color", "#000000"}, {"shadow", true},
                         {"glow", true}, {"glow_color", "#ffcc00"}}),
       call("create_composition",
            {{"name", "Retyped"}, {"width", 600}, {"height", 800}}),
       call("flow_text", {{"text", retyped}}),
       // A passage stopping short of the text: the rest is told.
       call("flow_text", {{"from_words", "w1x0 w1x1"}, {"asset", ""},
                          {"to_words", "w1x3"}, {"first_page", 2}}),
       // Nothing being made: a wait says so; a second is refused.
       call("wait_for_generations", Json::object()),
       call("wait_for_generations", Json::object())});
  // A polygon -- a badge -- in a three-colour radial gradient.
  script->replies.push_back(
      {call("add_shape", {{"page", 1}, {"shape", "polygon"},
                          {"points", {{300, 40}, {360, 140}, {240, 140}}},
                          {"gradient", {"#ff0000", "#00ff00", "#0000ff"}},
                          {"gradient_type", "radial"}})});
  AgentRequest req;
  req.project = *pid;
  req.prompt = "Lay this out as a booklet:\n\n" + passage;
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  const Json st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  const Json steps = jget(st, "steps", Json::array());
  REQUIRE(steps.size() == 17);
  for (std::size_t i = 0; i < 17; ++i) {
    CHECK(jget<std::string>(steps[i], "state", "") ==
          (i == 9 || i == 15 ? "failed" : "done"));
  }
  CHECK(jget(steps[14], "result", Json()).contains("note"));
  {
    // The badge: its box from its corners, they in its units, its
    // gradient's colours spread evenly round its middle.
    const AssetId re = *AssetId::parse(jget<std::string>(
        jget(steps[11], "result", Json()), "composition", ""));
    auto ra = ctl.project(*pid)->asset(re);
    REQUIRE_OK(ra);
    bool seen = false;
    for (const auto& l : ra->layers) {
      auto m = ctl.project(*pid)->asset(*l.source);
      for (const auto& o : m.ok() && m->markup ? m->markup->objects
                                               : Json::array()) {
        if (jget<std::string>(o, "kind", "") != "polygon") {
          continue;
        }
        seen = true;
        CHECK(jget(o, "x0", 0.0) == 240 && jget(o, "x1", 0.0) == 360);
        CHECK(o["points"].size() == 3);
        CHECK(o["points"][0][0].get<double>() == 0.5);
        const Json g = jget(o, "gradient", Json());
        CHECK(jget<std::string>(g, "kind", "") == "radial");
        REQUIRE(g["stops"].size() == 3);
        CHECK(jget(g["stops"][1], "at", 0.0) == 0.5);
      }
    }
    CHECK(seen);
  }
  // A passage not in the person's message: refused, told so. Words laid
  // over the cover's: told so.
  CHECK(to_text(jget(steps[9], "error", Json())).find("not in") !=
        std::string::npos);
  CHECK(to_text(jget(steps[10], "result", Json())).find("The Title") !=
        std::string::npos);
  {
    const AssetId re = *AssetId::parse(jget<std::string>(
        jget(steps[11], "result", Json()), "composition", ""));
    auto ra = ctl.project(*pid)->asset(re);
    REQUIRE_OK(ra);
    std::string words;
    for (const auto& l : ra->layers) {
      if (auto m = ctl.project(*pid)->asset(*l.source); m.ok()) {
        words += to_text(m->markup->objects);
      }
    }
    CHECK(words.find("CHANGED") == std::string::npos);
    CHECK(words.find("w1x5 ") != std::string::npos);
    CHECK(words.find("w2x0 ") != std::string::npos);
  }
  {
    // Its words found though an empty asset was given too; what the
    // message has before and after the passage told.
    const Json r13 = jget(steps[13], "result", Json());
    const Json more = jget(r13, "more_in_message", Json());
    CHECK(jget<std::string>(more, "next_words", "").starts_with("w1x4"));
    CHECK(jget(more, "words", 0) > 500);
    const Json before = jget(r13, "before_in_message", Json());
    CHECK(jget<std::string>(before, "starts", "").starts_with("It was"));
  }
  const Json flowed = jget(steps[5], "result", Json());
  // Below the rule: 80 and a margin. The whole passage: nothing more.
  CHECK(jget(jget(flowed, "area", Json()), "y", 0) >= 82);
  CHECK(!flowed.contains("more_in_message"));
  CHECK(jget(flowed, "first_page", 0) == 2);
  const int last = jget(flowed, "last_page", 0);
  CHECK(last >= 4);
  CHECK(jget(flowed, "composition_pages", 0) == last);
  CHECK(jget(flowed, "headings", Json::array()).size() == 3);
  CHECK(jget(flowed, "columns", 0) == 1);

  const AssetId comp = *AssetId::parse(jget<std::string>(
      jget(steps[0], "result", Json()), "composition", ""));
  auto a = ctl.project(*pid)->asset(comp);
  REQUIRE_OK(a);
  CHECK(a->pages == last);
  // The every-page markups: the background at the bottom, the header
  // across every page; the cover's words on page 1 alone.
  REQUIRE(!a->layers.empty());
  CHECK(a->layers.front().name == "Every Page (Background)");
  CHECK(a->layers.front().time.offset == 0);
  CHECK(a->layers.front().time.duration == 0);
  int fronts = 0, progress = 0;
  std::string folder;
  for (const auto& f : a->layer_folders) {
    if (f.name == "Page Progress") {
      folder = f.id;
    }
  }
  CHECK(a->layer_folders.size() == 1);
  std::string laid;
  for (const auto& l : a->layers) {
    if (l.name == "Every Page") {
      ++fronts;
      CHECK(l.time.duration == 0);
      continue;
    }
    if (!folder.empty() && l.folder == folder) {
      ++progress;
      continue;
    }
    if (l.name.starts_with("Every Page") || !l.source) {
      continue;
    }
    // Every other layer is its page's alone.
    CHECK(l.time.duration == 1);
    auto m = ctl.project(*pid)->asset(*l.source);
    REQUIRE_OK(m);
    if (l.time.offset == 0) {
      const std::string cover = to_text(m->markup->objects);
      CHECK(cover.find("The Title") != std::string::npos);
      CHECK(cover.find("w0x0") == std::string::npos);
      // "Over": a glow in the colour asked, inside a shadow.
      for (const auto& o : m->markup->objects) {
        if (jget<std::string>(o, "text", "") == "Over") {
          const Json fx = jget(o, "effects", Json::array());
          REQUIRE(fx.size() == 2);
          CHECK(jget<std::string>(fx[0], "kind", "") == "glow");
          CHECK(jget<std::string>(fx[1], "kind", "") == "shadow");
          CHECK(jget(fx[0], "color", Json::array())[1].get<double>() > 0.7);
          CHECK(jget(fx[1], "dy", 0.0) > 0);
        }
      }
      continue;
    }
    for (const auto& o : m->markup->objects) {
      if (jget<std::string>(o, "kind", "") == "text") {
        laid += jget<std::string>(o, "text", "") + " ";
        CHECK(jget(o, "y0", 0.0) > 82);
      }
    }
  }
  CHECK(fronts == 1);
  CHECK(progress == last - 1);  // the second bar replaced the first
  // Every word of the passage, once, in order (the headings' marks and
  // the inline punctuation as read).
  auto squash = [](const std::string& t) {
    std::string o;
    for (char c : t) {
      if (c == '#') {
        continue;
      }
      if (c == ' ' || c == '\n') {
        if (!o.empty() && o.back() != ' ') {
          o += ' ';
        }
      } else {
        o += c;
      }
    }
    while (!o.empty() && o.back() == ' ') {
      o.pop_back();
    }
    while (!o.empty() && o.front() == ' ') {
      o.erase(o.begin());
    }
    return o;
  };
  // Pages are listed by layer, a page at a time, in order.
  CHECK(squash(laid) == squash(passage));
  // Drawn: the background and the header on a text page; the segments
  // filled to the page -- all on the last, the first alone on page 2.
  const fs::path p2 = root / "p2.png", pl = root / "pl.png";
  REQUIRE_OK(ctl.flatten(*pid, comp, p2, std::nullopt, std::nullopt, {}, 1));
  REQUIRE_OK(ctl.flatten(*pid, comp, pl, std::nullopt, std::nullopt, {},
                         last - 1));
  const auto bg = pixel_at(p2, 5, 400);
  CHECK(bg[0] == 0xf4 && bg[1] == 0xef && bg[2] == 0xe6);
  int header = 0;
  for (int x = 300; x < 560; x += 2) {
    header += pixel_at(p2, x, 30)[0] < 0x80;
  }
  CHECK(header > 0);
  // The bar: 48 px margins, near the foot.
  const int y = 800 - 36 - 5 + 2;
  CHECK(pixel_at(pl, 60, y)[0] > 0x90 && pixel_at(pl, 60, y)[1] < 0x50);
  CHECK(pixel_at(pl, 540, y)[0] > 0x90 && pixel_at(pl, 540, y)[1] < 0x50);
  CHECK(pixel_at(p2, 60, y)[1] < 0x50);
  CHECK(pixel_at(p2, 540, y)[1] > 0x90);
  // Each edit one command.
  int agent_steps = 0;
  for (const auto& h : ctl.undo_history(*pid)) {
    agent_steps += jget<std::string>(h, "kind", "") == "agent";
  }
  CHECK(agent_steps == 13);
  REQUIRE_OK(ctl.agent_stop(*id));
}

// Each edit WAITS for the person: approved, it runs; skipped, it does
// not, and the model is told why. A read never waits.
TEST(agent, review_waits_and_a_skip_is_told)
{
  auto root = test::temp_dir("agent-review");
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "R.valtz", "review");
  REQUIRE_OK(pid);
  auto comp = ctl.create_composition(*pid, project::AssetClass::Still,
                                     {64, 64}, {0, 1}, "Doc");
  REQUIRE_OK(comp);
  script->replies = {
      {call("get_project", Json::object()),
       call("add_pages", {{"count", 2}})},
      {call("remove_page", {{"page", 1}})}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "Three pages";
  req.composition = *comp;
  req.review = true;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  Json st = agent_wait(ctl, *id, "review");
  REQUIRE(jget<std::string>(st, "state", "") == "review");
  // The read ran on its own; the edit waits.
  const Json steps = jget(st, "steps", Json::array());
  REQUIRE(steps.size() == 2);
  CHECK(jget<std::string>(steps[0], "state", "") == "done");
  CHECK(jget<std::string>(steps[1], "state", "") == "proposed");
  CHECK(ctl.project(*pid)->asset(*comp)->pages == 1);
  // A step not waiting is refused.
  CHECK(!ctl.agent_review(*id, 1, "approve").ok());
  REQUIRE_OK(ctl.agent_review(*id, 2, "approve"));
  // The next one waits too: skipped, with the person's note.
  for (int i = 0; i < 500; ++i) {
    st = ctl.agent_state(*id);
    if (jget(st, "steps", Json::array()).size() == 3 &&
        jget<std::string>(st, "state", "") == "review") {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(ctl.project(*pid)->asset(*comp)->pages == 3);
  REQUIRE_OK(ctl.agent_review(*id, 3, "skip", Json(), "keep the first"));
  st = agent_wait(ctl, *id, "done");
  CHECK(jget<std::string>(st, "state", "") == "done");
  CHECK(ctl.project(*pid)->asset(*comp)->pages == 3);
  {
    std::lock_guard lk(script->mu);
    const auto& last = script->seen.back();
    CHECK(last.back().role == "tool");
    CHECK(last.back().content.find("keep the first") != std::string::npos);
  }
  // Done, the person says more: it goes on in the same conversation.
  script->replies.push_back({call("add_pages", Json::object())});
  REQUIRE_OK(ctl.agent_set_review(*id, false));
  REQUIRE_OK(ctl.agent_say(*id, "One more page."));
  for (int i = 0; i < 500; ++i) {
    if (ctl.project(*pid)->asset(*comp)->pages == 4) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(ctl.project(*pid)->asset(*comp)->pages == 4);
  REQUIRE_OK(ctl.agent_stop(*id));
  st = agent_wait(ctl, *id, "stopped");
  CHECK(jget<std::string>(st, "state", "") == "stopped");
}

// A picture it generates is a task: it LANDS on the page it named, in
// its box, under everything -- while the model goes on -- and the model
// is told where it went before it finishes.
TEST(agent, a_generated_picture_lands_in_its_box)
{
  auto root = test::temp_dir("agent-land");
  const fs::path krea = root / "models" / "krea" / "Krea-2-Turbo";
  fs::create_directories(krea);
  std::ofstream(krea / "model.safetensors") << "w";
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "L.valtz", "land");
  REQUIRE_OK(pid);
  script->replies = {
      {call("create_composition",
            {{"name", "Deck"}, {"width", 320}, {"height", 180},
             {"pages", 2}})},
      {call("generate_image", {{"prompt", "a red wall"}, {"page", 2},
                               {"x", 160}, {"y", 0}, {"width", 160},
                               {"height", 180}, {"below_all", true},
                               {"model", "krea2-turbo"}}),
       call("add_shape", {{"page", 2}, {"shape", "rect"}, {"x", 200},
                          {"y", 60}, {"width", 40}, {"height", 40},
                          {"fill", "#0000ff"}})}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "A deck with a red wall";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  // The model finished with its picture still out: it waits for it.
  Json st = agent_wait(ctl, *id, "waiting");
  REQUIRE(jget<std::string>(st, "state", "") == "waiting");
  REQUIRE(engine->held() == 1);
  const JobId job =
      *JobId::parse(jget<std::string>(jget(st, "steps", Json())[1], "job",
                                      ""));
  const engine::JobSpec* spec = engine->spec(job);
  REQUIRE(spec != nullptr);
  // Made at the box's shape.
  const double made_w = jget(spec->params, "width", 0.0);
  const double made_h = jget(spec->params, "height", 0.0);
  REQUIRE(made_h > 0);
  CHECK(std::abs(made_w / made_h - 160.0 / 180.0) < 0.02);
  REQUIRE(write_png(root / "red.png", 80, 90, {0xff, 0, 0, 0xff}));
  engine->start(job);
  engine->finish(job, root / "red.png");
  st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  const Json steps = jget(st, "steps", Json::array());
  CHECK(jget<std::string>(steps[1], "state", "") == "done");
  CHECK(jget(jget(steps[1], "result", Json()), "page", 0) == 2);
  const AssetId comp =
      *AssetId::parse(jget<std::string>(st, "composition", ""));
  const fs::path p2 = root / "p2.png";
  REQUIRE_OK(ctl.flatten(*pid, comp, p2, std::nullopt, std::nullopt, {}, 1));
  CHECK(pixel_at(p2, 300, 170)[0] > 200);   // the picture, in its box
  CHECK(pixel_at(p2, 80, 90)[3] == 0);      // left of it: nothing
  CHECK(pixel_at(p2, 220, 80)[2] > 200);    // the shape, above it
  // The model was told where it landed.
  std::lock_guard lk(script->mu);
  bool told = false;
  for (const auto& m : script->seen.back()) {
    told = told || (m.role == "user" &&
                    m.content.find("has finished") != std::string::npos);
  }
  CHECK(told);
}

// A TIMELINE cut from clips (DESIGN §9b): parts of two clips, the second
// out of the first through black, a picture after them through white, a
// title over the first second, a clip's own sound turned down -- and
// music that LANDS from the start, cut at its length and faded out. Times
// are seconds, as the model gives them; what each tool did is read back
// from the layers.
TEST(agent, the_tools_cut_a_timeline)
{
  auto root = test::temp_dir("agent-timeline");
  for (const char* repo : {"m-a-p/YuE2-3B", "m-a-p/YuE2-Vae"}) {
    const fs::path d = root / "models" / repo;
    fs::create_directories(d);
    std::ofstream(d / "model.safetensors") << "w";
  }
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "V.valtz", "vlog");
  REQUIRE_OK(pid);
  REQUIRE(write_png(root / "red.png", 32, 32, {0xff, 0, 0, 0xff}));
  REQUIRE(write_png(root / "blue.png", 32, 32, {0, 0, 0xff, 0xff}));
  REQUIRE(write_tone(root / "tone.wav", 2, 440, 0.5));
  auto red = import_and_wait(ctl, *pid, root / "red.png");
  auto blue = import_and_wait(ctl, *pid, root / "blue.png");
  auto tone = import_and_wait(ctl, *pid, root / "tone.wav");
  REQUIRE(red && blue && tone);
  // Two clips of two seconds at 24, with sound: movies imported.
  const auto clip = [&](AssetId pic, const char* name)
      -> std::optional<AssetId> {
    auto shot = ctl.create_composition(
        *pid, project::AssetClass::Composition, {32, 32}, {24, 1}, name);
    if (!shot.ok()) {
      return std::nullopt;
    }
    auto in = ctl.instantiate(*pid, pic, *shot);
    project::LayerTime two;
    two.duration = 48;
    if (!in.ok() || !ctl.set_layer_time(*pid, *shot, in->second, two).ok() ||
        !ctl.instantiate(*pid, *tone, *shot).ok()) {
      return std::nullopt;
    }
    auto file = ctl.rendered(*pid, *shot);
    if (!file.ok()) {
      return std::nullopt;
    }
    const fs::path to = root / (std::string(name) + ".mov");
    fs::copy_file(*file, to);
    return import_and_wait(ctl, *pid, to);
  };
  auto a = clip(*red, "beach");
  auto b = clip(*blue, "market");
  REQUIRE(a && b);

  script->replies = {
      {call("create_composition",
            {{"name", "Trip"}, {"kind", "timeline"}, {"width", 64},
             {"height", 36}, {"fps", 24}})},
      {call("add_clip", {{"asset", a->str()}, {"in", 0.5}, {"out", 1.5}}),
       // At the beach's end, as a model puts it: the dip still
       // overlaps the beach, as a dissolve does.
       call("add_clip", {{"asset", b->str()}, {"start", 1.0},
                         {"transition", "dip-black"},
                         {"transition_seconds", 0.25}})},
      {call("add_text", {{"text", "Lisbon"}, {"start", 0}, {"duration", 1},
                         {"x", 4}, {"y", 4}, {"width", 56}, {"height", 12},
                         {"size", 8}}),
       // Its panel, asked for after the words: behind them.
       call("add_shape", {{"shape", "rect"}, {"start", 0}, {"duration", 1},
                          {"x", 2}, {"y", 2}, {"width", 60}, {"height", 16},
                          {"fill", "#00000080"}}),
       call("add_clip", {{"asset", red->str()}, {"duration", 1}}),
       call("set_layer_sound", {{"layer", "0"}, {"volume", 0.25}})},
      {call("generate_music", {{"style", "lo-fi hip hop, mellow piano"},
                               {"seconds", 3}, {"fade_out", 1}}),
       call("set_layer_time", {{"layer", "1"}, {"out", 1.0}}),
       // A dip between two that do not meet (the market now ends at
       // 1.75, the picture starts at 2.75): the picture moves to overlap
       // it by half a second, as for a dissolve.
       call("set_transition", {{"from", "1"}, {"to", "3"},
                               {"kind", "dip-white"}}),
       call("get_composition", Json::object())}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "A vlog of my trip with lo-fi music";
  req.row = {*a, *b};
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  // The model finished with its music still out: it waits for it.
  Json st = agent_wait(ctl, *id, "waiting");
  REQUIRE(jget<std::string>(st, "state", "") == "waiting");
  const auto songs = engine->held_of(engine::kOpGenerateAudio);
  REQUIRE(songs.size() == 1);
  // A little past its length: its cut fades out over music.
  CHECK(jget(engine->spec(songs[0])->params, "max_seconds", 0.0) >= 4);
  REQUIRE(write_tone(root / "song.wav", 5, 220, 0.5));
  engine->start(songs[0]);
  engine->finish(songs[0], root / "song.wav");
  st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  const Json steps = jget(st, "steps", Json::array());
  for (const auto& x : steps) {
    CHECK(jget<std::string>(x, "state", "") == "done");
  }
  const AssetId tl =
      *AssetId::parse(jget<std::string>(st, "composition", ""));
  auto comp = ctl.project(*pid)->asset(tl);
  REQUIRE_OK(comp);
  CHECK(comp->cls == project::AssetClass::Composition);
  CHECK(comp->rate == Rational(24, 1));
  // Bottom first: the clips and the picture under the title, the music
  // on top.
  std::vector<std::string> ids;
  for (const auto& l : comp->layers) {
    ids.push_back(l.id);
  }
  REQUIRE(ids.size() == 5);
  CHECK(ids[0] == "" && ids[1] == "1" && ids[2] == "3" && ids[3] == "2" &&
        ids[4] == "4");
  auto plan = ctl.sound_plan(*pid, tl);
  REQUIRE_OK(plan);
  const auto& L = plan->layers;
  REQUIRE(L.size() == 5);
  const auto near = [](double x, double y) { return std::abs(x - y) < 0.03; };
  // The beach's part: 0.5-1.5 s of it, from 0.
  CHECK(near(L[0].timing.start, 0) && near(L[0].timing.length, 1));
  CHECK(near(L[0].timing.in, 0.5) && near(L[0].timing.out, 1.5));
  // The market from 0.75 (out of the beach through black), trimmed to
  // 1 s.
  CHECK(near(L[1].timing.start, 0.75) && near(L[1].timing.length, 1));
  REQUIRE(comp->transitions.size() == 2);
  CHECK(comp->transitions[0].from == "" && comp->transitions[0].to == "1");
  CHECK(comp->transitions[0].kind == "dip-black");
  CHECK(comp->transitions[1].from == "1" && comp->transitions[1].to == "3");
  CHECK(comp->transitions[1].kind == "dip-white");
  // The picture, added after the market as it was (2.75), a second --
  // moved to 1.25 by the dip asked into it.
  CHECK(near(L[2].timing.start, 1.25) && near(L[2].timing.length, 1));
  // The title over the first second, above the clips: its panel and its
  // words on one markup, the panel drawn first.
  CHECK(comp->layers[3].time.offset == 0);
  CHECK(comp->layers[3].time.duration == 24);
  {
    auto mk = ctl.project(*pid)->asset(*comp->layers[3].source);
    REQUIRE_OK(mk);
    const Json objs = mk->markup ? mk->markup->objects : Json();
    REQUIRE(objs.is_array() && objs.size() == 2);
    CHECK(jget<std::string>(objs[0], "kind", "") == "rect");
    CHECK(jget<std::string>(objs[1], "kind", "") == "text");
  }
  // The beach's own sound at a quarter.
  const auto quiet = Controller::sound_keys_of(*comp, "");
  REQUIRE(quiet.keys.size() == 1);
  CHECK(near(quiet.keys[0].value.volume, 0.25));
  // The music: from 0, asked for 3 s but cut where the clips end (the
  // picture's 2.25), faded out over its last second.
  CHECK(near(L[4].timing.start, 0) && near(L[4].timing.length, 2.25));
  const auto music = Controller::sound_keys_of(*comp, "4");
  REQUIRE(music.keys.size() == 3);
  CHECK(music.keys[0].frame == 0 && near(music.keys[0].value.volume, 0.8));
  CHECK(music.keys[1].frame == 30 && near(music.keys[1].value.volume, 0.8));
  CHECK(music.keys[2].frame == 54 && music.keys[2].value.volume == 0);
  // get_composition told it in seconds, the title's words with it.
  const std::string told = to_text(jget(steps[10], "result", Json()));
  CHECK(told.find("\"kind\":\"title\"") != std::string::npos);
  CHECK(told.find("Lisbon") != std::string::npos);
  CHECK(told.find("\"dip-black\"") != std::string::npos);
  // Each edit one command -- and the music's landing one of its own,
  // of its kind: the trim came after its request.
  int edits = 0;
  for (const auto& h : ctl.undo_history(*pid)) {
    edits += jget<std::string>(h, "kind", "") == "agent";
  }
  CHECK(edits == 11);
  // Shortened, the music running past the end is cut there, its fade
  // moved with it.
  script->replies.push_back(
      {call("set_timeline_length", {{"seconds", 2.0}})});
  REQUIRE_OK(ctl.agent_say(*id, "Make it two seconds."));
  for (int i = 0; i < 500; ++i) {
    st = ctl.agent_state(*id);
    if (jget(st, "steps", Json::array()).size() == 12 &&
        jget<std::string>(st, "state", "") == "done") {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  auto shorter = ctl.project(*pid)->asset(tl);
  REQUIRE_OK(shorter);
  auto plan2 = ctl.sound_plan(*pid, tl);
  REQUIRE_OK(plan2);
  CHECK(near(plan2->layers[4].timing.length, 2.0));
  const auto faded = Controller::sound_keys_of(*shorter, "4");
  REQUIRE(faded.keys.size() == 3);
  CHECK(faded.keys[1].frame == 24 && faded.keys[2].frame == 48);
  // Undone: the length, then the landing -- its layer.
  REQUIRE_OK(ctl.undo(*pid));
  REQUIRE_OK(ctl.undo(*pid));
  auto after = ctl.project(*pid)->asset(tl);
  REQUIRE_OK(after);
  CHECK(after->layers.size() == 4);
  REQUIRE_OK(ctl.agent_stop(*id));
}

// A LONG SCENE, shot after shot (DESIGN §9b): the model splits it into
// shots and asks for each with generate_video -- the first from its words
// (FL2VA), each next one CARRYING ON from the end of the one before it
// (Ref2VA, its tail picture and sound) and asked for while that one is
// still being made: held, and sent the moment it lands; a cut drawing on
// a picture of the character. Each prompt is put together in MiniMax H3's
// guide form, its references by the model's names. The shots land in
// order end to end, however they finish; one whose shot before it fails
// fails with it.
TEST(agent, a_long_scene_is_shot_after_shot)
{
  auto root = test::temp_dir("agent-scene");
  for (const char* f :
       {"MiniMaxAI/MiniMax-H3/FL2VA/model.safetensors",
        "MiniMaxAI/MiniMax-H3/Ref2VA/model.safetensors",
        "lightx2v/Minimax-h3-Turbo/"
        "minimax_h3_fl2v_turbo_8step_v1.0_768p_bf16.safetensors",
        "lightx2v/Minimax-h3-Turbo/"
        "minimax_h3_ref2v_turbo_8step_v1.0_768p_bf16.safetensors"}) {
    const fs::path file = root / "models" / f;
    fs::create_directories(file.parent_path());
    std::ofstream(file) << "w";
  }
  auto script = std::make_shared<ScriptedBackend::Shared>();
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  cfg.agent_backend = [script] {
    return std::unique_ptr<agent::Backend>(
        std::make_unique<ScriptedBackend>(script));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  auto pid = ctl.create_project(root / "S.valtz", "scene");
  REQUIRE_OK(pid);
  // The keeper's picture, and what each shot comes back as: a 2 s clip
  // with sound.
  REQUIRE(write_png(root / "keeper.png", 32, 32, {0x30, 0x60, 0x90, 0xff}));
  REQUIRE(write_tone(root / "tone.wav", 2, 330, 0.5));
  auto keeper = import_and_wait(ctl, *pid, root / "keeper.png");
  auto tone = import_and_wait(ctl, *pid, root / "tone.wav");
  REQUIRE(keeper && tone);
  fs::path shot_file;
  {
    auto c = ctl.create_composition(*pid, project::AssetClass::Composition,
                                    {32, 32}, {24, 1}, "made");
    REQUIRE_OK(c);
    auto in = ctl.instantiate(*pid, *keeper, *c);
    REQUIRE_OK(in);
    project::LayerTime two;
    two.duration = 48;
    REQUIRE_OK(ctl.set_layer_time(*pid, *c, in->second, two));
    REQUIRE_OK(ctl.instantiate(*pid, *tone, *c));
    auto file = ctl.rendered(*pid, *c);
    REQUIRE_OK(file);
    shot_file = root / "shot.mov";
    fs::copy_file(*file, shot_file);
  }
  const Json style = "[Shot 1] A lighthouse keeper climbs a spiral stair.";
  script->replies = {
      {call("create_composition",
            {{"name", "Lighthouse"}, {"kind", "timeline"}, {"width", 960},
             {"height", 544}, {"fps", 24}})},
      {call("generate_video",
            {{"description", style},
             {"soundscape", "Wind, footsteps on iron."},
             {"seconds", 2}}),
       call("generate_video",
            {{"continues", true},
             {"subjects", "<Subject 1> is the lighthouse keeper, grey "
                          "beard, yellow oilskin, in <Video 1>."},
             {"summary", "<Subject 1> reaches the lamp room."},
             {"description", "[Shot 1] He opens the hatch."},
             {"seconds", 2}}),
       // As a model writes a scene: no subjects (the shot before's are
       // carried on), a start in the scene's seconds (it lands after the
       // shot before all the same).
       call("generate_video",
            {{"continues", true},
             {"summary", "<Subject 1> lights the lamp."},
             {"description", "[Shot 1] He strikes a match."},
             {"music", "Low strings swell."},
             {"start", 1.0},
             {"seconds", 2}}),
       call("generate_video",
            {{"references", Json::array({keeper->str()})},
             {"subjects", "<Subject 1> is the lighthouse keeper in "
                          "<Picture 1>."},
             {"summary", "<Subject 1> looks out to sea from the gallery."},
             {"description", "[Shot 1] Wide shot from the sea at dusk."},
             {"seconds", 2}})}};
  AgentRequest req;
  req.project = *pid;
  req.prompt = "A lighthouse keeper lights the lamp at dusk";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  Json st = agent_wait(ctl, *id, "waiting");
  REQUIRE(jget<std::string>(st, "state", "") == "waiting");
  // Sent now: the first shot (words) and the cut (a picture); the two
  // carrying on are held.
  auto clips = engine->held_of(engine::kOpGenerateVideo);
  REQUIRE(clips.size() == 2);
  const engine::JobSpec* s1 = engine->spec(clips[0]);
  const engine::JobSpec* s4 = engine->spec(clips[1]);
  REQUIRE(s1 && s4);
  CHECK(s1->model.id == "minimax-h3-fl2va");
  CHECK(s4->model.id == "minimax-h3-ref2va");
  CHECK(jget(s1->params, "frames", 0) == 56);  // 2 s, up to 17n + 5
  CHECK(jget(s1->params, "width", 0) == 960);
  CHECK(jget(s1->params, "height", 0) == 544);
  const auto p1 = jget<std::string>(s1->params, "prompt", "");
  CHECK(p1.find("integrated_multimodal_description: [Shot 1] A "
                "lighthouse keeper") != std::string::npos);
  CHECK(p1.find("overall_soundscape: Wind") != std::string::npos);
  CHECK(p1.find("non_diegetic_music: N/A") != std::string::npos);
  const auto p4 = jget<std::string>(s4->params, "prompt", "");
  CHECK(p4.find("[reference generation] <Subject 1> looks out") !=
        std::string::npos);
  CHECK(p4.find("<Subject 1> (appears in [Shot 1]): fully_preserved") !=
        std::string::npos);
  CHECK(p4.find("video continuation") == std::string::npos);
  {
    const Json steps = jget(st, "steps", Json::array());
    REQUIRE(steps.size() == 5);
    for (int i : {1, 2, 3, 4}) {
      CHECK(jget<std::string>(steps[i], "state", "") == "waiting");
    }
    CHECK(jget(jget(steps[2], "result", Json()), "after_step", 0) == 2);
    CHECK(jget(jget(steps[3], "result", Json()), "after_step", 0) == 3);
  }
  // The cut lands first -- in its place, after the third shot.
  engine->start(clips[1]);
  engine->finish(clips[1], shot_file);
  // The first lands: the second is sent, carrying on from it.
  engine->start(clips[0]);
  engine->finish(clips[0], shot_file);
  std::vector<JobId> next;
  for (int i = 0; i < 200 && next.empty(); ++i) {
    next = engine->held_of(engine::kOpGenerateVideo);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(next.size() == 1);
  st = ctl.agent_state(*id);
  const auto shot1 = AssetId::parse(jget<std::string>(
      jget(jget(st, "steps", Json())[1], "result", Json()), "asset", ""));
  REQUIRE(shot1);
  const engine::JobSpec* s2 = engine->spec(next[0]);
  REQUIRE(s2);
  CHECK(s2->model.id == "minimax-h3-ref2va");
  const auto p2 = jget<std::string>(s2->params, "prompt", "");
  CHECK(p2.find("subject_definitions:\n<Subject 1> is the lighthouse "
                "keeper") != std::string::npos);
  CHECK(p2.find("<Video 1> is the source video whose final moment") !=
        std::string::npos);
  CHECK(p2.find("<Audio 1> is the synchronized audio track of <Video 1>") !=
        std::string::npos);
  CHECK(p2.find("[video continuation + audio reference] The target video "
                "continues directly from the end of <Video 1>.") !=
        std::string::npos);
  CHECK(p2.find("[Shot 1] The shot continues seamlessly from the final "
                "moment of <Video 1>. He opens the hatch.") !=
        std::string::npos);
  bool continues = false;
  for (const auto& e : jget(s2->params, "reference_media", Json())) {
    continues = continues ||
                (jget<std::string>(e, "role", "") == "continue" &&
                 jget<std::string>(e, "asset", "") == shot1->str());
  }
  CHECK(continues);
  engine->start(next[0]);
  engine->finish(next[0], shot_file);
  // ...and the third from the second.
  next.clear();
  for (int i = 0; i < 200 && next.empty(); ++i) {
    next = engine->held_of(engine::kOpGenerateVideo);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(next.size() == 1);
  {
    const auto p3 =
        jget<std::string>(engine->spec(next[0])->params, "prompt", "");
    CHECK(p3.find("non_diegetic_music:\nLow strings swell.") !=
          std::string::npos);
    CHECK(p3.find("subject_definitions:\n<Subject 1> is the lighthouse "
                  "keeper, grey beard") != std::string::npos);
  }
  engine->start(next[0]);
  engine->finish(next[0], shot_file);
  st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  for (const auto& x : jget(st, "steps", Json::array())) {
    CHECK(jget<std::string>(x, "state", "") == "done");
  }
  // End to end, in the order asked: each where the one before it was
  // booked to end (2.33 s a shot), whatever order they finished in.
  const AssetId tl =
      *AssetId::parse(jget<std::string>(st, "composition", ""));
  auto plan = ctl.sound_plan(*pid, tl);
  REQUIRE_OK(plan);
  std::vector<double> starts;
  for (const auto& l : plan->layers) {
    starts.push_back(l.timing.start);
  }
  std::ranges::sort(starts);
  REQUIRE(starts.size() == 4);
  const double shot = 56.0 / 24.0;
  for (int i = 0; i < 4; ++i) {
    CHECK(std::abs(starts[static_cast<std::size_t>(i)] - i * shot) < 0.05);
  }
  {
    auto comp = ctl.project(*pid)->asset(tl);
    REQUIRE_OK(comp);
    CHECK(comp->layers.size() == 4);
  }

  // More: a shot, and one held behind it -- the first fails, and the
  // held one fails with it, never sent.
  script->replies.push_back(
      {call("generate_video", {{"description", "[Shot 1] Dawn."},
                               {"seconds", 2}}),
       call("generate_video", {{"continues", true},
                               {"subjects", "<Subject 1> is the sea in "
                                            "<Video 1>."},
                               {"summary", "The sun rises."},
                               {"description", "[Shot 1] Sunrise."}})});
  REQUIRE_OK(ctl.agent_say(*id, "Add the dawn after it."));
  next.clear();
  for (int i = 0; i < 200 && next.empty(); ++i) {
    next = engine->held_of(engine::kOpGenerateVideo);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(next.size() == 1);
  st = agent_wait(ctl, *id, "waiting");
  engine->start(next[0]);
  engine->fail(next[0], "out of memory");
  st = agent_wait(ctl, *id, "done");
  REQUIRE(jget<std::string>(st, "state", "") == "done");
  const Json steps = jget(st, "steps", Json::array());
  REQUIRE(steps.size() == 7);
  CHECK(jget<std::string>(steps[5], "state", "") == "failed");
  CHECK(jget<std::string>(steps[6], "state", "") == "failed");
  CHECK(jget<std::string>(steps[6], "error", "").find("was not made") !=
        std::string::npos);
  CHECK(engine->held_of(engine::kOpGenerateVideo).empty());
  REQUIRE_OK(ctl.agent_stop(*id));
}

// A FLEET member runs the agent's turns (DESIGN §9b): a Mac with the
// memory for the helper, beside this one, whose own tools work on the
// document here. Every turn goes to the member that holds the
// conversation, under the key it keeps it by in its own fleet folder, as
// long as the lease; the session over, the member lets it go.
TEST(agent, a_fleet_member_runs_the_turns)
{
  if (!std::getenv("VALTZ_TEST_FLEET")) {
    SKIP("set VALTZ_TEST_FLEET=1 (two members on this Mac's loopback)");
  }
  auto root = test::temp_dir("agent-fleet");
  const std::string key = fleet::derive_key("Studio", "a-long-test-secret");
  auto conf = [&](const char* name, bool discoverable) {
    fleet::Config c;
    c.member_id = Uuid::v7().str();
    c.member_name = name;
    c.fleet = "Studio";
    c.key = key;
    c.discoverable = discoverable;
    (void)fleet::save_config(root / (std::string(name) + ".json"), c);
  };
  conf("big", true);
  conf("here", false);
  // The member: a helper "installed" where it is looked for.
  const fs::path helper =
      root / "big-models" / "mlx-community" / "Qwen3.5-9B-OptiQ-4bit";
  fs::create_directories(helper);
  std::ofstream(helper / "config.json") << "{}";
  std::ofstream(helper / "model.safetensors") << "w";
  auto make = [&](const char* name, const fs::path& models,
                  HeldEngine** e) {
    ControllerConfig cfg;
    cfg.support_root = root / (std::string(name) + "-support");
    cfg.model_roots = {models};
    cfg.with_engine = false;
    cfg.extensions = false;
    cfg.fleet = true;
    cfg.fleet_config = root / (std::string(name) + ".json");
    cfg.engine_factory = [e] {
      auto x = std::make_unique<HeldEngine>();
      *e = x.get();
      return std::unique_ptr<engine::Engine>(std::move(x));
    };
    return Controller::create(cfg);
  };
  HeldEngine* big_engine = nullptr;
  HeldEngine* here_engine = nullptr;
  auto big = make("big", root / "big-models", &big_engine);
  REQUIRE_OK(big);
  auto here = make("here", root / "here-models", &here_engine);
  REQUIRE_OK(here);
  Controller& ctl = **here;
  int port = 0;
  REQUIRE(eventually([&] {
    port = jget((*big)->fleet_status(), "port", 0);
    return port > 0;
  }, 10));
  ctl.fleet_connect("127.0.0.1", port);
  REQUIRE(eventually([&] {
    for (const auto& m : jget(ctl.fleet_status(), "members", Json())) {
      if (!jget<std::string>(jget(m, "self", Json::object()), "assistant",
                             "").empty()) {
        return true;
      }
    }
    return false;
  }, 15));
  REQUIRE_OK(ctl.fleet_configure(
      {{"offload", {{"tasks", {{"agent-turn", "always"}}}}}}));
  auto pid = ctl.create_project(root / "F.valtz", "fleet agent");
  REQUIRE_OK(pid);
  AgentRequest req;
  req.project = *pid;
  req.prompt = "A deck";
  req.review = false;
  auto id = ctl.agent_start(req);
  REQUIRE_OK(id);
  // The first turn: on the member, kept under its key in its folder.
  std::vector<JobId> turns;
  REQUIRE(eventually([&] {
    turns = big_engine->held_of("agent-turn");
    return !turns.empty();
  }, 15));
  CHECK(here_engine->held_of("agent-turn").empty());
  std::string kept;
  {
    const engine::JobSpec* spec = big_engine->spec(turns[0]);
    REQUIRE(spec != nullptr);
    const Json conv = jget(spec->params, "conversation", Json::object());
    kept = jget<std::string>(conv, "context", "");
    CHECK(kept.starts_with("fleet-here-valtz-agent-"));
    CHECK(jget<std::string>(conv, "context_dir", "").find("big-files") !=
          std::string::npos);
    CHECK(jget(spec->params, "keep_loaded", 0.0) >= 600);
    CHECK(jget(conv, "messages", Json::array()).size() == 2);
    CHECK(!jget(conv, "tools", Json::array()).empty());
  }
  big_engine->turn(turns[0],
                   {{"text", "<tool_call>...</tool_call>"},
                    {"content", ""},
                    {"finish", "tool_calls"},
                    {"tool_calls", Json::array(
                        {{{"name", "create_composition"},
                          {"arguments", "{\"name\":\"Deck\",\"width\":64,"
                                        "\"height\":36}"}}})},
                    {"restored", "new"}});
  // Its tool ran HERE, on this Mac's document; the next turn went to the
  // same member, the conversation one message longer each way.
  REQUIRE(eventually([&] {
    turns = big_engine->held_of("agent-turn");
    return !turns.empty();
  }, 15));
  {
    const engine::JobSpec* spec = big_engine->spec(turns[0]);
    REQUIRE(spec != nullptr);
    const Json conv = jget(spec->params, "conversation", Json::object());
    CHECK(jget<std::string>(conv, "context", "") == kept);
    const Json msgs = jget(conv, "messages", Json::array());
    REQUIRE(msgs.size() == 4);
    CHECK(msgs[2]["content"] == "<tool_call>...</tool_call>");
    CHECK(msgs[3]["role"] == "tool");
  }
  Json st = ctl.agent_state(*id);
  REQUIRE(jget(st, "steps", Json::array()).size() == 1);
  CHECK(jget<std::string>(jget(st, "steps", Json())[0], "state", "") ==
        "done");
  CHECK(jget<std::string>(jget(st, "backend", Json()), "kind", "") ==
        "fleet");
  CHECK(jget<std::string>(jget(st, "backend", Json()), "where", "") ==
        "big");
  big_engine->turn(turns[0], {{"text", "Made it."},
                              {"content", "Made it."},
                              {"finish", "stop"},
                              {"restored", "memory"}});
  st = agent_wait(ctl, *id, "done");
  CHECK(jget<std::string>(st, "state", "") == "done");
  // Over: the member lets the conversation go.
  REQUIRE_OK(ctl.agent_stop(*id));
  CHECK(eventually([&] {
    const auto d = big_engine->dropped();
    return std::ranges::find(d, kept) != d.end();
  }, 15));
}

// MOSS-IMAGE's own prompt-enhancer skill on the helper (DESIGN §9): the
// skill as its makers publish it, then Valtz's note -- reply with the
// draft itself, laid out on the picture's canvas, the size asked for --
// and the reply, a LAYOUT, compiled into the prompt their script makes
// for that canvas.
TEST(controller, moss_images_enhancer_writes_a_layout_compiled_here)
{
  auto root = test::temp_dir("ctl-moss-enhance");
  const fs::path helper =
      root / "models" / "mlx-community" / "Qwen3.5-9B-OptiQ-4bit";
  fs::create_directories(helper);
  std::ofstream(helper / "config.json") << "{}";
  std::ofstream(helper / "model.safetensors") << "w";
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  EnhancePromptRequest req;
  req.prompt = "a poster for a tea shop called Leaf";
  req.model = "moss-image-sft";
  req.size = "1024x768";
  auto job = ctl.enhance_prompt(req);
  REQUIRE_OK(job);
  const auto chats = engine->held_of(engine::kOpChat);
  REQUIRE(chats.size() == 1);
  const auto text =
      jget<std::string>(engine->spec(chats[0])->params, "text", "");
  CHECK(text.find("# MOSS-Image Prompt Enhancer") != std::string::npos);
  CHECK(text.find("# Pixel Layout draft") != std::string::npos);
  CHECK(text.find("made at exactly 1024 x 768 pixels") != std::string::npos);
  CHECK(text.find("a tea shop called Leaf (Output size: 1024x768.)") !=
        std::string::npos);
  engine->turn(chats[0],
               {{"text", R"({"width": 1024, "height": 768,
                  "bbox_coord_space": "abs", "bbox_format": "xyxy",
                  "summary": {"caption": "A tea shop poster."},
                  "items": [{"label": "text",
                             "caption": "Bold green lettering.",
                             "text": "Leaf",
                             "bbox": [312, 96, 712, 256]}]})"}});
  std::string prompt;
  for (int i = 0; i < 50 && prompt.empty(); ++i) {
    Event ev;
    if (ctl.events().wait(ev, 100) && ev.job == *job &&
        ev.kind == "assist.enhanced") {
      prompt = jget<std::string>(ev.data, "prompt", "");
    }
  }
  CHECK(prompt ==
        R"({"summary":{"caption":"A tea shop poster."},"items":[)"
        R"({"label":"text","caption":"Bold green lettering.","text":"Leaf",)"
        R"("bbox":[-288,-200,-128,200]}],"bbox_coord_space":)"
        R"("abs_centralized","bbox_format":"yxyx"})");
}

// An enhance's speed comes with its suggestion, as an agent's turn's
// does: vpipe's counts for its chat -- what it read and wrote, its
// drafter's rounds -- on Valtz's clock (Settings › Agentic Helper › Show
// the Helper's Speed; valtzctl enhance --speed).
TEST(controller, an_enhances_speed_comes_with_it)
{
  auto root = test::temp_dir("ctl-enhance-speed");
  const fs::path helper =
      root / "models" / "mlx-community" / "Qwen3.5-9B-OptiQ-4bit";
  fs::create_directories(helper);
  std::ofstream(helper / "config.json") << "{}";
  std::ofstream(helper / "model.safetensors") << "w";
  ControllerConfig cfg;
  cfg.support_root = root / "support";
  cfg.model_roots = {root / "models"};
  cfg.with_engine = false;
  cfg.extensions = false;
  HeldEngine* engine = nullptr;
  cfg.engine_factory = [&engine] {
    auto e = std::make_unique<HeldEngine>();
    engine = e.get();
    return std::unique_ptr<engine::Engine>(std::move(e));
  };
  auto made = Controller::create(cfg);
  REQUIRE_OK(made);
  REQUIRE(engine != nullptr);
  Controller& ctl = **made;
  EnhancePromptRequest req;
  req.prompt = "a fox in snow";
  auto job = ctl.enhance_prompt(req);
  REQUIRE_OK(job);
  const auto chats = engine->held_of(engine::kOpChat);
  REQUIRE(chats.size() == 1);
  engine->turn(chats[0],
               {{"text", R"({"prompt": "A red fox in fresh snow."})"},
                {"prefill_ms", 250}, {"prefilled", 1000},
                {"decode_ms", 4000}, {"decoded", 300}, {"ctx_pos", 1301},
                {"drafter", "mtp"}, {"rounds", 120}, {"drafted", 240},
                {"accepted", 180}});
  Json stats;
  for (int i = 0; i < 50 && stats.is_null(); ++i) {
    Event ev;
    if (ctl.events().wait(ev, 100) && ev.job == *job &&
        ev.kind == "assist.enhanced") {
      CHECK(jget<std::string>(ev.data, "prompt", "") ==
            "A red fox in fresh snow.");
      stats = jget(ev.data, "stats", Json());
    }
  }
  REQUIRE(stats.is_object());
  CHECK(stats["prefill_tokens"] == 1000);
  CHECK(stats["prefill_tps"] == 4000.0);
  CHECK(stats["decode_tokens"] == 300);
  CHECK(stats["decode_tps"] == 75.0);
  CHECK(stats["ctx_pos"] == 1301);
  CHECK(stats["drafter"] == "mtp");
  CHECK(stats["rounds"] == 120);
  CHECK(stats["per_round"] == 2.5);
  CHECK(stats["accepted_pct"] == 75);
  CHECK(stats.contains("turn_ms"));
  CHECK(!stats.contains("first_ms"));   // no words were streamed
}
