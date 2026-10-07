// The valtz-vpipe plugin, end to end in a real vpipe session: graphs of
// valtz-source -> valtz-sink driven through Valtz's own SourceWriter and
// SinkReader. No models; these run on any machine with the engine.
//
// This file (with the engine) is the exception to "only
// core/src/engine/vpipe includes vpipe": it tests exactly that seam.

#include "testing.h"

#include "engine/vpipe/flex-json.h"
#include "engine/vpipe/host-exchange.h"
#include "engine/vpipe/job-progress.h"
#include "valtz/engine/engine.h"
#include "valtz-vpipe/exchange.h"

#include "vpipe/vpipe.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <mutex>
#include <thread>

using namespace valtz;
using namespace valtz::engine;
namespace ex = valtz::exchange;
namespace fs = std::filesystem;

namespace {

// One session with the plugin loaded, destroyed on scope exit (or
// earlier, to show that a beat outlives it).
struct TestSession {
  vpipe::SessionIntf* s = nullptr;

  explicit TestSession(const fs::path& dir)
  {
    Json cfg = {
      {"db", {{"path", (dir / "vpipe-db").string()}}},
      {"log", {{"level", "warn"}}},
      {"plugins", Json::array({vp::find_plugin().string()})},
    };
    s = const_cast<vpipe::SessionIntf*>(
        vpipe::SessionManager::get().create_session(cfg.dump()));
  }
  ~TestSession() { destroy(); }

  void
  destroy()
  {
    if (s) {
      vpipe::SessionManager::get().destroy_session(s);
      s = nullptr;
    }
  }
};

// The pipeline stopped and unloaded if the test leaves early -- a
// REQUIRE that fails returns -- before the readers declared ahead of it
// are destroyed: a stop cancels a reader's `next`, so its join ends (the
// engine does the same, vpipe-engine.cc). A failed check then fails the
// test instead of hanging it: under load, a 10 s wait_pipelines once
// timed out and the reader waited for ever. `release()` when the test
// unloads it itself.
struct Unloader {
  TestSession&          ts;
  vpipe::PipelineHandle h;
  bool                  released = false;

  void release() { released = true; }
  ~Unloader()
  {
    if (!released && ts.s) {
      ts.s->stop_pipeline(h);
      ts.s->unload_pipeline(h);
    }
  }
};

Json
graph(const std::string& id, const Json& sink_cfg)
{
  return {
    {"id", id},
    {"stages", Json::array({
      {{"id", "src"}, {"type", ex::kSourceType},
       {"iports", Json::array()}, {"config", Json::object()}},
      {{"id", "sink"}, {"type", ex::kSinkType},
       {"iports", Json::array({{{"src", "src"}, {"oport", 0}}})},
       {"config", sink_cfg}},
    })},
    {"subpipelines", Json::array()},
  };
}

// Collects what a SinkReader delivers.
struct Collected {
  std::mutex              mu;
  std::vector<vp::SinkBeat> beats;

  vp::SinkReader::Fn
  fn()
  {
    return [this](vp::SinkBeat&& b) {
      std::lock_guard lk(mu);
      beats.push_back(std::move(b));
    };
  }
};

Json
stats(vpipe::PipelineHandle& h)
{
  vpipe::CommandHandle c = h.stage("sink").command(ex::kCmdStats);
  if (c.wait(5000) != vpipe::CommandState::Replied) {
    return Json::object();
  }
  return Json::parse(c.result_json(), nullptr, false);
}

bool
plugin_present()
{
  return !vp::find_plugin().empty();
}

}

TEST(vpipe_plugin, declares_its_commands)
{
  REQUIRE(plugin_present());
  TestSession ts(test::temp_dir("vpx-decl"));
  REQUIRE(ts.s);
  vpipe::PipelineHandle h = ts.s->load_pipeline(
      graph("decl", Json::object()).dump());
  REQUIRE(h);
  Json sink = Json::parse(h.stage("sink").commands_json(), nullptr, false);
  Json src = Json::parse(h.stage("src").commands_json(), nullptr, false);
  auto names = [](const Json& cmds) {
    std::vector<std::string> v;
    for (const auto& c : cmds) {
      v.push_back(jget<std::string>(c, "name", ""));
    }
    return v;
  };
  CHECK(names(sink) == (std::vector<std::string>{ex::kCmdNext,
                                                 ex::kCmdStats}));
  CHECK(names(src) == (std::vector<std::string>{ex::kCmdLease, ex::kCmdPush,
                                                ex::kCmdEmit,
                                                ex::kCmdFinish}));
  // Commands are refused, not queued, before launch.
  vpipe::CommandHandle early = h.stage("sink").command(ex::kCmdNext);
  CHECK(early.wait(0) == vpipe::CommandState::Failed);
  ts.s->unload_pipeline(h);
}

// The whole point: the bytes Valtz fills in a lease are the bytes it
// reads back from the sink -- same address, no copy on the way -- and
// they stay valid after the pipeline unloads and the session is gone.
TEST(vpipe_plugin, lease_reaches_sink_in_place)
{
  REQUIRE(plugin_present());
  TestSession ts(test::temp_dir("vpx-lease"));
  REQUIRE(ts.s);
  vpipe::PipelineHandle h = ts.s->load_pipeline(
      graph("lease", {{ex::kSinkPolicy, ex::kSinkPolicyQueue}}).dump());
  REQUIRE(h);
  REQUIRE(ts.s->launch_pipeline(h).code == 0);

  Collected got;
  vp::SinkReader reader(h.stage("sink"), got.fn());
  vp::SourceWriter src(h.stage("src"));
  Unloader unload{ts, h};
  auto lease = src.lease(Tensor::DType::U8, {3, 4, 5}, {{"tag", 7}});
  REQUIRE_OK(lease);
  REQUIRE(lease->size() >= 60);
  CHECK(lease->mtl_buffer() != nullptr);  // Metal shared memory
  std::uint8_t* leased = lease->data();
  for (int i = 0; i < 60; ++i) {
    leased[i] = static_cast<std::uint8_t>(i);
  }
  lease->commit();
  REQUIRE_OK(src.finish());
  REQUIRE(ts.s->wait_pipelines(10000).code == 0);
  reader.join();
  CHECK(reader.reached_eos());

  REQUIRE(got.beats.size() == 1);
  TensorPtr t = got.beats[0].tensor;
  REQUIRE(t);
  CHECK(t->data == leased);
  CHECK(t->mtl_buffer == lease->mtl_buffer());
  CHECK(t->shape == (std::vector<std::int64_t>{3, 4, 5}));
  CHECK(t->byte_size == 60);
  CHECK(got.beats[0].seq == 1);
  CHECK(jget(t->meta, "tag", 0) == 7);

  unload.release();
  ts.s->unload_pipeline(h);
  ts.destroy();
  bool intact = true;
  for (int i = 0; i < 60; ++i) {
    intact = intact && t->data[i] == i;
  }
  CHECK(intact);
}

TEST(vpipe_plugin, push_gathers_strides_and_emit_sends_json)
{
  REQUIRE(plugin_present());
  TestSession ts(test::temp_dir("vpx-push"));
  REQUIRE(ts.s);
  // depth 1: the sink back-pressures the source while the reader drains.
  vpipe::PipelineHandle h = ts.s->load_pipeline(
      graph("push", {{ex::kSinkPolicy, ex::kSinkPolicyQueue},
                     {ex::kSinkDepth, 1}}).dump());
  REQUIRE(h);
  REQUIRE(ts.s->launch_pipeline(h).code == 0);
  Collected got;
  vp::SinkReader reader(h.stage("sink"), got.fn());
  vp::SourceWriter src(h.stage("src"));
  Unloader unload{ts, h};

  // [2,3] u8 with padded rows: a row stride of 4 elements.
  auto store = std::make_shared<std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{0, 1, 2, 99, 3, 4, 5, 99});
  Tensor padded;
  padded.dtype = Tensor::DType::U8;
  padded.shape = {2, 3};
  padded.strides = {4, 1};
  padded.data = store->data();
  padded.byte_size = store->size();
  padded.owner = store;
  REQUIRE_OK(src.push(padded, {{"i", 1}}));
  REQUIRE_OK(src.emit({{"text", "hello"}}));
  REQUIRE_OK(src.finish());
  REQUIRE(ts.s->wait_pipelines(10000).code == 0);
  reader.join();
  CHECK(reader.reached_eos());

  REQUIRE(got.beats.size() == 2);
  TensorPtr t = got.beats[0].tensor;
  REQUIRE(t);
  CHECK(t->contiguous());
  CHECK(t->byte_size == 6);
  bool packed = true;
  for (int i = 0; i < 6; ++i) {
    packed = packed && t->data[i] == i;
  }
  CHECK(packed);
  CHECK(jget(t->meta, "i", 0) == 1);
  CHECK(!got.beats[1].tensor);
  CHECK(jget<std::string>(got.beats[1].meta, "text", "") == "hello");
  CHECK(got.beats[1].seq == 2);
  unload.release();
  ts.s->unload_pipeline(h);
}

TEST(vpipe_plugin, latest_keeps_the_newest)
{
  REQUIRE(plugin_present());
  TestSession ts(test::temp_dir("vpx-latest"));
  REQUIRE(ts.s);
  vpipe::PipelineHandle h = ts.s->load_pipeline(
      graph("latest", {{ex::kSinkPolicy, ex::kSinkPolicyLatest},
                       {ex::kSinkDepth, 1}}).dump());
  REQUIRE(h);
  REQUIRE(ts.s->launch_pipeline(h).code == 0);
  vp::SourceWriter src(h.stage("src"));
  // Nobody reads yet: a `latest` sink never makes the producer wait.
  auto store = std::make_shared<std::vector<std::uint8_t>>(4, 0);
  for (int i = 0; i < 5; ++i) {
    (*store)[0] = static_cast<std::uint8_t>(i);
    Tensor t;
    t.shape = {4};
    t.data = store->data();
    t.byte_size = 4;
    t.owner = store;
    REQUIRE_OK(src.push(t, {{"i", i}}));
  }
  REQUIRE_OK(src.finish());
  // Let the sink see all five and the end, then read.
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(10);
  Json st;
  do {
    st = stats(h);
    if (jget(st, ex::kEos, false)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < deadline);
  CHECK(jget(st, ex::kEos, false));
  CHECK(jget(st, ex::kReceived, 0) == 5);
  CHECK(jget(st, ex::kDropped, 0) == 4);

  Collected got;
  vp::SinkReader reader(h.stage("sink"), got.fn());
  Unloader unload{ts, h};
  REQUIRE(ts.s->wait_pipelines(10000).code == 0);
  reader.join();
  CHECK(reader.reached_eos());
  REQUIRE(got.beats.size() == 1);
  CHECK(got.beats[0].seq == 5);
  CHECK(got.beats[0].dropped == 4);
  REQUIRE(got.beats[0].tensor);
  CHECK(got.beats[0].tensor->data[0] == 4);
  unload.release();
  ts.s->unload_pipeline(h);
}

TEST(vpipe_plugin, stop_ends_a_waiting_reader)
{
  REQUIRE(plugin_present());
  TestSession ts(test::temp_dir("vpx-stop"));
  REQUIRE(ts.s);
  vpipe::PipelineHandle h = ts.s->load_pipeline(
      graph("stop", Json::object()).dump());
  REQUIRE(h);
  REQUIRE(ts.s->launch_pipeline(h).code == 0);
  Collected got;
  vp::SinkReader reader(h.stage("sink"), got.fn());
  Unloader unload{ts, h};
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ts.s->stop_pipeline(h);
  reader.join();  // the stop cancels its `next`
  CHECK(!reader.reached_eos());
  CHECK(!reader.ended_by().empty());
  CHECK(got.beats.empty());
  unload.release();
  ts.s->unload_pipeline(h);
}

// Json <-> FlexData, tree to tree: every kind survives, and the number
// kinds stay apart (JSON text would make them all "a number").
TEST(vpipe_plugin, flex_json_round_trip_keeps_kinds)
{
  const Json j = {
    {"null", nullptr}, {"yes", true}, {"neg", -3},
    {"big", std::uint64_t{18446744073709551615ull}}, {"real", 2.5},
    {"three", 3.0}, {"text", "héllo"},
    {"list", Json::array({1, "two", Json::object({{"k", false}})})},
    {"nested", {{"deep", {{"x", 1}}}}},
  };
  const vpipe::FlexData f = vp::to_flex(j);
  REQUIRE(f.is_object());
  CHECK(vp::field(f, "neg").is_int());
  CHECK(vp::field(f, "big").is_uint());
  CHECK(vp::field_uint(f, "big") == 18446744073709551615ull);
  CHECK(vp::field(f, "three").is_real());   // 3.0 stays a real
  CHECK(vp::field(f, "null").is_null());
  CHECK(vp::field_str(f, "text") == "héllo");
  CHECK(vp::field_str(f, "missing", "fallback") == "fallback");
  CHECK(vp::field_uint(f, "text", 9) == 9);  // wrong kind: the fallback
  const Json back = vp::from_flex(f);
  CHECK(back == j);
  CHECK(back["three"].is_number_float());
  CHECK(back["big"].is_number_unsigned());
}

// The session's live progress reports, read as the engine reads them:
// the document SessionIntf::progress() hands over, through flex-json.
TEST(vpipe_plugin, session_progress_document)
{
  REQUIRE(plugin_present());
  TestSession ts(test::temp_dir("vpx-progress"));
  REQUIRE(ts.s);
  const Json doc = vp::from_flex(ts.s->progress());
  REQUIRE(doc.is_object());
  CHECK(doc["version"].is_number_unsigned());
  CHECK(doc["version"].get<std::uint64_t>() == ts.s->progress_version());
  CHECK(doc["items"].is_array());
  CHECK(doc["items"].empty());  // nothing running
  CHECK(vp::JobProgress::last_id(doc) == 0);
  vp::JobProgress p(0, false);
  CHECK(p.read(doc, 0).phase == kPhasePrepare);
}

namespace {

Json
report(std::uint64_t id, const std::string& desc, std::uint64_t done,
       std::uint64_t total, std::uint64_t seq = 0)
{
  return {{"id", id},       {"desc", desc}, {"done", done},
          {"total", total}, {"detail", ""}, {"seq", seq ? seq : id},
          {"elapsed_ms", 1500}};
}

Json
reports(Json items)
{
  return {{"version", 1}, {"items", std::move(items)}};
}

}

// vpipe's report list -> a job's phase: its names mapped, an earlier
// job's reports ignored, the oldest still counting followed.
TEST(engine_progress, phases_from_reports)
{
  vp::JobProgress p(/*after=*/4, /*download=*/false);
  // Before anything counts, and with only an earlier job's leftover.
  CHECK(p.read(reports(Json::array()), 0).phase == kPhasePrepare);
  CHECK(p.read(reports({report(3, "denoise", 2, 10)}), 0).phase ==
        kPhasePrepare);

  // The denoise, per block: its count and fraction.
  vp::JobPhase d = p.read(reports({report(5, "denoise", 120, 480)}), 0);
  CHECK(d.phase == kPhaseDenoise);
  CHECK(d.done == 120 && d.total == 480);
  CHECK(d.fraction() == 0.25f);
  CHECK(d.elapsed == 1.5);
  CHECK(d.data()["phase"] == "denoise");
  CHECK(d.same(p.read(reports({report(5, "denoise", 120, 480, 9)}), 0)));
  CHECK(!d.same(p.read(reports({report(5, "denoise", 121, 480)}), 0)));

  // Between the denoise and the decode nothing is live: finishing.
  CHECK(p.read(reports(Json::array()), 0).phase == kPhaseFinish);

  // A clip's frames and its sound side by side: the older, still
  // counting, whichever moved last.
  const Json both = reports({report(6, "vae decode", 3, 15, 40),
                             report(7, "audio vae decode", 1, 4, 41)});
  CHECK(p.read(both, 0).phase == kPhaseDecode);
  // The frames done (a bar left open at its total): the sound.
  const Json sound = reports({report(6, "vae decode", 15, 15, 40),
                              report(7, "audio vae decode", 2, 4, 41)});
  CHECK(p.read(sound, 0).phase == kPhaseSound);
  // Every one at its total: the newest moved.
  const Json done = reports({report(6, "vae decode", 15, 15, 42),
                             report(7, "audio vae decode", 4, 4, 41)});
  CHECK(p.read(done, 0).phase == kPhaseDecode);

  // Indeterminate counts, and names Valtz has no word for.
  vp::JobPhase r = p.read(reports({report(8, "encoding references", 0, 0),
                                   report(9, "lora fuse", 1, 2)}), 0);
  CHECK(r.phase == kPhaseReferences);
  CHECK(r.fraction() == -1.0f);
  vp::JobPhase w = p.read(reports({report(9, "lora fuse", 1, 2)}), 0);
  CHECK(w.phase == kPhaseWork);
  CHECK(w.label == "lora fuse");

  // A download names its big file; between them it is still
  // downloading, uncounted.
  vp::JobProgress f(/*after=*/0, /*download=*/true);
  vp::JobPhase dl =
      f.read(reports({report(1, "model.safetensors", 5, 10)}), 0);
  CHECK(dl.phase == kPhaseDownload);
  CHECK(dl.label == "model.safetensors");
  vp::JobPhase gap = f.read(reports(Json::array()), 0);
  CHECK(gap.phase == kPhaseDownload);
  CHECK(gap.label.empty() && gap.total == 0);
}

// A clip's writer ("save video") counts its frames: an export's phase,
// counted; in a generation, the frames written after the decode --
// finishing.
TEST(engine_progress, a_clip_s_writer)
{
  vp::JobProgress e(/*after=*/0, /*download=*/false, /*exporting=*/true);
  CHECK(e.read(reports(Json::array()), 0).phase == kPhasePrepare);
  vp::JobPhase x = e.read(reports({report(1, "save video", 900, 3600)}), 0);
  CHECK(x.phase == kPhaseExport);
  CHECK(x.done == 900 && x.total == 3600);
  CHECK(x.fraction() == 0.25f);
  // Written: the soundtrack is joined -- finishing.
  CHECK(e.read(reports(Json::array()), 0).phase == kPhaseFinish);
  vp::JobProgress g(/*after=*/0, /*download=*/false);
  CHECK(g.read(reports({report(1, "vae decode", 9, 9),
                        report(2, "save video", 5, 9)}), 0)
            .phase == kPhaseFinish);
}

// A song (generate-audio) reports its three passes under one name:
// planning the score and writing the song uncounted, with how far each
// has got in its detail; then the flow matching, counted -- a denoise;
// then the decoder's sound.
TEST(engine_progress, a_song_s_passes)
{
  vp::JobProgress p(/*after=*/0, /*download=*/false);
  auto song = [](std::uint64_t done, std::uint64_t total,
                 const std::string& detail) {
    Json r = report(1, "song", done, total);
    r["detail"] = detail;
    return reports({r});
  };
  vp::JobPhase plan = p.read(song(0, 0, "planning score: 412 tokens"), 0);
  CHECK(plan.phase == kPhaseScore);
  CHECK(plan.made == 412);
  CHECK(plan.data()["made"] == 412.0);
  CHECK(plan.fraction() == -1.0f);
  vp::JobPhase sung = p.read(song(0, 0, "writing the song: 42.3 s"), 1);
  CHECK(sung.phase == kPhaseSong);
  CHECK(std::abs(sung.made - 42.3) < 1e-9);
  CHECK(!sung.same(p.read(song(0, 0, "writing the song: 42.4 s"), 1)));
  vp::JobPhase flow = p.read(song(8, 32, "synthesizing"), 2);
  CHECK(flow.phase == kPhaseDenoise);
  CHECK(flow.made == -1);
  CHECK(!flow.data().contains("made"));
  CHECK(flow.fraction() == 0.25f);
  CHECK(p.read(reports({report(2, "audio vae decode", 1, 3)}), 3).phase ==
        kPhaseSound);
  // Past the flow matching, nothing live: finishing.
  CHECK(p.read(reports(Json::array()), 4).phase == kPhaseFinish);
}

// Between counts the phase moves on its own, at the recent pace: in step
// with it, slowing short of the next count, never past it, and never
// backwards when the count arrives.
TEST(engine_progress, estimate_between_counts)
{
  vp::JobProgress p(0, false);
  auto at = [&](std::uint64_t done, double t) {
    return p.read(reports({report(1, "denoise", done, 300)}), t);
  };
  // One count: no pace yet, the estimate is the count.
  vp::JobPhase a = at(10, 0);
  CHECK(a.estimate == 10.0 / 300);
  CHECK(a.rate == 0);
  // One unit in 20 s: a minute per percent.
  vp::JobPhase b = at(11, 20);
  CHECK(b.rate > 0);
  CHECK(std::abs(b.rate * 20 * 300 - 1) < 1e-9);
  // Half-way into the next unit at that pace.
  vp::JobPhase c = at(11, 30);
  CHECK(std::abs(c.estimate * 300 - 11.5) < 1e-9);
  // Every second it has moved, and it never reaches the next count.
  double last = c.estimate;
  for (double t = 31; t <= 120; t += 1) {
    const double e = at(11, t).estimate;
    CHECK(e > last);
    CHECK(e < 12.0 / 300);
    last = e;
  }
  // The count arrives: at or past the estimate.
  vp::JobPhase d = at(12, 121);
  CHECK(d.estimate >= last);
  CHECK(d.estimate == 12.0 / 300);
  // Uncounted, and finished: no estimate to make.
  CHECK(p.read(reports({report(2, "vae decode", 0, 0)}), 122).estimate ==
        -1);
  CHECK(at(300, 130).estimate == 1.0);
}

// An extension's backend (DESIGN §8a): one this vpipe would refuse -- an
// ABI outside its window, a feature it lacks -- is kept out of the
// session and named; one vpipe does not load is found out by the stage
// it should have registered. Neither takes the engine down.
TEST(vpipe_plugin, extension_backends_are_checked)
{
  if (!plugin_present()) {
    SKIP("valtz-vpipe.so not found");
  }
  Json d;
  CHECK(vp::backend_refusal(1, {}, &d) == "abi");
  const int abi = jget(d, "current", 0);
  CHECK(abi >= 8);
  CHECK(vp::backend_refusal(abi + 1, {}, &d) == "abi");
  CHECK(vp::backend_refusal(abi, {"no-such-feature/1"}, &d) == "feature");
  CHECK(jget<std::string>(d, "feature", "") == "no-such-feature/1");
  CHECK(vp::backend_refusal(abi, {"stage-commands/1"}, &d).empty());

  const auto dir = test::temp_dir("backends");
  std::ofstream(dir / "broken.so") << "not a plugin";
  EngineConfig cfg;
  cfg.state_dir = dir / "engine";
  cfg.temp_dir = dir / "tmp";
  cfg.log_level = "error";
  cfg.backends = {
    {"com.acme.old", dir / "broken.so", 1, {}, {"acme-old"}},
    {"com.acme.broken", dir / "broken.so", abi, {}, {"acme-generate"}}};
  auto e = make_vpipe_engine(cfg);
  REQUIRE(e && e->available());
  const Json b = e->backends();
  REQUIRE(b.size() == 2);
  CHECK(jget<std::string>(b[0], "state", "") == "refused");
  CHECK(jget<std::string>(b[0], "why", "") == "abi");
  CHECK(jget<std::string>(b[1], "state", "") == "not-loaded");
  CHECK(jget<std::string>(b[1], "why", "") == "stage");
  CHECK(jget<std::string>(b[1]["args"], "stage", "") == "acme-generate");
  e->shutdown();
}

// valtz-tap (exchange.h): the frames going by are counted -- a [F, C, H,
// W] beat is F, any other tensor one -- `wait` replies once a count is
// reached (it waits; the beats do not) or at the end of the stream, and
// past `max_frames` the stream is cut: how the engine feeds an upscaler a
// group at a time (DESIGN §4f).
TEST(vpipe_plugin, tap_counts_waits_and_cuts)
{
  REQUIRE(plugin_present());
  TestSession ts(test::temp_dir("vpx-tap"));
  REQUIRE(ts.s);
  const Json g = {
    {"id", "tap"},
    {"stages", Json::array({
      {{"id", "src"}, {"type", ex::kSourceType},
       {"iports", Json::array()}, {"config", Json::object()}},
      {{"id", "tap"}, {"type", ex::kTapType},
       {"iports", Json::array({{{"src", "src"}, {"oport", 0}}})},
       {"config", {{ex::kTapMaxFrames, 5}}}},
      {{"id", "sink"}, {"type", ex::kSinkType},
       {"iports", Json::array({{{"src", "tap"}, {"oport", 0}}})},
       {"config", {{ex::kSinkPolicy, ex::kSinkPolicyQueue},
                   {ex::kSinkDepth, 16}}}},
    })},
    {"subpipelines", Json::array()},
  };
  vpipe::PipelineHandle h = ts.s->load_pipeline(g.dump());
  REQUIRE(h);
  REQUIRE(ts.s->launch_pipeline(h).code == 0);
  Collected got;
  vp::SinkReader reader(h.stage("sink"), got.fn());
  vp::SourceWriter src(h.stage("src"));
  Unloader unload{ts, h};

  // A wait sent before anything is fed is answered once it is.
  Result<vp::TapCount> early = make_error(Code::Internal, "not run");
  std::thread waiter([&] { early = vp::tap_wait(h.stage("tap"), 2); });
  for (int i = 0; i < 3; ++i) {
    auto l = src.lease(Tensor::DType::U8, {3, 2, 2});
    REQUIRE_OK(l);
    l->commit();
  }
  waiter.join();
  REQUIRE_OK(early);
  CHECK(early->passed >= 2);
  CHECK(!early->eos);
  auto three = vp::tap_wait(h.stage("tap"), 3);
  REQUIRE_OK(three);
  CHECK(three->passed == 3);

  // A stack of four frames counts four; under the cut, it passes whole.
  auto stack = src.lease(Tensor::DType::U8, {4, 3, 2, 2});
  REQUIRE_OK(stack);
  stack->commit();
  // Past the cut: dropped.
  auto extra = src.lease(Tensor::DType::U8, {3, 2, 2});
  REQUIRE_OK(extra);
  extra->commit();
  REQUIRE_OK(src.finish());
  // More than will ever pass: answered at the end, eos.
  auto end = vp::tap_wait(h.stage("tap"), 100, [] { return true; });
  if (end.ok()) {
    CHECK(end->eos);
    CHECK(end->passed == 7);
    CHECK(end->cut == 1);
  }
  REQUIRE(ts.s->wait_pipelines(10000).code == 0);
  reader.join();
  CHECK(reader.reached_eos());
  REQUIRE(got.beats.size() == 4);
  REQUIRE(got.beats[3].tensor);
  CHECK(got.beats[3].tensor->shape ==
        (std::vector<std::int64_t>{4, 3, 2, 2}));
  unload.release();
  ts.s->unload_pipeline(h);
}

// An upscale's progress (DESIGN §4f) is its frames written, once they
// begin: each group's own denoise shows only before the first is out.
TEST(engine_progress, an_upscale_is_its_frames_restored)
{
  vp::JobProgress u(/*after=*/0, /*download=*/false, /*exporting=*/false,
                    /*restoring=*/true);
  CHECK(u.read(reports({report(1, "denoise", 12, 30)}), 0).phase ==
        kPhaseDenoise);
  const vp::JobPhase p = u.read(reports({report(1, "save video", 21, 124),
                                         report(2, "denoise", 7, 30)}), 0);
  CHECK(p.phase == kPhaseRestore);
  CHECK(p.done == 21 && p.total == 124);
  // Its writer counts a group's 21 frames at once, in pieces -- as a
  // real 72-frame run did: 4, 16, 21, 22, 42, 43, 63, 72. Estimated by
  // GROUP: no pace before two boundaries are crossed; then from the last
  // one toward the next at the last group's pace, every second, never
  // backwards (a stray piece does not restart it), never past the next
  // boundary or the total.
  vp::JobProgress g(/*after=*/0, /*download=*/false, /*exporting=*/false,
                    /*restoring=*/true, /*group=*/21);
  auto at = [&](std::uint64_t done, double t) {
    return g.read(reports({report(1, "save video", done, 72)}), t);
  };
  at(4, 0);
  at(16, 1);
  at(21, 2);                                        // a boundary crossed
  CHECK(at(22, 3).estimate == 22.0 / 72);
  CHECK(at(22, 20).estimate == 22.0 / 72);          // one: no pace yet
  at(42, 32);                                       // 30 s a group
  at(43, 33);
  CHECK(std::abs(at(43, 47).estimate * 72 - 52.5) < 1e-6);  // half-way
  double last = at(43, 47).estimate;
  for (double t = 48; t <= 61; t += 1) {
    const double e = at(43, t).estimate;
    CHECK(e > last && e < 63.0 / 72);
    last = e;
  }
  const double crossed = at(63, 62).estimate;
  CHECK(crossed >= last && crossed == 63.0 / 72);
  // The last group is 9 frames, padded whole: approached at a whole
  // group's pace, never reached.
  CHECK(std::abs(at(63, 77).estimate * 72 - 67.5) < 1e-6);
  last = crossed;
  for (double t = 63; t <= 600; t += 9) {
    const double e = at(63, t).estimate;
    CHECK(e >= last && e < 1.0);
    last = e;
  }
  CHECK(at(72, 601).estimate == 1.0);
}
