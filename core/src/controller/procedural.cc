// PROGRAMS DRAWN (DESIGN §4t, media/procedural.h): a program kept as a
// text asset, its rendering a derived picture or clip made from it --
// a task like a generation's, in a line of its own.

#include "valtz/controller/controller.h"

#include "valtz/base/log.h"
#include "valtz/base/message.h"
#include "valtz/base/text.h"
#include "valtz/media/movie.h"
#include "valtz/media/probe.h"

#include <cmath>
#include <format>
#include <fstream>
#include <random>
#include <set>

namespace valtz {

namespace fs = std::filesystem;
using project::AssetKind;

namespace {

// What a rendering is made by: its recipe's op.
constexpr std::string_view kOpRenderProgram = "render-program";
// A program's tag, and the Assets folder programs go in -- found by its
// role, whatever the person renamed it.
constexpr const char* kProgramTag = "program";
constexpr const char* kProgramsFolderRole = "programs";
constexpr const char* kProgramsFolderName = "Programs";
// The longest clip a program is drawn as; the most assets it reads.
constexpr double kMaxSeconds = 600;
constexpr std::size_t kMaxAssets = 32;


// A name's ".js" taken off.
std::string
bare(std::string name)
{
  if (name.ends_with(".js")) {
    name.resize(name.size() - 3);
  }
  return name;
}

Json
log_json(const std::vector<media::ProgramLog>& log, std::size_t most = 60)
{
  Json out = Json::array();
  for (const auto& l : log) {
    if (out.size() >= most) {
      break;
    }
    Json j = {{"level", l.level}, {"text", l.text}};
    if (!l.source.empty()) {
      j["source"] = l.source;
    }
    if (l.line > 0) {
      j["line"] = l.line;
    }
    if (l.column > 0) {
      j["column"] = l.column;
    }
    out.push_back(std::move(j));
  }
  return out;
}

// The first errors, as words: why a program failed.
std::string
errors_text(const std::vector<media::ProgramLog>& log, int most = 3)
{
  std::string out;
  int n = 0;
  for (const auto& l : log) {
    if (l.level != "error" || n++ >= most) {
      continue;
    }
    out += (out.empty() ? "" : "; ") + media::to_text(l);
  }
  return out;
}

}  // namespace

bool
Controller::is_program(const project::Asset& a)
{
  return a.kind == AssetKind::Text &&
         std::ranges::find(a.tags, kProgramTag) != a.tags.end();
}

Result<ProceduralStarted>
Controller::render_procedural(ProceduralRequest req)
{
  auto undo = command_(req.project, "procedural");
  VALTZ_ASSIGN(ProceduralJob j, procedural_define_(req));
  const ProceduralStarted started{j.job, j.program, j.asset};
  std::lock_guard lk(_bg_mu);
  _bg.emplace_back([this, j = std::move(j)] {
    CoreWrites own;
    (void)procedural_run_(j);
  });
  return started;
}

Result<Controller::ProceduralJob>
Controller::procedural_define_(const ProceduralRequest& req)
{
  project::Project* p = project(req.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (req.width < 16 || req.height < 16 ||
      req.width > media::kProgramMaxSide ||
      req.height > media::kProgramMaxSide) {
    return make_error(Code::InvalidArgument, std::format(
        "a program is drawn at 16 to {} pixels a side, not {} x {}",
        media::kProgramMaxSide, req.width, req.height));
  }
  if (!(req.seconds >= 0) || req.seconds > kMaxSeconds) {
    return make_error(Code::InvalidArgument, std::format(
        "a program's clip is at most {} s", kMaxSeconds));
  }
  // The program: kept new, a new version of one -- its words kept once
  // they are drawn -- or one as it is.
  bool made = false;
  std::optional<std::string> next_words;
  AssetId program;
  std::string name = one_line(req.name);
  if (!one_line(req.code).empty()) {
    if (req.program) {
      VALTZ_ASSIGN(project::Asset a, p->asset(*req.program));
      if (a.kind != AssetKind::Text) {
        return make_error(Code::InvalidArgument, std::format(
            "\"{}\" is not a program: a program is a text", a.name));
      }
      next_words = req.code;
      program = *req.program;
      if (name.empty()) {
        name = bare(a.name);
      }
    } else {
      if (name.empty()) {
        name = "Program";
      }
      VALTZ_ASSIGN(project::Asset a,
                   p->add_text(name + ".js", req.code, {kProgramTag}));
      program = a.id;
      made = true;
      // In the Programs folder, made the first time.
      if (auto folders = p->folders(); folders.ok()) {
        std::string folder;
        for (const auto& f : *folders) {
          if (jget<std::string>(f, "role", "") == kProgramsFolderRole) {
            folder = jget<std::string>(f, "id", "");
          }
        }
        if (folder.empty()) {
          folder = AssetId::make().str();
          folders->push_back({{"id", folder},
                              {"name", kProgramsFolderName},
                              {"role", kProgramsFolderRole}});
          if (!p->set_folders(*folders).ok()) {
            folder.clear();
          }
        }
        if (!folder.empty()) {
          (void)p->set_asset_folder(program, folder);
        }
      }
      post_("assets.changed", JobId{}, {{"project", req.project},
                                        {"asset", program},
                                        {"reason", "program"}});
    }
  } else if (req.program) {
    VALTZ_ASSIGN(project::Asset a, p->asset(*req.program));
    if (a.kind != AssetKind::Text) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is not a program: a program is a text", a.name));
    }
    program = *req.program;
    if (name.empty()) {
      name = bare(a.name);
    }
  } else {
    return make_error(Code::InvalidArgument,
                      "no program: its code, or a program of the project");
  }

  // Its rate: the one asked, else the project's.
  Rational fps{24, 1};
  if (req.fps > 0) {
    fps = Rational(static_cast<std::int64_t>(std::lround(req.fps * 1000)),
                   1000);
  } else if (auto out = p->output(); out.ok() && out->fps.valid() &&
                                     out->fps.num > 0) {
    fps = out->fps;
  }
  const int frames =
      req.seconds > 0
          ? std::max(1, static_cast<int>(std::lround(req.seconds *
                                                     fps.to_double())))
          : 0;
  std::uint64_t seed = 0;
  if (req.seed) {
    seed = *req.seed;
  } else {
    std::random_device rd;
    seed = (static_cast<std::uint64_t>(rd()) << 20) ^ rd();
    seed &= (std::uint64_t{1} << 52) - 1;  // a number JavaScript keeps
  }
  std::string codec(media::kDefaultIntermediate);
  if (auto out = p->output(); out.ok() && !out->intermediate.empty()) {
    codec = out->intermediate;
  }

  project::Recipe r;
  r.op = std::string(kOpRenderProgram);
  r.params = {{"width", req.width},
              {"height", req.height},
              {"frames", frames},
              {"fps_num", fps.num},
              {"fps_den", fps.den},
              {"seed", seed},
              {"epoch_ms", project::now_ms()},
              {"codec", frames > 0 ? codec : std::string()}};
  r.inputs = {{"program", program, 0}};
  // Its assets: inputs of the drawing, each under the name the program
  // reads it by -- kept, whatever the asset is called later.
  Json named = Json::array();
  std::set<AssetId> seen;
  for (const auto& id : req.assets) {
    if (!seen.insert(id).second || id == program) {
      continue;
    }
    VALTZ_ASSIGN(project::Asset a, p->asset(id));
    if (a.kind != AssetKind::Image && a.kind != AssetKind::Video &&
        a.kind != AssetKind::Audio && a.kind != AssetKind::Text) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is a {}: a program reads pictures, clips, sounds and "
          "texts", a.name, project::to_str(a.kind)));
    }
    if (a.kind == AssetKind::Video && a.cls != project::AssetClass::Flat &&
        a.cls != project::AssetClass::Generated) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is a timeline: a program reads a clip of its own -- "
          "flatten it first", a.name));
    }
    if (seen.size() > kMaxAssets) {
      return make_error(Code::InvalidArgument, std::format(
          "a program reads {} assets at most", kMaxAssets));
    }
    r.inputs.push_back({"asset", id, 0});
    named.push_back({{"id", id.str()}, {"name", a.name}});
  }
  if (!named.empty()) {
    r.params["assets"] = std::move(named);
  }
  // The same program, size, clock and seed draw the same frames.
  r.deterministic = true;
  r.created_ms = project::now_ms();
  const AssetKind kind = frames > 0 ? AssetKind::Video : AssetKind::Image;
  // Drawn again: the drawing asked for, of this program and this kind,
  // its next version -- its recipe pointed at this one once it is drawn
  // (pointed there first, a try that failed left it "recipe changed").
  std::optional<AssetId> again;
  if (req.drawing) {
    auto d = p->asset(*req.drawing);
    auto dr = d.ok() ? p->recipe(d->recipe)
                     : Result<project::Recipe>(d.error());
    if (dr.ok() && d->kind == kind && dr->op == kOpRenderProgram &&
        !dr->inputs.empty() && dr->inputs.front().asset == program) {
      again = *req.drawing;
    }
  }
  if (again) {
    VALTZ_ASSIGN(ProceduralJob j,
                 procedural_job_(req.project, *again, std::move(r)));
    j.made_program = made;
    j.code = std::move(next_words);
    return j;
  }
  VALTZ_ASSIGN(project::Asset a,
               p->define_derived(result_name_(kind, name), kind,
                                 std::move(r), 120, /*numbered=*/true));
  VALTZ_ASSIGN(ProceduralJob j, procedural_job_(req.project, a.id));
  j.made_program = made;
  j.made_asset = true;
  j.code = std::move(next_words);
  return j;
}

Result<Controller::ProceduralJob>
Controller::procedural_job_(ProjectId pid, AssetId aid,
                            std::optional<project::Recipe> next)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  VALTZ_ASSIGN(project::Recipe r,
               next ? Result<project::Recipe>(*next) : p->recipe(a.recipe));
  if (r.op != kOpRenderProgram || r.inputs.empty()) {
    return make_error(Code::InvalidArgument,
                      std::format("\"{}\" is not drawn by a program",
                                  a.name));
  }
  VALTZ_ASSIGN(auto inputs, p->resolve_inputs(r));
  ProceduralJob j;
  j.job = JobId::make();
  j.project = pid;
  j.program = r.inputs.front().asset;
  j.asset = aid;
  j.recipe = std::move(next);
  j.stop = std::make_shared<std::atomic<bool>>(false);
  const Json& q = r.params;
  j.render.width = jget(q, "width", 1024);
  j.render.height = jget(q, "height", 1024);
  j.render.frames = jget(q, "frames", 0);
  j.render.fps = Rational(jget<std::int64_t>(q, "fps_num", 24),
                          jget<std::int64_t>(q, "fps_den", 1));
  j.render.seed = jget<std::uint64_t>(q, "seed", 0);
  j.render.epoch_ms = jget(q, "epoch_ms", 0.0);
  j.render.codec = jget<std::string>(q, "codec", "");
  if (j.render.codec.empty()) {
    j.render.codec = std::string(media::kDefaultIntermediate);
  }

  JobRecord rec;
  rec.id = j.job;
  rec.op = std::string(kOpRenderProgram);
  rec.purpose = "build";
  rec.title = std::format("Draw “{}”", a.name);
  rec.project = pid;
  rec.asset = aid;
  rec.recipe = r;
  rec.inputs = std::move(inputs);
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  // Its commit joins the command it was asked in.
  if (project::Workspace* w = workspace_(pid)) {
    if (const auto seq = w->undo().recording()) {
      std::lock_guard lk(_job_commands_mu);
      _job_commands[j.job] = seq;
    }
  }
  {
    std::lock_guard lk(_procedural_stops_mu);
    _procedural_stops[j.job] = j.stop;
  }
  post_("job.queued", rec.id, {{"title", rec.title}, {"op", rec.op},
                               {"purpose", rec.purpose}, {"project", pid},
                               {"asset", aid}, {"program", j.program}});
  return j;
}

Result<std::vector<media::ProgramAsset>>
Controller::program_assets_(project::Project& p, const project::Recipe& r)
{
  std::vector<media::ProgramAsset> out;
  const Json named = jget(r.params, "assets", Json::array());
  for (const auto& in : r.inputs) {
    if (in.role != "asset") {
      continue;
    }
    VALTZ_ASSIGN(project::Asset a, p.asset(in.asset));
    media::ProgramAsset x;
    x.id = in.asset.str();
    x.name = a.name;
    // The name the program reads it by: as it was when it was given.
    for (const auto& n : named) {
      if (jget<std::string>(n, "id", "") == x.id) {
        x.name = jget<std::string>(n, "name", a.name);
      }
    }
    const bool flat = a.cls == project::AssetClass::Flat ||
                      a.cls == project::AssetClass::Generated;
    switch (a.kind) {
    case AssetKind::Image: x.kind = "image"; break;
    case AssetKind::Video: x.kind = "video"; break;
    case AssetKind::Audio: x.kind = "audio"; break;
    default: x.kind = "text"; break;
    }
    if (x.kind == "image" && !flat) {
      // A still composition, a markup: as drawn (its first page).
      VALTZ_ASSIGN(x.file, rendered_frame_(p.id(), in.asset, 0, 0, 0));
    } else if ((x.kind == "video" || x.kind == "audio") && !flat) {
      return make_error(Code::InvalidArgument, std::format(
          "\"{}\" is a composition: a program reads a clip or a sound of "
          "its own -- flatten it first", a.name));
    } else {
      VALTZ_ASSIGN(x.file, p.media_path(in.asset));
    }
    x.type = media::program_type(x.file);
    if (x.type.empty() && x.kind == "image") {
      VALTZ_ASSIGN(x.file, rendered_frame_(p.id(), in.asset, 0, 0, 0));
      x.type = "image/png";
    } else if (x.type.empty()) {
      x.type = x.kind == "text" ? "text/plain"
               : x.kind == "audio" ? "audio/wav"
                                   : "video/quicktime";
    }
    if (auto v = p.version(in.asset); v.ok()) {
      x.width = v->info.frame.width;
      x.height = v->info.frame.height;
      x.seconds = v->info.duration.seconds();
    }
    if (x.kind == "image" && !flat) {
      if (auto f = own_frame(p.id(), in.asset); f.ok()) {
        x.width = f->width;
        x.height = f->height;
      }
    }
    out.push_back(std::move(x));
  }
  return out;
}

Result<Controller::ProceduralOutcome>
Controller::procedural_run_(const ProceduralJob& j,
                            std::vector<media::ProgramLog>* log_out)
{
  auto forget = [&] {
    std::lock_guard lk(_procedural_stops_mu);
    _procedural_stops.erase(j.job);
  };
  // Gone without a rendering: what it made -- the drawing, and the
  // program the request made when nothing else is made from it.
  auto clean = [&] {
    auto undo = join_(j.project, j.job);
    project::Project* p = project(j.project);
    if (!p) {
      return;
    }
    if (j.made_asset) {
      (void)p->remove_asset(j.asset);
    }
    if (j.made_program) {
      auto deps = p->dependents(j.program, false);
      if (deps.ok() && deps->empty()) {
        (void)p->remove_asset(j.program);
      }
    }
    post_("assets.changed", JobId{}, {{"project", j.project},
                                      {"asset", j.asset},
                                      {"reason", "removed"}});
  };
  auto failed = [&](const Error& e, const std::vector<media::ProgramLog>& l)
      -> Error {
    const bool stopped = e.code == Code::Cancelled;
    clean();
    finish_job_(j.job, stopped ? JobState::Cancelled : JobState::Failed,
                e.message);
    if (stopped) {
      post_("job.cancelled", j.job, {{"project", j.project}});
    } else {
      post_("job.failed", j.job, {{"code", to_str(e.code)},
                                  {"message", e.message},
                                  {"project", j.project},
                                  {"log", log_json(l)}});
    }
    forget();
    return e;
  };

  // One at a time: a page on the GPU beside another's frames was slower
  // for both.
  std::unique_lock one(_procedural_mu);
  if (j.stop->load()) {
    return failed(make_error(Code::Cancelled, "stopped"), {});
  }
  project::Project* p = project(j.project);
  if (!p) {
    return failed(make_error(Code::NotFound, msg::kProjectNotOpen), {});
  }
  // Its next words, or its words as kept.
  auto text = j.code ? Result<std::string>(*j.code)
                     : p->read_text(j.program);
  if (!text.ok()) {
    return failed(text.error(), {});
  }
  _jobs.update(j.job, [](JobRecord& r) { r.state = JobState::Running; });
  post_("job.started", j.job, {{"engine", "WebKit"}});

  const fs::path tmp = p->blobs().tmp_dir();
  media::ProgramRender spec = j.render;
  spec.program = tmp / (j.job.str() + ".js");
  {
    std::ofstream f(spec.program, std::ios::binary);
    f.write(text->data(), static_cast<std::streamsize>(text->size()));
    if (!f) {
      return failed(make_error(Code::Io, std::format(
                        "cannot write {}", spec.program.string())),
                    {});
    }
  }
  // Its assets, served to its page as they are now.
  {
    auto a = p->asset(j.asset);
    auto r = j.recipe ? Result<project::Recipe>(*j.recipe)
             : a.ok() ? p->recipe(a->recipe)
                      : Result<project::Recipe>(a.error());
    if (!r.ok()) {
      return failed(r.error(), {});
    }
    auto files = program_assets_(*p, *r);
    if (!files.ok()) {
      return failed(files.error(), {});
    }
    spec.assets = std::move(*files);
  }
  const bool hevc = spec.codec == "hevc";
  spec.out = tmp / (j.job.str() + (spec.frames == 0 ? ".png"
                                   : hevc           ? ".mp4"
                                                    : ".mov"));
  struct Gone {
    fs::path p;
    ~Gone()
    {
      std::error_code ec;
      fs::remove(p, ec);
    }
  } gone{spec.program};

  using Clock = std::chrono::steady_clock;
  auto told = Clock::now() - std::chrono::seconds(1);
  auto progress = [&](int done, int of) {
    if (done < of &&
        Clock::now() - told < std::chrono::milliseconds(200)) {
      return;
    }
    told = Clock::now();
    const double f = of > 0 ? static_cast<double>(done) / of : 0.0;
    _jobs.update(j.job, [f](JobRecord& r) {
      r.progress = static_cast<float>(f);
    });
    post_("job.progress", j.job,
          {{"progress", f}, {"phase", "drawing"}, {"step", done},
           {"steps", of}});
  };
  // A clip's frames on the stage as they are drawn: its preview.
  auto preview = [&](media::ProgramPreview pv) {
    const int of = std::max(1, spec.frames);
    const double f = static_cast<double>(pv.frame + 1) / of;
    auto t = engine::make_owned_tensor(engine::Tensor::DType::U8,
                                       {3, pv.height, pv.width},
                                       std::move(pv.rgb), Json::object());
    post_("job.preview", j.job,
          {{"step", pv.frame + 1}, {"steps", of}, {"progress", f}}, t);
  };
  // A clip's soundtrack, when its program makes one: at the project's
  // sample rate, joined to its picture.
  if (spec.frames > 0) {
    spec.sound = tmp / (j.job.str() + ".wav");
    if (auto o = p->output(); o.ok() && o->sample_rate > 0) {
      spec.sample_rate = o->sample_rate;
    }
  }
  std::vector<media::ProgramLog> log;
  auto run = media::render_program(spec, progress, j.stop.get(), &log,
                                   preview);
  if (log_out) {
    *log_out = log;
  }
  struct Wav {
    fs::path p;
    ~Wav()
    {
      std::error_code ec;
      if (!p.empty()) {
        fs::remove(p, ec);
      }
    }
  } wav{spec.sound};
  if (!run.ok()) {
    std::error_code ec;
    fs::remove(spec.out, ec);
    return failed(run.error(), log);
  }
  // Nothing drawn -- the page clear at its first and its last frame: no
  // picture to keep (a helper's first try left an empty clip in Assets).
  if (run->empty) {
    std::error_code ec;
    fs::remove(spec.out, ec);
    // Why, as the page held it -- and its errors.
    const std::string errs = errors_text(log);
    std::string why = run->empty_why.empty()
                          ? std::string("the page is clear at its first "
                                        "and its last frame")
                          : run->empty_why;
    if (!errs.empty()) {
      why += "; " + errs;
    }
    return failed(make_error(Code::Engine,
                             "the program drew nothing: " + why),
                  log);
  }

  // Its sound joined: PCM in a ProRes .mov, AAC in an HEVC .mp4.
  if (!run->sound.empty()) {
    const fs::path joined = tmp / (j.job.str() + "-sound" +
                                   spec.out.extension().string());
    auto st = media::add_soundtrack(
        spec.out, run->sound, joined, 0, 0,
        spec.codec == "hevc" ? media::SoundCodec::Aac
                             : media::SoundCodec::Pcm);
    std::error_code ec;
    fs::remove(spec.out, ec);
    if (!st.ok()) {
      fs::remove(joined, ec);
      return failed(st.error(), log);
    }
    spec.out = joined;
  }
  VALTZ_ASSIGN(media::MediaInfo info, media::probe_file(spec.out));
  auto undo = join_(j.project, j.job);
  // Drawn: its next size and length kept, its next words -- the
  // program's next version.
  if (j.recipe) {
    if (auto st = p->set_recipe(j.asset, *j.recipe); !st.ok()) {
      return failed(st.error(), log);
    }
  }
  if (j.code) {
    if (auto v = p->update_text(j.program, *j.code); !v.ok()) {
      return failed(v.error(), log);
    }
    post_("assets.changed", JobId{}, {{"project", j.project},
                                      {"asset", j.program},
                                      {"reason", "program"}});
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(j.asset));
  VALTZ_ASSIGN(project::Recipe r, p->recipe(a.recipe));
  auto inputs = p->resolve_inputs(r);
  if (!inputs.ok()) {
    return failed(inputs.error(), log);
  }
  project::Project::BuildRecord b;
  b.recipe = std::move(r);
  b.inputs = std::move(*inputs);
  b.output = spec.out;
  b.info = std::move(info);
  b.engine = "WebKit (valtz-procedural)";
  b.host = _host;
  b.outputs["log"] = log_json(log);
  b.outputs["errors"] = run->errors;
  b.timing = {{"total", run->seconds}};
  auto v = p->commit_build(j.asset, std::move(b));
  if (!v.ok()) {
    return failed(v.error(), log);
  }
  post_("assets.changed", j.job, {{"project", j.project},
                                  {"asset", j.asset},
                                  {"version", v->number},
                                  {"reason", "built"}});
  finish_job_(j.job, JobState::Finished, "");
  post_("job.finished", j.job,
        {{"project", j.project},
         {"asset", j.asset},
         {"program", j.program},
         {"errors", run->errors},
         {"log", log_json(log)}});
  forget();
  return ProceduralOutcome{*run, j.program, j.asset};
}

}  // namespace valtz
