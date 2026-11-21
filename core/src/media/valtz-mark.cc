// The Valtz mark as media (media/valtz-mark.h): its sonic logo
// synthesized, its pictures drawn by tools/valtz-mark.

#include "valtz/media/valtz-mark.h"

#include "valtz/base/version.h"
#include "valtz/media/movie.h"
#include "valtz/media/sound.h"

#include <mach-o/dyld.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <numbers>
#include <string>

extern char** environ;

namespace valtz::media {

namespace fs = std::filesystem;

namespace {

// A note struck: its pitch (MIDI), when, how hard, and -- damped by the
// next chord -- when its damper falls (negative: never).
struct Note {
  int    midi;
  double at;
  double velocity;
  double off;
};

double
hz(int midi)
{
  return 440.0 * std::pow(2.0, (midi - 69) / 12.0);
}

// A small, fixed pseudo-random source: the hammer's noise and the
// strings' phases, the same every time.
struct Lcg {
  std::uint32_t s = 0x2545F491u;
  double next()
  {
    s = s * 1664525u + 1013904223u;
    return (s >> 8) / double(1u << 24);
  }
};

// One note into `out` (interleaved stereo). A struck string's partials,
// stretched by its stiffness, the hammer's place shaping them (a seventh
// along: the seventh partial missing, as a piano's is nearly); three
// strings a note, a little apart, so the tone beats; each partial dying
// in two stages -- the prompt sound, then the after-sound -- the higher
// ones sooner, a lower note longer. The damper takes it away in a tenth
// of a second. Placed in the stereo field by its pitch, as a piano
// sounds from the player's seat.
void
strike(const Note& n, int rate, std::vector<float>& out, Lcg& rng)
{
  const double f0 = hz(n.midi);
  const std::size_t frames = out.size() / 2;
  const std::size_t start = static_cast<std::size_t>(n.at * rate);
  const double nyquist = 0.45 * rate;
  const double b = 3.5e-4 * std::pow(f0 / 220.0, 0.5);   // inharmonicity
  const double sustain = 3.2 * std::pow(220.0 / f0, 0.55);  // seconds
  const double pan = std::clamp((n.midi - 60) / 24.0, -1.0, 1.0);
  const double theta = (pan + 1) * std::numbers::pi / 4;
  const double gl = std::cos(theta), gr = std::sin(theta);
  constexpr double kCents[3] = {-0.9, 0.15, 1.1};

  std::vector<double> amp, freq, tau1, tau2, phase;
  for (int k = 1; k <= 24; ++k) {
    const double fk = k * f0 * std::sqrt(1 + b * k * k);
    if (fk >= nyquist) {
      break;
    }
    const double place =
        std::abs(std::sin(std::numbers::pi * k / 7.0)) + 0.04;
    // Softer strikes have fewer highs.
    const double tilt = std::pow(k, -(1.15 - 0.45 * n.velocity));
    const double a = place * tilt;
    // The strings of a note struck together: one phase, a hair apart.
    const double ph = 2 * std::numbers::pi * rng.next();
    for (int s = 0; s < 3; ++s) {
      amp.push_back(a / 3);
      freq.push_back(fk * std::pow(2.0, kCents[s] / 1200.0));
      tau1.push_back(0.22 * sustain / (1 + 0.35 * (k - 1)));
      tau2.push_back(sustain / (1 + 0.12 * (k - 1)));
      phase.push_back(ph + 0.15 * s);
    }
  }
  const double gain = 0.22 * n.velocity;
  // The hammer's knock: a short burst of noise, low-passed.
  double lp = 0;
  const double lpk = 1 - std::exp(-2 * std::numbers::pi * 1800.0 / rate);
  for (std::size_t i = start; i < frames; ++i) {
    const double t = double(i - start) / rate;
    double v = 0;
    for (std::size_t p = 0; p < amp.size(); ++p) {
      const double env =
          0.62 * std::exp(-t / tau1[p]) + 0.38 * std::exp(-t / tau2[p]);
      v += amp[p] * env *
           std::sin(2 * std::numbers::pi * freq[p] * t + phase[p]);
    }
    const double attack = 1 - std::exp(-t / 0.0018);
    v *= attack;
    if (t < 0.03) {
      lp += lpk * ((rng.next() * 2 - 1) - lp);
      v += 0.35 * lp * std::exp(-t / 0.005);
    }
    if (n.off >= 0 && n.at + t > n.off) {
      v *= std::exp(-(n.at + t - n.off) / 0.09);
    }
    v *= gain;
    out[2 * i] += static_cast<float>(v * gl);
    out[2 * i + 1] += static_cast<float>(v * gr);
  }
}

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

}  // namespace

std::vector<float>
sonic_logo(int rate)
{
  const std::size_t frames =
      static_cast<std::size_t>(std::lround(kMarkSeconds * rate));
  std::vector<float> out(frames * 2, 0.0f);
  constexpr double kResolve = 0.350;
  // A3 E4 G4, then A3 D4 F#4: the A struck again with the rest.
  const Note notes[] = {
      {57, 0.0, 0.78, kResolve},     {64, 0.0, 0.72, kResolve},
      {67, 0.0, 0.74, kResolve},     {57, kResolve, 0.84, -1},
      {62, kResolve, 0.80, -1},      {66, kResolve, 0.82, -1},
  };
  Lcg rng;
  for (const Note& n : notes) {
    strike(n, rate, out, rng);
  }
  // Faded out over the last 0.6 s, an equal-power fall to nothing.
  const double fade_from = kMarkSeconds - 0.6;
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = double(i) / rate;
    if (t <= fade_from) {
      continue;
    }
    const double x = std::min(1.0, (t - fade_from) / (kMarkSeconds -
                                                      fade_from));
    const float g = static_cast<float>(std::cos(x * std::numbers::pi / 2));
    out[2 * i] *= g;
    out[2 * i + 1] *= g;
  }
  float peak = 0;
  for (float s : out) {
    peak = std::max(peak, std::abs(s));
  }
  if (peak > 0) {
    const float k = 0.708f / peak;  // -3 dBFS
    for (float& s : out) {
      s *= k;
    }
  }
  return out;
}

Status
write_sonic_logo(const fs::path& wav, int rate)
{
  const std::vector<float> s = sonic_logo(rate);
  FloatWav w;
  VALTZ_TRY(w.open(wav, 2, rate));
  VALTZ_TRY(w.append_interleaved(s.data(), s.size() / 2));
  return w.close();
}

fs::path
valtz_mark_helper()
{
  if (const char* env = std::getenv("VALTZ_MARK_HELPER"); env && *env) {
    return env;
  }
  std::vector<fs::path> c;
  if (fs::path dir = executable_dir(); !dir.empty()) {
    c.push_back(dir / "valtz-mark");
    c.push_back(dir / ".." / "Helpers" / "valtz-mark");
  }
#ifdef VALTZ_MARK_HELPER_BUILD_PATH
  c.push_back(VALTZ_MARK_HELPER_BUILD_PATH);
#endif
  for (const auto& p : c) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) {
      fs::path q = fs::canonical(p, ec);
      return ec ? p : q;
    }
  }
  return {};
}

Status
render_valtz_mark(const MarkRender& r)
{
  const fs::path helper = valtz_mark_helper();
  if (helper.empty()) {
    return make_error(Code::Unsupported,
                      "the mark's renderer (valtz-mark) is not installed "
                      "with this Valtz");
  }
  if (r.width < 16 || r.height < 16 || r.width > 8192 || r.height > 8192) {
    return make_error(Code::InvalidArgument, std::format(
        "the mark is drawn at 16 to 8192 pixels a side, not {} x {}",
        r.width, r.height));
  }
  std::error_code ec;
  fs::create_directories(r.out.parent_path(), ec);
  // A clip is drawn silent beside its file, then joined to its sound.
  const bool join = r.frames > 0 && !r.sound.empty();
  const fs::path drawn =
      join ? fs::path(r.out.string() + ".video.mov") : r.out;
  std::vector<std::string> args = {
      helper.string(),
      "--width",   std::to_string(r.width),
      "--height",  std::to_string(r.height),
      "--frames",  std::to_string(r.frames),
      "--fps-num", std::to_string(r.fps.num),
      "--fps-den", std::to_string(r.fps.den),
      "--out",     drawn.string(),
  };
  std::vector<char*> argv;
  for (auto& a : args) {
    argv.push_back(a.data());
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (const int e = posix_spawn(&pid, args[0].c_str(), nullptr, nullptr,
                                argv.data(), environ);
      e != 0) {
    return make_error(Code::Io, std::format(
        "cannot start the mark's renderer ({}): error {}", args[0], e));
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 ||
      !fs::is_regular_file(drawn, ec)) {
    fs::remove(drawn, ec);
    return make_error(Code::Internal, std::format(
        "the mark's renderer failed (status {})", status));
  }
  if (!join) {
    return ok_status();
  }
  const Status st = add_soundtrack(drawn, r.sound, r.out, 0, 0,
                                   SoundCodec::Pcm, software_stamp(""));
  fs::remove(drawn, ec);
  return st;
}

}  // namespace valtz::media
