// PROGRAMS DRAWN (DESIGN §4t): JavaScript -- a canvas's 2D or WebGL,
// SVG, HTML; three.js and Babylon.js -- run in a WKWebView nobody sees,
// sandboxed (no network, no file), stepped a frame at a time on a clock
// of the render's own, each frame taken as WebKit draws the page: a
// picture or a movie.
//
// It runs in a PROCESS OF ITS OWN, tools/valtz-procedural, started for
// each render: WebKit wants a main thread, which a job's is not, and a
// page that never returns -- an endless loop -- keeps its web process
// busy until the process that made it ends. Killed, it ends with it.
// The core starts it, reads what it says, and kills it when the render
// is stopped or a frame runs past its time.

#ifndef VALTZ_MEDIA_PROCEDURAL_H
#define VALTZ_MEDIA_PROCEDURAL_H

#include "valtz/base/rational.h"
#include "valtz/base/result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace valtz::media {

// The largest side a program is drawn at.
inline constexpr int kProgramMaxSide = 8192;

// An ASSET of the project a program may read: served to its page under
// its own scheme by name, read-only -- the file as it is, a clip a frame
// at a time too -- and, small, written into the page as a data: URL.
struct ProgramAsset {
  std::string           id;
  std::string           name;
  std::string           kind;   // image | video | audio | text
  std::filesystem::path file;
  std::string           type;   // its MIME type
  int                   width = 0;
  int                   height = 0;
  double                seconds = 0;
};

// The most bytes an asset is written into the page as a data: URL:
// read at once, with no wait (`valtz.asset(name).data`).
inline constexpr std::uint64_t kProgramInlineBytes = 256 * 1024;

struct ProgramRender {
  std::filesystem::path program;  // its .js
  int                   width = 1024;
  int                   height = 1024;
  // Frames of a movie; 0 a still (a PNG, its transparency kept).
  int                   frames = 0;
  Rational              fps{24, 1};
  // Math.random's seed; what Date says at time 0 (ms since 1970).
  std::uint64_t         seed = 0;
  double                epoch_ms = 0;
  // .png for a still; a movie's .mov (ProRes) or .mp4 (HEVC), as `codec`
  // (an intermediate format: media/movie.h).
  std::filesystem::path out;
  std::string           codec = "prores422hq";
  // Seconds the program has to load, and each frame to draw.
  double                load_timeout = 60;
  double                frame_timeout = 30;
  // What it may read (ProgramAsset); nothing else.
  std::vector<ProgramAsset> assets;
  // A clip's SOUNDTRACK, made by its program on the page's offline audio
  // (valtz.audio): its sample rate, and where its WAV goes ("": none
  // asked).
  int                   sample_rate = 48000;
  std::filesystem::path sound;
};

// A clip's frame as the stage previews it: small (480 at most on its
// longer side), on black, planar 8-bit RGB [3, h, w].
struct ProgramPreview {
  int                       frame = 0;
  int                       width = 0;
  int                       height = 0;
  std::vector<std::uint8_t> rgb;
};

// A line the page said: its console, its errors, what it was refused.
struct ProgramLog {
  std::string level;   // error | warn | log
  std::string text;
  std::string source;  // "program.js", a library's URL; "" not known
  int         line = 0;
  int         column = 0;
};

struct ProgramRun {
  std::vector<ProgramLog> log;
  int                     errors = 0;
  // Loaded without an error thrown as the program ran.
  bool                    loaded = false;
  // Nothing drawn: the page clear at its first and its last frame --
  // and why, as the page was then: "its canvas was never added to the
  // page" (empty_why).
  bool                    empty = false;
  std::string             empty_why;
  double                  seconds = 0;
  // Its soundtrack's WAV (ProgramRender::sound), when it made one --
  // silent, none.
  std::filesystem::path   sound;
};

// The program drawn and written to `r.out`. `progress` (frames done, of)
// as each frame is taken; `stop` set: the process killed, Cancelled.
// Failed -- the program's process gone, a frame past its time -- the
// error says why and `log` (when given) holds what the page said before.
// Blocking.
// `preview`, a clip's: a frame every quarter second as it is drawn.
Result<ProgramRun> render_program(
    const ProgramRender& r,
    const std::function<void(int done, int of)>& progress = {},
    const std::atomic<bool>* stop = nullptr,
    std::vector<ProgramLog>* log = nullptr,
    const std::function<void(ProgramPreview)>& preview = {});

// Where the renderer is -- $VALTZ_PROCEDURAL_HELPER, beside the running
// executable (valtzctl in the app's Contents/Helpers), the app's
// Contents/Helpers, the build tree -- and the libraries it serves the
// page: $VALTZ_PROCEDURAL_LIBS, the bundle's Resources/procedural, the
// source tree's 3rd-party. Empty: not found.
std::filesystem::path procedural_helper();
std::filesystem::path procedural_libs();

// A file as a web page reads it as it is -- its MIME type: a picture
// WebKit decodes, a clip or a sound it plays, a text; "" another (a
// camera's RAW, a PSD: drawn into a PNG first).
std::string program_type(const std::filesystem::path&);
// A file as an asset of a program drawn without a project (valtzctl
// program --asset): named by its stem, its kind by its type, its size.
Result<ProgramAsset> program_asset_of(const std::filesystem::path&);

// A line of the log as a model or a person reads it: "error at
// program.js:12:5: ReferenceError: ...".
std::string to_text(const ProgramLog&);

}  // namespace valtz::media

#endif
