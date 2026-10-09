#include "valtz/assist/transcript.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

namespace valtz::assist {

namespace {

// "00:03.2", "1:02:03.2": tenths of a second, minutes from the start
// (hours once there are any).
std::string
clock(double s, bool tenths = true)
{
  s = std::max(0.0, s);
  const auto whole = static_cast<long long>(std::floor(s));
  const auto h = whole / 3600, m = whole / 60 % 60;
  const double sec = s - static_cast<double>(whole - whole % 60);
  std::string t = tenths ? std::format("{:04.1f}", sec)
                         : std::format("{:02d}", static_cast<int>(sec));
  return h > 0 ? std::format("{}:{:02d}:{}", h, m, t)
               : std::format("{:02d}:{}", m, t);
}

double
seconds_of(const Json& j, const char* key)
{
  return jget<double>(j, key, 0.0) / 1e6;
}

}

std::optional<TranscriptLine>
transcript_line(const Json& beat)
{
  TranscriptLine l;
  l.text = jget<std::string>(beat, "text", "");
  // "language English<asr_text>Hello ...": the language, then the words.
  static constexpr std::string_view kMark = "<asr_text>";
  if (const auto m = l.text.find(kMark); m != std::string::npos) {
    std::string lead = l.text.substr(0, m);
    l.text.erase(0, m + kMark.size());
    if (lead.starts_with("language ")) {
      lead.erase(0, 9);
    }
    const auto a = lead.find_first_not_of(" \t");
    if (a != std::string::npos) {
      l.language = lead.substr(a, lead.find_last_not_of(" \t") - a + 1);
    }
  }
  // Its words, without the space the decoder leaves either side.
  const auto first = l.text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return std::nullopt;
  }
  l.text = l.text.substr(first, l.text.find_last_not_of(" \t\r\n") - first
                                    + 1);
  l.start = seconds_of(beat, "start_us");
  l.end = std::max(l.start, seconds_of(beat, "end_us"));
  return l;
}

std::vector<SoundEvent>
sound_events(const std::vector<Json>& windows, double threshold)
{
  std::map<std::string, std::vector<SoundEvent>> by_label;
  for (const auto& w : windows) {
    const double start = seconds_of(w, "timestamp_us");
    const double end = start + seconds_of(w, "duration_us");
    for (const auto& t : jget(w, "tags", Json::array())) {
      const auto label = jget<std::string>(t, "label", "");
      const double score = jget(t, "score", 0.0);
      if (label.empty() || score < threshold) {
        continue;
      }
      auto& spans = by_label[label];
      // Touching or overlapping the label's last span: the same event.
      if (!spans.empty() && start <= spans.back().end + 1e-6) {
        spans.back().end = std::max(spans.back().end, end);
        spans.back().score = std::max(spans.back().score, score);
      } else {
        spans.push_back({start, end, label, score});
      }
    }
  }
  std::vector<SoundEvent> out;
  for (auto& [_, spans] : by_label) {
    out.insert(out.end(), spans.begin(), spans.end());
  }
  std::ranges::sort(out, [](const SoundEvent& a, const SoundEvent& b) {
    return a.start != b.start ? a.start < b.start : a.score > b.score;
  });
  return out;
}

std::string
transcript_summary(std::string_view name, const Transcript& t)
{
  // The languages heard, in the order first heard.
  std::vector<std::string> langs;
  for (const auto& l : t.lines) {
    if (!l.language.empty() && l.language != "None" &&
        std::ranges::find(langs, l.language) == langs.end()) {
      langs.push_back(l.language);
    }
  }
  std::string heard;
  for (const auto& l : langs) {
    heard += (heard.empty() ? " · " : ", ") + l;
  }
  std::string s = std::format("# Transcript of {} ({}){}\n\n", name,
                              clock(t.seconds, false), heard);
  if (t.lines.empty()) {
    s += "(no speech)\n";
  }
  for (const auto& l : t.lines) {
    s += std::format("[{} - {}] {}\n", clock(l.start), clock(l.end),
                     l.text);
  }
  if (t.tagged) {
    s += "\n## Sound events\n";
    if (t.events.empty()) {
      s += "(none heard)\n";
    }
    for (const auto& e : t.events) {
      s += std::format("[{} - {}] {} ({}%)\n", clock(e.start, false),
                       clock(std::min(e.end, std::max(e.start, t.seconds)),
                             false),
                       e.label,
                       static_cast<int>(std::lround(e.score * 100)));
    }
  }
  return s;
}

Json
to_json(const Transcript& t)
{
  Json lines = Json::array();
  for (const auto& l : t.lines) {
    lines.push_back({{"start", l.start}, {"end", l.end}, {"text", l.text},
                     {"language", l.language}});
  }
  Json events = Json::array();
  for (const auto& e : t.events) {
    events.push_back({{"start", e.start}, {"end", e.end},
                      {"label", e.label}, {"score", e.score}});
  }
  return {{"seconds", t.seconds}, {"tagged", t.tagged},
          {"lines", std::move(lines)}, {"events", std::move(events)}};
}

}
