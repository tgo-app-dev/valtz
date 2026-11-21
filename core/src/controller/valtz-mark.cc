// File › Insert Valtz Mark (DESIGN §10a Insert Valtz Mark): the mark and
// its sonic logo (media/valtz-mark.h) made a HIDDEN asset of the project
// -- once for each size and rate asked, reused after -- and put on a
// composition: a timeline gets the clip (the sweep and the chord), a
// still the mark settled, a composition of sound the chord alone; a
// picture's in the middle third of the frame each way.

#include "valtz/controller/controller.h"

#include "valtz/base/log.h"
#include "valtz/media/valtz-mark.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace valtz {

namespace fs = std::filesystem;
using project::AssetClass;
using project::AssetKind;

namespace {

constexpr const char* kMarkTag = "valtz-mark";

bool
has_tag(const project::Asset& a, std::string_view t)
{
  return std::find(a.tags.begin(), a.tags.end(), t) != a.tags.end();
}

}  // namespace

Result<Controller::MarkInserted>
Controller::insert_valtz_mark(ProjectId pid, std::optional<AssetId> onto,
                              std::int64_t offset)
{
  auto undo = command_(pid, "mark.insert");
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // Onto what is asked; else the project's; else a 960 x 544 timeline
  // made the project's for it.
  if (!onto) {
    onto = project_composition(pid);
  }
  if (!onto) {
    Rational rate{24, 1};
    if (auto o = project_output(pid); o.ok() && o->fps.num > 0) {
      rate = o->fps;
    }
    VALTZ_ASSIGN(AssetId made,
                 create_composition(pid, AssetClass::Composition,
                                    {960, 544}, rate, {}, true));
    onto = made;
    offset = 0;
  }
  VALTZ_ASSIGN(project::Asset c, p->asset(*onto));
  if (c.cls != AssetClass::Still && c.cls != AssetClass::Composition) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", c.name}});
  }
  const bool sound = c.kind == AssetKind::Audio;
  const bool still = c.cls == AssetClass::Still;
  int bw = 0, bh = 0;
  Rational rate = rate_of_(*p, c);
  if (!sound) {
    VALTZ_ASSIGN(media::PixelSize frame, shown_size_(*p, c.id, 0, 0));
    // A third of the frame each way, even (a 4:2:2 or 4:4:4 movie's
    // sides).
    bw = std::max(16, static_cast<int>(frame.width / 3) & ~1);
    bh = std::max(16, static_cast<int>(frame.height / 3) & ~1);
  }
  const std::string variant =
      sound   ? std::string("valtz-mark:sound")
      : still ? std::format("valtz-mark:still:{}x{}", bw, bh)
              : std::format("valtz-mark:clip:{}x{}@{}/{}", bw, bh,
                            rate.num, rate.den);

  // Made before (at this size and rate): the same hidden asset.
  std::optional<AssetId> mark;
  VALTZ_ASSIGN(auto all, p->assets());
  for (const auto& a : all) {
    if (a.head > 0 && has_tag(a, variant)) {
      mark = a.id;
      break;
    }
  }
  if (!mark) {
    const fs::path tmp = p->blobs().tmp_dir();
    const std::string stem =
        std::format("valtz-mark-{}", JobId::make().str());
    const fs::path wav = tmp / (stem + ".wav");
    std::error_code ec;
    struct Gone {
      fs::path p;
      ~Gone()
      {
        std::error_code e;
        fs::remove(p, e);
      }
    } wav_gone{wav};
    fs::path file;
    if (sound || !still) {
      VALTZ_TRY(media::write_sonic_logo(wav));
    }
    if (sound) {
      file = tmp / (stem + "-sound.wav");
      fs::rename(wav, file, ec);
      if (ec) {
        return make_error(Code::Io, std::format("cannot keep {}",
                                                file.string()));
      }
    } else {
      media::MarkRender r;
      r.width = bw;
      r.height = bh;
      r.out = tmp / (stem + (still ? ".png" : ".mov"));
      if (!still) {
        r.fps = rate;
        r.frames = std::max<int>(1, static_cast<int>(std::lround(
            media::kMarkSeconds * rate.num / std::max<std::int64_t>(
                                                 1, rate.den))));
        r.sound = wav;
      }
      VALTZ_TRY(media::render_valtz_mark(r));
      file = r.out;
    }
    VALTZ_ASSIGN(AssetId made, import_taken_(pid, file, "Valtz Mark", ""));
    VALTZ_TRY(p->update_asset(made, [&](project::Asset& x) -> Status {
      x.tags.push_back("hidden");
      x.tags.push_back(kMarkTag);
      x.tags.push_back(variant);
      return ok_status();
    }));
    mark = made;
  }

  // A new layer of its own on top, where the player is (a still: on the
  // page shown); a picture at its own size is centred on the frame.
  VALTZ_ASSIGN(auto put, instantiate(pid, *mark, *onto, std::nullopt,
                                     std::max<std::int64_t>(0, offset),
                                     /*fill=*/false));
  return MarkInserted{put.first, put.second, *mark};
}

}  // namespace valtz
