#include "valtz/assist/segments.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace valtz::assist {

namespace {

// Valtz's headers, the only ones read: a model's own sections
// ("overall_soundscape:") stay the model's.
constexpr std::array<std::string_view, 1> kHeaders = {kRegeneratePrompt};

std::string_view
trimmed(std::string_view s)
{
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
    s.remove_prefix(1);
  }
  while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
    s.remove_suffix(1);
  }
  return s;
}

// The header `line` opens with, and where its text starts; "" when none.
std::string_view
header_of(std::string_view line, std::size_t& text_at)
{
  std::size_t i = 0;
  while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) {
    ++i;
  }
  for (const auto h : kHeaders) {
    if (line.size() - i <= h.size()) {
      continue;
    }
    const bool same = std::equal(
        h.begin(), h.end(), line.begin() + static_cast<std::ptrdiff_t>(i),
        [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) ==
                 std::tolower(static_cast<unsigned char>(b));
        });
    std::size_t j = i + h.size();
    while (j < line.size() && line[j] == ' ') {
      ++j;
    }
    if (same && j < line.size() && line[j] == ':') {
      text_at = j + 1;
      return h;
    }
  }
  return {};
}

}

PromptSegments
prompt_segments(std::string_view prompt)
{
  PromptSegments out;
  std::string* into = nullptr;
  std::size_t body_end = prompt.size();
  std::size_t at = 0;
  while (at < prompt.size()) {
    std::size_t end = prompt.find('\n', at);
    const std::size_t next = end == std::string_view::npos ? prompt.size()
                                                           : end + 1;
    if (end == std::string_view::npos) {
      end = prompt.size();
    }
    const std::string_view line = prompt.substr(at, end - at);
    std::size_t text_at = 0;
    if (const auto h = header_of(line, text_at); !h.empty()) {
      if (!into) {
        body_end = at;
      }
      into = &out.segments[std::string(h)];
      if (!into->empty()) {
        *into += '\n';
      }
      *into += line.substr(text_at);
    } else if (into) {
      *into += '\n';
      *into += line;
    }
    at = next;
  }
  if (!into) {
    out.body = std::string(prompt);
    return out;
  }
  out.body = std::string(trimmed(prompt.substr(0, body_end)));
  for (auto it = out.segments.begin(); it != out.segments.end();) {
    it->second = std::string(trimmed(it->second));
    it = it->second.empty() ? out.segments.erase(it) : std::next(it);
  }
  return out;
}

std::string
with_segments(std::string_view body,
              const std::map<std::string, std::string>& segments)
{
  std::string out(trimmed(body));
  for (const auto h : kHeaders) {
    const auto it = segments.find(std::string(h));
    if (it == segments.end() || trimmed(it->second).empty()) {
      continue;
    }
    out += (out.empty() ? "" : "\n\n") + std::string(h) + ": " +
           std::string(trimmed(it->second));
  }
  return out;
}

}
