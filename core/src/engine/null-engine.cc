#include "valtz/engine/engine.h"

namespace valtz::engine {

namespace {

class NullEngine final : public Engine {
public:
  std::string description() const override { return "none"; }
  bool available() const override { return false; }
  bool supports(std::string_view) const override { return false; }

  Status
  submit(JobSpec spec, JobSink sink) override
  {
    JobEvent ev;
    ev.job = spec.id;
    ev.kind = JobEventKind::Failed;
    ev.error = Code::Unsupported;
    ev.text = "this build of Valtz has no execution engine";
    sink(ev);
    return ok_status();
  }

  Status cancel(JobId) override { return ok_status(); }
  void shutdown() override {}
};

}

std::unique_ptr<Engine>
make_null_engine()
{
  return std::make_unique<NullEngine>();
}

}
