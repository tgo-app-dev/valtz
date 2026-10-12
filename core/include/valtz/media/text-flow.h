// A LONG TEXT FLOWED onto pages: its paragraphs laid in an area of each
// page -- in columns -- as markup TEXT BOXES (markup.h), a page after
// another, until every word is placed (DESIGN §9b, "A long text on
// pages").
//
// Why the core lays it and not the agent's model: a model placing a
// box at a time guesses where words end -- a page cut through a line, a
// paragraph lost or said twice -- and copies a long text through its own
// output, slowly and not always word for word. Here every box is
// measured by the same framesetter that draws it (text_fit), a
// paragraph is cut only at one of its lines, and each word lands once,
// in order.
//
// THE TEXT is read as light Markdown: "# " a heading, "## " (or more) a
// subheading, "- " / "* " / "1. " a list's item, "> " a quotation, a
// blank line between paragraphs. A text with no blank line at all has a
// paragraph a line (as text copied from a page often is); else a
// paragraph's lines are joined, unless `keep_line_breaks` (a poem, an
// address). Inline marks -- **bold**, *italic*, `code`, [a link](url) --
// are taken out: a box has one style. With `auto_headings`, in a text
// that marks none, a short line standing alone before a longer paragraph
// -- no sentence's end -- is a heading (the first a title).
//
// THE LAYOUT: each block as a box of its own, a column's width (an item
// and a quotation set in), one under another with space between; a
// heading kept with the first two lines of what follows it; a paragraph
// that does not fit is CUT at a line, at least two lines either side
// (one line left alone moves on), the boxes of a justified paragraph
// cut mid-way stretching their last line (`justify_last`). A column
// full, the next; the last full, a new page.

#ifndef VALTZ_MEDIA_TEXT_FLOW_H
#define VALTZ_MEDIA_TEXT_FLOW_H

#include "valtz/base/json.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace valtz::media {

// A block of the text, as read.
struct FlowBlock {
  enum class Kind { Title, Heading, Paragraph, Item, Quote };
  Kind        kind = Kind::Paragraph;
  std::string text;
  std::string marker;  // an item's: "•", "1.", ...
};

// The text read into blocks (the rules above). `found_headings`: true
// when the headings were found by `auto_headings`, not marked.
std::vector<FlowBlock> read_flow_blocks(const std::string& text,
                                        bool keep_line_breaks,
                                        bool auto_headings,
                                        bool* found_headings = nullptr);

// How a kind of block is set.
struct FlowStyle {
  std::string           font = "Georgia";
  double                size = 28;  // pixels
  bool                  bold = false;
  bool                  italic = false;
  std::array<double, 4> color{0.13, 0.13, 0.13, 1};  // sRGB, 0..1
  double                line_spacing = 1.4;  // markup.h
  std::string           align = "left";
};

struct FlowSpec {
  std::vector<FlowBlock> blocks;
  // The area on each page, canvas pixels: x, y, width, height.
  std::array<double, 4>  area{0, 0, 0, 0};
  int                    columns = 1;
  double                 gutter = 0;
  // Where the text starts on its FIRST page, canvas y (0: the area's
  // top) -- below a title laid there.
  double                 first_top = 0;
  FlowStyle              body;
  FlowStyle              title;    // a "# " heading
  FlowStyle              heading;  // a "## " one
  // A quotation's bar.
  std::array<double, 4>  accent{0.13, 0.13, 0.13, 1};
  // Space between paragraphs, pixels (< 0: by the body's size and line
  // spacing -- clearly more than between its lines).
  double                 paragraph_gap = -1;
  // Each heading (or title) starts a new page, unless it is the page's
  // first block.
  bool                   page_per_heading = false;
  // At most this many pages: the rest is left (FlowResult::left).
  int                    max_pages = 200;
};

struct FlowPage {
  // Markup objects (markup.h), without ids, drawn in this order.
  Json        objects = Json::array();
  // Its first words, and where its text ends: the last column used (from
  // 0) and the canvas y it reaches there.
  std::string starts;
  int         last_column = 0;
  double      ends_at = 0;
};

struct FlowResult {
  std::vector<FlowPage> pages;
  // Each heading (title or not) and the page (from 0) it is on.
  struct Heading {
    std::string text;
    int         page = 0;
    bool        title = false;
  };
  std::vector<Heading>  headings;
  // Bytes of text not placed (max_pages reached).
  std::size_t           left = 0;
};

FlowResult flow_text(const FlowSpec&);

}  // namespace valtz::media

#endif
