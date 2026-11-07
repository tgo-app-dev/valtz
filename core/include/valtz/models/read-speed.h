// How fast a model's FILES COME OFF THE DRIVE they are on (DESIGN §8b;
// Settings > Capabilities' Test Read Speed, `valtzctl read-speed`): what
// loading it is bounded by -- a preloaded model's weights are its files
// read whole, a streamed one's blocks are read again every step -- and
// so what keeping it on another drive costs (DESIGN §5a: USB 3 carries
// 10 Gb/s at most).
//
// AS THE ENGINE READS IT. A checkpoint the engine reads with a reader
// of its own (`reader`: vpipe's pread_into -- 1 MB pieces, four in
// flight, on the file's 4 KB grid, a tensor off the grid staged and
// copied out) is read through THAT, tensor by tensor in the file's order
// as a load reads them, into a page-aligned buffer as a Metal buffer is:
// the figure is what a load gets, staging and all. A drive holding no
// such file (a Draw Things checkpoint, a GGUF: converted as they load)
// is read raw, `streams` runs at once a `chunk` at a time.
//
// PAST THE MEMORY CACHE. Read with F_NOCACHE (the engine's reads too),
// so the drive serves every byte and nothing read stays in memory: a
// test of a 60 GB model on a 24 GB Mac evicts nothing of what runs.
// Pages ALREADY in memory -- a model just used -- would come from memory
// at memory's speed whatever F_NOCACHE says, so they are found first
// (mincore on a mapping of each file) and left out: counted as
// `cached`, not read, not timed -- a tensor with more than 1% of its
// pages in memory skipped whole, as vpipe's own benchmark does.
//
// SAMPLED. At most `max_bytes` or `max_seconds`, whichever comes first,
// spread evenly through what is not in memory -- a drive's first
// gigabytes may be faster (an SSD's cache) or its last ones slower --
// tensors whole, raw reads in runs of `run` bytes. A model whose files
// lie on several drives (its parts linked from another) is measured a
// drive at a time, each its share of the budget: one figure each, and
// the whole's.

#ifndef VALTZ_MODELS_READ_SPEED_H
#define VALTZ_MODELS_READ_SPEED_H

#include "valtz/base/json.h"
#include "valtz/base/paths.h"
#include "valtz/models/checkpoint-reader.h"
#include "valtz/models/model-store.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace valtz::models {

struct ReadSpeedOptions {
  std::uint64_t max_bytes = 16'000'000'000ull;
  double        max_seconds = 10.0;
  int           streams = 4;
  std::size_t   chunk = 8u << 20;
  std::uint64_t run = 256ull << 20;
  // The engine's reader; none: every file read raw.
  CheckpointOpener reader;
  // A tensor past this is left out of the sample: its buffer.
  std::uint64_t max_tensor = 1ull << 30;
  // Asked between chunks and tensors: true stops the test where it is.
  std::function<bool()> stop;
};

// One drive's files and how fast they came off it.
struct ReadSpeedPlace {
  std::filesystem::path path;    // the folder its files share
  VolumeInfo            volume;
  std::uint64_t         total = 0;    // its files' bytes
  std::uint64_t         cached = 0;   // in memory already: not read
  std::uint64_t         read = 0;     // read off the drive
  std::uint64_t         engine = 0;   // of it, by the engine's reader
  double                seconds = 0;
  int                   files = 0;
  bool                  whole = false;   // all that was not in memory
  // Decimal gigabytes a second, as Valtz shows sizes; 0 when nothing
  // was read.
  double gb_per_s() const;
};

struct ReadSpeed {
  std::vector<ReadSpeedPlace> places;
  std::uint64_t total = 0, cached = 0, read = 0, engine = 0;
  double        seconds = 0;
  int           files = 0;
  bool          whole = false;
  bool          stopped = false;
  std::string   error;    // a file that would not open or read
  double gb_per_s() const;
  // {"total", "cached", "read", "engine", "seconds", "gbps", "files",
  //  "whole", "stopped", "error", "places": [{"path", "volume": {"name",
  //  "protocol", "device", "internal", "network"}, "total", "cached",
  //  "read", "engine", "seconds", "gbps", "files", "whole"}]}
  Json to_json() const;
};

ReadSpeed measure_read_speed(const std::vector<std::filesystem::path>& files,
                             const ReadSpeedOptions& opt = {});

// The files a load of an installed model reads: its one file, or every
// file in its folder; and its parts linked apart (Draw Things'), those
// there.
std::vector<std::filesystem::path> files_to_load(const InstallInfo& info);

}  // namespace valtz::models

#endif
