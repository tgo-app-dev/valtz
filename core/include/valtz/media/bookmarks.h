// A sound's or a clip's BOOKMARKS (DESIGN §4w): the moments its words
// tell -- a transcript's lines of speech and the sounds heard in it
// (DESIGN §4h), a video summary's scenes (§4i) -- read from the data those
// text assets keep (their version's outputs "transcript", "summary").
//
// ATTACHED to a sound or a clip (project::Asset::bookmarks), as beats are
// (media/beats.h), they go with it onto every timeline: each layer showing
// it has them where they play -- through its marks, its speed, its start
// (bookmarks_on_timeline) -- for the timeline to mark, step through and
// snap to.

#ifndef VALTZ_MEDIA_BOOKMARKS_H
#define VALTZ_MEDIA_BOOKMARKS_H

#include "valtz/base/json.h"
#include "valtz/media/timing.h"

#include <string>
#include <string_view>
#include <vector>

namespace valtz::media {

struct Bookmark {
  // What it marks: a line spoken, a sound heard (AudioSet's label), a
  // scene seen.
  enum class Kind { Speech, Sound, Scene };
  Kind        kind = Kind::Speech;
  // Seconds -- of its source, or of a timeline once placed -- where it
  // starts and ends (an end before its start: none).
  double      start = 0;
  double      end = 0;
  // Its words: the line, the label, the scene told.
  std::string text;
};

// "speech", "sound", "scene".
std::string_view kind_name(Bookmark::Kind);

// A transcript's ({"lines": [{start, end, text}], "events": [{start, end,
// label}]}): each line spoken and each sound heard, in order of start.
std::vector<Bookmark> bookmarks_from_transcript(const Json&);
// A video summary's ({"scenes": [{start, end, text}]}): each scene.
std::vector<Bookmark> bookmarks_from_summary(const Json&);

// Where `marks` -- in seconds of a layer's SOURCE -- fall on its timeline:
// those that START inside the span it shows (its marks), at the timeline
// second the layer's speed brings each to, each end at most where the
// layer ends; in order. A layer of no time (a picture) has none.
std::vector<Bookmark> bookmarks_on_timeline(
    const std::vector<Bookmark>& marks, const LayerTiming& timing);

}

#endif
