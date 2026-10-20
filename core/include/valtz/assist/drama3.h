#ifndef VALTZ_ASSIST_DRAMA3_H
#define VALTZ_ASSIST_DRAMA3_H

// FISH AUDIO DRAMA 3's prompts (DESIGN §4g). Drama 3 reads its SCRIPT as
// written: directions in square brackets where they act, sounds and
// pauses where they happen, paired tags round the words they change, and
// speaker tags (<|speaker:0|> .. <|speaker:15|>) starting each speaker's
// lines -- its voices cloned from a clip AND the words said in it. Fish
// Audio's own guide (3rd-party/fish-audio-drama3) writes a scene with
// NAMES, "Narrator: [calm] The ship ...", and says the request sends a
// speaker tag in each name's place: Valtz does that at Start, so the
// prompt box reads as the guide does.

#include "valtz/assist/transcript.h"

#include <string>
#include <string_view>
#include <vector>

namespace valtz::assist {

// The most speakers one text may carry.
inline constexpr int kDrama3Speakers = 16;

struct Drama3Script {
  std::string              text;      // as the model reads it
  // The names of a scene, in the order they first speak: speaker 0 is
  // the first. Empty for a text of one voice, or one tagged already.
  std::vector<std::string> names;
};

// `prompt` as Drama 3 reads it. A SCENE -- every line "Name: line", a
// name of one to three words (32 bytes at most; no brackets, no
// sentence's punctuation), a full-width colon too, a line without a name
// going on with the speaker before -- has each name made its speaker's
// tag, in the order the names first speak. It is taken for a scene only
// when it is surely one: a name speaks twice, or there are as many
// `voices` as names (two at least) -- "Step one: ... Step two: ..." is
// spoken as written. A text with speaker tags of its own is as written,
// and so is any other. Lines are kept as lines.
Drama3Script drama3_script(std::string_view prompt, int voices = 0);

// A voice's WORDS, for Drama 3 to clone it by: what is said in its first
// seconds -- the transcript's lines from its start, whole, as long as
// they end within `keep` seconds -- and how long the clip is cut to (the
// last line's end, and a little of the silence after it, before the next
// line). A first line longer than `keep` is taken alone, its clip as
// long as it. Empty words: nothing said.
struct Drama3Voice {
  std::string text;
  double      seconds = 0;
};
Drama3Voice drama3_voice(const Transcript&, double keep = 12.0);
// The same from a transcript asset's data (to_json's form).
Drama3Voice drama3_voice(const Json& transcript, double keep = 12.0);

// Fish Audio's Drama 3 guide (its documentation page, MDX) as the
// enhancer's model reads it: plain text -- the front matter, imports and
// components left out; each audio example the line it speaks, each
// warning a note; and of its sections only those about directing a script
// (not "What's new", "Voice cast", "How to use", nor its shell example:
// they are about Fish Audio's web service).
std::string drama3_guide(std::string_view mdx);

}

#endif
