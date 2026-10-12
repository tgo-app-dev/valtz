#include "valtz/media/text-flow.h"

#include "valtz/base/text.h"
#include "valtz/media/markup.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string_view>

namespace valtz::media {

namespace {

using Kind = FlowBlock::Kind;

bool
blank(char c)
{
  return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

std::string
trim(std::string_view s)
{
  std::size_t a = 0, b = s.size();
  while (a < b && blank(s[a])) {
    ++a;
  }
  while (b > a && blank(s[b - 1])) {
    --b;
  }
  return std::string(s.substr(a, b - a));
}

bool
word_char(char c)
{
  return std::isalnum(static_cast<unsigned char>(c)) ||
         static_cast<unsigned char>(c) >= 0x80;
}

// Inline Markdown taken out: a box has one style. [a link](url) and
// ![a picture](url) leave their words; ** and __ go; a * or _ goes where
// it opens or closes a word (not between words -- "5 * 3", snake_case);
// backticks go.
std::string
plain(std::string_view s)
{
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '!' && i + 1 < s.size() && s[i + 1] == '[') {
      continue;
    }
    if (c == '[') {
      const auto close = s.find(']', i + 1);
      if (close != std::string_view::npos && close + 1 < s.size() &&
          s[close + 1] == '(') {
        const auto end = s.find(')', close + 2);
        if (end != std::string_view::npos) {
          out += plain(s.substr(i + 1, close - i - 1));
          i = end;
          continue;
        }
      }
    }
    if ((c == '*' || c == '_') && i + 1 < s.size() && s[i + 1] == c) {
      ++i;
      continue;
    }
    if (c == '`') {
      continue;
    }
    if (c == '*' || c == '_') {
      const bool before = i > 0 && !blank(s[i - 1]);
      const bool after = i + 1 < s.size() && !blank(s[i + 1]);
      const bool word_before = i > 0 && word_char(s[i - 1]);
      const bool word_after = i + 1 < s.size() && word_char(s[i + 1]);
      // Opening (nothing of a word before, a word after) or closing.
      if ((after && !word_before) || (before && !word_after)) {
        if (!(word_before && word_after)) {
          continue;
        }
      }
    }
    out += c;
  }
  return out;
}

int
words_in(std::string_view s)
{
  int n = 0;
  bool in = false;
  for (char c : s) {
    if (blank(c)) {
      in = false;
    } else if (!in) {
      in = true;
      ++n;
    }
  }
  return n;
}

// A heading's text ends no sentence: no full stop, comma, colon...
bool
ends_sentence(std::string_view s)
{
  static constexpr std::string_view kEnds[] = {
      ".", ",", ";", ":", "!", "?", ")", "]", "\"", "'",
      "\xe2\x80\xa6",                          // …
      "\xe2\x80\x9d", "\xe2\x80\x99",          // ” ’
      "\xe3\x80\x82", "\xef\xbc\x8c", "\xef\xbc\x81",  // 。，！
      "\xef\xbc\x9f", "\xef\xbc\x9a", "\xef\xbc\x9b"}; // ？：；
  for (auto e : kEnds) {
    if (s.ends_with(e)) {
      return true;
    }
  }
  return false;
}

// "- ", "* ", "+ ", "• ", "– " or "12. " / "12) ": an item's marker and
// where its words start.
std::optional<std::pair<std::string, std::size_t>>
item_marker(std::string_view t)
{
  for (std::string_view m : {"- ", "* ", "+ ", "\xe2\x80\xa2 ",
                             "\xe2\x80\x93 "}) {
    if (t.starts_with(m)) {
      return std::pair{std::string("\xe2\x80\xa2"), m.size()};
    }
  }
  std::size_t d = 0;
  while (d < t.size() && d < 3 && std::isdigit(static_cast<unsigned char>(
                                       t[d]))) {
    ++d;
  }
  if (d > 0 && d + 1 < t.size() && (t[d] == '.' || t[d] == ')') &&
      t[d + 1] == ' ') {
    return std::pair{std::string(t.substr(0, d)) + ".", d + 2};
  }
  return std::nullopt;
}

// A line of three or more -, * or _ alone: a section's break.
bool
rule_line(std::string_view t)
{
  if (t.size() < 3) {
    return false;
  }
  const char c = t[0];
  if (c != '-' && c != '*' && c != '_') {
    return false;
  }
  return std::ranges::all_of(t, [c](char x) { return x == c || x == ' '; });
}

}  // namespace

std::vector<FlowBlock>
read_flow_blocks(const std::string& text, bool keep_line_breaks,
                 bool auto_headings, bool* found_headings)
{
  if (found_headings) {
    *found_headings = false;
  }
  std::vector<std::string> lines;
  {
    std::size_t at = 0;
    while (at <= text.size()) {
      const auto nl = text.find('\n', at);
      lines.push_back(text.substr(
          at, nl == std::string::npos ? std::string::npos : nl - at));
      if (nl == std::string::npos) {
        break;
      }
      at = nl + 1;
    }
  }
  // Blank lines between paragraphs: a paragraph's lines are joined (or
  // kept, keep_line_breaks). None at all: a paragraph a line.
  bool seen = false, gap = false, blank_lines = false;
  for (const auto& l : lines) {
    if (trim(l).empty()) {
      gap = seen;
    } else {
      blank_lines = blank_lines || gap;
      seen = true;
      gap = false;
    }
  }
  const bool join = blank_lines || keep_line_breaks;

  std::vector<FlowBlock> blocks;
  std::vector<int> line_counts;
  FlowBlock open;
  int open_lines = 0;
  bool marked = false;
  auto flush = [&] {
    std::string t = trim(plain(open.text));
    if (!t.empty()) {
      open.text = std::move(t);
      blocks.push_back(std::move(open));
      line_counts.push_back(open_lines);
    }
    open = FlowBlock{};
    open_lines = 0;
  };
  for (const auto& raw : lines) {
    const std::string t = trim(raw);
    if (t.empty() || rule_line(t)) {
      flush();
      continue;
    }
    std::size_t hashes = 0;
    while (hashes < t.size() && hashes < 7 && t[hashes] == '#') {
      ++hashes;
    }
    if (hashes >= 1 && hashes <= 6 && hashes < t.size() &&
        t[hashes] == ' ') {
      flush();
      std::string h = trim(std::string_view(t).substr(hashes));
      while (!h.empty() && h.back() == '#') {
        h.pop_back();
      }
      open.kind = hashes == 1 ? Kind::Title : Kind::Heading;
      open.text = trim(h);
      open_lines = 1;
      flush();
      marked = true;
      continue;
    }
    if (auto m = item_marker(t)) {
      flush();
      open.kind = Kind::Item;
      open.marker = m->first;
      open.text = t.substr(m->second);
      open_lines = 1;
      continue;
    }
    if (t[0] == '>') {
      const std::string q = trim(std::string_view(t).substr(1));
      if (open.kind != Kind::Quote || open.text.empty() || !join) {
        flush();
        open.kind = Kind::Quote;
        open.text = q;
      } else {
        open.text += keep_line_breaks ? "\n" : " ";
        open.text += q;
      }
      ++open_lines;
      continue;
    }
    if (!open.text.empty() && join) {
      open.text += keep_line_breaks ? "\n" : " ";
      open.text += t;
      ++open_lines;
      continue;
    }
    flush();
    open.kind = Kind::Paragraph;
    open.text = t;
    open_lines = 1;
  }
  flush();

  // Headings found: a short line standing alone, ending no sentence,
  // before a longer block -- where the text marks none, and only while
  // they are few (a list of short lines is not headings).
  if (auto_headings && !marked) {
    std::vector<std::size_t> found;
    int paragraphs = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
      const auto& b = blocks[i];
      if (b.kind != Kind::Paragraph) {
        continue;
      }
      ++paragraphs;
      if (line_counts[i] != 1 || b.text.size() > 90 ||
          words_in(b.text) > 12 || ends_sentence(b.text) ||
          i + 1 >= blocks.size()) {
        continue;
      }
      const auto& next = blocks[i + 1];
      if (next.kind == Kind::Paragraph &&
          next.text.size() >= b.text.size() * 2 && next.text.size() > 60) {
        found.push_back(i);
      } else if (next.kind == Kind::Item || next.kind == Kind::Quote) {
        found.push_back(i);
      }
    }
    if (!found.empty() &&
        found.size() * 10 <= static_cast<std::size_t>(paragraphs) * 4 + 5) {
      for (auto i : found) {
        blocks[i].kind = i == 0 ? Kind::Title : Kind::Heading;
      }
      if (found_headings) {
        *found_headings = true;
      }
    }
  }
  return blocks;
}

namespace {

Json
rgba(const std::array<double, 4>& c)
{
  return Json::array({c[0], c[1], c[2], c[3]});
}

Json
box_object(const FlowStyle& s, double x, double y, double w, double h,
           const std::string& text, const std::string& align,
           bool justify_last)
{
  Json o = {{"kind", "text"},
            {"box", true},
            {"x0", x},
            {"y0", y},
            {"x1", x + w},
            {"y1", y + h},
            {"text", text},
            {"stroke", rgba(s.color)},
            {"fill", Json::array({0, 0, 0, 0})},
            {"width", 0},
            {"font",
             {{"family", s.font},
              {"size", s.size},
              {"bold", s.bold},
              {"italic", s.italic},
              {"underline", false}}},
            {"align", align},
            {"line_spacing", s.line_spacing}};
  if (justify_last) {
    o["justify_last"] = true;
  }
  return o;
}

std::string
first_words(const std::string& s, int n)
{
  std::size_t i = 0;
  int words = 0;
  bool in = false;
  for (; i < s.size(); ++i) {
    if (blank(s[i])) {
      if (in && ++words == n) {
        break;
      }
      in = false;
    } else {
      in = true;
    }
  }
  std::string out(utf8_prefix(s.substr(0, i), 80));
  return out.size() < s.size() ? out + std::string(kEllipsis) : out;
}

}  // namespace

FlowResult
flow_text(const FlowSpec& spec)
{
  FlowResult out;
  const int ncol = std::clamp(spec.columns, 1, 4);
  const double gutter = std::max(0.0, spec.gutter);
  const auto& area = spec.area;
  const double colw = (area[2] - gutter * (ncol - 1)) / ncol;
  const double bottom = area[1] + area[3];
  if (colw < 8 || area[3] < 8 || spec.blocks.empty()) {
    return out;
  }
  const double par_gap =
      spec.paragraph_gap >= 0
          ? spec.paragraph_gap
          : spec.body.size * (0.4 + 0.5 * spec.body.line_spacing);
  const int max_pages = std::max(1, spec.max_pages);

  int col = 0;
  double y = 0;
  bool fresh = true;  // nothing yet in this column
  bool full = false;  // max_pages reached
  auto top = [&] {
    return out.pages.size() == 1 && spec.first_top > area[1]
               ? std::min(spec.first_top, bottom - 1)
               : area[1];
  };
  auto new_page = [&] {
    if (static_cast<int>(out.pages.size()) >= max_pages) {
      full = true;
      return;
    }
    out.pages.emplace_back();
    col = 0;
    y = top();
    fresh = true;
  };
  auto next_column = [&] {
    if (col + 1 < ncol) {
      ++col;
      y = top();
      fresh = true;
    } else {
      new_page();
    }
  };
  new_page();

  struct Set {
    const FlowStyle* style;
    double           left = 0;   // set in from the column's left
    double           right = 0;  // and its right
  };
  FlowStyle quote_style = spec.body;
  quote_style.italic = true;
  auto set_of = [&](const FlowBlock& b) {
    switch (b.kind) {
      case Kind::Title:
        return Set{&spec.title};
      case Kind::Heading:
        return Set{&spec.heading};
      case Kind::Item:
        return Set{&spec.body, spec.body.size * 1.5};
      case Kind::Quote:
        return Set{&quote_style, spec.body.size * 1.6,
                   spec.body.size * 0.8};
      case Kind::Paragraph:
        break;
    }
    return Set{&spec.body};
  };
  auto heading = [](Kind k) { return k == Kind::Title || k == Kind::Heading; };
  // The lines `text` takes set so, at its width.
  auto measure = [&](const Set& st, const std::string& text) {
    const double w = colw - st.left - st.right;
    return text_fit(box_object(*st.style, 0, 0, w, 1, text,
                               st.style->align, false),
                    true);
  };
  auto gap_before = [&](Kind prev, Kind k) {
    if (fresh) {
      return 0.0;
    }
    if (k == Kind::Title) {
      return spec.title.size * 0.9;
    }
    if (k == Kind::Heading) {
      return spec.heading.size * 1.0;
    }
    if (prev == Kind::Title) {
      return spec.title.size * 0.4;
    }
    if (prev == Kind::Heading) {
      return spec.heading.size * 0.4;
    }
    if (prev == Kind::Item && k == Kind::Item) {
      return spec.body.size * 0.3;
    }
    return par_gap;
  };
  auto note_start = [&](const std::string& text) {
    if (out.pages.back().starts.empty()) {
      out.pages.back().starts = first_words(text, 8);
    }
  };

  Kind prev = Kind::Paragraph;
  for (std::size_t i = 0; i < spec.blocks.size(); ++i) {
    const FlowBlock& b = spec.blocks[i];
    const Set st = set_of(b);
    // A chapter a page: a heading after anything on its page starts the
    // next.
    if (spec.page_per_heading && heading(b.kind) && !heading(prev) &&
        !out.pages.back().objects.empty() && !full) {
      new_page();
      if (full) {
        out.left += b.text.size();
        for (std::size_t j = i + 1; j < spec.blocks.size(); ++j) {
          out.left += spec.blocks[j].text.size();
        }
        break;
      }
    }
    const FlowStyle& s = *st.style;
    std::string text = b.text;
    bool first_piece = true;
    // A heading is kept with the first two lines of what follows it.
    double keep = 0;
    if (heading(b.kind) && i + 1 < spec.blocks.size()) {
      const FlowBlock& n = spec.blocks[i + 1];
      const TextFit nf = measure(set_of(n), n.text);
      if (!nf.line_heights.empty()) {
        keep = (b.kind == Kind::Title ? spec.title.size
                                      : spec.heading.size) * 0.4 +
               nf.line_heights[std::min<std::size_t>(
                   2, nf.line_heights.size()) - 1];
      }
    }
    while (!text.empty() && !full) {
      const TextFit f = measure(st, text);
      const std::size_t lines = f.line_heights.size();
      if (lines == 0) {
        break;
      }
      const double before = gap_before(prev, b.kind);
      const double room = bottom - y - before;
      const double whole = f.line_heights.back();
      std::size_t k = lines;
      if (whole + keep > room && !(fresh && whole <= bottom - y)) {
        if (heading(b.kind)) {
          if (!fresh) {
            next_column();
            continue;
          }
        } else {
          k = 0;
          while (k < lines && f.line_heights[k] <= room) {
            ++k;
          }
          if (!fresh) {
            // At least two lines here and two on: else it moves on.
            if (lines - k == 1) {
              --k;
            }
            if (k < 2) {
              next_column();
              continue;
            }
          } else if (k == 0) {
            k = 1;  // a column too short for a line: one anyway
          } else if (lines - k == 1 && k >= 3) {
            --k;
          }
        }
      }
      const double x = area[0] + col * (colw + gutter);
      const double w = colw - st.left - st.right;
      const double at = y + before;
      const double h = f.line_heights[k - 1];
      std::string piece = text;
      std::string rest;
      // Cut mid-paragraph (not at a line of its own): its last line is
      // stretched when it is justified, as the paragraph's other lines.
      bool running = false;
      if (k < lines) {
        const std::size_t cut = f.line_ends[k - 1];
        running = cut > 0 && text[cut - 1] != '\n';
        piece = trim(std::string_view(text).substr(0, cut));
        rest = trim(std::string_view(text).substr(cut));
      }
      FlowPage& page = out.pages.back();
      // A quotation's bar beside it; an item's marker in its indent.
      if (b.kind == Kind::Quote) {
        const double bx = x + spec.body.size * 0.45;
        page.objects.push_back(
            {{"kind", "line"},
             {"x0", bx},
             {"y0", at + s.size * 0.15},
             {"x1", bx},
             {"y1", at + h - s.size * 0.1},
             {"stroke", rgba(spec.accent)},
             {"fill", Json::array({0, 0, 0, 0})},
             {"width", std::max(2.0, std::round(s.size * 0.12))}});
      }
      if (b.kind == Kind::Item && first_piece) {
        const double mw = st.left - s.size * 0.45;
        page.objects.push_back(box_object(s, x, at, mw,
                                          f.line_heights.front(), b.marker,
                                          "right", false));
      }
      page.objects.push_back(box_object(
          s, x + st.left, at, w, h, piece, s.align,
          running && s.align == "justified"));
      note_start(piece);
      if (heading(b.kind) && first_piece) {
        out.headings.push_back({b.text,
                                static_cast<int>(out.pages.size()) - 1,
                                b.kind == Kind::Title});
      }
      y = at + h;
      fresh = false;
      page.last_column = col;
      page.ends_at = y;
      prev = b.kind;
      first_piece = false;
      text = std::move(rest);
      if (!text.empty()) {
        next_column();
      }
    }
    if (full) {
      // The pages are all used: what is left of the text, unplaced.
      out.left += text.size();
      for (std::size_t j = i + 1; j < spec.blocks.size(); ++j) {
        out.left += spec.blocks[j].text.size();
      }
      break;
    }
  }
  return out;
}

}  // namespace valtz::media
