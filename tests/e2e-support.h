// END-TO-END support: a Controller over the models installed here (the
// models folder Settings › Storage names, `locations.json`), the real
// engine, and helpers that wait for its jobs while counting what they
// said. Every e2e test is behind VALTZ_TEST_E2E=1 and SKIPS -- naming the
// model -- when what it needs is not here: a skip is not a pass.

#ifndef VALTZ_TESTS_E2E_SUPPORT_H
#define VALTZ_TESTS_E2E_SUPPORT_H

#include "testing.h"

#include "valtz/base/paths.h"
#include "valtz/controller/controller.h"
#include "valtz/media/probe.h"
#include "valtz/models/model-store.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace valtz::e2e {

namespace fs = std::filesystem;

// Set: the suite runs; its value may narrow it ("quick").
inline const char*
gate()
{
  return std::getenv("VALTZ_TEST_E2E");
}

inline bool
quick()
{
  const char* g = gate();
  return g && std::string(g) == "quick";
}

// What a job did, as its events said it.
struct Ran {
  std::optional<JobRecord> record;
  int                      previews = 0;
  int                      progress = 0;
  std::string              text;      // job.text, streamed
  Json                     finished;  // job.finished's data
  Json                     assist;    // assist.enhanced / assist.intent
  double                   seconds = 0;

  bool ok() const { return record && record->state == JobState::Finished; }
  AssetId asset() const { return record ? record->asset : AssetId{}; }
};

// A Controller over the installed models, a project in a temp folder.
struct Rig {
  fs::path                    root;
  std::unique_ptr<Controller> ctl;
  ProjectId                   pid;

  bool
  open(const std::string& tag)
  {
    root = test::temp_dir("e2e-" + tag);
    ControllerConfig cfg;
    cfg.support_root = root / "support";
    cfg.model_roots =
        models::ModelStore::default_roots(AppPaths::standard().models);
    cfg.extensions = false;
    auto made = Controller::create(cfg);
    if (!made.ok()) {
      std::printf("  controller: %s\n", made.error().message.c_str());
      return false;
    }
    ctl = std::move(*made);
    auto p = ctl->create_project(root / (tag + ".valtz"), tag);
    if (!p.ok()) {
      std::printf("  project: %s\n", p.error().message.c_str());
      return false;
    }
    pid = *p;
    return true;
  }

  ~Rig()
  {
    if (ctl) {
      ctl->shutdown();
    }
    std::error_code ec;
    fs::remove_all(root, ec);
  }

  // `id` runs `op` of `modality` here: installed, with what it needs
  // beside it, and an engine that has the op.
  bool
  runs(const char* id, const char* modality, const char* op) const
  {
    const auto* e = ctl->catalog().find(id);
    return e && ctl->runs_here(*e, modality, op);
  }

  // The job to its end -- up to `seconds` -- its events counted on the
  // way. A refusal is a Ran with no record (its message printed).
  Ran
  wait(Result<JobId> j, int seconds = 900)
  {
    Ran r;
    if (!j.ok()) {
      std::printf("  refused: %s\n", j.error().message.c_str());
      return r;
    }
    const JobId id = *j;
    const auto t0 = std::chrono::steady_clock::now();
    const auto until = t0 + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until) {
      Event ev;
      if (ctl->events().wait(ev, 100) && ev.job == id) {
        if (ev.kind == "job.preview") {
          ++r.previews;
        } else if (ev.kind == "job.progress") {
          ++r.progress;
        } else if (ev.kind == "job.text") {
          r.text += jget<std::string>(ev.data, "text", "");
        } else if (ev.kind == "job.finished") {
          r.finished = ev.data;
        } else if (ev.kind == "assist.enhanced" ||
                   ev.kind == "assist.intent") {
          r.assist = ev.data;
        }
      }
      auto rec = ctl->jobs().get(id);
      if (rec && rec->state != JobState::Queued &&
          rec->state != JobState::Running) {
        // What is still on the bus for it.
        while (ctl->events().wait(ev, 50)) {
          if (ev.job == id && (ev.kind == "assist.enhanced" ||
                               ev.kind == "assist.intent")) {
            r.assist = ev.data;
          }
        }
        r.record = rec;
        break;
      }
    }
    r.seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - t0)
                    .count();
    if (!r.record) {
      r.record = ctl->jobs().get(id);
      std::printf("  job %s: not ended in %d s\n", id.str().c_str(),
                  seconds);
      (void)ctl->cancel(id);
    } else if (r.record->state != JobState::Finished) {
      std::printf("  job %s: %s %s\n", id.str().c_str(),
                  to_str(r.record->state), r.record->message.c_str());
    }
    std::printf("  %s: %.1f s, %d previews\n",
                r.record ? r.record->op.c_str() : "?", r.seconds,
                r.previews);
    return r;
  }

  std::optional<fs::path>
  media(AssetId a) const
  {
    auto p = ctl->project(pid)->media_path(a);
    if (!p.ok()) {
      return std::nullopt;
    }
    return *p;
  }

  std::optional<media::MediaInfo>
  probe(AssetId a) const
  {
    auto p = media(a);
    if (!p) {
      return std::nullopt;
    }
    auto info = media::probe_file(*p);
    if (!info.ok()) {
      return std::nullopt;
    }
    return *info;
  }

  // A file imported, as the asset of its name.
  std::optional<AssetId>
  import(const fs::path& f)
  {
    if (!ctl->import_files(pid, {f}).ok()) {
      return std::nullopt;
    }
    for (int i = 0; i < 1200; ++i) {
      auto all = ctl->project(pid)->assets();
      if (all.ok()) {
        for (const auto& a : *all) {
          if (a.name == f.filename().string()) {
            return a.id;
          }
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return std::nullopt;
  }
};

// A small 8-bit sRGB PNG of one colour, or -- `split` -- its left half
// `rgba` and its right half the complement: a picture with an edge.
inline bool
write_png(const fs::path& path, size_t w, size_t h,
          std::array<std::uint8_t, 4> rgba, bool split = false)
{
  std::vector<std::uint8_t> px(w * h * 4);
  for (size_t y = 0; y < h; ++y) {
    for (size_t x = 0; x < w; ++x) {
      const size_t i = y * w + x;
      const bool other = split && x >= w / 2;
      for (int k = 0; k < 3; ++k) {
        px[i * 4 + k] = other ? std::uint8_t(255 - rgba[k]) : rgba[k];
      }
      px[i * 4 + 3] = rgba[3];
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
  const bool ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  CFRelease(url);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return ok;
}

// A picture file's pixels, 16-bit RGBA as decoded, and its size.
struct Pixels {
  size_t                     w = 0, h = 0;
  std::vector<std::uint16_t> px;

  bool empty() const { return px.empty(); }

  // The channel's mean, of 65535.
  double
  mean(int k) const
  {
    double s = 0;
    for (size_t i = k; i < px.size(); i += 4) {
      s += px[i];
    }
    return px.empty() ? 0 : s / double(px.size() / 4);
  }

  // The spread of the luma, of 65535: a flat picture's is ~0.
  double
  spread() const
  {
    const size_t n = px.size() / 4;
    if (n == 0) {
      return 0;
    }
    double s = 0, s2 = 0;
    for (size_t i = 0; i < n; ++i) {
      const double l = 0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] +
                       0.0722 * px[i * 4 + 2];
      s += l;
      s2 += l * l;
    }
    const double m = s / n;
    return std::sqrt(std::max(0.0, s2 / n - m * m));
  }
};

inline Pixels
pixels_of_file(const fs::path& file)
{
  Pixels out;
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
  out.w = CGImageGetWidth(img);
  out.h = CGImageGetHeight(img);
  out.px.resize(out.w * out.h * 4);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      out.px.data(), out.w, out.h, 16, out.w * 8, cs,
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
          kCGBitmapByteOrder16Little);
  CGContextDrawImage(ctx, CGRectMake(0, 0, out.w, out.h), img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  CGImageRelease(img);
  return out;
}

// The largest difference at a pixel's channel, of 65535 (65536: other
// sizes).
inline int
most_apart(const Pixels& a, const Pixels& b)
{
  if (a.px.size() != b.px.size()) {
    return 65536;
  }
  int m = 0;
  for (size_t i = 0; i < a.px.size(); ++i) {
    m = std::max(m, std::abs(int(a.px[i]) - int(b.px[i])));
  }
  return m;
}

inline Json
agent_wait(Controller& ctl, const std::string& id, const std::string& state,
           double seconds)
{
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(int(seconds * 1000));
  Json st;
  while (std::chrono::steady_clock::now() < until) {
    st = ctl.agent_state(id);
    const std::string now = jget<std::string>(st, "state", "");
    if (now == state || now == "stopped" || now == "failed") {
      return st;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  return st;
}

}

// The gate and a model check, as the first lines of an e2e test.
#define E2E_GATE()                                                      \
  do {                                                                  \
    if (!::valtz::e2e::gate()) {                                        \
      SKIP("set VALTZ_TEST_E2E=1 (real engine, installed models)");     \
    }                                                                   \
  } while (0)

#define E2E_NEEDS(rig, id, modality, op)                                \
  do {                                                                  \
    if (!(rig).runs(id, modality, op)) {                                \
      SKIP(std::string("needs ") + id + " (" + modality + " " + op +    \
           ") installed");                                              \
    }                                                                   \
  } while (0)

#endif
