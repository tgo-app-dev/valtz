#include "valtz/assist/drama3.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iterator>
#include <map>
#include <optional>

namespace valtz::assist {

namespace {

std::string_view
trim(std::string_view s)
{
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

std::vector<std::string_view>
lines_of(std::string_view s)
{
  std::vector<std::string_view> out;
  std::size_t at = 0;
  while (at <= s.size()) {
    auto end = s.find('\n', at);
    if (end == std::string_view::npos) {
      end = s.size();
    }
    std::string_view l = s.substr(at, end - at);
    if (!l.empty() && l.back() == '\r') {
      l.remove_suffix(1);
    }
    out.push_back(l);
    at = end + 1;
  }
  return out;
}

// "Narrator: [calm] The ship ..." -> {"Narrator", "[calm] The ship ..."}:
// a speaker's name -- one to three words, no brackets, no sentence's
// punctuation -- then a colon (a full-width one too).
std::optional<std::pair<std::string, std::string_view>>
turn_of(std::string_view line)
{
  line = trim(line);
  std::size_t colon = line.find(':');
  std::size_t skip = 1;
  if (const auto wide = line.find("\xef\xbc\x9a");
      wide != std::string_view::npos &&
      (colon == std::string_view::npos || wide < colon)) {
    colon = wide;
    skip = 3;
  }
  if (colon == std::string_view::npos) {
    return std::nullopt;
  }
  const std::string_view name = trim(line.substr(0, colon));
  if (name.empty() || name.size() > 32) {
    return std::nullopt;
  }
  int words = 0;
  bool in_word = false;
  for (const char ch : name) {
    if (std::string_view("[](){}<>\".!?,;|").find(ch) !=
        std::string_view::npos) {
      return std::nullopt;
    }
    const bool space = std::isspace(static_cast<unsigned char>(ch));
    words += !space && !in_word;
    in_word = !space;
  }
  if (words > 3) {
    return std::nullopt;
  }
  return std::pair{std::string(name), trim(line.substr(colon + skip))};
}

// `s` with each Markdown link made its text: "[English](/guide/en)".
std::string
unlinked(std::string_view s)
{
  std::string out;
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] == '[') {
      const auto close = s.find("](", i);
      const auto end = close == std::string_view::npos
                           ? std::string_view::npos
                           : s.find(')', close + 2);
      if (end != std::string_view::npos &&
          s.substr(i + 1, close - i - 1).find('[') ==
              std::string_view::npos) {
        out += s.substr(i + 1, close - i - 1);
        i = end + 1;
        continue;
      }
    }
    out += s[i++];
  }
  return out;
}

// A component's attribute: title="Sheila".
std::string
attribute(std::string_view tag, std::string_view name)
{
  const std::string key = std::string(name) + "=\"";
  const auto at = tag.find(key);
  if (at == std::string_view::npos) {
    return {};
  }
  const auto from = at + key.size();
  const auto end = tag.find('"', from);
  return end == std::string_view::npos
             ? std::string()
             : std::string(tag.substr(from, end - from));
}

}

Drama3Script
drama3_script(std::string_view prompt, int voices)
{
  Drama3Script out;
  const std::string_view text = trim(prompt);
  out.text = std::string(text);
  if (text.find("<|speaker:") != std::string_view::npos) {
    return out;
  }
  // Each turn: its name's place, and its lines.
  struct Turn {
    std::size_t speaker = 0;
    std::string words;
  };
  std::vector<Turn> turns;
  std::map<std::string, int> times;
  for (const std::string_view line : lines_of(text)) {
    if (trim(line).empty()) {
      continue;
    }
    if (auto t = turn_of(line)) {
      auto at = std::ranges::find(out.names, t->first);
      if (at == out.names.end()) {
        out.names.push_back(t->first);
        at = out.names.end() - 1;
      }
      ++times[t->first];
      turns.push_back({static_cast<std::size_t>(at - out.names.begin()),
                       std::string(t->second)});
      continue;
    }
    if (turns.empty()) {
      out.names.clear();
      return out;   // it does not open on a name: no scene
    }
    std::string& w = turns.back().words;
    w += (w.empty() ? "" : "\n") + std::string(trim(line));
  }
  const auto n = static_cast<int>(out.names.size());
  const bool twice = std::ranges::any_of(times, [](const auto& kv) {
    return kv.second > 1;
  });
  const bool cast = n >= 2 && voices >= n;
  if (n == 0 || n > kDrama3Speakers || (!twice && !cast)) {
    out.names.clear();
    return out;
  }
  out.text.clear();
  for (const auto& t : turns) {
    out.text += std::format("{}<|speaker:{}|>{}",
                            out.text.empty() ? "" : "\n", t.speaker,
                            t.words);
  }
  return out;
}

Drama3Voice
drama3_voice(const Transcript& t, double keep)
{
  std::vector<TranscriptLine> lines;
  for (const auto& l : t.lines) {
    if (!trim(l.text).empty() && l.end > l.start) {
      lines.push_back(l);
    }
  }
  std::ranges::sort(lines, {}, &TranscriptLine::start);
  Drama3Voice out;
  double end = 0;
  std::size_t taken = 0;
  for (; taken < lines.size(); ++taken) {
    const auto& l = lines[taken];
    if (taken > 0 && l.end > keep + 0.05) {
      break;
    }
    out.text += (out.text.empty() ? "" : " ") + std::string(trim(l.text));
    end = l.end;
  }
  if (out.text.empty()) {
    return out;
  }
  // A little of the silence after it, up to the next line.
  double cut = end + 0.3;
  if (taken < lines.size()) {
    cut = std::min(cut, std::max(end, lines[taken].start - 0.02));
  }
  if (t.seconds > 0) {
    cut = std::min(cut, t.seconds);
  }
  out.seconds = std::max(cut, end);
  return out;
}

Drama3Voice
drama3_voice(const Json& transcript, double keep)
{
  Transcript t;
  t.seconds = jget(transcript, "seconds", 0.0);
  for (const auto& l : jget(transcript, "lines", Json::array())) {
    TranscriptLine tl;
    tl.start = jget(l, "start", 0.0);
    tl.end = jget(l, "end", 0.0);
    tl.text = jget<std::string>(l, "text", "");
    t.lines.push_back(std::move(tl));
  }
  return drama3_voice(t, keep);
}

std::string
drama3_guide(std::string_view mdx)
{
  // Its front matter, between the first two "---" lines.
  if (mdx.starts_with("---")) {
    const auto end = mdx.find("\n---", 3);
    if (end != std::string_view::npos) {
      const auto next = mdx.find('\n', end + 4);
      mdx = next == std::string_view::npos ? std::string_view()
                                           : mdx.substr(next + 1);
    }
  }
  static constexpr std::string_view kLeftOut[] = {
      "What's new", "Voice cast", "How to use", "Support"};
  std::string out;
  bool skip_section = false;
  bool in_code = false;
  bool skip_code = false;
  std::string component;   // a component's lines, until it closes
  std::string note;        // a warning's words
  bool in_note = false;
  auto emit = [&](std::string_view l) {
    out += unlinked(l);
    out += '\n';
  };
  for (const std::string_view line : lines_of(mdx)) {
    const std::string_view l = trim(line);
    if (in_code) {
      if (l.starts_with("```")) {
        in_code = false;
        if (!skip_code && !skip_section) {
          emit("```");
        }
        continue;
      }
      if (!skip_code && !skip_section) {
        out += std::string(line) + '\n';
      }
      continue;
    }
    if (l.starts_with("## ")) {
      const std::string_view title = trim(l.substr(3));
      skip_section = std::ranges::find(kLeftOut, title) !=
                     std::end(kLeftOut);
    }
    if (skip_section) {
      continue;
    }
    if (l.starts_with("```")) {
      // A shell example is about the web service; a script is kept.
      const std::string_view lang = trim(l.substr(3));
      skip_code = !lang.starts_with("text") && !lang.empty();
      in_code = true;
      if (!skip_code) {
        emit("```");
      }
      continue;
    }
    if (l.starts_with("import ") || l.starts_with("export ")) {
      continue;
    }
    if (!component.empty() || l.starts_with("<AudioClip")) {
      component += std::string(l) + ' ';
      if (l.ends_with("/>")) {
        const std::string words = attribute(component, "text");
        if (!words.empty()) {
          const std::string who = attribute(component, "title");
          emit(who.empty() ? std::format("Example: {}", words)
                           : std::format("Example ({}): {}", who, words));
        }
        component.clear();
      }
      continue;
    }
    if (l == "<Warning>") {
      in_note = true;
      note.clear();
      continue;
    }
    if (l == "</Warning>") {
      in_note = false;
      emit(std::format("Note: {}", trim(note)));
      continue;
    }
    if (in_note) {
      note += std::string(l) + ' ';
      continue;
    }
    if (l.starts_with("<") && l.ends_with("/>")) {
      continue;   // a component of the page's own (<Support />)
    }
    emit(line);
  }
  // Blank lines run together.
  std::string tidy;
  int blanks = 0;
  for (const std::string_view l : lines_of(out)) {
    blanks = trim(l).empty() ? blanks + 1 : 0;
    if (blanks <= 1) {
      tidy += std::string(l) + '\n';
    }
  }
  return std::string(trim(tidy)) + '\n';
}

}
