#ifndef VALTZ_ASSIST_MAGI2_H
#define VALTZ_ASSIST_MAGI2_H

// MAGI-2-PREVIEW's prompts, as its makers' prompt enhancement writes them
// (3rd-party/magi2: prompts/t2v.md for a clip from words, prompts/i2v.md
// for one opening on a picture). Their enhancer sends the template, the
// request in its place, as ONE user message -- no system message, the
// picture beside it for i2v -- and asks for a JSON object of their schema,
// which it renders to Markdown (pe_markdown.py) for the model to read: top-
// level keys as headings, arrays of objects as numbered subsections, every
// key kept, empty values dropped. Valtz runs no script: this is that
// request and that renderer, the enhancer's reply rendered here (DESIGN
// §9).

#include "valtz/base/result.h"

#include <string>
#include <string_view>

namespace valtz::assist {

// The template (`i2v`: the picture one) with `request` in its place and no
// variation seed, as enhancer.py fills it: what the helper is sent.
std::string magi2_request(bool i2v, std::string_view request);

// The reply's JSON object rendered as pe_markdown.render_markdown renders
// it with enhancer.py's options -- empty values dropped, keys kept, strings
// unquoted, their newlines written \n, a primitive list on one line joined
// by " | ", scalar fields before nested ones, headings to six levels, then
// italic labels -- in the reply's own key order. Read leniently, as the
// helper has no schema to hold it: prose or a code fence around the object
// skipped. Refused where no object is found, or one with none of the
// schema's top-level keys (`i2v` says which); one missing some rendered as
// it is.
Result<std::string> magi2_prompt(std::string_view reply, bool i2v);

}

#endif
