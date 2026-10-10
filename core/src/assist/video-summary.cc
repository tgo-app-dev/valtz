#include "valtz/assist/video-summary.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace valtz::assist {

namespace {

// "0:12", "1:02:03": whole seconds, minutes from the start (hours once
// there are any).
std::string
clock(double s)
{
  const auto whole =
      static_cast<long long>(std::floor(std::max(0.0, s)));
  const auto h = whole / 3600, m = whole / 60 % 60, sec = whole % 60;
  return h > 0 ? std::format("{}:{:02d}:{:02d}", h, m, sec)
               : std::format("{}:{:02d}", m, sec);
}

}

// Plain words in the clip's order; the reader's language. The scene's
// times are the clip's, so the model's "at the start" means the scene's.
const char* const kSummaryScenePrompt =
    "These are frames from {start} to {end} of a video, one every {every} "
    "seconds. Describe what happens in this part: the place, who or what "
    "is seen, what they do, any words on screen, and how the camera "
    "moves. Write two to four sentences of plain prose in {language}. Do "
    "not mention frames or timestamps, and do not guess at what is not "
    "shown.";
const char* const kSummaryOverallPrompt =
    "A video was described part by part, in order:\n\n{scenes}\n"
    "Write a summary of the whole video in {language}: what it shows and "
    "how it unfolds, in one paragraph of three to six sentences of plain "
    "prose. Do not list the parts or their times.";

std::optional<SummaryScene>
summary_scene(const Json& beat)
{
  if (jget<std::string>(beat, "kind", "") != "scene") {
    return std::nullopt;
  }
  SummaryScene s;
  s.start = jget(beat, "start", 0.0);
  s.end = jget(beat, "end", s.start);
  s.frames = jget(beat, "frames", 0);
  s.at_cut = jget(beat, "at_cut", false);
  s.text = jget<std::string>(beat, "text", "");
  if (s.text.empty()) {
    return std::nullopt;
  }
  return s;
}

std::string
summary_overall(const Json& beat)
{
  return jget<std::string>(beat, "kind", "") == "overall"
             ? jget<std::string>(beat, "text", "")
             : std::string();
}

std::string
summary_text(std::string_view name, const VideoSummary& v)
{
  std::string s = std::format("# Summary of {} ({})\n\n", name,
                              clock(v.seconds));
  if (!v.overall.empty()) {
    s += v.overall + "\n\n";
  }
  if (v.scenes.empty()) {
    s += "(nothing seen)\n";
    return s;
  }
  s += "## Scenes\n";
  for (const auto& sc : v.scenes) {
    const double end = v.seconds > 0 ? std::min(sc.end, v.seconds) : sc.end;
    s += std::format("\n**{} - {}** {}\n", clock(sc.start),
                     clock(std::max(sc.start, end)), sc.text);
  }
  return s;
}

Json
to_json(const VideoSummary& v)
{
  Json scenes = Json::array();
  for (const auto& s : v.scenes) {
    scenes.push_back({{"start", s.start},
                      {"end", v.seconds > 0 ? std::min(s.end, v.seconds)
                                            : s.end},
                      {"frames", s.frames},
                      {"at_cut", s.at_cut},
                      {"text", s.text}});
  }
  return {{"seconds", v.seconds},
          {"every", v.every},
          {"overall", v.overall},
          {"scenes", std::move(scenes)}};
}

std::string
language_name(std::string_view tag)
{
  static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
    {"zh-Hans", "Simplified Chinese"}, {"zh-Hant", "Traditional Chinese"},
    {"zh-CN", "Simplified Chinese"},   {"zh-TW", "Traditional Chinese"},
    {"zh-HK", "Traditional Chinese"},  {"zh", "Simplified Chinese"},
    {"ja", "Japanese"},  {"ko", "Korean"},   {"fr", "French"},
    {"de", "German"},    {"es", "Spanish"},  {"it", "Italian"},
    {"pt", "Portuguese"}, {"ru", "Russian"}, {"nl", "Dutch"},
  };
  // The tag itself, or with a region or a script after it ("zh-Hant-TW",
  // "fr-CA"): the most particular entry first.
  for (const auto& [t, n] : kNames) {
    if (tag == t || (tag.size() > t.size() && tag.starts_with(t) &&
                     (tag[t.size()] == '-' || tag[t.size()] == '_'))) {
      return std::string(n);
    }
  }
  return "English";
}

}
