#ifndef VALTZ_ASSIST_TRANSCRIPT_H
#define VALTZ_ASSIST_TRANSCRIPT_H

// A sound TRANSCRIBED (DESIGN §4h): what was said, line by line -- the
// speech model's (Qwen3-ASR) words for each stretch the voice detector
// (Silero) found -- and the sound EVENTS the tagger (BEATs, AudioSet's
// labels) heard, window by window, joined into spans. What a transcript
// asset holds is the SUMMARY of both, as text a person reads and edits,
// beside them as data (AssetVersion::outputs "transcript").

#include "valtz/base/json.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::assist {

// One stretch of speech: seconds from the sound's start, its words, and
// the language they were heard in (Qwen3-ASR's own word for it).
struct TranscriptLine {
  double      start = 0;
  double      end = 0;
  std::string text;
  std::string language;
};

// A sound heard over a span: AudioSet's label, its best score there.
struct SoundEvent {
  double      start = 0;
  double      end = 0;
  std::string label;
  double      score = 0;
};

struct Transcript {
  std::vector<TranscriptLine> lines;
  std::vector<SoundEvent>     events;
  double                      seconds = 0;  // the sound's length
  bool                        tagged = false;  // the tagger ran
};

// A transcriber's beat ({text, start_us, end_us}) as a line -- its
// "language X<asr_text>" lead-in, Qwen3-ASR's, taken off as its language;
// none for one without words.
std::optional<TranscriptLine> transcript_line(const Json& beat);

// The tagger's windows ({timestamp_us, duration_us, tags: [{label,
// score}]}) as EVENTS: each label scoring `threshold` or more, its
// windows joined where they touch or overlap, its best score kept; in
// order of start, then score.
std::vector<SoundEvent> sound_events(const std::vector<Json>& windows,
                                     double threshold = 0.3);

// The summary, in Markdown (the Prompt Editor draws it as it reads; as
// plain text it reads the same): a heading naming `name`, the sound's
// length and the languages heard; its speech line by line with each
// stretch's time -- "(no speech)" when nothing was said; then the sounds
// heard under their own heading, left out when the tagger did not run.
std::string transcript_summary(std::string_view name, const Transcript&);

Json to_json(const Transcript&);

}

#endif
