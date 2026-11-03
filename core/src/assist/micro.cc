#include "valtz/assist/micro.h"

#include "valtz/assist/assistant.h"
#include "valtz/assist/segments.h"
#include "valtz/base/text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <format>
#include <regex>

namespace valtz::assist {

namespace {

std::string_view
trimmed_view(std::string_view s)
{
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

std::string
lower(std::string_view s)
{
  std::string out(s);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// The row's tags and H3's labels as words: "<valtz_ref_img_0>" and
// "<Picture 1>" both "picture 1", a clip's "clip", a sound's "sound".
std::string
tags_as_words(std::string_view text)
{
  static const std::regex row("<valtz_ref_(img|vid|aud)_([0-9]+)>");
  static const std::regex h3("<(Picture|Video|Audio|Subject) ([0-9]+)>");
  std::string s(text);
  std::string out;
  auto at = s.cbegin();
  for (std::sregex_iterator it(s.begin(), s.end(), row), end; it != end;
       ++it) {
    const auto& m = *it;
    out.append(at, m[0].first);
    const std::string k = m[1].str();
    out += k == "img" ? "picture " : k == "vid" ? "clip " : "sound ";
    out += std::to_string(std::stoi(m[2].str()) + 1);
    at = m[0].second;
  }
  out.append(at, s.cend());
  return std::regex_replace(out, h3, "$1 $2");
}


}  // namespace

std::string
micro_name_request(std::string_view prompt)
{
  const std::string words = without_markdown(
      tags_as_words(prompt_segments(prompt).body));
  return std::format(
      "Name this prompt for an image, video or sound generator in 2 to 6 "
      "words: a short title saying what it makes, in the prompt's own "
      "language. Reply with the title alone -- no quotes, no punctuation "
      "at the end.\n\nThe prompt:\n{}",
      utf8_prefix(trimmed_view(words), 4000));
}

std::string
micro_name(std::string_view reply)
{
  // Its first line with anything on it.
  std::string_view line;
  while (!reply.empty()) {
    const auto end = reply.find('\n');
    line = trimmed_view(reply.substr(0, end));
    reply = end == std::string_view::npos ? std::string_view()
                                          : reply.substr(end + 1);
    if (!line.empty()) {
      break;
    }
  }
  // A label before it ("Title:", "Name:"): a word and a colon.
  if (const auto colon = line.find(':');
      colon != std::string_view::npos && colon <= 12 &&
      line.substr(0, colon).find(' ') == std::string_view::npos) {
    line = trimmed_view(line.substr(colon + 1));
  }
  std::string s(line);
  // Quotes and emphasis round it, end punctuation after it -- ASCII, and
  // the curly and CJK ones as UTF-8.
  static const std::array<std::string_view, 12> kWrap = {
    "\"", "'", "`", "*", "_", "\xe2\x80\x9c", "\xe2\x80\x9d",
    "\xe2\x80\x98", "\xe2\x80\x99", "\xe3\x80\x8c", "\xe3\x80\x8d",
    "\xe3\x80\x8a",
  };
  static const std::array<std::string_view, 7> kEnd = {
    ".", "!", "?", "\xe3\x80\x82", "\xef\xbc\x81", "\xef\xbc\x9f",
    "\xe2\x80\xa6",
  };
  for (bool cut = true; cut && !s.empty();) {
    cut = false;
    for (auto w : kWrap) {
      if (s.starts_with(w)) {
        s.erase(0, w.size());
        cut = true;
      }
      if (s.ends_with(w)) {
        s.erase(s.size() - w.size());
        cut = true;
      }
    }
    for (auto e : kEnd) {
      if (s.ends_with(e)) {
        s.erase(s.size() - e.size());
        cut = true;
      }
    }
    s = std::string(trimmed_view(s));
  }
  s = one_line(s);
  int words = s.empty() ? 0 : 1;
  for (const char c : s) {
    words += c == ' ';
  }
  if (s.empty() || words > 10) {
    return {};
  }
  return std::string(utf8_prefix(s, 64));
}

std::string
micro_refine_request(int pictures)
{
  std::string lines;
  for (int i = 1; i <= std::max(1, pictures); ++i) {
    lines += std::format("Picture {}: <phrase>\n", i);
  }
  return std::format(
      "The {} above show who or what will appear in a video{}. The video "
      "decides what they do and where. For each picture, describe only "
      "its main subject's appearance in a short noun phrase -- what it "
      "is, its face, hair or fur, clothes, colours, materials -- as a "
      "casting note would. Leave out what it is doing and where it is: "
      "no sleeping, walking, sitting, curled, mid-stride, in the snow. "
      "Example: a woman with short grey hair in a red wool coat. Reply "
      "with one line per picture and nothing else:\n{}",
      pictures > 1 ? "pictures" : "picture",
      pictures > 1 ? std::format(": Picture 1 to Picture {}, in order",
                                 pictures)
                   : ": Picture 1",
      lines);
}

namespace {

// A clause of a casting note that is no look: a pose, an act, a place.
bool
says_where_or_how(std::string_view clause)
{
  static constexpr std::array<std::string_view, 22> kWords = {
    "sleep", "asleep", "walking", "sitting", "standing", "lying",
    "running", "curled", "mid-stride", "jumping", "resting", "looking",
    "in the ", "on the ", "at the ", "against ", "eyes closed",
    "closed eyes", "open eyes", "eyes open", "captured", "background",
  };
  const std::string low = lower(clause);
  return std::ranges::any_of(kWords, [&](std::string_view w) {
    return low.find(w) != std::string::npos;
  });
}

}  // namespace

std::vector<std::string>
micro_pictures(std::string_view reply, int pictures)
{
  std::vector<std::string> out(std::max(0, pictures));
  for (std::size_t at = 0; at <= reply.size();) {
    auto end = reply.find('\n', at);
    if (end == std::string_view::npos) {
      end = reply.size();
    }
    std::string_view line = trimmed_view(reply.substr(at, end - at));
    at = end + 1;
    while (!line.empty() && (line.front() == '*' || line.front() == '-')) {
      line = trimmed_view(line.substr(1));
    }
    const std::string low = lower(line);
    if (!low.starts_with("picture ")) {
      continue;
    }
    std::size_t i = 8, n = 0;
    while (i < line.size() &&
           std::isdigit(static_cast<unsigned char>(line[i]))) {
      n = n * 10 + static_cast<std::size_t>(line[i] - '0');
      ++i;
    }
    std::string_view rest = line.substr(i);
    while (!rest.empty() && (rest.front() == '*' || rest.front() == ' ')) {
      rest.remove_prefix(1);
    }
    if (n == 0 || n > out.size() || !rest.starts_with(':')) {
      continue;
    }
    rest = trimmed_view(rest.substr(1));
    // Its first clause always; the others where they say a look.
    std::string phrase;
    for (std::size_t c = 0; c <= rest.size();) {
      auto comma = rest.find(',', c);
      if (comma == std::string_view::npos) {
        comma = rest.size();
      }
      const std::string_view part = trimmed_view(rest.substr(c, comma - c));
      c = comma + 1;
      if (part.empty() || (!phrase.empty() && says_where_or_how(part))) {
        continue;
      }
      if (phrase.empty()) {
        // "a sleeping orange fox": its pose's word out of the first.
        std::string kept;
        for (std::size_t w = 0; w <= part.size();) {
          auto sp = part.find(' ', w);
          if (sp == std::string_view::npos) {
            sp = part.size();
          }
          const std::string_view word = part.substr(w, sp - w);
          w = sp + 1;
          static constexpr std::array<std::string_view, 9> kPose = {
            "sleeping", "walking", "sitting", "standing", "running",
            "resting", "lying", "jumping", "curled",
          };
          if (!word.empty() &&
              std::ranges::find(kPose, lower(word)) == kPose.end()) {
            kept += (kept.empty() ? "" : " ") + std::string(word);
          }
        }
        phrase = kept;
        continue;
      }
      phrase += ", " + std::string(part);
    }
    while (!phrase.empty() &&
           (phrase.back() == '.' || phrase.back() == ' ')) {
      phrase.pop_back();
    }
    if (phrase.empty()) {
      continue;
    }
    // "A red fox ..." mid-sentence as "a red fox ..."; "a" or "an" as
    // the word after it is now ("a sleeping orange fox" lost a word).
    std::string first = lower(phrase.substr(0, phrase.find(' ')));
    if ((first == "a" || first == "an") &&
        phrase.find(' ') != std::string::npos) {
      phrase.erase(0, phrase.find(' ') + 1);
      first = lower(phrase.substr(0, phrase.find(' ')));
    }
    static constexpr std::array<std::string_view, 7> kDet = {
      "the", "one", "two", "three", "some", "his", "her",
    };
    if (std::ranges::find(kDet, first) != kDet.end()) {
      phrase[0] = static_cast<char>(
          std::tolower(static_cast<unsigned char>(phrase[0])));
    } else if (std::isalpha(static_cast<unsigned char>(phrase[0]))) {
      const bool vowel =
          std::string_view("aeiou").find(static_cast<char>(std::tolower(
              static_cast<unsigned char>(phrase[0])))) !=
          std::string_view::npos;
      phrase = (vowel ? "an " : "a ") + phrase;
    }
    out[n - 1] = std::string(utf8_prefix(phrase, 400));
  }
  return out;
}

std::string
micro_refine_words(std::string_view prompt,
                   const std::vector<std::string>& pictures)
{
  if (std::ranges::all_of(pictures, &std::string::empty)) {
    return {};
  }
  std::string out;
  for (std::size_t i = 0; i < prompt.size();) {
    bool put = false;
    if (prompt[i] == '<' && prompt.compare(i, 9, "<Picture ") == 0) {
      std::size_t j = i + 9, n = 0;
      while (j < prompt.size() &&
             std::isdigit(static_cast<unsigned char>(prompt[j]))) {
        n = n * 10 + static_cast<std::size_t>(prompt[j] - '0');
        ++j;
      }
      if (j < prompt.size() && prompt[j] == '>' && n >= 1 &&
          n <= pictures.size() && !pictures[n - 1].empty()) {
        std::string phrase = pictures[n - 1];
        // A sentence's start: the text's, a line's, after ". " or a
        // shot's "] ".
        std::size_t k = out.size();
        while (k > 0 && out[k - 1] == ' ') {
          --k;
        }
        if (k == 0 || out[k - 1] == '\n' || out[k - 1] == '.' ||
            out[k - 1] == ']' || out[k - 1] == ':' || out[k - 1] == '!' ||
            out[k - 1] == '?') {
          phrase[0] = static_cast<char>(
              std::toupper(static_cast<unsigned char>(phrase[0])));
        }
        out += phrase;
        i = j + 1;
        put = true;
      }
    }
    if (!put) {
      out += prompt[i++];
    }
  }
  return refine_prompt(out);
}

}
