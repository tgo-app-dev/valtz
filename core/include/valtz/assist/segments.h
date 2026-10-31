// VALTZ'S OWN SEGMENTS of a prompt (DESIGN §4q): what is written for
// Valtz rather than for the model, each after a header of Valtz's at the
// start of a line -- "regenerate_prompt:" the words a clip's regenerate
// pass reads (a Ref2VA clip's refine runs FL2VA, which reads no
// references). The model is sent the text before the first header; each
// segment runs from its header -- the rest of that line -- to the next
// header or the end. Headers are matched whole, in any case, a colon
// after them. A person writes one, the enhancer writes one, and later a
// small model of Valtz's own may (a micro agent's text task): each
// reads and writes them through here.

#ifndef VALTZ_ASSIST_SEGMENTS_H
#define VALTZ_ASSIST_SEGMENTS_H

#include <map>
#include <string>
#include <string_view>

namespace valtz::assist {

// The words a clip's regenerate pass reads in place of the model's.
inline constexpr std::string_view kRegeneratePrompt = "regenerate_prompt";

struct PromptSegments {
  std::string                        body;      // what the model reads
  std::map<std::string, std::string> segments;  // by header, trimmed
};

// `prompt` split into its body and Valtz's segments. A prompt with none
// is all body, as written (trailing space and all).
PromptSegments prompt_segments(std::string_view prompt);

// `body`, then each segment (an empty one left out) under its header, a
// blank line before each: what prompt_segments reads back.
std::string with_segments(std::string_view body,
                          const std::map<std::string, std::string>&);

}

#endif
