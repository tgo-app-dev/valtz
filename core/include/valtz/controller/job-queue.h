// The controller's view of jobs: what was asked, for which project and
// asset, and where it stands. The engine owns execution; this table is
// what the UI lists and what an engine event is matched back to.

#ifndef VALTZ_CONTROLLER_JOB_QUEUE_H
#define VALTZ_CONTROLLER_JOB_QUEUE_H

#include "valtz/base/id.h"
#include "valtz/base/json.h"
#include "valtz/project/records.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace valtz {

enum class JobState : std::uint8_t {
  Queued, Running, Finished, Failed, Cancelled,
};

const char* to_str(JobState);

struct JobRecord {
  JobId                      id;
  std::string                op;         // engine op, or "import"
  std::string                purpose;    // "enhance", "intent", ...
  std::string                title;
  ProjectId                  project;
  AssetId                    asset;      // the derived asset it builds
  project::Recipe            recipe;     // as submitted
  std::vector<project::ResolvedInput> inputs;
  std::string                model_digest;
  std::string                destination;  // an export's file
  // The files an export wrote where they were asked to go: a still's
  // pages, each beside `destination` with its number.
  std::vector<std::string>   outputs;
  // The base (or a clip's first frame) as the model got it, when it was
  // rendered for it -- a look of its own: kept for the history.
  project::BlobRef           rendered_base;
  // Where it runs: "" this Mac; a fleet member's id, its name, its
  // engine (DESIGN §11). A job this Mac serves for one: "fleet" its
  // purpose, `runner_name` the member it is for.
  std::string                runner;
  std::string                runner_name;
  std::string                runner_engine;
  JobState                   state = JobState::Queued;
  float                      progress = 0.0f;
  std::string                message;
  std::int64_t               created_ms = 0;
  std::int64_t               finished_ms = 0;
};

class JobTable {
public:
  void add(JobRecord);
  std::optional<JobRecord> get(JobId) const;
  // Apply `fn` to the record under the lock; false if absent.
  template <class Fn>
  bool
  update(JobId id, Fn&& fn)
  {
    std::lock_guard lk(_mu);
    auto it = _jobs.find(id);
    if (it == _jobs.end()) {
      return false;
    }
    fn(it->second);
    return true;
  }
  std::vector<JobRecord> list() const;
  Json to_json() const;

private:
  mutable std::mutex                       _mu;
  std::unordered_map<JobId, JobRecord>     _jobs;
};

void to_json(Json&, const JobRecord&);

}

#endif
