// A program drawn, by valtz-procedural in a process of its own
// (media/procedural.h): the spec written, the process started, what it
// says read line by line, the process killed when it is stopped or goes
// quiet past its time.

#include "valtz/media/procedural.h"

#include "valtz/base/json.h"
#include "valtz/base/log.h"
#include "valtz/media/probe.h"

#include <mach-o/dyld.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <format>
#include <fstream>

extern char** environ;

namespace valtz::media {

namespace fs = std::filesystem;

namespace {

fs::path
executable_dir()
{
  char buf[PATH_MAX];
  std::uint32_t n = sizeof(buf);
  if (_NSGetExecutablePath(buf, &n) != 0) {
    return {};
  }
  std::error_code ec;
  fs::path p = fs::canonical(buf, ec);
  return ec ? fs::path(buf).parent_path() : p.parent_path();
}

fs::path
first_of(const std::vector<fs::path>& candidates, bool dir)
{
  for (const auto& c : candidates) {
    std::error_code ec;
    if (dir ? fs::is_directory(c, ec) : fs::is_regular_file(c, ec)) {
      fs::path p = fs::canonical(c, ec);
      return ec ? c : p;
    }
  }
  return {};
}

ProgramLog
log_of(const Json& j)
{
  ProgramLog l;
  l.level = jget<std::string>(j, "level", "log");
  l.text = jget<std::string>(j, "text", "");
  l.source = jget<std::string>(j, "source", "");
  l.line = jget(j, "line", 0);
  l.column = jget(j, "column", 0);
  return l;
}

}  // namespace

fs::path
procedural_helper()
{
  if (const char* env = std::getenv("VALTZ_PROCEDURAL_HELPER");
      env && *env) {
    return env;
  }
  std::vector<fs::path> c;
  if (fs::path dir = executable_dir(); !dir.empty()) {
    c.push_back(dir / "valtz-procedural");
    c.push_back(dir / ".." / "Helpers" / "valtz-procedural");
  }
#ifdef VALTZ_PROCEDURAL_HELPER_BUILD_PATH
  c.push_back(VALTZ_PROCEDURAL_HELPER_BUILD_PATH);
#endif
  return first_of(c, false);
}

fs::path
procedural_libs()
{
  if (const char* env = std::getenv("VALTZ_PROCEDURAL_LIBS"); env && *env) {
    return env;
  }
  std::vector<fs::path> c;
  if (fs::path helper = procedural_helper(); !helper.empty()) {
    c.push_back(helper.parent_path() / ".." / "Resources" / "procedural");
  }
  if (fs::path dir = executable_dir(); !dir.empty()) {
    c.push_back(dir / ".." / "Resources" / "procedural");
  }
#ifdef VALTZ_PROCEDURAL_LIBS_SOURCE_PATH
  c.push_back(VALTZ_PROCEDURAL_LIBS_SOURCE_PATH);
#endif
  for (const auto& d : c) {
    std::error_code ec;
    if (fs::is_regular_file(d / "three" / "build" / "three.module.js", ec)) {
      fs::path p = fs::canonical(d, ec);
      return ec ? d : p;
    }
  }
  return {};
}

std::string
program_type(const fs::path& f)
{
  std::string e = f.extension().string();
  std::ranges::transform(e, e.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (e == ".png") return "image/png";
  if (e == ".jpg" || e == ".jpeg") return "image/jpeg";
  if (e == ".gif") return "image/gif";
  if (e == ".webp") return "image/webp";
  if (e == ".heic" || e == ".heif") return "image/heic";
  if (e == ".mov") return "video/quicktime";
  if (e == ".mp4" || e == ".m4v") return "video/mp4";
  if (e == ".m4a" || e == ".aac") return "audio/mp4";
  if (e == ".wav") return "audio/wav";
  if (e == ".mp3") return "audio/mpeg";
  if (e == ".aif" || e == ".aiff") return "audio/aiff";
  if (e == ".flac") return "audio/flac";
  if (e == ".txt" || e == ".md" || e == ".json" || e == ".csv") {
    return "text/plain";
  }
  return {};
}

Result<ProgramAsset>
program_asset_of(const fs::path& f)
{
  ProgramAsset a;
  a.id = f.filename().string();
  a.name = f.stem().string();
  a.file = f;
  a.type = program_type(f);
  if (a.type.empty()) {
    return make_error(Code::Unsupported, std::format(
        "{}: a program reads PNG, JPEG, GIF, WebP and HEIC pictures, "
        "QuickTime and MP4 clips, sounds and texts", f.string()));
  }
  a.kind = a.type.starts_with("image/")   ? "image"
           : a.type.starts_with("video/") ? "video"
           : a.type.starts_with("audio/") ? "audio"
                                          : "text";
  if (a.kind != "text") {
    VALTZ_ASSIGN(MediaInfo info, probe_file(f));
    a.width = info.frame.width;
    a.height = info.frame.height;
    a.seconds = info.duration.seconds();
  }
  return a;
}

std::string
to_text(const ProgramLog& l)
{
  std::string at;
  if (!l.source.empty()) {
    at = l.source;
    if (l.line > 0) {
      at += std::format(":{}", l.line);
      if (l.column > 0) {
        at += std::format(":{}", l.column);
      }
    }
  } else if (l.line > 0) {
    at = std::format("line {}", l.line);
  }
  return at.empty() ? std::format("{}: {}", l.level, l.text)
                    : std::format("{} at {}: {}", l.level, at, l.text);
}

Result<ProgramRun>
render_program(const ProgramRender& r,
               const std::function<void(int, int)>& progress,
               const std::atomic<bool>* stop, std::vector<ProgramLog>* log,
               const std::function<void(ProgramPreview)>& preview)
{
  const fs::path helper = procedural_helper();
  if (helper.empty()) {
    return make_error(Code::Unsupported,
                      "the program renderer (valtz-procedural) is not "
                      "installed with this Valtz");
  }
  const fs::path libs = procedural_libs();
  if (libs.empty()) {
    return make_error(Code::Unsupported,
                      "Valtz's copies of three.js and Babylon.js are not "
                      "installed with this Valtz");
  }
  if (r.width < 16 || r.height < 16 || r.width > kProgramMaxSide ||
      r.height > kProgramMaxSide) {
    return make_error(Code::InvalidArgument, std::format(
        "a program is drawn at 16 to {} pixels a side, not {} x {}",
        kProgramMaxSide, r.width, r.height));
  }
  std::error_code ec;
  fs::create_directories(r.out.parent_path(), ec);
  const fs::path spec_file = fs::path(r.out.string() + ".spec.json");
  // A clip's previews, a folder of their own beside the output.
  const fs::path previews =
      preview && r.frames > 0 ? fs::path(r.out.string() + ".previews")
                              : fs::path();
  if (!previews.empty()) {
    fs::create_directories(previews, ec);
  }
  {
    Json spec = {
        {"program", r.program.string()},
        {"libs", libs.string()},
        {"width", r.width},
        {"height", r.height},
        {"frames", r.frames},
        {"fps_num", r.fps.num},
        {"fps_den", r.fps.den},
        {"seed", r.seed},
        {"epoch", r.epoch_ms},
        {"out", r.out.string()},
        {"codec", r.codec},
        {"load_timeout", r.load_timeout},
        {"frame_timeout", r.frame_timeout},
        {"assets", Json::array()},
        {"rate", r.sample_rate},
        {"sound", r.sound.string()},
        {"previews", previews.string()},
    };
    Json& assets = spec["assets"];
    for (const auto& a : r.assets) {
      assets.push_back({{"id", a.id},
                        {"name", a.name},
                        {"kind", a.kind},
                        {"file", a.file.string()},
                        {"type", a.type},
                        {"width", a.width},
                        {"height", a.height},
                        {"seconds", a.seconds},
                        {"inline", kProgramInlineBytes}});
    }
    std::ofstream f(spec_file, std::ios::binary);
    f << valtz::to_text(spec);
    if (!f) {
      return make_error(Code::Io, std::format("cannot write {}",
                                              spec_file.string()));
    }
  }
  struct Gone {
    fs::path p;
    fs::path dir;
    ~Gone()
    {
      std::error_code e;
      fs::remove(p, e);
      if (!dir.empty()) {
        fs::remove_all(dir, e);
      }
    }
  } gone{spec_file, previews};

  int out[2];
  if (pipe(out) != 0) {
    return make_error(Code::Io, "cannot start the program renderer");
  }
  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
  posix_spawn_file_actions_addclose(&fa, out[0]);
  posix_spawn_file_actions_addclose(&fa, out[1]);
  const std::string exe = helper.string();
  const std::string arg = spec_file.string();
  char* argv[] = {const_cast<char*>(exe.c_str()),
                  const_cast<char*>(arg.c_str()), nullptr};
  pid_t pid = 0;
  const int spawned =
      posix_spawn(&pid, exe.c_str(), &fa, nullptr, argv, environ);
  posix_spawn_file_actions_destroy(&fa);
  close(out[1]);
  if (spawned != 0) {
    close(out[0]);
    return make_error(Code::Io, std::format(
        "cannot start the program renderer ({}): error {}", exe, spawned));
  }

  using Clock = std::chrono::steady_clock;
  const auto t0 = Clock::now();
  auto heard = Clock::now();
  ProgramRun run;
  std::string failed;
  bool done = false;
  bool cancelled = false;
  bool quiet = false;
  std::string buf;
  // Past the renderer's own watchdog, a margin: it says why itself.
  const double patience =
      std::max(r.load_timeout, r.frame_timeout) + 30.0;
  for (;;) {
    if (stop && stop->load()) {
      cancelled = true;
      break;
    }
    if (std::chrono::duration<double>(Clock::now() - heard).count() >
        patience) {
      quiet = true;
      break;
    }
    pollfd p{out[0], POLLIN, 0};
    const int ready = poll(&p, 1, 100);
    if (ready <= 0) {
      continue;
    }
    char chunk[16384];
    const ssize_t n = read(out[0], chunk, sizeof chunk);
    if (n <= 0) {
      break;  // it ended
    }
    heard = Clock::now();
    buf.append(chunk, static_cast<std::size_t>(n));
    for (std::size_t nl = buf.find('\n'); nl != std::string::npos;
         nl = buf.find('\n')) {
      const std::string line = buf.substr(0, nl);
      buf.erase(0, nl + 1);
      const Json j = Json::parse(line, nullptr, /*allow_exceptions=*/false);
      if (!j.is_object()) {
        continue;
      }
      const auto event = jget<std::string>(j, "event", "");
      if (event == "log") {
        ProgramLog l = log_of(j);
        if (l.level == "error") {
          ++run.errors;
        }
        run.log.push_back(std::move(l));
      } else if (event == "loaded") {
        run.loaded = jget(j, "ok", false);
      } else if (event == "frame") {
        if (progress) {
          progress(jget(j, "frame", 0), jget(j, "of", 0));
        }
      } else if (event == "sound") {
        run.sound = jget<std::string>(j, "file", "");
      } else if (event == "preview") {
        // Read, handed on, removed.
        const fs::path f = jget<std::string>(j, "file", "");
        ProgramPreview p;
        p.frame = jget(j, "frame", 0);
        p.width = jget(j, "width", 0);
        p.height = jget(j, "height", 0);
        const std::size_t want = static_cast<std::size_t>(p.width) *
                                 static_cast<std::size_t>(p.height) * 3;
        std::ifstream in(f, std::ios::binary);
        p.rgb.resize(want);
        if (want > 0 &&
            in.read(reinterpret_cast<char*>(p.rgb.data()),
                    static_cast<std::streamsize>(want))) {
          preview(std::move(p));
        }
        std::error_code e;
        fs::remove(f, e);
      } else if (event == "done") {
        done = true;
        run.empty = jget(j, "empty", false);
        run.empty_why = jget<std::string>(j, "why", "");
      } else if (event == "failed") {
        failed = jget<std::string>(j, "text", "");
      }
    }
  }
  if (cancelled || quiet) {
    kill(pid, SIGKILL);
  }
  close(out[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  run.seconds = std::chrono::duration<double>(Clock::now() - t0).count();
  if (log) {
    *log = run.log;
  }
  if (cancelled) {
    fs::remove(r.out, ec);
    return make_error(Code::Cancelled, "stopped");
  }
  if (quiet) {
    fs::remove(r.out, ec);
    return make_error(Code::Engine,
                      "the program renderer stopped answering");
  }
  if (!done || !failed.empty()) {
    fs::remove(r.out, ec);
    if (failed.empty()) {
      failed = WIFSIGNALED(status)
                   ? std::format("the program renderer ended (signal {})",
                                 WTERMSIG(status))
                   : std::format("the program renderer ended (exit {})",
                                 WEXITSTATUS(status));
    }
    return make_error(Code::Engine, failed);
  }
  VALTZ_LOG_INFO("procedural", "{} x {} {} in {:.2f} s ({} errors)",
                 r.width, r.height,
                 r.frames > 0 ? std::format("{} frames", r.frames)
                              : std::string("still"),
                 run.seconds, run.errors);
  return run;
}

}  // namespace valtz::media
