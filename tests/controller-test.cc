#include "testing.h"

#include <algorithm>
#include <array>

#include "valtz/controller/controller.h"
#include "valtz/media/camera.h"
#include "valtz/media/markup.h"
#include "valtz/media/probe.h"
#include "valtz/media/sound.h"
#include "valtz/project/migrate.h"
#include "valtz/project/project-file.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
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
    _jobs.push_back({std::move(spec), std::move(sink)});
    return ok_status();
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

  // Its result, `file`, then the end.
  void
  finish(JobId id, const fs::path& file)
  {
    auto it = find(id);
    auto sink = it->sink;
    const fs::path out = it->spec.output_dir / std::format(
        "{}.png", id.str());
    fs::copy_file(file, out, fs::copy_options::overwrite_existing);
    _jobs.erase(it);
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Output;
    ev.output = out;
    if (auto info = media::probe_file(out); info.ok()) {
      ev.output_info = *info;
    }
    sink(ev);
    engine::JobEvent end;
    end.job = id;
    end.kind = engine::JobEventKind::Finished;
    sink(end);
  }

  std::size_t held() const { return _jobs.size(); }

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
  std::vector<Job>::iterator
  find(JobId id)
  {
    return std::ranges::find_if(_jobs, [&](const Job& j) {
      return j.spec.id == id;
    });
  }
  std::vector<Job> _jobs;
};

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

  // Both gated models are said to be, in Capabilities' tree.
  int gated = 0;
  for (const auto& f : jget(ctl.capability_tree(), "families",
                            Json::array())) {
    for (const auto& m : jget(f, "members", Json::array())) {
      const auto id = jget<std::string>(m, "model", "");
      if (jget(m, "gated", false)) {
        ++gated;
        CHECK(id == "krea2-turbo" || id == "flux2-klein-9b");
      }
    }
  }
  CHECK(gated == 2);

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
  auto w = layer_weights(two, {{0, 1, false}}, 1.4);   // cut: a still on
  CHECK(w.opacity[0] == 1 && w.opacity[1] == 0);
  w = layer_weights(two, {{0, 1, false}}, 1.6);        // after the middle
  CHECK(w.opacity[0] == 0 && w.opacity[1] == 1);
  w = layer_weights(two, {{0, 1, true}}, 1.25);        // a dissolve
  CHECK(w.opacity[0] == 1 && std::abs(w.opacity[1] - 0.25) < 1e-9);
  CHECK(std::abs(w.gain[0] - 0.75) < 1e-9 &&
        std::abs(w.gain[1] - 0.25) < 1e-9);
  w = layer_weights(two, {}, 1.5);                     // none: they stack
  CHECK(w.opacity[0] == 1 && w.opacity[1] == 1);

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
  plan.transitions = {{0, 1, true}};
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

// Layer FOLDERS (DESIGN §6a): layers gathered under a folder lie together
// in the stack -- a new layer above one of them joins it, layers placed
// in or out by a drag, one moved between two of them joins it -- shown or
// hidden together, renamed, ungrouped; a folder left empty goes.
// A still put on a timeline runs a second from where it was put, and its
// block's end stretches it (a clip's length is its marks'); markup is
// drawn at the player's frame -- on a markup showing there, else on a new
// one from there, a second long (DESIGN §6a).
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
  // What it drafts with: its MTP head until DFlash 2 is chosen; the 27B
  // 4-bit names its DFlash 2 drafter.
  CHECK(ctl.assistants()["drafter"] == "mtp");
  REQUIRE_OK(ctl.set_assistant_drafter("dflash", 4));
  CHECK(ctl.assistants()["drafter"] == "dflash");
  CHECK(ctl.assistants()["drafter_bits"] == 4);
  CHECK(ctl.set_assistant_drafter("eagle", 8).code() ==
        Code::InvalidArgument);
  for (const auto& m : ctl.assistants()["models"]) {
    if (m["id"] == "qwen3.8-27b-4bit") {
      CHECK(m["dflash"]["id"] == "qwen3.8-27b-dflash2");
    }
  }
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
