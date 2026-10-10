#ifndef VALTZ_ASSIST_VIDEO_SUMMARY_H
#define VALTZ_ASSIST_VIDEO_SUMMARY_H

// A clip SUMMARIZED (DESIGN §4i): what a vision-language model saw in it,
// scene by scene -- the plugin's valtz-video-summary cuts it where the
// picture changes and has each scene told in a few sentences -- and the
// whole clip in a paragraph written from them. What a summary asset holds
// is that as text a person reads and edits, beside it as data
// (AssetVersion::outputs "summary"), as a transcript is held.

#include "valtz/base/json.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::assist {

// One scene: seconds from the clip's start, its frames read, whether it
// ended where the picture changed (else it ran long, or the clip ended),
// and the model's words for it.
struct SummaryScene {
  double      start = 0;
  double      end = 0;
  int         frames = 0;
  bool        at_cut = false;
  std::string text;
};

struct VideoSummary {
  std::vector<SummaryScene> scenes;
  std::string               overall;   // "" for a clip of one scene
  double                    seconds = 0;  // the clip's length
  double                    every = 1;    // seconds between frames read
};

// The stage's beat for a scene ({kind: "scene", index, start, end, frames,
// at_cut, text}) as one; none for another kind, or a scene with no words.
std::optional<SummaryScene> summary_scene(const Json& beat);
// The overall beat's words ({kind: "overall", text}); "" for another.
std::string summary_overall(const Json& beat);

// The summary, in Markdown: a heading naming `name` and the clip's length,
// the whole in a paragraph, then each scene with its span. A scene's end
// is kept within the clip.
std::string summary_text(std::string_view name, const VideoSummary&);

Json to_json(const VideoSummary&);

// The language a model is asked to write in, named as it reads it, from
// the interface's language tag: "zh-Hans" Simplified Chinese, "zh-Hant"
// Traditional Chinese, "ja" Japanese, "ko" Korean, ... -- "English" for
// "en", for none, and for one not known.
std::string language_name(std::string_view tag);

// What the stage's model is told after a scene's frames and with the
// scenes' words (the stage fills {start} {end} {every} {language}
// {scenes}).
extern const char* const kSummaryScenePrompt;
extern const char* const kSummaryOverallPrompt;

}

#endif
