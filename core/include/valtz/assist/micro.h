// THE MICRO AGENT's tasks (DESIGN §9c): what Valtz asks its small model
// (the tier's first, LFM3-VL 2B) and how it reads the reply. Each task is
// short -- a prompt of a few hundred tokens, a reply of a line or a few
// sections -- asked without thinking, the model's own sampler, so a
// reply is read leniently: a title's quotes, a "Title:" before it, a
// section's labels in another case, are the model's and not the task's.
// A reply that does not read is no reply: the caller keeps what it had
// (the prompt's words as its name; the refine's words made by rule).

#ifndef VALTZ_ASSIST_MICRO_H
#define VALTZ_ASSIST_MICRO_H

#include <string>
#include <string_view>
#include <vector>

namespace valtz::assist {

// A SHORT NAME for a prompt and what is made from it: the request, its
// words as the model reads them (Valtz's segments and Markdown left out,
// a row's tags as "picture 1", "clip 1", "sound 1").
std::string micro_name_request(std::string_view prompt);
// The reply's title: its first line, quotes and a label before it and
// end punctuation taken off, at most 64 bytes on a character; "" when it
// is none (empty, a sentence of more than ten words, the prompt itself
// given back).
std::string micro_name(std::string_view reply);

// A Ref2VA clip's prompt REWRITTEN FOR ITS 2K PASS (DESIGN §4q): the
// refine runs FL2VA, which reads no references, so what <Picture 1>,
// <Picture 2>, ... are is said in words. The model is asked only what it
// sees -- each picture's subject as a casting note says it, a noun
// phrase of its look, not its pose or place (a 2B model asked to rewrite
// the whole prompt described the picture's moment and lost the prompt's
// action) -- the pictures shown to it ahead of the text, `pictures` of
// them, in their order; Valtz puts the phrases in the labels' place.
std::string micro_refine_request(int pictures);
// The reply's phrase for each of `pictures` ("Picture 2: a red fox
// ..."), "" where it gave none: a clause naming a pose or a place
// ("curled up", "in the snow") left out, an article put before it ("a
// red fox with ...").
std::vector<std::string> micro_pictures(std::string_view reply,
                                        int pictures);
// `prompt` (as its model read it) with each <Picture N> its phrase --
// capitalized where a sentence starts -- then as the refine reads it
// (refine_prompt: the base guide's three sections, the other labels
// words); "" when no phrase was given.
std::string micro_refine_words(std::string_view prompt,
                               const std::vector<std::string>& pictures);

// A TRAINING SAMPLE CAPTIONED (the LoRA trainer, DESIGN §8c): the model
// shown the sample -- a picture, or `frames` frames of a clip in order --
// and told what the trigger word stands for (`meaning`, the person's
// words), asked for one caption that names the subject by the trigger
// and says everything BUT its own look: what it does, where, the
// setting, the light, the framing -- the look is what the trigger is to
// learn, and a caption that tells it teaches the words, not the word.
std::string training_caption_request(std::string_view trigger,
                                     std::string_view meaning, int frames);
// The reply's caption: its first paragraph, quotes and a "Caption:"
// before it taken off, the trigger written as given where the model
// changed its case, at most 600 bytes on a character; "" when none (no
// words, or the trigger not in it and the subject not there -- the
// model said it is absent: kept as it said, the trigger left out).
std::string training_caption(std::string_view reply,
                             std::string_view trigger);

}

#endif
