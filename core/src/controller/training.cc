// LoRA TRAINING (DESIGN §8c): a picture or clip model's adapter trained
// by vpipe's train-lora from the samples the trainer was given -- work in
// no project, kept in the models folder where Custom's list takes it.
// Qwen-Image 2.1 trains on pictures; MiniMax H3 FL2VA on pictures and
// clips with their soundtracks; Ref2VA on those with their references;
// MiniMax-H3-Image on pictures, or edit pairs (catalog `train`: `shape`,
// `media`, `references`).
//
// vpipe's training-dataset reads a FOLDER of pictures, each one's
// caption a .txt beside it. The pictures are wherever the person has
// them, so each run lays out a folder of its own: a link to each
// picture, under a name of its place in the list, and its caption
// written beside it (the one given, else its own .txt copied). Repeated
// (`repeats`), they go in a subfolder "<N>_set", which vpipe reads N
// times an epoch. A sample's REFERENCES -- Ref2VA's, an edit's source --
// go in a `<stem>.refs` beside it, a path a line, as vpipe reads them.
// The encoded dataset is vpipe's cache (`cache_dir` auto: a folder per
// dataset), so a run again over the same samples encodes only what
// changed.

#include "valtz/controller/controller.h"

#include "valtz/assist/micro.h"
#include "valtz/base/log.h"
#include "valtz/base/message.h"
#include "valtz/base/text.h"
#include "valtz/media/thumbnail.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <future>
#include <fstream>
#include <optional>
#include <set>

namespace valtz {

namespace {

namespace fs = std::filesystem;

std::string
lower_ext(const fs::path& p)
{
  std::string e = p.extension().string();
  std::ranges::transform(e, e.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return e;
}

bool
is_picture(const fs::path& p)
{
  static const std::set<std::string> kPictures = {
      ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff"};
  return kPictures.contains(lower_ext(p));
}

// What vpipe's training-dataset reads as a clip (its `media: auto`).
bool
is_clip(const fs::path& p)
{
  static const std::set<std::string> kClips = {
      ".mp4", ".mov", ".mkv", ".webm", ".avi", ".m4v"};
  return kClips.contains(lower_ext(p));
}

// A run's folder name: its name without what a folder name cannot hold.
std::string
folder_name(std::string_view name)
{
  std::string out;
  for (char c : name) {
    out += c == '/' || c == ':' || c == '\\' ? '-' : c;
  }
  while (!out.empty() && (out.front() == '.' || out.front() == ' ')) {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return std::string(utf8_prefix(out, 120));
}

std::string
read_text(const fs::path& p)
{
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

// Only `keys` of `in`, and none of them "automatic": 0, "" or null
// leave the stage's own default.
Json
settled(const Json& in, std::initializer_list<std::string_view> keys)
{
  Json out = Json::object();
  if (!in.is_object()) {
    return out;
  }
  for (const auto k : keys) {
    const std::string key(k);
    if (!in.contains(key)) {
      continue;
    }
    const Json& v = in[key];
    if (v.is_null() || (v.is_string() && v.get<std::string>().empty()) ||
        (v.is_number() && v.get<double>() == 0 && key != "seed" &&
         key != "caption_dropout" && key != "grad_clip" && key != "ema") ||
        (v.is_array() && v.empty())) {
      continue;
    }
    out[key] = v;
  }
  return out;
}

// A sample: its file, its caption given, its references (Ref2VA's, an
// edit's source) -- given, else the `.refs` beside it, each made
// absolute against its folder.
struct Sample {
  fs::path file;
  std::string caption;
  std::vector<fs::path> refs;
};

std::vector<fs::path>
refs_beside(const fs::path& file)
{
  std::vector<fs::path> out;
  fs::path rp = file;
  rp.replace_extension(".refs");
  std::ifstream in(rp);
  std::string line;
  while (std::getline(in, line)) {
    while (!line.empty() && std::isspace(static_cast<unsigned char>(
                                line.back()))) {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    fs::path q(line);
    out.push_back(q.is_relative() ? file.parent_path() / q : q);
  }
  return out;
}

// The samples a request names: each path, a folder's samples in it (and
// under it, but a `references` folder's, as vpipe reads one), in name
// order; captions and references given kept with theirs. Clips where
// the model takes them.
std::vector<Sample>
samples_of(const Json& items, bool clips)
{
  const auto takes = [&](const fs::path& p) {
    return is_picture(p) || (clips && is_clip(p));
  };
  std::vector<Sample> out;
  std::set<fs::path> seen;
  for (const auto& it : items.is_array() ? items : Json::array()) {
    const fs::path p = it.is_string()
        ? fs::path(it.get<std::string>())
        : fs::path(jget<std::string>(it, "path", ""));
    const std::string caption = it.is_object()
        ? jget<std::string>(it, "caption", "") : std::string();
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      std::vector<fs::path> found;
      for (auto i = fs::recursive_directory_iterator(
               p, fs::directory_options::skip_permission_denied, ec);
           !ec && i != fs::recursive_directory_iterator(); i.increment(ec)) {
        if (i->is_directory(ec) &&
            i->path().filename() == "references") {
          i.disable_recursion_pending();
          continue;
        }
        if (i->is_regular_file(ec) && takes(i->path()) &&
            !i->path().filename().string().starts_with(".")) {
          found.push_back(i->path());
        }
      }
      std::ranges::sort(found);
      for (auto& f : found) {
        if (seen.insert(f).second) {
          auto refs = refs_beside(f);
          out.push_back({std::move(f), std::string(), std::move(refs)});
        }
      }
    } else if (fs::is_regular_file(p, ec) && takes(p) &&
               seen.insert(p).second) {
      std::vector<fs::path> refs;
      if (it.is_object() && it.contains("references")) {
        for (const auto& r : jget(it, "references", Json::array())) {
          if (r.is_string() && !r.get<std::string>().empty()) {
            refs.emplace_back(r.get<std::string>());
          }
        }
      } else {
        refs = refs_beside(p);
      }
      out.push_back({p, caption, std::move(refs)});
    }
  }
  return out;
}

// Where runs are kept: <models>/trained, then the folders a run was
// asked into (<support>/trainer.json "folders"), each once.
std::vector<fs::path>
run_roots(const AppPaths& paths)
{
  std::vector<fs::path> roots{paths.models / "trained"};
  const Json j = Json::parse(read_text(paths.support / "trainer.json"),
                             nullptr, false);
  for (const auto& f : jget(j, "folders", Json::array())) {
    if (f.is_string() && !f.get<std::string>().empty()) {
      const fs::path p = f.get<std::string>();
      if (std::ranges::find(roots, p) == roots.end()) {
        roots.push_back(p);
      }
    }
  }
  return roots;
}

void
keep_root(const AppPaths& paths, const fs::path& root)
{
  auto roots = run_roots(paths);
  if (std::ranges::find(roots, root) != roots.end()) {
    return;
  }
  Json j = Json::parse(read_text(paths.support / "trainer.json"), nullptr,
                       false);
  if (!j.is_object()) {
    j = Json::object();
  }
  if (!j.contains("folders") || !j["folders"].is_array()) {
    j["folders"] = Json::array();
  }
  j["folders"].push_back(root.string());
  std::ofstream(paths.support / "trainer.json", std::ios::binary)
      << to_text(j, 2);
}

// A run's folder by its name: where one of that name is, else in
// <models>/trained.
fs::path
run_folder(const AppPaths& paths, const std::string& name)
{
  std::error_code ec;
  for (const auto& r : run_roots(paths)) {
    if (fs::is_directory(r / name, ec)) {
      return r / name;
    }
  }
  return paths.models / "trained" / name;
}

// The model a run trains on, by the catalog: one with a `train` block --
// a copy (jget's value is a temporary: a pointer into it dangled, and
// the trainer read every default as none).
std::optional<Json>
train_block(const models::ModelEntry& m)
{
  const Json vp = jget(m.engine, "vpipe", Json::object());
  if (!vp.is_object() || !vp.contains("train") ||
      !vp["train"].is_object()) {
    return std::nullopt;
  }
  return vp["train"];
}

}  // namespace

Json
Controller::trainer_info() const
{
  Json models = Json::array();
  for (const auto& m : _catalog.models()) {
    const auto t = train_block(m);
    if (!t) {
      continue;
    }
    const bool installed =
        _store->info(m).state == models::InstallState::Installed;
    models.push_back({{"id", m.id}, {"name", m.name},
                      {"installed", installed}, {"defaults", *t},
                      {"license", m.license}});
  }
  return {{"models", std::move(models)},
          {"default_folder", (_paths.models / "trained").string()},
          {"ready", _engine && _engine->supports(engine::kOpTrainLora)},
          {"mac", {{"matrix_cores", _hw.gpu_matrix_cores},
                   {"ram_gb", _hw.ram_bytes >> 30}}},
          {"runs", trained_loras()},
          {"captioner", training_captioner()}};
}

Result<JobId>
Controller::train_lora(const Json& req)
{
  const std::string model_id = jget<std::string>(req, "model", "");
  const auto* m = _catalog.find(model_id);
  if (!m || !train_block(*m)) {
    return make_error(Code::InvalidArgument, msg::kTrainNoModel,
                      {{"model", m ? m->name : model_id}});
  }
  const auto info = _store->info(*m);
  if (info.state != models::InstallState::Installed) {
    return make_error(Code::NotFound, msg::kTrainModelMissing,
                      {{"model", m->name}});
  }
  if (!_engine->supports(engine::kOpTrainLora)) {
    return make_error(Code::Unsupported, msg::kEngineCannotRun,
                      {{"operation", std::string(engine::kOpTrainLora)}});
  }
  const std::string name =
      folder_name(jget<std::string>(req, "name", ""));
  if (name.empty()) {
    return make_error(Code::InvalidArgument, msg::kTrainNoName);
  }
  {
    std::lock_guard lk(_training_mu);
    if (_training.contains(name)) {
      return make_error(Code::Busy, msg::kTrainRunning, {{"name", name}});
    }
  }
  // How the model trains (catalog `train`): its graph's shape, the media
  // it takes, whether its samples carry references.
  const Json tb = *train_block(*m);
  const std::string shape = jget<std::string>(tb, "shape", "");
  const auto media = jget(tb, "media", std::vector<std::string>{"pictures"});
  const bool takes_clips = std::ranges::find(media, "clips") != media.end();
  const bool takes_refs = !jget<std::string>(tb, "references", "").empty();
  auto pics = samples_of(jget(req, "items", Json::array()), takes_clips);
  if (pics.empty()) {
    return make_error(Code::InvalidArgument, msg::kTrainNoPictures);
  }
  bool any_clip = false;
  bool any_refs = false;
  for (auto& smp : pics) {
    any_clip = any_clip || is_clip(smp.file);
    if (!takes_refs) {
      smp.refs.clear();
    }
    for (const auto& r : smp.refs) {
      std::error_code rec;
      if (!fs::is_regular_file(r, rec)) {
        return make_error(Code::NotFound, msg::kTrainReferenceMissing,
                          {{"file", smp.file.filename().string()},
                           {"reference", r.string()}});
      }
    }
    any_refs = any_refs || !smp.refs.empty();
  }
  // MiniMax-H3-Image's text encoder is another model's (`encoder_from`):
  // the first of them here, by its folder.
  fs::path encoder;
  if (shape == "h3-image") {
    const Json vp = jget(m->engine, "vpipe", Json::object());
    for (const auto& id :
         jget(vp, "encoder_from", std::vector<std::string>{})) {
      const auto* ee = _catalog.find(id);
      if (ee && _store->info(*ee).state == models::InstallState::Installed) {
        encoder = _store->info(*ee).path();
        break;
      }
    }
    if (encoder.empty()) {
      return make_error(Code::NotFound, msg::kTrainNoEncoder,
                        {{"model", m->name}});
    }
  }

  // Its folder: the adapter and its checkpoints, previews, the dataset
  // laid out anew each run (what a stopped run resumes from stays).
  // Where asked, else where a run of its name is (it resumes there).
  const std::string asked = jget<std::string>(req, "folder", "");
  const fs::path folder = asked.empty() ? run_folder(_paths, name)
                                        : fs::path(asked) / name;
  if (!asked.empty()) {
    keep_root(_paths, fs::path(asked));
  }
  const fs::path data = folder / "dataset";
  std::error_code ec;
  fs::remove_all(data, ec);
  const int repeats =
      std::clamp(jget(req, "repeats", 1), 1, 100);
  const fs::path into = repeats > 1
      ? data / std::format("{}_set", repeats) : data;
  fs::create_directories(into, ec);
  fs::create_directories(folder / "previews", ec);
  if (ec) {
    return make_error(Code::Io, std::format(
        "{} could not be made: {}", folder.string(), ec.message()));
  }
  Json listed = Json::array();
  for (std::size_t i = 0; i < pics.size(); ++i) {
    const auto& pic = pics[i].file;
    const auto& given = pics[i].caption;
    const std::string stem = std::format(
        "{:04}-{}", i + 1, std::string(utf8_prefix(pic.stem().string(), 80)));
    const fs::path link = into / (stem + pic.extension().string());
    fs::create_symlink(pic, link, ec);
    if (ec) {
      return make_error(Code::Io, std::format(
          "{} could not be linked: {}", pic.string(), ec.message()));
    }
    std::string caption = given;
    if (caption.empty()) {
      fs::path side = pic;
      side.replace_extension(".txt");
      if (fs::is_regular_file(side, ec)) {
        caption = read_text(side);
      }
    }
    if (!caption.empty()) {
      std::ofstream(into / (stem + ".txt"), std::ios::binary) << caption;
    }
    Json refs = Json::array();
    if (!pics[i].refs.empty()) {
      std::ofstream f(into / (stem + ".refs"), std::ios::binary);
      for (const auto& r : pics[i].refs) {
        f << fs::absolute(r).string() << "\n";
        refs.push_back(fs::absolute(r).string());
      }
    }
    listed.push_back({{"path", pic.string()}, {"caption", caption},
                      {"references", refs}});
  }

  Json ds = settled(jget(req, "dataset", Json::object()),
                    {"trigger_word", "caption_template", "missing_caption",
                     "shuffle_tags", "keep_tags", "caption_variants",
                     "resolution", "bucket_step", "max_aspect", "flip",
                     "alpha", "preview_prompts", "validation", "fps",
                     "frames", "windows", "audio", "task_mix"});
  // A clip model's media as the dataset holds them; a picture model's
  // pictures alone.
  ds["media"] = !takes_clips ? "pictures"
                : any_clip ? "auto" : "pictures";
  if (!takes_clips) {
    for (const char* k : {"fps", "frames", "windows", "audio", "task_mix"}) {
      ds.erase(k);
    }
  }
  if (shape != "h3") {
    ds.erase("task_mix");
  }
  ds["dir"] = data.string();
  ds["captions"] = "sidecar";
  Json opt = settled(jget(req, "optimizer", Json::object()),
                     {"optimizer", "learning_rate", "lr_schedule",
                      "warmup_steps", "grad_clip", "ema", "weight_decay",
                      "min_lr_ratio", "restarts", "loraplus_lr_ratio"});
  Json tr = settled(jget(req, "train", Json::object()),
                    {"rank", "alpha", "targets", "steps", "epochs",
                     "batch_size", "seed", "caption_dropout",
                     "timestep_sampling", "save_every", "preview_every",
                     "preview_steps", "preview_cfg", "preview_seed",
                     "recompute", "ane", "i8_gemm", "resume", "init_from",
                     "cache", "audio_loss_weight"});
  if (shape.empty()) {
    tr.erase("audio_loss_weight");
  } else {
    // H3 is guidance-distilled: its previews take no CFG.
    tr.erase("preview_cfg");
  }
  // The catalog's learning rate where none is asked: vpipe's AdamW
  // default (1e-4) taught Qwen-Image 2.1 nothing of a subject in 1,200
  // steps on 12 pictures, 4e-4 the subject in scenes it never saw
  // (measured 2026-11-22, DESIGN §8c).
  if (const auto o = jget<std::string>(opt, "optimizer", "adamw");
      !opt.contains("learning_rate") &&
      (o == "adamw" || o == "adamw8bit")) {
    if (const double lr = jget(tb, "learning_rate", 0.0);
        lr > 0) {
      opt["learning_rate"] = lr;
    }
  }
  // int8 on a GPU with matrix cores, the Neural Engine's split before.
  if (!_hw.gpu_matrix_cores) {
    tr.erase("i8_gemm");
  } else {
    tr.erase("ane");
  }
  tr["output_dir"] = folder.string();
  tr["name"] = name;

  engine::JobSpec spec;
  spec.id = JobId::make();
  spec.op = std::string(engine::kOpTrainLora);
  spec.model.id = m->id;
  spec.model.dir = info.path();
  // Its previews named by when it began: a resumed run's follow the
  // run's before them, in order.
  spec.params = {{"dataset", ds}, {"optimizer", opt}, {"train", tr},
                 {"run", std::format("{:013}", project::now_ms())},
                 {"shape", shape}, {"clips", any_clip},
                 {"edit", shape == "h3-image" && any_refs},
                 {"reference_short_edge",
                  jget(tb, "reference_short_edge", 0)}};
  if (!encoder.empty()) {
    spec.model.encoder = encoder;
  }

  const std::string trigger = jget<std::string>(ds, "trigger_word", "");
  const Json run = {{"name", name}, {"model", m->id},
                    {"model_name", m->name}, {"trigger", trigger},
                    {"trigger_meaning",
                     jget<std::string>(req, "trigger_meaning", "")},
                    {"repeats", repeats}, {"items", listed},
                    {"dataset", ds}, {"optimizer", opt}, {"train", tr},
                    {"created_ms", project::now_ms()}};
  {
    std::ofstream f(folder / "run.json", std::ios::binary);
    f << to_text(run, 2);
  }

  JobRecord rec;
  rec.id = spec.id;
  rec.op = spec.op;
  rec.purpose = "train";
  rec.title = std::format("Train {}", name);
  rec.destination = folder.string();
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  {
    std::lock_guard lk(_training_mu);
    _training[name] = spec.id;
  }
  post_("job.queued", spec.id, {{"title", rec.title}, {"op", rec.op},
                                {"purpose", rec.purpose},
                                {"model", m->id}, {"name", name}});
  VALTZ_LOG_INFO("train", "training {} on {}: {} samples (x{}){}{}, into "
                 "{}", name, m->id, pics.size(), repeats,
                 any_clip ? ", clips" : "", any_refs ? ", references" : "",
                 folder.string());

  auto ended = [this, name, folder](bool finished) {
    {
      std::lock_guard lk(_training_mu);
      _training.erase(name);
    }
    Json run = Json::parse(read_text(folder / "run.json"), nullptr, false);
    if (run.is_object()) {
      run["ended_ms"] = project::now_ms();
      if (finished) {
        run["finished_ms"] = project::now_ms();
      }
      std::ofstream(folder / "run.json", std::ios::binary)
          << to_text(run, 2);
    }
  };
  auto st = _engine->submit(std::move(spec),
                            [this, ended](const engine::JobEvent& ev) {
    switch (ev.kind) {
    case engine::JobEventKind::Started:
      _jobs.update(ev.job, [](JobRecord& r) { r.state = JobState::Running; });
      post_("job.started", ev.job, {{"engine", ev.text}});
      break;
    case engine::JobEventKind::Progress: {
      if (ev.progress >= 0) {
        _jobs.update(ev.job, [&](JobRecord& r) { r.progress = ev.progress; });
      }
      Json d = {{"progress", ev.progress}};
      if (ev.data.is_object()) {
        d.update(ev.data);
      }
      post_("job.progress", ev.job, d);
      break;
    }
    case engine::JobEventKind::Output:
      _jobs.update(ev.job, [&](JobRecord& r) {
        r.outputs.push_back(ev.output.string());
      });
      break;
    case engine::JobEventKind::Finished: {
      std::string path;
      if (auto r = _jobs.get(ev.job); r && !r->outputs.empty()) {
        path = r->outputs.back();
      }
      ended(true);
      finish_job_(ev.job, JobState::Finished, path);
      post_("job.finished", ev.job, {{"path", path}});
      post_("models.changed", JobId{});
      break;
    }
    case engine::JobEventKind::Failed:
      ended(false);
      finish_job_(ev.job, JobState::Failed, ev.text);
      post_("job.failed", ev.job, {{"code", to_str(ev.error)},
                                   {"message", ev.text}});
      break;
    case engine::JobEventKind::Cancelled:
      // Stopped: vpipe saved the adapter and its state, which a run of
      // the same name resumes from.
      ended(false);
      finish_job_(ev.job, JobState::Cancelled, "");
      post_("job.cancelled", ev.job);
      break;
    default:
      break;
    }
  });
  if (!st.ok()) {
    ended(false);
    finish_job_(rec.id, JobState::Failed, st.error().message);
    return st.error();
  }
  return rec.id;
}

Json
Controller::trained_loras() const
{
  std::error_code ec;
  Json runs = Json::array();
  std::map<std::string, JobId> live;
  {
    std::lock_guard lk(_training_mu);
    live = _training;
  }
  std::set<std::string> seen;
  for (const auto& root : run_roots(_paths)) {
  ec.clear();
  for (auto i = fs::directory_iterator(root, ec);
       !ec && i != fs::directory_iterator(); i.increment(ec)) {
    if (!i->is_directory(ec)) {
      continue;
    }
    const fs::path folder = i->path();
    const std::string name = folder.filename().string();
    // A name once: the first root's (run_folder's).
    if (!seen.insert(name).second) {
      continue;
    }
    const Json run = Json::parse(read_text(folder / "run.json"), nullptr,
                                 false);
    Json cps = Json::array();
    std::vector<fs::path> previews;
    std::string adapter;
    for (auto f = fs::directory_iterator(folder, ec);
         !ec && f != fs::directory_iterator(); f.increment(ec)) {
      const std::string fn = f->path().filename().string();
      if (f->path().extension() != ".safetensors") {
        continue;
      }
      if (fn == name + ".safetensors") {
        adapter = f->path().string();
      } else if (fn.starts_with(name + "-")) {
        const std::string n = f->path().stem().string().substr(
            name.size() + 1);
        int step = 0;
        for (char c : n) {
          step = std::isdigit(static_cast<unsigned char>(c))
              ? step * 10 + (c - '0') : step;
        }
        cps.push_back({{"path", f->path().string()}, {"step", step}});
      }
    }
    ec.clear();
    std::sort(cps.begin(), cps.end(), [](const Json& a, const Json& b) {
      return a["step"].get<int>() < b["step"].get<int>();
    });
    for (auto f = fs::directory_iterator(folder / "previews", ec);
         !ec && f != fs::directory_iterator(); f.increment(ec)) {
      // A picture model's previews are PNGs, a clip model's MP4s.
      if (f->path().extension() == ".png" ||
          f->path().extension() == ".mp4") {
        previews.push_back(f->path());
      }
    }
    ec.clear();
    std::ranges::sort(previews);
    Json pv = Json::array();
    for (const auto& p : previews) {
      pv.push_back(p.string());
    }
    const auto job = live.find(name);
    runs.push_back(
        {{"name", name},
         {"folder", folder.string()},
         {"model", jget<std::string>(run, "model", "")},
         {"model_name", jget<std::string>(run, "model_name", "")},
         {"trigger", jget<std::string>(run, "trigger", "")},
         {"items", jget(run, "items", Json::array())},
         {"request", run.is_object() ? run : Json::object()},
         {"adapter", adapter},
         {"checkpoints", std::move(cps)},
         {"previews", std::move(pv)},
         {"resumable", fs::is_directory(folder / (name + ".resume"), ec)},
         {"created_ms", jget<std::int64_t>(run, "created_ms", 0)},
         {"finished_ms", jget<std::int64_t>(run, "finished_ms", 0)},
         {"job", job == live.end() ? std::string() : job->second.str()},
         {"parent", root.string()}});
  }
  }
  std::sort(runs.begin(), runs.end(), [](const Json& a, const Json& b) {
    return a["created_ms"].get<std::int64_t>() >
           b["created_ms"].get<std::int64_t>();
  });
  return runs;
}

Status
Controller::remove_trained(const std::string& name)
{
  const std::string n = folder_name(name);
  {
    std::lock_guard lk(_training_mu);
    if (_training.contains(n)) {
      return make_error(Code::Busy, msg::kTrainRunning, {{"name", n}});
    }
  }
  const fs::path folder = run_folder(_paths, n);
  std::error_code ec;
  if (n.empty() || !fs::is_directory(folder, ec)) {
    return make_error(Code::NotFound, std::format("no run named {}", n));
  }
  fs::remove_all(folder, ec);
  if (ec) {
    return make_error(Code::Io, ec.message());
  }
  post_("models.changed", JobId{});
  return ok_status();
}

Status
Controller::finish_trained(const std::string& name)
{
  const std::string n = folder_name(name);
  {
    std::lock_guard lk(_training_mu);
    if (_training.contains(n)) {
      return make_error(Code::Busy, msg::kTrainRunning, {{"name", n}});
    }
  }
  const fs::path folder = run_folder(_paths, n);
  std::error_code ec;
  fs::remove_all(folder / (n + ".resume"), ec);
  if (ec) {
    return make_error(Code::Io, ec.message());
  }
  // A run stopped before its end has checkpoints alone: the latest is
  // its adapter, under the run's own name.
  const fs::path adapter = folder / (n + ".safetensors");
  if (!fs::exists(adapter, ec)) {
    fs::path latest;
    for (auto i = fs::directory_iterator(folder, ec);
         !ec && i != fs::directory_iterator(); i.increment(ec)) {
      const std::string fn = i->path().filename().string();
      if (i->path().extension() == ".safetensors" &&
          fn.starts_with(n + "-") && fn > latest.filename().string()) {
        latest = i->path();
      }
    }
    if (!latest.empty()) {
      fs::copy_file(latest, adapter, ec);
    }
  }
  post_("models.changed", JobId{});
  return ok_status();
}

Result<Json>
Controller::train_command(JobId job, const std::string& name,
                          const Json& args)
{
  if (name != "save" && name != "preview") {
    return make_error(Code::InvalidArgument,
                      std::format("a training takes no {}", name));
  }
  return _engine->job_command(job, name, args);
}

}  // namespace valtz

namespace valtz {

bool
Controller::sees_(const models::ModelEntry& m) const
{
  if (m.has(models::Capability::VideoSummary) ||
      m.has(models::Capability::Caption)) {
    return true;
  }
  // A GGUF's eyes beside it (catalog `eyes`: the release's vision tower),
  // where they are here.
  const auto* eyes = m.eyes.empty() ? nullptr : _catalog.find(m.eyes);
  return eyes != nullptr &&
         _store->info(*eyes).state == models::InstallState::Installed;
}

Controller::Captioner
Controller::captioner_() const
{
  const auto here = [&](const models::ModelEntry* m) {
    return m != nullptr &&
           _store->info(*m).state == models::InstallState::Installed;
  };
  if (const auto* a = agent_model(); here(a) && sees_(*a)) {
    return {a, true};
  }
  if (const auto* mi = micro_model();
      here(mi) && engine_runs_(models::Capability::MicroAgent)) {
    return {mi, false};
  }
  if (const auto* h = assistant_model(); here(h) && sees_(*h)) {
    return {h, true};
  }
  return {};
}

Json
Controller::training_captioner() const
{
  const Captioner c = captioner_();
  return c.model ? Json{{"id", c.model->id}, {"name", c.model->name}}
                 : Json();
}

Result<Json>
Controller::caption_training_sample(const std::filesystem::path& file,
                                    const std::string& trigger,
                                    const std::string& meaning)
{
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    return make_error(Code::NotFound, msg::kTrainReferenceMissing,
                      {{"file", file.filename().string()},
                       {"reference", file.string()}});
  }
  const Captioner who = captioner_();
  if (!who.model) {
    return make_error(Code::Unsupported, msg::kTrainNoCaptioner);
  }
  // What it is shown: the picture, or three frames of the clip -- small,
  // as the agent is shown pictures.
  const bool clip = is_clip(file);
  const fs::path dir = _paths.support / "cache" / "train-captions";
  fs::create_directories(dir, ec);
  std::vector<engine::JobInput> shown;
  const std::string stem = JobId::make().str();
  const std::vector<double> at = clip ? std::vector<double>{0.1, 0.5, 0.9}
                                      : std::vector<double>{0.1};
  for (std::size_t i = 0; i < at.size(); ++i) {
    const fs::path small = dir / std::format("{}-{}.jpg", stem, i);
    VALTZ_TRY(media::write_thumbnail(
        file, clip ? media::MediaType::Video : media::MediaType::Image,
        clip ? 512 : 768, small, at[i]));
    shown.push_back({"image", small, {}, {}});
  }
  auto said = std::make_shared<std::promise<std::optional<std::string>>>();
  auto got = said->get_future();
  VALTZ_TRY(submit_micro_(
      JobId::make(), "Caption a training sample",
      assist::training_caption_request(trigger, meaning,
                                       static_cast<int>(at.size())),
      shown, 160,
      [said](std::optional<std::string> r) { said->set_value(std::move(r)); },
      who.helper, who.helper ? who.model : nullptr));
  const auto reply = got.get();
  for (const auto& s : shown) {
    fs::remove(s.path, ec);
  }
  const std::string caption =
      reply ? assist::training_caption(*reply, trigger) : std::string();
  if (caption.empty()) {
    return make_error(Code::Engine, msg::kTrainCaptionFailed,
                      {{"model", who.model->name}});
  }
  return Json{{"caption", caption}, {"model", who.model->name}};
}

}  // namespace valtz
