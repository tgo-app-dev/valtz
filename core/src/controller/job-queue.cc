#include "valtz/controller/job-queue.h"

#include <algorithm>

namespace valtz {

const char*
to_str(JobState s)
{
  switch (s) {
  case JobState::Queued:    return "queued";
  case JobState::Running:   return "running";
  case JobState::Finished:  return "finished";
  case JobState::Failed:    return "failed";
  case JobState::Cancelled: return "cancelled";
  }
  return "?";
}

void
JobTable::add(JobRecord r)
{
  std::lock_guard lk(_mu);
  _jobs[r.id] = std::move(r);
}

std::optional<JobRecord>
JobTable::get(JobId id) const
{
  std::lock_guard lk(_mu);
  auto it = _jobs.find(id);
  if (it == _jobs.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<JobRecord>
JobTable::list() const
{
  std::vector<JobRecord> out;
  {
    std::lock_guard lk(_mu);
    for (const auto& [_, r] : _jobs) {
      out.push_back(r);
    }
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
    return a.id < b.id;  // v7 ids sort by creation time
  });
  return out;
}

Json
JobTable::to_json() const
{
  Json arr = Json::array();
  for (const auto& r : list()) {
    arr.push_back(r);
  }
  return arr;
}

void
to_json(Json& j, const JobRecord& r)
{
  j = {
    {"id", r.id},
    {"op", r.op},
    {"purpose", r.purpose},
    {"title", r.title},
    {"project", r.project},
    {"asset", r.asset},
    {"state", to_str(r.state)},
    {"progress", r.progress},
    {"message", r.message},
    {"created", r.created_ms},
    {"finished", r.finished_ms},
    {"runner", r.runner.empty() ? "local" : r.runner_name},
  };
}

}
