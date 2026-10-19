#ifndef VALTZ_ASSIST_MOSS_IMAGE_H
#define VALTZ_ASSIST_MOSS_IMAGE_H

// MOSS-IMAGE's prompts, as its makers' prompt-enhancer skill writes them
// (3rd-party/moss-image: SKILL.md, references/layout.md). The skill has a
// model write a DRAFT -- {"caption": ...} for an ordinary picture, or a
// LAYOUT where words, panels or what lies where matter -- and a script
// of theirs (scripts/compile_prompt.py) compile it into the prompt the
// model reads. Valtz runs no script: this is that compiler, and the
// enhancer's reply is compiled here (DESIGN §9).
//
// A LAYOUT draft: {width, height, bbox_coord_space "abs", bbox_format
// "xyxy", summary {caption, meta?, canvas?, palette?}, items [{label
// object | character | text, caption, text?, bbox [x0, y0, x1, y1] --
// pixels from the canvas's top left -- tags?, palette?}]}. Compiled, it
// is ONE line of JSON -- {summary, items, bbox_coord_space
// "abs_centralized", bbox_format "yxyx"} -- each box integer pixels
// about the canvas's centre in y-x order, its edges enclosed (left and
// top floored, right and bottom ceiled), and no canvas: the model reads
// the boxes in the pixels of the picture it makes, so a layout is made
// for ONE size, the picture's.

#include "valtz/base/json.h"
#include "valtz/base/result.h"

#include <string>
#include <string_view>

namespace valtz::assist {

// The draft compiled for a `width` x `height` picture (multiples of 16).
// A caption draft is its caption. A layout is compiled as the script
// compiles it, with the keys in the skill's order. Where the script
// refuses, a small helper's slip is mended rather than the reply lost --
// it has no second try: boxes laid out on another canvas (the draft's
// own width and height) are scaled to this one, and boxes given in
// fractions of the canvas made pixels; a box past the edge is clipped
// to it; an item with no box left, or no caption, is dropped; a text
// item without its words is an object; unknown fields are left out.
// What cannot be mended -- no summary caption, no item left -- is an
// error.
Result<std::string> moss_image_prompt(const Json& draft, int width,
                                      int height);

// A compiled layout's description (its summary's caption), or "" when
// `prompt` is not one: what a result made from it is named after.
std::string moss_layout_caption(std::string_view prompt);

}

#endif
