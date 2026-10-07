#include "valtz/assist/assistant.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <format>
#include <map>

namespace valtz::assist {

namespace {

constexpr std::array<const char*, 7> kIntentNames = {
  "generate-image", "edit-image", "generate-video", "animate-image",
  "upscale", "describe", "unknown",
};

std::string
lower(std::string_view s)
{
  std::string out(s);
  for (auto& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

bool
has_word(const std::string& hay, std::initializer_list<const char*> words)
{
  for (const char* w : words) {
    std::string_view needle(w);
    for (std::size_t pos = hay.find(needle); pos != std::string::npos;
         pos = hay.find(needle, pos + 1)) {
      bool left = pos == 0 ||
                  !std::isalnum(static_cast<unsigned char>(hay[pos - 1]));
      std::size_t end = pos + needle.size();
      bool right = end >= hay.size() ||
                   !std::isalnum(static_cast<unsigned char>(hay[end]));
      if (left && right) {
        return true;
      }
    }
  }
  return false;
}

// A song's length for the writer: what the user set, or room to choose.
std::string
song_length(double seconds)
{
  if (seconds <= 0) {
    return "Song length: not given (about 2 to 3 minutes).";
  }
  const int s = static_cast<int>(seconds + 0.5);
  return std::format("Song length: about {}:{:02}.", s / 60, s % 60);
}

// Speech's length, as its writer is told it: words to fit, at about 2.5
// a second.
std::string
speech_length(double seconds)
{
  if (seconds <= 0) {
    return "Length: not given -- as long as the request needs.";
  }
  const int s = static_cast<int>(seconds + 0.5);
  return std::format("Length: about {} seconds of speech -- about {} "
                     "words (or {} Chinese characters).",
                     s, static_cast<int>(s * 2.5), s * 4);
}

// Han characters (CJK Unified Ideographs, U+4E00..U+9FFF) in `s`.
bool
has_han(std::string_view s)
{
  for (std::size_t i = 0; i + 2 < s.size(); ++i) {
    const auto b0 = static_cast<unsigned char>(s[i]);
    const auto b1 = static_cast<unsigned char>(s[i + 1]);
    // E4 B8 80 .. E9 BF BF
    if (b0 >= 0xE4 && b0 <= 0xE9 && (b0 > 0xE4 || b1 >= 0xB8)) {
      return true;
    }
  }
  return false;
}

std::string
describe_attachments(const std::vector<Attachment>& atts)
{
  if (atts.empty()) {
    return "none";
  }
  std::string s;
  for (const auto& a : atts) {
    s += std::format("\n- {} \"{}\"", a.kind, a.name);
    if (!a.caption.empty()) {
      s += std::format(": {}", a.caption);
    }
  }
  return s;
}

}

const char*
to_str(Intent i)
{
  auto k = static_cast<std::size_t>(i);
  return k < kIntentNames.size() ? kIntentNames[k] : "unknown";
}

Intent
intent_from_str(std::string_view s)
{
  for (std::size_t i = 0; i < kIntentNames.size(); ++i) {
    if (s == kIntentNames[i]) {
      return static_cast<Intent>(i);
    }
  }
  return Intent::Unknown;
}

Result<Json>
extract_json_object(std::string_view s)
{
  for (std::size_t start = s.find('{'); start != std::string_view::npos;
       start = s.find('{', start + 1)) {
    int depth = 0;
    bool in_str = false;
    bool esc = false;
    for (std::size_t i = start; i < s.size(); ++i) {
      char c = s[i];
      if (in_str) {
        if (esc) {
          esc = false;
        } else if (c == '\\') {
          esc = true;
        } else if (c == '"') {
          in_str = false;
        }
        continue;
      }
      if (c == '"') {
        in_str = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}' && --depth == 0) {
        Json j = Json::parse(s.substr(start, i - start + 1), nullptr,
                             /*allow_exceptions=*/false);
        if (!j.is_discarded() && j.is_object()) {
          return j;
        }
        break;
      }
    }
  }
  return make_error(Code::Corrupt, "reply contained no JSON object");
}

std::string
tag_pictures(std::string_view prompt, const std::vector<InlinePicture>& at,
             std::string_view tag_format)
{
  std::string out;
  out.reserve(prompt.size());
  std::size_t k = 0;
  bool words = false;  // anything but space written yet
  for (std::size_t i = 0; i < prompt.size();) {
    if (prompt.substr(i).starts_with(kInlinePicture)) {
      i += kInlinePicture.size();
      const InlinePicture p = k < at.size() ? at[k] : InlinePicture{};
      ++k;
      if (tag_format.empty() || p.number <= 0 || (p.base && !words)) {
        continue;
      }
      std::string tag(tag_format);
      if (auto n = tag.find("{n}"); n != std::string::npos) {
        tag.replace(n, 3, std::to_string(p.number));
      }
      out += tag;
      words = true;
      continue;
    }
    const char c = prompt[i++];
    if (c == ' ' && (out.empty() || out.back() == ' ')) {
      continue;  // a run of spaces, or one at the start
    }
    if (!std::isspace(static_cast<unsigned char>(c))) {
      words = true;
    }
    out += c;
  }
  while (!out.empty() &&
         std::isspace(static_cast<unsigned char>(out.back()))) {
    out.pop_back();
  }
  std::size_t lead = 0;
  while (lead < out.size() &&
         std::isspace(static_cast<unsigned char>(out[lead]))) {
    ++lead;
  }
  return out.substr(lead);
}

namespace {

constexpr std::string_view kTagHead = "<valtz_ref_";

const char*
kind_name(RefKind k)
{
  switch (k) {
  case RefKind::Image: return "img";
  case RefKind::Video: return "vid";
  case RefKind::Audio: return "aud";
  default:             return "";
  }
}

// The tag starting at `at` ("<valtz_ref_img_12>"), if one does.
bool
read_tag(std::string_view s, std::size_t at, RefTag* t)
{
  if (!s.substr(at).starts_with(kTagHead)) {
    return false;
  }
  std::size_t i = at + kTagHead.size();
  RefKind kind = RefKind::Other;
  for (RefKind k : {RefKind::Image, RefKind::Video, RefKind::Audio}) {
    const std::string_view n = kind_name(k);
    if (s.substr(i).starts_with(n) && s.size() > i + n.size() &&
        s[i + n.size()] == '_') {
      kind = k;
      i += n.size() + 1;
      break;
    }
  }
  if (kind == RefKind::Other) {
    return false;
  }
  const std::size_t digits = i;
  int index = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9' && i - digits < 6) {
    index = index * 10 + (s[i] - '0');
    ++i;
  }
  if (i == digits || i >= s.size() || s[i] != '>') {
    return false;
  }
  *t = {kind, index, at, i + 1 - at};
  return true;
}

// The row index of the `index`-th medium of `kind`; -1 for none.
int
row_index(const std::vector<RefKind>& row, RefKind kind, int index)
{
  int seen = 0;
  for (std::size_t i = 0; i < row.size(); ++i) {
    if (row[i] == kind && seen++ == index) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

// Spaces collapsed and the ends trimmed, as tag_pictures leaves them.
std::string
tidy(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == ' ' && (out.empty() || out.back() == ' ')) {
      continue;
    }
    out += c;
  }
  while (!out.empty() &&
         std::isspace(static_cast<unsigned char>(out.back()))) {
    out.pop_back();
  }
  std::size_t lead = 0;
  while (lead < out.size() &&
         std::isspace(static_cast<unsigned char>(out[lead]))) {
    ++lead;
  }
  return out.substr(lead);
}

}

std::string
ref_tag(RefKind k, int index)
{
  return std::format("{}{}_{}>", kTagHead, kind_name(k), index);
}

std::vector<RefTag>
find_ref_tags(std::string_view text)
{
  std::vector<RefTag> out;
  for (std::size_t at = text.find(kTagHead); at != std::string_view::npos;
       at = text.find(kTagHead, at + 1)) {
    RefTag t;
    if (read_tag(text, at, &t)) {
      out.push_back(t);
      at += t.len - 1;
    }
  }
  return out;
}

std::string
positional_prompt(std::string_view prompt, const std::vector<int>& mentions,
                  const std::vector<RefKind>& row)
{
  std::string out;
  std::size_t k = 0;
  for (std::size_t i = 0; i < prompt.size();) {
    if (prompt.substr(i).starts_with(kInlinePicture)) {
      i += kInlinePicture.size();
      const int r = k < mentions.size() ? mentions[k] : -1;
      ++k;
      if (r < 0 || static_cast<std::size_t>(r) >= row.size() ||
          row[static_cast<std::size_t>(r)] == RefKind::Other) {
        continue;
      }
      // Its place within its kind.
      const RefKind kind = row[static_cast<std::size_t>(r)];
      int index = 0;
      for (int j = 0; j < r; ++j) {
        index += row[static_cast<std::size_t>(j)] == kind ? 1 : 0;
      }
      out += ref_tag(kind, index);
      continue;
    }
    out += prompt[i++];
  }
  return tidy(out);
}

std::vector<std::string>
unbound_ref_tags(std::string_view text, const std::vector<RefKind>& row)
{
  std::vector<std::string> out;
  for (const auto& t : find_ref_tags(text)) {
    if (row_index(row, t.kind, t.index) < 0) {
      out.emplace_back(text.substr(t.pos, t.len));
    }
  }
  return out;
}

std::string
mention_form(std::string_view text, const std::vector<RefKind>& row,
             const std::vector<int>& mentions, std::vector<int>* out)
{
  std::string s;
  out->clear();
  std::size_t k = 0;
  for (std::size_t i = 0; i < text.size();) {
    if (text.substr(i).starts_with(kInlinePicture)) {
      s += kInlinePicture;
      out->push_back(k < mentions.size() ? mentions[k] : -1);
      ++k;
      i += kInlinePicture.size();
      continue;
    }
    RefTag t;
    if (read_tag(text, i, &t)) {
      s += kInlinePicture;
      out->push_back(row_index(row, t.kind, t.index));
      i += t.len;
      continue;
    }
    s += text[i++];
  }
  return s;
}

namespace {

// An outline's text for `language`: as text_in picks a language, its
// value a string or its lines.
std::string
outline_text(const Json& v, std::string_view language)
{
  const auto text = [](const Json& t) -> std::string {
    if (t.is_string()) {
      return t.get<std::string>();
    }
    std::string out;
    if (t.is_array()) {
      for (const auto& line : t) {
        if (line.is_string()) {
          out += line.get<std::string>();
          out += '\n';
        }
      }
    }
    return out;
  };
  if (!v.is_object()) {
    return text(v);
  }
  const std::string_view alone = language.substr(0, language.find('-'));
  for (const std::string_view tag : {language, alone,
                                     std::string_view("en")}) {
    if (!tag.empty()) {
      if (auto it = v.find(std::string(tag)); it != v.end()) {
        return text(*it);
      }
    }
  }
  return v.empty() ? std::string() : text(v.begin().value());
}

void
replace_all(std::string& s, std::string_view from, std::string_view to)
{
  for (auto at = s.find(from); at != std::string::npos;
       at = s.find(from, at + to.size())) {
    s.replace(at, from.size(), to);
  }
}

}

std::string
prompt_outline(const Json& outline, std::string_view language,
               const std::vector<RefKind>& row)
{
  const std::string text = outline_text(outline, language);
  if (text.empty()) {
    return {};
  }
  int count[3] = {0, 0, 0};
  for (RefKind k : row) {
    if (k != RefKind::Other) {
      ++count[static_cast<int>(k)];
    }
  }
  struct Marker {
    std::string_view name;  // "{image}"
    std::string_view cond;  // "{if image}"
    std::string_view each;  // "{each image}"
    RefKind          kind;
  };
  static constexpr Marker kMarkers[] = {
    {"{image}", "{if image}", "{each image}", RefKind::Image},
    {"{video}", "{if video}", "{each video}", RefKind::Video},
    {"{audio}", "{if audio}", "{each audio}", RefKind::Audio},
  };
  std::vector<std::string> out;
  int subject = 0;
  const auto emit = [&](std::string line) {
    for (auto at = line.find("{subject}"); at != std::string::npos;
         at = line.find("{subject}", at)) {
      const std::string n = std::to_string(++subject);
      line.replace(at, 9, n);
      at += n.size();
    }
    out.push_back(std::move(line));
  };
  std::size_t from = 0;
  while (from <= text.size()) {
    std::size_t end = text.find('\n', from);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string line = text.substr(from, end - from);
    from = end + 1;
    if (line.empty()) {
      subject = 0;  // a new section
      out.emplace_back();
      continue;
    }
    // Kept only when the row holds that kind (none: nothing at all).
    bool keep = true;
    if (line.starts_with("{if none}")) {
      keep = row.empty() ||
             std::all_of(row.begin(), row.end(),
                         [](RefKind k) { return k == RefKind::Other; });
      line.erase(0, 9);
    }
    for (const Marker& m : kMarkers) {
      if (line.starts_with(m.cond)) {
        keep = count[static_cast<int>(m.kind)] > 0;
        line.erase(0, m.cond.size());
      }
    }
    if (!keep) {
      continue;
    }
    // Written once per medium of its kind, named by its tag.
    const Marker* rep = nullptr;
    for (const Marker& m : kMarkers) {
      if (line.starts_with(m.each)) {
        line.erase(0, m.each.size());
        rep = &m;
        break;
      }
    }
    for (const Marker& m : kMarkers) {
      if (!rep && line.find(m.name) != std::string::npos) {
        rep = &m;
      }
    }
    if (!rep) {
      emit(std::move(line));
      continue;
    }
    for (int i = 0; i < count[static_cast<int>(rep->kind)]; ++i) {
      std::string l = line;
      replace_all(l, rep->name, ref_tag(rep->kind, i));
      emit(std::move(l));
    }
  }
  // Sections left with nothing between them close up; no blank lines at
  // either end.
  std::string joined;
  bool blank = true;
  for (const auto& l : out) {
    if (l.empty()) {
      blank = true;
      continue;
    }
    if (blank && !joined.empty()) {
      joined += '\n';
    }
    joined += l;
    joined += '\n';
    blank = false;
  }
  while (!joined.empty() && joined.back() == '\n') {
    joined.pop_back();
  }
  return joined;
}

std::string
prompt_words(std::string_view prompt)
{
  std::vector<int> ignore;
  std::string s = tag_pictures(mention_form(prompt, {}, {}, &ignore), {},
                               "");
  // A medium dropped before punctuation leaves no space before it
  // ("from , both" -> "from, both").
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == ' ' && i + 1 < s.size() &&
        std::string_view(",.;:!?").find(s[i + 1]) != std::string_view::npos) {
      continue;
    }
    out += s[i];
  }
  return out;
}

std::string
without_markdown(std::string_view s)
{
  auto word = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) ||
           static_cast<unsigned char>(c) >= 0x80;
  };
  std::string out;
  out.reserve(s.size());
  bool line_start = true;
  for (std::size_t i = 0; i < s.size();) {
    const std::string_view rest = s.substr(i);
    // A heading's #s, and the space after them.
    if (line_start && rest.front() == '#') {
      while (i < s.size() && s[i] == '#') ++i;
      while (i < s.size() && s[i] == ' ') ++i;
      line_start = false;
      continue;
    }
    if (rest.starts_with("<u>") || rest.starts_with("</u>")) {
      i += rest.starts_with("<u>") ? 3 : 4;
      continue;
    }
    const char c = s[i];
    if (c == '*' || c == '`' || (c == '~' && rest.starts_with("~~"))) {
      i += c == '~' ? 2 : 1;
      continue;
    }
    // `_` that opens or closes emphasis -- not one inside a word
    // ("snake_case").
    if (c == '_') {
      std::size_t j = i;
      while (j < s.size() && s[j] == '_') ++j;
      const bool before = i > 0 && word(s[i - 1]);
      const bool after = j < s.size() && word(s[j]);
      if (!(before && after)) {
        i = j;
        continue;
      }
    }
    out += c;
    line_start = c == '\n';
    ++i;
  }
  return out;
}

std::string
tag_inline(std::string_view prompt, const std::vector<std::string>& tags)
{
  std::string out;
  out.reserve(prompt.size());
  std::size_t k = 0;
  for (std::size_t i = 0; i < prompt.size();) {
    if (prompt.substr(i).starts_with(kInlinePicture)) {
      i += kInlinePicture.size();
      if (k < tags.size() && !tags[k].empty()) {
        out += tags[k];
      }
      ++k;
      continue;
    }
    const char c = prompt[i++];
    if (c == ' ' && (out.empty() || out.back() == ' ')) {
      continue;  // a run of spaces, or one at the start
    }
    out += c;
  }
  while (!out.empty() &&
         std::isspace(static_cast<unsigned char>(out.back()))) {
    out.pop_back();
  }
  std::size_t lead = 0;
  while (lead < out.size() &&
         std::isspace(static_cast<unsigned char>(out[lead]))) {
    ++lead;
  }
  return out.substr(lead);
}

namespace {

std::string_view
trimmed(std::string_view s)
{
  const auto ws = " \t\r\n\f\v";
  const auto a = s.find_first_not_of(ws);
  if (a == std::string_view::npos) {
    return {};
  }
  return s.substr(a, s.find_last_not_of(ws) - a + 1);
}

// "[Verse]", "[Chorus 2]": a header on a line of its own -- brackets
// round a short name, nothing else on the line. The name, or "" for a
// line that is not one.
std::string_view
section_name(std::string_view line)
{
  const auto t = trimmed(line);
  if (t.size() < 3 || t.size() > 48 || t.front() != '[' ||
      t.back() != ']') {
    return {};
  }
  const auto name = t.substr(1, t.size() - 2);
  if (name.find_first_of("[]") != std::string_view::npos ||
      trimmed(name).empty()) {
    return {};
  }
  return trimmed(name);
}

bool
names_style(std::string_view name)
{
  auto lower = [](std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return out;
  };
  const std::string n = lower(name);
  return n == "style" || n == "tags";
}

}

std::string
keep_song_lyrics(std::string_view suggestion, std::string_view request)
{
  const SongText asked = split_song(request);
  if (asked.sections == 0 || asked.lines == 0) {
    return std::string(suggestion);
  }
  const std::string style = split_song(suggestion).style;
  const std::string lyrics(trimmed(asked.lyrics));
  return style.empty() ? lyrics : style + "\n\n" + lyrics;
}

SongText
split_song(std::string_view prompt)
{
  SongText out;
  std::vector<std::string_view> style;
  std::string lyrics;
  bool in_lyrics = false;
  bool in_style_section = false;
  std::size_t at = 0;
  while (at <= prompt.size()) {
    auto end = prompt.find('\n', at);
    if (end == std::string_view::npos) {
      end = prompt.size();
    }
    std::string_view line = prompt.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    at = end + 1;
    if (const auto name = section_name(line); !name.empty()) {
      in_style_section = names_style(name);
      if (!in_style_section) {
        in_lyrics = true;
        ++out.sections;
        lyrics += std::string(trimmed(line));
        lyrics += '\n';
      }
      continue;
    }
    if (in_style_section || !in_lyrics) {
      if (!trimmed(line).empty()) {
        style.push_back(trimmed(line));
      }
      continue;
    }
    if (!trimmed(line).empty()) {
      ++out.lines;
    }
    lyrics += std::string(line);
    lyrics += '\n';
  }
  for (auto s : style) {
    // One list of tags: a line's own trailing comma is the joint.
    while (!s.empty() && (s.back() == ',' || s.back() == ' ')) {
      s.remove_suffix(1);
    }
    if (s.empty()) {
      continue;
    }
    if (!out.style.empty()) {
      out.style += ", ";
    }
    out.style += s;
  }
  out.lyrics = std::string(trimmed(lyrics));
  return out;
}

namespace {

// "Sound event" -> "sound_event": a speech field's name, any case, a
// space or "_" between its words; "" when the line is not one.
std::string
speech_field(std::string_view name)
{
  std::string k;
  for (char c : trimmed(name)) {
    k += c == ' ' ? '_' : static_cast<char>(std::tolower(
                              static_cast<unsigned char>(c)));
  }
  for (const char* f : {"instruction", "quality", "sound_event",
                        "ambient_sound", "language"}) {
    if (k == f) {
      return k;
    }
  }
  return {};
}

// All in parentheses, ASCII or full-width: an outline's hint, unfilled.
bool
hint_only(std::string_view v)
{
  v = trimmed(v);
  return (v.starts_with("(") && v.ends_with(")")) ||
         (v.starts_with("\xef\xbc\x88") && v.ends_with("\xef\xbc\x89"));
}

}

SpeechText
split_speech(std::string_view prompt)
{
  SpeechText out;
  std::string text;
  bool in_text = false;
  std::size_t at = 0;
  while (at <= prompt.size()) {
    auto end = prompt.find('\n', at);
    if (end == std::string_view::npos) {
      end = prompt.size();
    }
    std::string_view line = prompt.substr(at, end - at);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    at = end + 1;
    if (!in_text) {
      std::string_view l = trimmed(line);
      if (l.empty()) {
        continue;
      }
      if (l.starts_with("- ")) {
        l.remove_prefix(2);
      }
      // "Field: value" (a full-width colon too).
      std::size_t colon = l.find(':');
      std::size_t skip = 1;
      if (const auto wide = l.find("\xef\xbc\x9a");
          wide != std::string_view::npos &&
          (colon == std::string_view::npos || wide < colon)) {
        colon = wide;
        skip = 3;
      }
      if (colon != std::string_view::npos) {
        if (const auto f = speech_field(l.substr(0, colon)); !f.empty()) {
          std::string v(trimmed(l.substr(colon + skip)));
          if (hint_only(v) || v == "None" || v == "none") {
            v.clear();
          }
          (f == "instruction"     ? out.instruction
           : f == "quality"       ? out.quality
           : f == "sound_event"   ? out.sound_event
           : f == "ambient_sound" ? out.ambient_sound
                                  : out.language) = std::move(v);
          continue;
        }
      }
      in_text = true;
    }
    if (hint_only(line)) {
      continue;
    }
    text += std::string(line);
    text += '\n';
  }
  out.text = std::string(trimmed(text));
  return out;
}

std::string
speech_prompt(const SpeechText& s)
{
  std::string out;
  for (const auto& [name, v] :
       {std::pair{"Instruction", &s.instruction},
        std::pair{"Quality", &s.quality},
        std::pair{"Sound event", &s.sound_event},
        std::pair{"Ambient sound", &s.ambient_sound},
        std::pair{"Language", &s.language}}) {
    if (!trimmed(*v).empty()) {
      out += std::format("{}: {}\n", name, trimmed(*v));
    }
  }
  if (!out.empty() && !s.text.empty()) {
    out += '\n';
  }
  return out + std::string(trimmed(s.text));
}

std::string
speech_language(std::string_view text)
{
  // Each script's letters counted; the most common names the language
  // (kana beside Han is Japanese).
  std::map<std::string, int> n;
  bool latin = false, accented = false;
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    char32_t cp = c;
    int len = 1;
    if (c >= 0xf0 && i + 3 < text.size()) {
      cp = (c & 0x07) << 18 | (text[i + 1] & 0x3f) << 12 |
           (text[i + 2] & 0x3f) << 6 | (text[i + 3] & 0x3f);
      len = 4;
    } else if (c >= 0xe0 && i + 2 < text.size()) {
      cp = (c & 0x0f) << 12 | (text[i + 1] & 0x3f) << 6 |
           (text[i + 2] & 0x3f);
      len = 3;
    } else if (c >= 0xc0 && i + 1 < text.size()) {
      cp = (c & 0x1f) << 6 | (text[i + 1] & 0x3f);
      len = 2;
    }
    i += static_cast<std::size_t>(len);
    if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) {
      latin = true;
    } else if (cp >= 0xc0 && cp <= 0x24f) {
      accented = true;
    } else if ((cp >= 0x4e00 && cp <= 0x9fff) ||
               (cp >= 0x3400 && cp <= 0x4dbf)) {
      ++n["Chinese"];
    } else if (cp >= 0x3040 && cp <= 0x30ff) {
      n["Japanese"] += 4;  // kana: Japanese, whatever Han is beside it
    } else if (cp >= 0xac00 && cp <= 0xd7af) {
      ++n["Korean"];
    } else if (cp >= 0x400 && cp <= 0x4ff) {
      ++n["Russian"];
    } else if (cp >= 0x600 && cp <= 0x6ff) {
      ++n["Arabic"];
    } else if (cp >= 0x370 && cp <= 0x3ff) {
      ++n["Greek"];
    } else if (cp >= 0x590 && cp <= 0x5ff) {
      ++n["Hebrew"];
    } else if (cp >= 0xe00 && cp <= 0xe7f) {
      ++n["Thai"];
    } else if (cp >= 0x900 && cp <= 0x97f) {
      ++n["Hindi"];
    }
  }
  std::string best;
  int most = 0;
  for (const auto& [lang, k] : n) {
    if (k > most) {
      most = k;
      best = lang;
    }
  }
  if (!best.empty()) {
    return best;
  }
  return latin && !accented ? "English" : "";
}

namespace {

// "00:05.167": where a clip of `seconds` ends, in the guides' notation.
std::string
timecode(double seconds)
{
  const auto ms = static_cast<long long>(seconds * 1000 + 0.5);
  return std::format("{:02}:{:02}.{:03}", ms / 60000, ms / 1000 % 60,
                     ms % 1000);
}

// A clip written to its makers' guide: the guide whole, then what Valtz
// knows that the guide leaves to the writer -- the mode, the length, the
// shape, the references' labels -- and the reply's form.
std::string
clip_skill_prompt(const EnhanceRequest& r)
{
  std::string s =
      "You write prompts for a video generation model that makes the "
      "picture and its sound together. Its makers publish the "
      "prompt-writing skill below, in full, with the guide files it "
      "names. Follow it exactly: its field names, their order, its "
      "labels and its timing notation.\n\n"
      "=== The skill ===\n\n";
  std::string_view sys = r.system;
  while (!sys.empty() &&
         std::isspace(static_cast<unsigned char>(sys.back()))) {
    sys.remove_suffix(1);
  }
  s += sys;
  s += "\n\n=== This request ===\n\n";
  if (r.video_mode == "I2VA") {
    s += "Mode: I2VA -- the video opens on <Picture 1>, its first frame "
         "at 0.00 seconds: [Shot 1] starts from what it shows and "
         "develops forward. (Valtz writes the I2VA instruction line "
         "itself: do not write it.)\n";
  } else if (r.video_mode == "Ref2VA") {
    s += "Mode: full-reference (Ref2VA): the six sections.\n";
    if (!r.video_tasks.empty()) {
      s += std::format(
          "The summary's task types, from how the references are used: "
          "[{}]. Change them only where the request says otherwise (a "
          "picture to be a frame of the video: keyframe completion).\n",
          r.video_tasks);
    }
  } else {
    s += "Mode: T2VA -- the video is made from the text alone: no "
         "instruction line.\n";
  }
  if (r.seconds > 0) {
    s += std::format(
        "Length: {:.2f} seconds. The video ends at {}: every cut falls "
        "before it, and the description holds what happens in that "
        "time -- no more, no less.\n",
        r.seconds, timecode(r.seconds));
  }
  if (!r.size.empty()) {
    int w = 0, h = 0;
    if (std::sscanf(r.size.c_str(), "%dx%d", &w, &h) == 2 && w > 0 &&
        h > 0) {
      s += std::format("Frame: {} ({}).\n", r.size,
                       w > h ? "landscape" : w < h ? "portrait" : "square");
    }
  }
  if (r.references.empty()) {
    s += r.video_mode == "Ref2VA" ? "References: none named.\n"
                                  : "";
  } else {
    s += "References, named as the model names them -- use exactly "
         "these labels, and no other <Picture N>, <Video N> or "
         "<Audio N>:\n";
    for (const auto& line : r.references) {
      s += "- " + line + "\n";
    }
    // A small assistant invents the kinds it read about in the guide.
    for (const char* kind : {"<Picture", "<Video", "<Audio"}) {
      if (std::ranges::none_of(r.references, [&](const std::string& l) {
            return l.find(kind) != std::string::npos;
          })) {
        s += std::format("There is no {} N> here.\n", kind);
      }
    }
  }
  s += "Language: write every section in English. Dialogue, lyrics and "
       "on-screen text stay in the language the user wrote them in, word "
       "for word; translate the rest of a request written in another "
       "language.\n"
       "Keep everything the user asked for and invent nothing that "
       "contradicts it; where the request is brief, add concrete visual "
       "and sound detail that fits it.\n\n"
       "Reply with ONLY a JSON object, no prose: each section's text "
       "under its name, in this order, every one written (Valtz puts "
       "them together in the guide's form), line breaks as \\n:\n";
  if (r.video_mode == "Ref2VA") {
    s += "{\"subject_definitions\": \"<one line per label: <Subject 1> "
         "is the ... in <Picture 1>, with ...>\", "
         "\"summary\": \"[task types] <one short paragraph>\", "
         "\"retention_analysis\": \"<one line per label defined>\", "
         "\"detailed_description\": \"<the style in one or two "
         "sentences>\\n[Shot 1] <no timestamp; 350-500 words in all, "
         "shot by shot; a speaker as <Subject N> (S1), the words as "
         "<d>[Language] ...</d>>\\n[Shot 2] At MM:SS.mmm, <only if a "
         "cut is wanted>\", "
         "\"overall_soundscape\": \"<1-4 sentences>\", "
         "\"non_diegetic_music\": \"<1-3 sentences, or N/A>\", "
         "\"title\": \"2-5 word name\"}\n\n";
  } else {
    s += "{\"integrated_multimodal_description\": \"[Shot 1] <no "
         "timestamp: the style and framing, then what happens; whoever "
         "speaks or sings named once with an ID, \"the woman (S1) says: "
         "<d>[English] ...</d>\"> [Shot 2] At MM:SS.mmm, <only if a cut "
         "is wanted>\", "
         "\"overall_soundscape\": \"<1-4 sentences>\", "
         "\"non_diegetic_music\": \"<1-3 sentences, or N/A>\", "
         "\"title\": \"2-5 word name\"}\n\n";
  }
  s += "The user's request:\n";
  s += r.prompt;
  return s;
}

}

std::string
retag(std::string_view text,
      const std::vector<std::pair<std::string, std::string>>& names)
{
  if (names.empty()) {
    return std::string(text);
  }
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    bool hit = false;
    if (text[i] == '<') {
      for (const auto& [from, to] : names) {
        if (!from.empty() && text.substr(i).starts_with(from)) {
          out += to;
          i += from.size();
          hit = true;
          break;
        }
      }
    }
    if (!hit) {
      out += text[i++];
    }
  }
  return out;
}

std::string
build_enhance_prompt(const EnhanceRequest& r)
{
  if (!r.system.empty() && !r.video_mode.empty()) {
    return clip_skill_prompt(r);
  }
  if (!r.system.empty()) {
    // The model maker's own rewriter: its instructions, the pictures
    // named as it names them, then the request. (vpipe's chat has no
    // system turn of its own yet -- DESIGN 15, item 5 -- so they open
    // the user's.)
    std::string s;
    // Their language rules turn on the request's language; a small
    // assistant, reading instructions half in Chinese, needs telling.
    s = std::format("The user's request below is written in {}.\n",
                    has_han(r.prompt) ? "Chinese" : "English");
    if (r.english && has_han(r.prompt)) {
      s += "Write the rewritten prompt in English.\n";
    }
    if (r.pictures > 0) {
      s += "The input images above are, in order:";
      for (int i = 1; i <= r.pictures; ++i) {
        s += std::format(" <image{}>{}", i, i < r.pictures ? "," : ".");
      }
      if (r.pictures > 1) {
        s += " <image1> is the image being edited.";
      }
      s += "\n\n";
    }
    std::string_view sys = r.system;
    while (!sys.empty() &&
           std::isspace(static_cast<unsigned char>(sys.back()))) {
      sys.remove_suffix(1);
    }
    s += sys;
    // The edit instructions end on "The user's edit instruction to
    // rewrite is:"; the others need the request introduced.
    s += sys.ends_with(':') ? "\n" : "\n\nThe user's request is:\n";
    s += r.prompt;
    if (!r.size.empty()) {
      s += std::format(" (Output size: {}.)", r.size);
    }
    if (r.speech) {
      s += "\n\n" + speech_length(r.seconds);
      s += r.voice
          ? "\nA reference voice is given: the speech is in its voice, "
            "so the instruction describes the delivery only."
          : "\nNo reference voice is given: the instruction chooses the "
            "voice too.";
    } else if (r.target == "audio") {
      s += "\n\n" + song_length(r.seconds);
    }
    return s;
  }
  // The language the prompt is written in: English for a model that
  // reads only English (a request in another language translated);
  // otherwise the request's own, said outright -- a small assistant
  // reading English instructions drifts into English.
  const char* lang = has_han(r.prompt) ? "Chinese" : "English";
  const std::string write_in =
      r.english
          ? std::string("in English") +
                (has_han(r.prompt) ? ", translating the request" : "")
          : std::format("in {}, the language of the request", lang);
  // A song: its style as tags, then its lyrics under section headers --
  // the two parts split_song reads back.
  if (r.target == "audio") {
    return std::format(
        "You write prompts for a song generation model. Rewrite the "
        "user's request as a song: FIRST one line of comma-separated "
        "style tags -- genre, mood, instruments, the voice (\"warm "
        "female vocal\"), tempo or feel -- THEN, after a blank line, "
        "the lyrics, in sections, each headed on a line of its own by "
        "[Intro], [Verse], [Pre-Chorus], [Chorus], [Bridge] or [Outro], "
        "a blank line between sections. Keep any lyrics the user wrote "
        "word for word; write the rest {}. For music without words, "
        "give the style line alone. {}\n\n"
        "Reply with ONLY a JSON object, no prose, the prompt's line "
        "breaks as \\n:\n"
        "{{\"prompt\": \"...\", \"title\": \"2-5 word name\"}}\n\n"
        "User request: {}\n"
        "Style hint: {}",
        write_in, song_length(r.seconds), r.prompt,
        r.style.empty() ? "none" : r.style);
  }
  const bool video = r.target == "video";
  return std::format(
      "You rewrite prompts for a {} generation model. Expand the user's "
      "request into one vivid, concrete prompt: subject, setting, "
      "composition, lighting, lens or camera language, style{}. Keep "
      "every element the user asked for, invent nothing that contradicts "
      "it, and do not add text or watermarks unless requested. Write the "
      "prompt {}, 60 to 120 words.\n\n"
      "Reply with ONLY a JSON object, no prose:\n"
      "{{\"prompt\": \"...\", \"negative\": \"...\", "
      "\"title\": \"2-5 word name\"}}\n\n"
      "User request: {}\n"
      "Style hint: {}\n"
      "Attachments: {}",
      video ? "video" : "image",
      video ? ", and the motion and camera movement over time" : "",
      write_in, r.prompt, r.style.empty() ? "none" : r.style,
      describe_attachments(r.attachments));
}

namespace {

// MiniMax H3's sections, in its guides' order: the base modes' three
// fields, and the full-reference mode's six.
constexpr std::array<std::string_view, 3> kBaseSections = {
  "integrated_multimodal_description", "overall_soundscape",
  "non_diegetic_music",
};
constexpr std::array<std::string_view, 6> kRefSections = {
  "subject_definitions", "summary", "retention_analysis",
  "detailed_description", "overall_soundscape", "non_diegetic_music",
};

bool
is_section(std::string_view key)
{
  return std::ranges::find(kRefSections, key) != kRefSections.end() ||
         key == kBaseSections[0];
}

}

namespace {

// A section's text split where another section's name opens a line ("\n
// overall_soundscape: ..."): a small model writes the rest inside one.
// The sections found after the first, by name; `head` keeps the text
// before them.
std::map<std::string, std::string>
split_sections(std::string_view text, std::string& head)
{
  std::map<std::string, std::string> found;
  std::string* into = &head;
  std::size_t at = 0;
  while (at <= text.size()) {
    std::size_t end = text.find('\n', at);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    const std::string_view line = text.substr(at, end - at);
    at = end + 1;
    bool opened = false;
    const auto t = trimmed(line);
    for (auto name : {kBaseSections[0], kRefSections[0], kRefSections[1],
                      kRefSections[2], kRefSections[3], kRefSections[4],
                      kRefSections[5]}) {
      if (t.starts_with(name) && t.size() > name.size() &&
          trimmed(t.substr(name.size())).starts_with(':')) {
        into = &found[std::string(name)];
        *into = std::string(trimmed(trimmed(t.substr(name.size()))
                                        .substr(1)));
        opened = true;
        break;
      }
    }
    if (!opened) {
      if (!into->empty()) {
        *into += '\n';
      }
      *into += line;
    }
  }
  return found;
}

// "[Shot 1] At 00:00.000, " -> "[Shot 1] ": the first shot has no
// timestamp (both guides), and a small model writes one.
std::string
untimed_first_shot(std::string text)
{
  constexpr std::string_view kShot = "[Shot 1]";
  const auto at = text.find(kShot);
  if (at == std::string::npos) {
    return text;
  }
  std::size_t i = at + kShot.size();
  while (i < text.size() && text[i] == ' ') {
    ++i;
  }
  if (text.compare(i, 3, "At ") != 0) {
    return text;
  }
  std::size_t j = i + 3;
  while (j < text.size() &&
         (std::isdigit(static_cast<unsigned char>(text[j])) ||
          text[j] == ':' || text[j] == '.')) {
    ++j;
  }
  if (j == i + 3) {
    return text;
  }
  if (text.compare(j, 8, " seconds") == 0) {
    j += 8;
  }
  if (j < text.size() && text[j] == ',') {
    ++j;
    while (j < text.size() && text[j] == ' ') {
      ++j;
    }
    // Its sentence starts again with a capital.
    if (j < text.size()) {
      text[j] = static_cast<char>(
          std::toupper(static_cast<unsigned char>(text[j])));
    }
    text.erase(at + kShot.size() + 1, j - (at + kShot.size() + 1));
  }
  return text;
}

}

std::string
clip_sections(const std::map<std::string, std::string>& given)
{
  // Sections written inside another taken out, where theirs is empty.
  std::map<std::string, std::string> sections;
  for (const auto& [name, text] : given) {
    std::string head;
    for (auto& [k, v] : split_sections(text, head)) {
      if (k == name) {
        // Its own name at its head: its text.
        head += (trimmed(head).empty() ? "" : "\n") + v;
      } else if (!given.contains(k) || trimmed(given.at(k)).empty()) {
        sections[k] = std::move(v);
      }
    }
    if (!sections.contains(name) || !trimmed(head).empty()) {
      sections[name] = std::move(head);
    }
  }
  // Full reference when any of its own sections is there.
  const bool full = std::ranges::any_of(kRefSections, [&](auto k) {
    return k != "overall_soundscape" && k != "non_diegetic_music" &&
           sections.contains(std::string(k));
  });
  std::string out;
  auto put = [&](std::string_view name) {
    auto it = sections.find(std::string(name));
    if (it == sections.end()) {
      return;
    }
    std::string_view text = trimmed(it->second);
    // Its name again at its head, as a small model writes it.
    if (text.starts_with(name)) {
      const auto rest = trimmed(text.substr(name.size()));
      if (rest.starts_with(':')) {
        text = trimmed(rest.substr(1));
      }
    }
    if (text.empty()) {
      return;
    }
    if (!out.empty()) {
      out += "\n\n";
    }
    // The base guide writes a field's text on its line, the
    // full-reference one on the lines after.
    std::string body(text);
    if (name == "detailed_description" ||
        name == "integrated_multimodal_description") {
      body = untimed_first_shot(std::move(body));
    }
    out += std::string(name) + (full ? ":\n" : ": ") + body;
  };
  if (full) {
    for (auto k : kRefSections) {
      put(k);
    }
  } else {
    for (auto k : kBaseSections) {
      put(k);
    }
  }
  return out;
}

Result<EnhanceResult>
parse_enhance_reply(std::string_view reply)
{
  VALTZ_ASSIGN(Json j, extract_json_object(reply));
  EnhanceResult out;
  // Valtz's own shape, or a model maker's rewriter's.
  out.prompt = jget<std::string>(j, "prompt", "");
  if (out.prompt.empty()) {
    out.prompt = jget<std::string>(j, "rewritten_prompt", "");
  }
  // A song in parts (the yue2-song skill): the style line, then its
  // lyrics -- each a string, or (a small model's way) a list of tags or
  // of lines.
  if (out.prompt.empty() && (j.contains("style") || j.contains("lyrics"))) {
    auto joined = [&](const char* key, const char* sep) {
      const Json v = jget(j, key, Json());
      if (v.is_string()) {
        return v.get<std::string>();
      }
      std::string t;
      for (const auto& e : v.is_array() ? v : Json::array()) {
        if (e.is_string()) {
          t += (t.empty() ? "" : sep) + e.get<std::string>();
        }
      }
      return t;
    };
    const std::string style(trimmed(joined("style", ", ")));
    const std::string lyrics(trimmed(joined("lyrics", "\n")));
    out.prompt = style;
    if (!lyrics.empty()) {
      out.prompt += (style.empty() ? "" : "\n\n") + lyrics;
    }
  }
  // Speech (the moss-tts skill): its fields, then the words -- as the
  // prompt box holds them (split_speech reads them back).
  if (out.prompt.empty() && j.contains("text") &&
      (j.contains("instruction") || j.contains("language"))) {
    auto field = [&](const char* key) {
      const Json v = jget(j, key, Json());
      return v.is_string() ? std::string(trimmed(v.get<std::string>()))
                           : std::string();
    };
    SpeechText sp;
    sp.instruction = field("instruction");
    sp.quality = field("quality");
    sp.sound_event = field("sound_event");
    sp.ambient_sound = field("ambient_sound");
    sp.language = field("language");
    sp.text = field("text");
    out.prompt = speech_prompt(sp);
  }
  // A clip in sections (MiniMax H3's skill): put together in the
  // guide's order and form -- each a string, or a list of its lines.
  if (out.prompt.empty()) {
    std::map<std::string, std::string> sections;
    for (const auto& [key, v] : j.items()) {
      if (!is_section(key)) {
        continue;
      }
      std::string t;
      if (v.is_string()) {
        t = v.get<std::string>();
      }
      for (const auto& e : v.is_array() ? v : Json::array()) {
        if (e.is_string()) {
          t += (t.empty() ? "" : "\n") + e.get<std::string>();
        }
      }
      sections[key] = t;
    }
    if (std::ranges::any_of(sections, [](const auto& kv) {
          return is_section(kv.first);
        })) {
      out.prompt = clip_sections(sections);
    }
  }
  out.negative = jget<std::string>(j, "negative", "");
  out.title = jget<std::string>(j, "title", "");
  out.aspect = jget<std::string>(j, "wh_ratio", "");
  out.follow = jget<std::string>(j, "ratio_follow", "");
  if (out.prompt.size() < 8) {
    return make_error(Code::Corrupt, "enhanced prompt is missing");
  }
  return out;
}

namespace {

void
append_utf8(std::string& out, char32_t cp)
{
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  }
}

// Four hex digits at s[i], or -1.
long
hex4(std::string_view s, std::size_t i)
{
  if (i + 4 > s.size()) {
    return -1;
  }
  long v = 0;
  for (std::size_t k = i; k < i + 4; ++k) {
    const char c = s[k];
    const int d = c >= '0' && c <= '9'   ? c - '0'
                  : c >= 'a' && c <= 'f' ? c - 'a' + 10
                  : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                         : -1;
    if (d < 0) {
      return -1;
    }
    v = v * 16 + d;
  }
  return v;
}

// A trailing character whose bytes have not all come, left out.
void
drop_cut_character(std::string& s)
{
  std::size_t n = s.size();
  std::size_t back = 0;
  while (n > 0 && back < 4 &&
         (static_cast<unsigned char>(s[n - 1]) & 0xc0) == 0x80) {
    --n;
    ++back;
  }
  if (n == 0) {
    return;
  }
  const auto lead = static_cast<unsigned char>(s[n - 1]);
  const std::size_t need = lead >= 0xf0 ? 3 : lead >= 0xe0 ? 2
                           : lead >= 0xc0 ? 1 : 0;
  if (back < need) {
    s.resize(n - 1);
  }
}

// A JSON string from s[i] (just past its opening quote) as far as it has
// come: escapes decoded, an escape or a character cut short left out.
// `done` once its closing quote is read; `i` is then past it.
std::string
partial_string(std::string_view s, std::size_t& i, bool& done)
{
  std::string out;
  done = false;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"') {
      ++i;
      done = true;
      return out;
    }
    if (c != '\\') {
      out += c;
      ++i;
      continue;
    }
    if (i + 1 >= s.size()) {
      break;
    }
    const char e = s[i + 1];
    if (e == 'u') {
      long cp = hex4(s, i + 2);
      if (cp < 0) {
        break;
      }
      std::size_t used = 6;
      // A surrogate pair: both halves, or wait for the second.
      if (cp >= 0xd800 && cp < 0xdc00) {
        if (i + 12 > s.size()) {
          break;
        }
        const long lo = s[i + 6] == '\\' && s[i + 7] == 'u'
                            ? hex4(s, i + 8) : -1;
        cp = lo >= 0xdc00 && lo < 0xe000
                 ? 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00)
                 : 0xfffd;
        used = lo >= 0xdc00 && lo < 0xe000 ? 12 : 6;
      }
      append_utf8(out, static_cast<char32_t>(cp));
      i += used;
      continue;
    }
    switch (e) {
    case 'n': out += '\n'; break;
    case 't': out += '\t'; break;
    case 'r': out += '\r'; break;
    case 'b': out += '\b'; break;
    case 'f': out += '\f'; break;
    default: out += e; break;  // \" \\ \/
    }
    i += 2;
  }
  drop_cut_character(out);
  return out;
}

// Past a value of no interest (a number, an object, ...): false while it
// has not all come.
bool
skip_value(std::string_view s, std::size_t& i)
{
  int depth = 0;
  while (i < s.size()) {
    const char c = s[i];
    if (c == '"') {
      ++i;
      bool done = false;
      partial_string(s, i, done);
      if (!done) {
        return false;
      }
      continue;
    }
    if (c == '{' || c == '[') {
      ++depth;
    } else if (c == '}' || c == ']') {
      if (depth == 0) {
        return true;
      }
      --depth;
    } else if (c == ',' && depth == 0) {
      return true;
    }
    ++i;
  }
  return false;
}

}  // namespace

std::string
partial_enhance_reply(std::string_view s)
{
  std::size_t i = s.find('{');
  if (i == std::string_view::npos) {
    return {};
  }
  ++i;
  std::string prompt, rewritten, style, lyrics;
  std::map<std::string, std::string> sections;
  bool song = false;
  // Speech (the moss-tts skill): its fields and words, as they come.
  SpeechText speech;
  bool speaks = false;
  auto space = [&] {
    while (i < s.size() &&
           std::isspace(static_cast<unsigned char>(s[i]))) {
      ++i;
    }
  };
  for (;;) {
    space();
    if (i >= s.size() || s[i] == '}') {
      break;
    }
    if (s[i] == ',') {
      ++i;
      continue;
    }
    if (s[i] != '"') {
      break;
    }
    ++i;
    bool done = false;
    const std::string key = partial_string(s, i, done);
    if (!done) {
      break;
    }
    space();
    if (i >= s.size() || s[i] != ':') {
      break;
    }
    ++i;
    space();
    if (i >= s.size()) {
      break;
    }
    std::string* into = key == "prompt"             ? &prompt
                        : key == "rewritten_prompt" ? &rewritten
                        : key == "style"            ? &style
                        : key == "lyrics"           ? &lyrics
                        : is_section(key)           ? &sections[key]
                        : key == "instruction"      ? &speech.instruction
                        : key == "quality"          ? &speech.quality
                        : key == "sound_event"      ? &speech.sound_event
                        : key == "ambient_sound"   ? &speech.ambient_sound
                        : key == "language"         ? &speech.language
                        : key == "text"             ? &speech.text
                                                    : nullptr;
    song = song || key == "style" || key == "lyrics";
    speaks = speaks || key == "instruction" || key == "language";
    if (s[i] == '"') {
      ++i;
      std::string v = partial_string(s, i, done);
      if (into) {
        *into = std::move(v);
      }
      if (!done) {
        break;
      }
    } else if (s[i] == '[' && into && into != &prompt &&
               into != &rewritten) {
      // A song's parts or a clip's sections as a list (a small model's
      // way): tags or lines.
      ++i;
      const char* sep = into == &style ? ", " : "\n";
      bool closed = false;
      done = true;
      while (done) {
        space();
        if (i >= s.size()) {
          break;
        }
        if (s[i] == ']') {
          ++i;
          closed = true;
          break;
        }
        if (s[i] == ',') {
          ++i;
          continue;
        }
        if (s[i] != '"') {
          if (!skip_value(s, i)) {
            break;
          }
          continue;
        }
        ++i;
        std::string e = partial_string(s, i, done);
        *into += (into->empty() ? "" : sep) + e;
      }
      if (!closed) {
        break;
      }
    } else if (!skip_value(s, i)) {
      break;
    }
  }
  if (!prompt.empty()) {
    return std::string(trimmed(prompt));
  }
  if (!rewritten.empty()) {
    return std::string(trimmed(rewritten));
  }
  if (std::ranges::any_of(sections, [](const auto& kv) {
        return is_section(kv.first);
      })) {
    return clip_sections(sections);
  }
  if (speaks) {
    return speech_prompt(speech);
  }
  if (!song) {
    return {};
  }
  std::string out(trimmed(style));
  const std::string_view l = trimmed(lyrics);
  if (!l.empty()) {
    out += (out.empty() ? "" : "\n\n") + std::string(l);
  }
  return out;
}

std::string
build_intent_prompt(std::string_view text, const std::vector<Attachment>& a)
{
  return std::format(
      "Classify what the user wants from an image/video app. Choose "
      "exactly one intent:\n"
      "generate-image, edit-image (change an attached picture), "
      "generate-video, animate-image (turn an attached picture into "
      "video), upscale, describe (explain or caption the attachments).\n\n"
      "Reply with ONLY a JSON object, no prose:\n"
      "{{\"intent\": \"...\", \"confidence\": 0.0-1.0, "
      "\"subject\": \"main subject or empty\", "
      "\"params\": {{\"aspect\": \"16:9|1:1|9:16|...\", "
      "\"duration_s\": number or null, \"style\": \"...\"}}}}\n\n"
      "User text: {}\nAttachments: {}",
      text, describe_attachments(a));
}

Result<IntentResult>
parse_intent_reply(std::string_view reply)
{
  VALTZ_ASSIGN(Json j, extract_json_object(reply));
  IntentResult out;
  out.intent = intent_from_str(jget<std::string>(j, "intent", ""));
  if (out.intent == Intent::Unknown) {
    return make_error(Code::Corrupt, "reply named no known intent");
  }
  out.confidence = std::clamp(jget(j, "confidence", 0.5f), 0.0f, 1.0f);
  out.subject = jget<std::string>(j, "subject", "");
  out.params = jget(j, "params", Json::object());
  out.from_model = true;
  return out;
}

IntentResult
heuristic_intent(std::string_view text, const std::vector<Attachment>& a)
{
  const std::string t = lower(text);
  const bool has_image = std::any_of(a.begin(), a.end(), [](auto& x) {
    return x.kind == "image";
  });
  const bool has_video = std::any_of(a.begin(), a.end(), [](auto& x) {
    return x.kind == "video";
  });
  const bool wants_video = has_word(t, {"video", "clip", "animate",
                                        "animation", "motion", "moving",
                                        "timelapse", "seconds", "footage"});
  IntentResult r;
  r.confidence = 0.6f;
  if (has_word(t, {"upscale", "upres", "enlarge", "4k", "8k",
                   "super-resolution", "sharpen"})) {
    r.intent = Intent::Upscale;
  } else if (has_word(t, {"describe", "caption", "what is", "what's",
                          "explain"}) &&
             (has_image || has_video)) {
    r.intent = Intent::Describe;
  } else if (has_image && wants_video) {
    r.intent = Intent::AnimateImage;
  } else if (wants_video) {
    r.intent = Intent::GenerateVideo;
  } else if (has_image &&
             has_word(t, {"change", "edit", "replace", "remove", "add",
                          "make", "turn", "recolor", "background",
                          "retouch", "fix"})) {
    r.intent = Intent::EditImage;
  } else if (has_image && t.empty()) {
    r.intent = Intent::Describe;
    r.confidence = 0.4f;
  } else {
    r.intent = Intent::GenerateImage;
    r.confidence = t.empty() ? 0.2f : 0.5f;
  }
  if (has_word(t, {"portrait", "vertical", "9:16", "phone"})) {
    r.params["aspect"] = "9:16";
  } else if (has_word(t, {"landscape", "wide", "cinematic", "16:9",
                          "widescreen"})) {
    r.params["aspect"] = "16:9";
  } else if (has_word(t, {"square", "1:1"})) {
    r.params["aspect"] = "1:1";
  }
  return r;
}

void
to_json(Json& j, const IntentResult& r)
{
  j = {{"intent", to_str(r.intent)},
       {"confidence", r.confidence},
       {"subject", r.subject},
       {"params", r.params},
       {"from_model", r.from_model}};
}

void
to_json(Json& j, const EnhanceResult& r)
{
  j = {{"prompt", r.prompt}, {"negative", r.negative}, {"title", r.title}};
  if (!r.aspect.empty()) {
    j["aspect"] = r.aspect;
  }
  if (!r.follow.empty()) {
    j["follow"] = r.follow;
  }
}

}
