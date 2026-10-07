// The in-process libvpipe engine.
//
// ONE vpipe Session for the life of the app. That is not a convenience:
// vpipe's model manager -- weight sharing, the wired pool, the memory
// plan that decides whether a DiT streams -- is per Session, so an LLM
// and a DiT in two sessions would each size the machine as if the other
// did not exist.
//
// Jobs run one at a time on a single control thread. vpipe's own worker
// pool does the parallel work inside a graph; serializing GRAPHS is the
// memory policy for now (on a 16 GB Mac a DiT and a 9B LLM do not both
// fit), and it keeps launch/wait/stop on one thread, which
// SessionIntf::wait_pipelines requires. The queue is the app's TASK
// QUEUE (DESIGN §3a): in the order asked, but a chat -- someone waiting
// at the prompt -- goes before the work still queued.
//
// Data crosses through the valtz-vpipe PLUGIN (vpipe-plugin/), which
// vpipe loads into this process from the session config: its
// `valtz-sink` stages hand every beat over in place, read here by one
// SinkReader thread per sink (host-exchange.h). Without the plugin the
// engine cannot see a single result, so a plugin that does not load
// makes the engine unavailable, with the reason in the log.

#include "valtz/models/hardware.h"
#include "valtz/engine/engine.h"

#include "valtz/base/log.h"
#include "valtz/base/text.h"
#include "valtz/media/exif.h"
#include "valtz/media/model-input.h"
#include "valtz/media/movie.h"
#include "valtz/media/probe.h"
#include "engine/vpipe/flex-json.h"
#include "engine/vpipe/graph-builder.h"
#include "engine/vpipe/host-exchange.h"
#include "engine/vpipe/job-progress.h"

#include "valtz-vpipe/exchange.h"

#include "vpipe/system-monitor.h"
#include "vpipe/vpipe.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

namespace valtz::engine {

namespace {

namespace fs = std::filesystem;

struct Queued {
  JobSpec spec;
  JobSink sink;
};

class VpipeEngine final : public Engine {
public:
  explicit VpipeEngine(const EngineConfig& cfg)
  {
    // Until the session says otherwise, no backend is loaded.
    for (const auto& b : cfg.backends) {
      _backends.push_back({{"extension", b.extension},
                           {"file", b.file.string()},
                           {"state", "not-loaded"}, {"why", "engine"},
                           {"args", Json::object()}});
    }
    std::error_code ec;
    fs::create_directories(cfg.state_dir, ec);
    fs::create_directories(cfg.temp_dir, ec);
    // vpipe resolves its scratch root from the CWD unless told; a GUI
    // app's CWD is "/", which is not writable.
    setenv("VPIPE_TMPDIR", cfg.temp_dir.c_str(), /*overwrite=*/0);

    const fs::path plugin = cfg.vpipe_plugin.empty() ? vp::find_plugin()
                                                     : cfg.vpipe_plugin;
    if (plugin.empty()) {
      VALTZ_LOG_ERROR("engine", "{} not found (looked in the app "
                      "bundle, beside the executable and in the build "
                      "tree; set VALTZ_VPIPE_PLUGIN)",
                      valtz::exchange::kPluginFile);
      return;
    }
    // The session config goes in as a document: no JSON text (flex-json.h).
    vpipe::FlexData plugins = vpipe::FlexData::make_array();
    plugins.as_array().push_back(vp::flex_str(plugin.string()));
    // The extensions' backends this vpipe would load, beside it; each one
    // it would refuse named here and kept out.
    for (std::size_t i = 0; i < _backends.size(); ++i) {
      const auto& b = cfg.backends[i];
      Json detail;
      const std::string why = vp::backend_refusal(b.abi, b.required,
                                                  &detail);
      if (!why.empty()) {
        _backends[i]["state"] = "refused";
        _backends[i]["why"] = why;
        _backends[i]["args"] = detail;
        VALTZ_LOG_WARN("engine", "extension {}: {} not loaded ({}: {})",
                       b.extension, b.file.string(), why,
                       to_text(detail));
        continue;
      }
      plugins.as_array().push_back(vp::flex_str(b.file.string()));
    }
    const std::string db = (cfg.state_dir / "vpipe-db").string();
    const vpipe::FlexData scfg = vp::flex_object({
      {"db", vp::flex_object({{"path", vp::flex_str(db)}})},
      {"log", vp::flex_object({{"level", vp::flex_str(cfg.log_level)}})},
      {"plugins", std::move(plugins)},
    });
    auto& mgr = vpipe::SessionManager::get();
    _session = const_cast<vpipe::SessionIntf*>(mgr.create_session(scfg));
    if (!_session) {
      VALTZ_LOG_ERROR("engine", "vpipe refused the session config");
      return;
    }
    // vpipe's log, to the host as well as to stdout (the Log view).
    if (cfg.on_log) {
      _session->set_log_listener(cfg.on_log);
    }
    if (!plugin_loaded_()) {
      VALTZ_LOG_ERROR("engine", "vpipe did not load {} (its reason is in "
                      "the vpipe log)", plugin.string());
      mgr.destroy_session(_session);
      _session = nullptr;
      return;
    }
    // Each backend vpipe was given: loaded when every stage it says it
    // registers is there.
    for (std::size_t i = 0; i < _backends.size(); ++i) {
      if (_backends[i]["state"] == "refused") {
        continue;
      }
      std::string missing;
      for (const auto& type : cfg.backends[i].stages) {
        if (!has_stage_(type)) {
          missing = type;
          break;
        }
      }
      if (missing.empty()) {
        _backends[i]["state"] = "loaded";
        _backends[i]["why"] = "";
        VALTZ_LOG_INFO("engine", "extension {}: {} loaded",
                       cfg.backends[i].extension,
                       cfg.backends[i].file.string());
      } else {
        _backends[i]["why"] = "stage";
        _backends[i]["args"] = {{"stage", missing}};
        VALTZ_LOG_WARN("engine", "extension {}: vpipe did not load {} "
                       "(no stage {}; its reason is in the vpipe log)",
                       cfg.backends[i].extension,
                       cfg.backends[i].file.string(), missing);
      }
    }
    // vpipe renders its own user-facing messages in the UI language when
    // it has them (en, zh-cn, zh-tw; it maps zh-Hans / zh-Hant itself);
    // any other language keeps its English.
    if (!cfg.language.empty() &&
        _session->set_language(cfg.language).code != 0) {
      VALTZ_LOG_DEBUG("engine", "vpipe has no '{}' messages; English",
                      cfg.language);
    }
    _desc = std::format("vpipe {} ({})", vpipe::vpipe_version_number(),
                        vpipe::vpipe_build_hash());
    VALTZ_LOG_INFO("engine", "{} ready, plugin {}", _desc, plugin.string());
    _worker = std::thread([this] { run_loop_(); });
  }

  ~VpipeEngine() override { shutdown(); }

  std::string description() const override { return _desc; }
  bool available() const override { return _session != nullptr; }
  Json backends() const override { return _backends; }

  Json
  machine_status() override
  {
    // Its IOReport subscription is made on first use: a status bar that
    // is never shown costs nothing.
    std::call_once(_monitor_once, [this] {
      _monitor = std::make_unique<vpipe::SystemMonitor>();
    });
    return vp::from_flex(_monitor->status());
  }

  Json
  gpu_thermal(int window_ms) override
  {
    return vp::from_flex(vpipe::SystemMonitor::gpu_thermal(window_ms));
  }

  bool
  supports(std::string_view op) const override
  {
    return op == kOpGenerateImage || op == kOpEditImage ||
           op == kOpGenerateVideo || op == kOpGenerateAudio ||
           op == kOpGenerateSpeech || op == kOpQuantizeModel ||
           op == kOpChat || op == kOpFetchModel || op == kOpExportMedia ||
           op == kOpUpscaleVideo || op == kOpUpscaleImage;
  }

  Status
  submit(JobSpec spec, JobSink sink) override
  {
    if (!_session) {
      return make_error(Code::Unsupported, "vpipe engine is not running");
    }
    if (!supports(spec.op)) {
      return make_error(Code::Unsupported, std::format(
          "vpipe engine cannot run '{}'", spec.op));
    }
    {
      std::lock_guard lk(_mu);
      if (_stop) {
        return make_error(Code::Cancelled, "engine is shutting down");
      }
      // An enhance or an intent is the person waiting at the prompt: it
      // goes before the TASKS still queued (DESIGN §3a), after the one
      // running -- a graph does not share the machine.
      auto at = _queue.end();
      if (spec.op == kOpChat) {
        at = std::ranges::find_if(_queue, [](const Queued& q) {
          return q.spec.op != kOpChat;
        });
      }
      _queue.insert(at, {std::move(spec), std::move(sink)});
    }
    _cv.notify_one();
    return ok_status();
  }

  Status
  cancel(JobId id) override
  {
    Queued dropped;
    bool found = false;
    {
      std::lock_guard lk(_mu);
      if (_running == id) {
        _cancel.store(true);
        return ok_status();
      }
      for (auto it = _queue.begin(); it != _queue.end(); ++it) {
        if (it->spec.id == id) {
          dropped = std::move(*it);
          _queue.erase(it);
          found = true;
          break;
        }
      }
    }
    if (!found) {
      return make_error(Code::NotFound, "no such job");
    }
    JobEvent ev;
    ev.job = id;
    ev.kind = JobEventKind::Cancelled;
    dropped.sink(ev);
    return ok_status();
  }

  void
  shutdown() override
  {
    {
      std::lock_guard lk(_mu);
      if (_stop) {
        return;
      }
      _stop = true;
      _cancel.store(true);
    }
    _cv.notify_all();
    if (_worker.joinable()) {
      _worker.join();
    }
    if (_session) {
      _session->set_log_listener({});
      vpipe::SessionManager::get().destroy_session(_session);
      _session = nullptr;
    }
  }

private:
  // The session config names the plugin, but vpipe reports a plugin that
  // fails to load only in its log. Ask for its stage instead.
  bool
  plugin_loaded_()
  {
    return has_stage_(valtz::exchange::kSourceType);
  }

  // Whether a stage of `type` can be made: what a loaded plugin
  // registered.
  bool
  has_stage_(const std::string& type)
  {
    vpipe::PipelineHandle p = _session->create_pipeline("valtz-probe");
    if (!p) {
      return false;
    }
    const bool ok = static_cast<bool>(p.insert_stage(type, "probe", {},
                                                     ""));
    _session->unload_pipeline(p);
    return ok;
  }

  void
  run_loop_()
  {
    for (;;) {
      Queued q;
      {
        std::unique_lock lk(_mu);
        _cv.wait(lk, [&] { return _stop || !_queue.empty(); });
        if (_stop) {
          break;
        }
        q = std::move(_queue.front());
        _queue.pop_front();
        _running = q.spec.id;
        _cancel.store(false);
      }
      run_job_(q);
      std::lock_guard lk(_mu);
      _running = JobId{};
    }
    // Fail whatever is still queued.
    std::deque<Queued> rest;
    {
      std::lock_guard lk(_mu);
      rest.swap(_queue);
    }
    for (auto& q : rest) {
      JobEvent ev;
      ev.job = q.spec.id;
      ev.kind = JobEventKind::Cancelled;
      q.sink(ev);
    }
  }

  void
  fail_(const Queued& q, Code code, std::string msg, Json data = {})
  {
    JobEvent ev;
    ev.job = q.spec.id;
    ev.kind = JobEventKind::Failed;
    ev.error = code;
    ev.text = std::move(msg);
    ev.data = std::move(data);
    q.sink(ev);
  }

  // What vpipe reported about memory since `from` (SessionIntf::reports,
  // DESIGN §15 0h): the refusal -- a stage that could not fit its work,
  // part by part, each budget's need and room -- and the resource plan's
  // peak by phase. Null without a refusal: the job did not fail for
  // memory.
  Json
  memory_refusal_(std::uint64_t from) const
  {
    const Json doc = vp::from_flex(_session->reports());
    Json refusal, plan;
    for (const auto& r : jget(doc, "items", Json::array())) {
      if (jget<std::uint64_t>(r, "id", 0) <= from) {
        continue;
      }
      const auto kind = jget<std::string>(r, "kind", "");
      if (kind == "memory") {
        refusal = jget(r, "data", Json::object());
      } else if (kind == "memory-plan") {
        plan = jget(r, "data", Json::object());
      }
    }
    if (refusal.is_null()) {
      return {};
    }
    Json out = {{"refusal", std::move(refusal)}};
    if (!plan.is_null()) {
      out["plan"] = std::move(plan);
    }
    return out;
  }

  // The refusal in a sentence, for the log and valtzctl: the step, what
  // it needed and the budget that was short.
  static std::string
  memory_sentence_(const Json& m)
  {
    const Json r = jget(m, "refusal", Json::object());
    auto gb = [](std::uint64_t b) {
      return std::format("{:.1f} GB", static_cast<double>(b) / (1 << 30));
    };
    std::string s = std::format(
        "not enough memory for the {} (needs ~{})",
        jget<std::string>(r, "step", "generation"),
        gb(jget<std::uint64_t>(r, "need", 0)));
    for (const auto& g : jget(r, "gates", Json::array())) {
      if (!jget(g, "ok", true)) {
        s += std::format("; {}: ~{} wanted, ~{} available",
                         jget<std::string>(g, "name", ""),
                         gb(jget<std::uint64_t>(g, "need", 0)),
                         gb(jget<std::uint64_t>(g, "have", 0)));
      }
    }
    return s;
  }

  // A sound written out (export-media in a sound format): no graph --
  // vpipe's load-audio cuts to the packet, and its AAC needs FFmpeg -- so
  // the job is AVFoundation's (media::write_sound), cut to the sample.
  // Files in, a file out, as any export.
  void
  export_sound_(Queued& q, const ExportFormat& f)
  {
    const JobSpec& spec = q.spec;
    if (spec.inputs.size() != 1) {
      fail_(q, Code::InvalidArgument, "an export takes one input");
      return;
    }
    JobEvent started;
    started.job = spec.id;
    started.kind = JobEventKind::Started;
    started.text = _desc;
    q.sink(started);
    const JobInput& in = spec.inputs.front();
    // The trim's marks, in its own count: from the mark-in, as many as
    // it keeps -- both marks kept.
    const media::Trim trim = media::trim_from_json(
        jget(spec.params, "trim", Json::object()));
    double start = 0, duration = 0;
    if (!trim.identity() && trim.rate.num > 0) {
      const double per = trim.rate.to_double();
      start = std::max<std::int64_t>(0, trim.in) / per;
      if (trim.out >= 0 && trim.out >= std::max<std::int64_t>(0, trim.in)) {
        duration = static_cast<double>(
            trim.out - std::max<std::int64_t>(0, trim.in) + 1) / per;
      }
    }
    std::error_code ec;
    fs::create_directories(spec.output_dir, ec);
    const fs::path out = spec.output_dir /
        std::format("{}.{}", spec.id.str(), f.extension);
    if (auto st = media::write_sound(in.path, out, f.name, start, duration);
        !st.ok()) {
      fail_(q, st.code(), st.error().message);
      return;
    }
    if (_cancel.load()) {
      fs::remove(out, ec);
      JobEvent ev;
      ev.job = spec.id;
      ev.kind = JobEventKind::Cancelled;
      q.sink(ev);
      return;
    }
    JobEvent o;
    o.job = spec.id;
    o.kind = JobEventKind::Output;
    o.output = out;
    if (auto info = media::probe_file(out); info.ok()) {
      o.output_info = *info;
    }
    q.sink(o);
    JobEvent done;
    done.job = spec.id;
    done.kind = JobEventKind::Finished;
    done.progress = 1.0f;
    q.sink(done);
  }

  void
  run_job_(Queued& q)
  {
    const JobSpec& spec = q.spec;
    if (spec.op == kOpExportMedia) {
      const auto* f = export_format(jget<std::string>(spec.params,
                                                      "format", ""));
      if (f && f->sound) {
        export_sound_(q, *f);
        return;
      }
    }
    Result<vp::BuiltGraph> built = make_error(Code::Internal, "");
    if (spec.op == kOpGenerateImage) {
      built = vp::build_text_to_image(spec);
    } else if (spec.op == kOpEditImage) {
      auto refs = reference_images_(spec);
      built = refs.ok() ? vp::build_image_edit(spec, *refs)
                        : Result<vp::BuiltGraph>(refs.error());
    } else if (spec.op == kOpGenerateVideo) {
      auto first = first_frame_(spec);
      auto refs = reference_media_(spec);
      built = !first.ok() ? Result<vp::BuiltGraph>(first.error())
              : !refs.ok() ? Result<vp::BuiltGraph>(refs.error())
                           : vp::build_video(spec, *first, *refs);
    } else if (spec.op == kOpGenerateAudio) {
      built = vp::build_audio(spec);
    } else if (spec.op == kOpGenerateSpeech) {
      built = vp::build_speech(spec);
    } else if (spec.op == kOpQuantizeModel) {
      built = vp::build_quantize_model(spec);
    } else if (spec.op == kOpUpscaleVideo) {
      built = vp::build_upscale_video(spec);
    } else if (spec.op == kOpUpscaleImage) {
      built = vp::build_upscale_image(spec);
    } else if (spec.op == kOpChat) {
      built = vp::build_chat(spec);
    } else if (spec.op == kOpFetchModel) {
      built = vp::build_fetch_model(spec);
    } else if (spec.op == kOpExportMedia) {
      built = vp::build_export(spec);
    }
    if (!built.ok()) {
      fail_(q, built.code(), built.error().message);
      return;
    }
    vp::BuiltGraph& g = *built;
    std::error_code ec;
    if (!spec.output_dir.empty()) {
      fs::create_directories(spec.output_dir, ec);
    }

    // What the sinks deliver, turned into job events. Called on the
    // SinkReader threads, which are joined before the terminal event.
    std::string result_text;
    std::mutex result_mu;
    auto on_preview = [&](vp::SinkBeat&& b) {
      JobEvent ev;
      ev.job = spec.id;
      ev.kind = JobEventKind::Preview;
      ev.step = jget(b.meta, "step", 0);
      ev.steps = jget(b.meta, "steps", 0);
      if (ev.steps > 0) {
        ev.progress = static_cast<float>(ev.step) / ev.steps;
      }
      // A video's preview is a clip, [F, 3, H, W]: how many frames, and
      // the rate that plays them over the clip's duration.
      if (const int frames = jget(b.meta, "frames", 0); frames > 0) {
        ev.data = {{"frames", frames}, {"fps", jget(b.meta, "fps", 24.0)}};
      }
      // Planar, as the engine produced it, and the engine's own bytes:
      // conversion for display happens at the consumer, where a GPU
      // path can bind the buffer.
      ev.tensor = std::move(b.tensor);
      if (ev.tensor) {
        q.sink(ev);
      }
    };
    auto on_text = [&](vp::SinkBeat&& b) {
      JobEvent ev;
      ev.job = spec.id;
      ev.kind = JobEventKind::Text;
      ev.text = jget<std::string>(b.meta, "text", "");
      if (!ev.text.empty()) {
        q.sink(ev);
      }
    };
    auto on_result = [&](vp::SinkBeat&& b) {
      std::lock_guard lk(result_mu);
      result_text += jget<std::string>(b.meta, "text", "");
    };
    // A song's score: one string beat, the ABC it followed.
    std::string score;
    auto on_score = [&](vp::SinkBeat&& b) {
      std::lock_guard lk(result_mu);
      if (b.meta.is_string()) {
        score = b.meta.get<std::string>();
      }
    };

    JobEvent started;
    started.job = spec.id;
    started.kind = JobEventKind::Started;
    started.text = _desc;
    q.sink(started);

    // What the job is doing, as vpipe's own UI would show it: its stages'
    // live reports, polled while the graph runs (job-progress.h). Reports
    // already in the list belong to an earlier job.
    // An upscale's writer counts a group's frames at once: its restore
    // is estimated a group at a time.
    const std::uint64_t group =
        g.movie && g.movie->gate.group > g.movie->gate.overlap
            ? static_cast<std::uint64_t>(g.movie->gate.group -
                                         g.movie->gate.overlap)
            : 0;
    vp::JobProgress progress(
        vp::JobProgress::last_id(vp::from_flex(_session->progress())),
        spec.op == kOpFetchModel, spec.op == kOpExportMedia,
        spec.op == kOpUpscaleVideo, group);
    std::uint64_t progress_seen = 0;
    std::optional<vp::JobPhase> phase;
    double sent = 0;
    auto report = [&](bool force) {
      const double t = std::chrono::duration<double>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
      // A count that moved, or -- while one is counting -- a second gone
      // by: its estimate has moved on (job-progress.h), and a long unit
      // must not leave the bar still.
      const std::uint64_t v = _session->progress_version();
      const bool beat = phase && phase->counting() && t - sent >= 1.0;
      if (!force && v == progress_seen && !beat) {
        return;
      }
      progress_seen = v;
      vp::JobPhase now =
          progress.read(vp::from_flex(_session->progress()), t);
      if (!beat && phase && phase->same(now)) {
        return;
      }
      JobEvent ev;
      ev.job = spec.id;
      ev.kind = JobEventKind::Progress;
      ev.progress = now.estimate >= 0 ? static_cast<float>(now.estimate)
                                      : now.fraction();
      ev.data = now.data();
      q.sink(ev);
      phase = std::move(now);
      sent = t;
    };
    // A chat's progress is its streamed text.
    const bool reports = spec.op != kOpChat;

    // A long export shows what it writes (kPhaseExport): a small frame
    // every half second, as a generation's preview -- drawn from the
    // frames going in when Valtz feeds them, else read from the source at
    // the writer's count (once a second: that is a decode of its own).
    const auto now_s = [] {
      return std::chrono::duration<double>(
          std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    double previewed = -1;
    const auto preview_due = [&](double every) {
      const double t = now_s();
      if (previewed >= 0 && t - previewed < every) {
        return false;
      }
      previewed = t;
      return true;
    };
    const auto send_preview = [&](media::PreviewPicture&& pic,
                                  std::int64_t done) {
      if (pic.rgb.empty()) {
        return;
      }
      JobEvent ev;
      ev.job = spec.id;
      ev.kind = JobEventKind::Preview;
      ev.step = static_cast<int>(done);
      ev.steps = static_cast<int>(g.frames);
      ev.progress = g.frames > 0
          ? std::min(1.0f, static_cast<float>(done) / g.frames)
          : -1.0f;
      ev.data = {{"frame", pic.frame}, {"phase", kPhaseExport}};
      ev.tensor = make_owned_tensor(
          Tensor::DType::U8, {3, pic.size.height, pic.size.width},
          std::move(pic.rgb), Json::object());
      q.sink(ev);
    };
    media::FramePreview fed_preview;
    if (g.movie) {
      const std::int64_t first = g.movie->first;
      fed_preview.wants = [&](std::int64_t) { return preview_due(0.5); };
      fed_preview.take = [&, first](media::PreviewPicture&& pic) {
        const std::int64_t done = pic.frame - first + 1;
        send_preview(std::move(pic), done);
      };
    }

    // Reports from here on are this job's: one graph runs at a time.
    const std::uint64_t reports_from = _session->reports_version();
    // The graph goes in as a document, converted node by node: its number
    // kinds intact, no JSON text written or parsed.
    vpipe::PipelineHandle h = _session->load_pipeline(vp::to_flex(g.spec));
    if (!h) {
      fail_(q, Code::Engine,
            "vpipe rejected the graph (details are in the log)");
      return;
    }
    if (auto st = _session->launch_pipeline(h); st.code != 0) {
      _session->unload_pipeline(h);
      // vpipe::to_str(Status) is declared in vpipe/status.h but not
      // defined in libvpipe; report the numeric code.
      fail_(q, Code::Engine, std::format("vpipe launch failed (status {})",
                                         st.code));
      return;
    }
    if (reports) {
      report(/*force=*/true);  // nothing counted yet: preparing
    }
    // A sink's inbox exists from launch to stop, so readers start now;
    // beats that arrived first are waiting in the sink's queue.
    std::vector<std::unique_ptr<vp::SinkReader>> readers;
    auto read = [&](const std::string& id, vp::SinkReader::Fn fn) {
      if (!id.empty()) {
        readers.push_back(
            std::make_unique<vp::SinkReader>(h.stage(id), std::move(fn)));
      }
    };
    read(g.preview_sink, on_preview);
    read(g.text_sink, on_text);
    read(g.result_sink, on_result);
    read(g.score_sink, on_score);

    // References go in now, decoded straight into beats the graph leased
    // us: no file is re-read by vpipe and no pixel is copied after the
    // decode.
    // While frames are fed, the job's own thread is busy feeding: each
    // frame polls vpipe's counts.
    const auto fed = [&] {
      if (reports) {
        report(/*force=*/false);
      }
    };
    if (auto st = feed_references_(h, g, fed, &fed_preview); !st.ok()) {
      _session->stop_pipeline(h);  // ends the readers' waits
      readers.clear();
      _session->unload_pipeline(h);
      fail_(q, st.code(), st.error().message);
      return;
    }

    // vpipe's own renderers repaint at 10 Hz; so does this.
    bool cancelled = false;
    for (;;) {
      auto w = _session->wait_pipelines(100);
      if (w.code == 0) {
        break;
      }
      if (_cancel.load()) {
        _session->stop_pipeline(h);
        cancelled = true;
        break;
      }
      if (reports) {
        report(/*force=*/false);
      }
      // A clip vpipe reads itself: its frame at the writer's count.
      if (!g.preview_from.empty() && phase &&
          phase->phase == kPhaseExport && phase->done > 0 &&
          preview_due(1.0)) {
        const auto n = static_cast<std::int64_t>(phase->done);
        auto pic = media::preview_movie_frame(
            g.preview_from, g.preview_start + (n - 1) / g.preview_fps,
            480);
        if (pic.ok()) {
          pic->frame = n - 1;
          send_preview(std::move(*pic), n);
        }
      }
    }
    // Every sink has answered end of stream, or the stop cancelled its
    // reader's command: the joins are prompt.
    readers.clear();
    _session->unload_pipeline(h);

    if (cancelled) {
      JobEvent ev;
      ev.job = spec.id;
      ev.kind = JobEventKind::Cancelled;
      q.sink(ev);
      return;
    }

    // A stage that refused for memory: that is the failure, with what
    // was asked for and what the machine had, not a missing file.
    if (Json m = memory_refusal_(reports_from); !m.is_null()) {
      std::string why = memory_sentence_(m);
      fail_(q, Code::OutOfMemory, std::move(why), {{"memory", std::move(m)}});
      return;
    }

    // A clip comes out in two halves -- vpipe's Apple-native writer is
    // video only -- joined here: the picture passed through, the sound
    // encoded to AAC.
    if (!g.video_part.empty()) {
      if (reports) {
        report(/*force=*/true);  // the reports are closed: finishing
      }
      if (auto st = join_clip_(g); !st.ok()) {
        fail_(q, st.code(), st.error().message);
        return;
      }
    }
    if (!g.pages.empty()) {
      // A still's pages: a file each, in order, each said by its number.
      for (std::size_t i = 0; i < g.pages.size(); ++i) {
        if (!fs::is_regular_file(g.pages[i], ec)) {
          fail_(q, Code::Engine, std::format(
              "vpipe finished without writing page {} (see the log)",
              i + 1));
          return;
        }
      }
      for (std::size_t i = 0; i < g.pages.size(); ++i) {
        JobEvent out;
        out.job = spec.id;
        out.kind = JobEventKind::Output;
        out.output = g.pages[i];
        if (auto info = media::probe_file(g.pages[i]); info.ok()) {
          out.output_info = *info;
        }
        out.data = {{"page", i}, {"pages", g.pages.size()}};
        q.sink(out);
      }
    } else if (spec.op == kOpGenerateImage || spec.op == kOpEditImage ||
        spec.op == kOpGenerateVideo || spec.op == kOpGenerateAudio ||
        spec.op == kOpGenerateSpeech ||
        spec.op == kOpExportMedia || spec.op == kOpUpscaleVideo ||
        spec.op == kOpUpscaleImage) {
      if (!fs::is_regular_file(g.output, ec)) {
        fail_(q, Code::Engine,
              "vpipe finished without writing its file (see the log)");
        return;
      }
      JobEvent out;
      out.job = spec.id;
      out.kind = JobEventKind::Output;
      out.output = g.output;
      if (auto info = media::probe_file(g.output); info.ok()) {
        out.output_info = *info;
      }
      if (!score.empty()) {
        out.data = {{"score", score}};
      }
      q.sink(out);
    } else if (spec.op == kOpChat) {
      if (result_text.empty()) {
        fail_(q, Code::Engine, "the language model returned nothing");
        return;
      }
      JobEvent out;
      out.job = spec.id;
      out.kind = JobEventKind::Output;
      out.text = result_text;
      q.sink(out);
    }
    JobEvent done;
    done.job = spec.id;
    done.kind = JobEventKind::Finished;
    done.progress = 1.0f;
    q.sink(done);
  }

  // The pictures an edit draws on -- the base (the picture being
  // edited) first, then the references -- with the size each DISPLAYS
  // at (EXIF orientation applied), which is what the graph is sized by.
  static Result<std::vector<vp::RefImage>>
  reference_images_(const JobSpec& spec)
  {
    std::vector<vp::RefImage> refs;
    for (const char* role : {"base", "reference"}) {
      for (const auto& in : spec.inputs) {
        if (in.role != role || in.info.type != media::MediaType::Image) {
          continue;
        }
        VALTZ_ASSIGN(media::PixelSize sz, media::oriented_size(in.path));
        refs.push_back({in.path, sz,
                        in.info.frame.alpha != media::AlphaMode::None,
                        in.role == "base"});
      }
    }
    return refs;
  }

  // The picture a clip opens on (input role "first"), if it has one.
  static Result<std::optional<vp::RefImage>>
  first_frame_(const JobSpec& spec)
  {
    for (const auto& in : spec.inputs) {
      if (in.role != "first") {
        continue;
      }
      if (in.info.type != media::MediaType::Image) {
        return make_error(Code::InvalidArgument,
                          "a clip can start from a picture only");
      }
      VALTZ_ASSIGN(media::PixelSize sz, media::oriented_size(in.path));
      return std::optional<vp::RefImage>(vp::RefImage{
          in.path, sz, in.info.frame.alpha != media::AlphaMode::None,
          true});
    }
    return std::optional<vp::RefImage>();
  }

  // A clip's REFERENCES (Ref2VA), in the order the model reads them:
  // its inputs "reference" and "continue", each with what the job
  // recorded of it (param "reference_media": its route, its trim, a
  // tail) -- a clip's span made frames of its own, a sound's seconds.
  static Result<std::vector<vp::RefMedia>>
  reference_media_(const JobSpec& spec)
  {
    const Json list = jget(spec.params, "reference_media", Json::array());
    std::vector<vp::RefMedia> out;
    std::size_t k = 0;
    for (const auto& in : spec.inputs) {
      if (in.role != "reference" && in.role != "continue") {
        continue;
      }
      const Json e = k < list.size() ? list[k] : Json::object();
      ++k;
      vp::RefMedia r;
      r.path = in.path;
      r.kind = jget<std::string>(e, "kind", "");
      r.list = jget<std::string>(e, "route", "list") == "list";
      r.audio = jget(e, "audio", false);
      const media::Trim t = media::trim_from_json(
          jget(e, "trim", Json::object()));
      if (r.kind == "video") {
        r.width = in.info.frame.width;
        r.height = in.info.frame.height;
        const Rational rate = in.info.frame_rate.num > 0
                                  ? in.info.frame_rate
                                  : Rational{24, 1};
        r.fps = rate.to_double();
        const std::int64_t total = in.info.frame_count;
        std::int64_t first = 0;
        std::int64_t last = total > 0 ? total - 1 : -1;
        // A trim counted at another rate is moved onto the clip's.
        if (!t.identity()) {
          const double k2 = t.rate.num > 0 ? r.fps / t.rate.to_double()
                                           : 1.0;
          first = std::max<std::int64_t>(0, std::llround(t.in * k2));
          if (t.out >= 0) {
            const std::int64_t o = std::llround(t.out * k2);
            last = last >= 0 ? std::min(last, o) : o;
          }
        }
        // A clip continued: its last frames, up to where it ends.
        if (const std::int64_t tail = jget<std::int64_t>(e, "tail_frames",
                                                          0);
            tail > 0 && last >= 0) {
          first = std::max(first, last - tail + 1);
        }
        r.first = first;
        r.count = last >= first ? last - first + 1 : -1;
        r.start_s = static_cast<double>(first) / r.fps;
        r.duration_s = r.count > 0 ? static_cast<double>(r.count) / r.fps
                                   : 0.0;
      } else if (r.kind == "audio" && !t.identity() && t.rate.num > 0) {
        const double per = t.rate.to_double();
        const std::int64_t first = std::max<std::int64_t>(0, t.in);
        r.start_s = static_cast<double>(first) / per;
        if (t.out >= first) {
          r.duration_s = static_cast<double>(t.out - first + 1) / per;
        }
      }
      out.push_back(std::move(r));
    }
    return out;
  }

  // A movie's picture and sound, written apart, as one file. A graph
  // that made no sound (a family without a soundtrack) keeps its
  // picture alone. The scratch halves go either way; an export's sound
  // is its source, which stays.
  static Status
  join_clip_(const vp::BuiltGraph& g)
  {
    std::error_code ec;
    if (!fs::is_regular_file(g.video_part, ec)) {
      return make_error(Code::Engine,
                        "vpipe finished without writing the clip (see the "
                        "log)");
    }
    Status st = ok_status();
    if (fs::is_regular_file(g.audio_part, ec)) {
      st = media::add_soundtrack(g.video_part, g.audio_part, g.output,
                                 g.audio_start, g.audio_duration);
    } else {
      VALTZ_LOG_WARN("engine", "the clip has no soundtrack");
      fs::rename(g.video_part, g.output, ec);
      if (ec) {
        st = make_error(Code::Io, ec.message());
      }
    }
    fs::remove(g.video_part, ec);
    if (!g.audio_is_source) {
      fs::remove(g.audio_part, ec);
    }
    return st;
  }

  // Decode each reference into a valtz-source lease and emit it. On a
  // failure the pipeline is stopped BEFORE the lease closes: closing
  // emits, and a stop turns that into a drop.
  //
  // F16, so what is deeper than 8 bits -- a 16-bit or float still, a
  // camera RAW's development, the adjustments laid on it -- reaches the
  // model's VAE intact: vpipe resamples F16 on the GPU in F16, and only
  // a vision tower that reads 8 bits rounds to them, inside vpipe.
  Status
  feed_references_(vpipe::PipelineHandle& h, const vp::BuiltGraph& g,
                   const std::function<void()>& fed,
                   const media::FramePreview* preview)
  {
    for (const auto& f : g.references) {
      vp::SourceWriter src(h.stage(f.stage));
      // What the samples mean, in vpipe's sideband colour keys
      // (common/beat-keys.h): BT.709 primaries, sRGB-encoded -- or
      // linear, for a file that keeps light. RGB samples, full range.
      const Json tags = {{"color_primaries", 1},
                         {"color_transfer", f.linear ? 8 : 13},
                         {"color_matrix", 0},
                         {"color_full_range", true}};
      VALTZ_ASSIGN(auto lease, src.lease(
          Tensor::DType::F16, {f.channels, f.size.height, f.size.width},
          tags));
      // Drawn by the GPU straight into the lease's Metal buffer.
      auto st = media::decode_planar(
          f.path, f.size, f.channels, media::Fit::Crop, f.adjust,
          media::Sample::F16, lease.data(), lease.size(),
          f.linear ? media::Space::Linear : media::Space::Srgb, f.crop,
          {lease.mtl_buffer(), lease.mtl_offset()});
      if (!st.ok()) {
        _session->stop_pipeline(h);
        return st;
      }
      lease.commit();
      if (auto fin = src.finish(); !fin.ok()) {
        _session->stop_pipeline(h);
        return fin;
      }
    }
    // A clip fed frame by frame (an export with a look): each frame
    // decoded with its look straight into a leased beat. A cancel stops
    // it between frames.
    if (g.movie) {
      const vp::MovieFeed& m = *g.movie;
      vp::SourceWriter src(h.stage(m.stage));
      std::optional<vp::SourceWriter::Lease> lease;
      const bool u8 = m.sample == media::Sample::U8;
      // GATED (an upscaler, MovieFeed::Gate): whole groups, each fed
      // only once the groups before it -- but `ahead` -- have come out
      // past the tap; each but the first opens with the last group's
      // closing `overlap` frames again. Waiting, the job still reports
      // and still hears a cancel.
      const vp::MovieFeed::Gate& gate = m.gate;
      const bool gated = gate.group > 0;
      const int step = gate.group - gate.overlap;
      std::int64_t group = 0;     // the group being fed
      int in_group = 0;           // its frames fed so far
      std::deque<std::vector<std::uint8_t>> tail;  // the last `overlap`
      std::vector<std::uint8_t> last;              // the clip's last frame
      const auto lease_raw = [&]() -> Status {
        VALTZ_ASSIGN(auto l, src.lease(
            u8 ? Tensor::DType::U8 : Tensor::DType::F16,
            {3, m.size.height, m.size.width}, m.tags));
        lease = std::move(l);
        return ok_status();
      };
      const auto keep = [&](const std::uint8_t* p, std::size_t n) {
        if (!gated) {
          return;
        }
        last.assign(p, p + n);
        if (gate.overlap > 0) {
          tail.emplace_back(p, p + n);
          while (tail.size() > static_cast<std::size_t>(gate.overlap)) {
            tail.pop_front();
          }
        }
      };
      // How many groups it may run ahead now: up to `gate.ahead` while
      // the memory left holds twice a group per group ahead, and a
      // reserve -- sensed at each group, the graph running. The second
      // group waits for the first: before then nothing is loaded, and
      // the memory left says nothing. VALTZ_UPSCALE_AHEAD forces it.
      int said_ahead = -1;
      const auto ahead_now = [&]() -> int {
        int a = gate.ahead;
        std::uint64_t room = 0;
        if (const char* f = std::getenv("VALTZ_UPSCALE_AHEAD")) {
          a = std::max(0, std::atoi(f));
        } else if (group <= 1 || gate.group_bytes == 0) {
          a = 0;
        } else if (a > 0) {
          constexpr std::uint64_t kReserve = 2ull << 30;
          room = models::reclaimable_ram_bytes();
          const std::uint64_t fit =
              room > kReserve ? (room - kReserve) / (2 * gate.group_bytes)
                              : 0;
          a = static_cast<int>(std::min<std::uint64_t>(a, fit));
        }
        if (a != said_ahead) {
          if (room > 0) {
            VALTZ_LOG_INFO("upscale", "group {}: {} ahead ({} MB "
                           "reclaimable, a group ~{} MB)", group, a,
                           room >> 20, gate.group_bytes >> 20);
          } else {
            VALTZ_LOG_INFO("upscale", "group {}: {} ahead (not sensed)",
                           group, a);
          }
          said_ahead = a;
        }
        return a;
      };
      // A group full: the next one starts once enough has come out, with
      // the overlap sent again.
      const auto next_group = [&]() -> Status {
        if (!gated || in_group < gate.group) {
          return ok_status();
        }
        ++group;
        in_group = 0;
        if (const std::int64_t done = group - ahead_now(); done > 0) {
          const auto want = static_cast<std::uint64_t>(done * step);
          VALTZ_ASSIGN(auto n, vp::tap_wait(h.stage(gate.tap), want, [&] {
            fed();
            return !_cancel.load();
          }));
          if (n.passed < want && n.eos) {
            return make_error(Code::Engine, std::format(
                "the upscaler stopped after {} frames (see the log)",
                n.passed));
          }
        }
        for (const auto& f : tail) {
          VALTZ_TRY(lease_raw());
          std::memcpy(lease->data(), f.data(),
                      std::min(lease->size(), f.size()));
          lease->commit();
          lease.reset();
          ++in_group;
        }
        return ok_status();
      };
      const auto lease_frame =
          [&](std::int64_t) -> Result<media::FrameBuffer> {
            if (_cancel.load()) {
              return make_error(Code::Cancelled, "cancelled");
            }
            VALTZ_TRY(next_group());
            VALTZ_TRY(lease_raw());
            return media::FrameBuffer{
                lease->data(), lease->size(),
                {lease->mtl_buffer(), lease->mtl_offset()}};
          };
      const auto frame_done = [&](std::int64_t) -> Status {
        keep(lease->data(), lease->size());
        lease->commit();
        lease.reset();
        ++in_group;
        fed();
        return ok_status();
      };
      // A stack drawn whole, or the clip with its look.
      Status st = m.stack
          ? media::decode_movie_stack(*m.stack, m.rate, m.first, m.count,
                                      m.size, lease_frame, frame_done,
                                      preview,
                                      m.color ? &*m.color : nullptr)
          : media::decode_movie(m.path, m.adjust, m.crop, m.rate, m.first,
                                m.count, m.size, lease_frame, frame_done,
                                preview, m.sample);
      // The last group made whole with the clip's last frame: what the
      // model makes of them, the tap cuts.
      while (st.ok() && gated && !last.empty() &&
             (group < gate.groups - 1 || in_group < gate.group)) {
        auto buf = lease_frame(-1);
        if (!buf.ok()) {
          st = buf.error();
          break;
        }
        std::memcpy(buf->data, last.data(),
                    std::min(buf->size, last.size()));
        st = frame_done(-1);
      }
      if (st.ok()) {
        st = src.finish();
      }
      if (!st.ok()) {
        _session->stop_pipeline(h);
        return st;
      }
    }
    // The base's EXIF, for the result: ONE beat whatever it holds --
    // save-image reads one per image, so a base without EXIF still
    // sends an (empty) object.
    if (!g.exif_from.empty()) {
      vp::SourceWriter src(h.stage(vp::kExifSource));
      Json meta = Json::object();
      const auto block = media::exif_block_for(
          g.exif_from, {g.width, g.height});
      if (!block.empty()) {
        meta[vp::kExifKey] = base64(block);
      }
      Status st = src.emit(meta);
      if (st.ok()) {
        st = src.finish();
      }
      if (!st.ok()) {
        _session->stop_pipeline(h);
        return st;
      }
    }
    return ok_status();
  }

  vpipe::SessionIntf*     _session = nullptr;
  std::string             _desc = "vpipe";
  Json                    _backends = Json::array();  // backends()
  std::thread             _worker;
  std::mutex              _mu;
  std::condition_variable _cv;
  std::deque<Queued>      _queue;
  bool                    _stop = false;
  JobId                   _running;
  std::atomic<bool>       _cancel{false};
  std::once_flag                        _monitor_once;
  std::unique_ptr<vpipe::SystemMonitor> _monitor;
};

}

std::unique_ptr<Engine>
make_vpipe_engine(const EngineConfig& cfg)
{
  auto e = std::make_unique<VpipeEngine>(cfg);
  if (!e->available()) {
    return make_null_engine();
  }
  return e;
}

}
