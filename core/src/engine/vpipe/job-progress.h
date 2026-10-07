// A job's PHASE, read off vpipe's live progress reports.
//
// vpipe's stages report progress as state -- a denoise counted per DiT
// block, a VAE decode per tile, a download per byte -- into its session's
// report list, which its console footer and web UI draw and a host reads
// with SessionIntf::progress(). The engine polls that list while a graph
// runs and turns it into Progress events with Valtz's phase names
// (engine.h, kPhase*), so the app hears as often as vpipe's own UI does
// rather than once per denoise step.
//
// The list is the SESSION's: reports from before this job (ids up to
// `after`) are not its. Of the live ones it follows the oldest still
// counting -- a decode and its soundtrack can run side by side, and
// following the most recently moved would flicker between them -- and,
// when every one has reached its total, the newest. With nothing live the
// phase is PREPARE until the job has denoised and FINISH after; a
// download's is DOWNLOAD, uncounted (vpipe counts only its big files).
//
// A clip's WRITER (avf-save-video's "save video") counts its frames:
// an export's phase is that (EXPORT); in a generation the frames are
// written as they are decoded, and after the decode it is FINISH.
//
// A SONG (generate-audio) reports its three passes under one name,
// "song": planning the score and writing the song, uncounted -- the
// model decides when each ends; the detail says how far ("planning
// score: 412 tokens", "writing the song: 42.3 s"), which becomes `made`
// -- then the flow matching, counted by step: a denoise.
//
// SPEECH (text-to-speech) reports "speech": the utterance spoken, its
// seconds so far in the detail ("speaking: 4.2 s", `made`) -- counted
// when a length was asked for -- then "decoding": its sound.
//
// BETWEEN COUNTS, AN ESTIMATE. vpipe's finest unit can still be long: a
// DiT block is one attention dispatch over the whole clip, and a long clip
// on a bounded box spends 20 s and more in one -- over a minute per
// percent. So the phase also carries `estimate`: the count plus the way
// into the next unit at the recent PACE (units per second over the last
// minute), which the engine refreshes every second. It follows the pace
// to 80% of a unit and then only approaches the next count, never
// reaching it: a unit that runs long slows the estimate rather than
// walking it past the truth, and the next real count meets or passes it,
// so it never goes backwards.
//
// GROUPS. An upscale's writer counts a group's frames at once (FlashVSR's
// 21; in pieces, a frame or two behind), so frame by frame the bar stood
// still for a group and jumped 17% on a 5 s clip. Its restore is
// estimated by GROUP instead: from the last group boundary its count
// crossed toward the next, at the pace of the last group (the time
// between the last two crossings) -- never past the next boundary, the
// total, or backwards; a stray piece of a count does not restart it.
// Before two crossings there is no pace (the first group carries the
// load): the count alone.

#ifndef VALTZ_ENGINE_VPIPE_JOB_PROGRESS_H
#define VALTZ_ENGINE_VPIPE_JOB_PROGRESS_H

#include "valtz/base/json.h"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>

namespace valtz::engine::vp {

struct JobPhase {
  std::string   phase;   // kPhase*
  std::uint64_t done = 0;
  std::uint64_t total = 0;  // 0: indeterminate
  std::string   detail;  // vpipe's right-hand text ("1.2 / 3.0 GB")
  std::string   label;   // a download's file; a "work" phase's own words
  double        elapsed = 0;  // s, the report's age; 0 with none
  double        estimate = -1;  // 0..1 (see above); -1 while uncounted
  double        rate = 0;  // the recent pace, of the whole per s; 0 unknown
  // What an uncounted phase has made so far, in its own measure -- a
  // song's score in tokens, the song in seconds; -1 for none.
  double        made = -1;

  float fraction() const;  // done / total, -1 when indeterminate

  // Still counting: a total, not reached.
  bool counting() const { return total > 0 && done < total; }

  // The same phase, at the same count: no new event. The age and the
  // estimate are left out -- they move on their own.
  bool
  same(const JobPhase& o) const
  {
    return phase == o.phase && done == o.done && total == o.total &&
           detail == o.detail && label == o.label && made == o.made;
  }

  Json data() const;  // a Progress event's data
};

class JobProgress {
public:
  // `after`: the newest report id when the job started (last_id()).
  // `download`: a fetch-model job, whose reports are files; `exporting`
  // an export-media job; `restoring` an upscale, whose frames written
  // are the whole of it (kPhaseRestore) -- each group's own denoise and
  // decode show only before the first frame is out.
  // `group`: an upscale's frames a group, past its overlap -- what its
  // writer's count moves by (see GROUPS above).
  JobProgress(std::uint64_t after, bool download, bool exporting = false,
              bool restoring = false, std::uint64_t group = 0)
      : _after(after), _download(download), _export(exporting),
        _restore(restoring), _group(group)
  {
  }

  // The phase per one SessionIntf::progress() document, read at `now`
  // (seconds, on a steady clock).
  JobPhase read(const Json& doc, double now);

  // The newest report's id in a document; 0 for none. Ids only grow, so
  // every report opened later has a larger one.
  static std::uint64_t last_id(const Json& doc);

private:
  // The pace of the report followed: when its count moved, and to what.
  void track_(std::uint64_t id, const JobPhase& p, double now);
  void track_groups_(const JobPhase& p, double now);
  bool grouped_(const JobPhase& p) const;
  void estimate_(JobPhase& p, double now) const;

  std::uint64_t _after;
  bool          _download;
  bool          _export;
  bool          _restore;
  std::uint64_t _group;
  bool          _past = false;  // denoised
  std::uint64_t _followed = 0;  // its id
  std::uint64_t _total = 0;
  std::deque<std::pair<double, std::uint64_t>> _moves;  // (when, done)
  // An upscale's restore: the group boundary its count is past, and when
  // it crossed the last two (when, boundary).
  std::uint64_t _base = 0;
  bool          _based = false;
  std::deque<std::pair<double, std::uint64_t>> _crossed;
};

}

#endif
