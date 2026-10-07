// Valtz's side of the valtz-vpipe plugin (vpipe-plugin/): reading a
// `valtz-sink` and writing a `valtz-source` through vpipe's public
// stage-command API. The vocabulary is valtz-vpipe/exchange.h, which the
// plugin compiles too.
//
// OUT (SinkReader). A tensor read from a sink is a VIEW of the engine's
// own bytes: Tensor::data points into the beat, Tensor::owner is the
// reply buffer's owner -- the beat itself -- and Tensor::mtl_buffer is
// the Metal buffer holding it, when there is one. The sink never touches
// a beat it handed out, so the bytes stay valid and unchanged until the
// last TensorPtr goes, on whatever thread that is, even after the
// pipeline stopped, unloaded, or the session is gone (DESIGN §15 item 0).
//
// IN (SourceWriter). lease() hands out a writable beat in engine memory
// (a Metal shared buffer: the GPU can fill it); commit() emits it with
// no copy. push() copies a Tensor in, emit() sends JSON.
//
// Every call is thread-safe (vpipe's command handles are) and none
// throws. A SinkReader runs on its own thread; a SourceWriter blocks the
// calling thread until the stage replies -- which a pipeline stop always
// ends.

#ifndef VALTZ_ENGINE_VPIPE_HOST_EXCHANGE_H
#define VALTZ_ENGINE_VPIPE_HOST_EXCHANGE_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/engine/tensor.h"

#include "vpipe/pipeline-handle.h"
#include "vpipe/stage-command.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace valtz::engine::vp {

// Where valtz-vpipe.so is: $VALTZ_VPIPE_PLUGIN when set; else the app
// bundle's Contents/PlugIns, then beside the executable, then (dev
// builds) the build tree. Empty when none of those exists.
std::filesystem::path find_plugin();

// Whether the vpipe this engine runs on would load an extension's plugin
// built for plugin ABI `abi` and requiring `required` host features: ""
// when it would; else "abi" (outside VPIPE_PLUGIN_ABI_OLDEST..VERSION) or
// "feature", with `*detail` the version window or the feature missing.
std::string backend_refusal(int abi,
                            const std::vector<std::string>& required,
                            Json* detail);

// One beat from a valtz-sink.
struct SinkBeat {
  std::uint64_t seq = 0;      // 1-based index into the sink this run
  std::uint64_t dropped = 0;  // beats dropped (policy latest) before it
  TensorPtr     tensor;       // null for data beats, and for element
                              // types Tensor has no DType for (i8)
  Json          meta;         // the tensor's sideband, or the data beat
};

// A replied `next` as a SinkBeat; *eos is set when the reply was the
// end of the stream (the SinkBeat is then empty).
Result<SinkBeat> sink_beat_from(const vpipe::CommandHandle&, bool* eos);

// Drains one valtz-sink on a thread of its own, handing each beat to
// `fn` in order, until the sink reports end of stream or the pipeline
// stops. Construct it after the pipeline is launched.
class SinkReader {
public:
  using Fn = std::function<void(SinkBeat&&)>;

  SinkReader(vpipe::StageHandle stage, Fn fn);
  ~SinkReader();  // joins

  SinkReader(const SinkReader&) = delete;
  SinkReader& operator=(const SinkReader&) = delete;

  void join();
  // After join(): the sink said end of stream (rather than being stopped
  // or refusing), and if not, why.
  bool reached_eos() const { return _eos; }
  const std::string& ended_by() const { return _why; }

private:
  void run_();

  vpipe::StageHandle _stage;
  Fn                 _fn;
  bool               _eos = false;
  std::string        _why;
  std::thread        _thread;
};

// A valtz-tap's count (exchange.h): frames gone on past it, frames cut
// past its max_frames, and whether its input has ended.
struct TapCount {
  std::uint64_t passed = 0;
  std::uint64_t cut = 0;
  bool          eos = false;
};
// Blocks until `frames` frames have gone by the tap, or its stream ended
// first (`eos`), or the pipeline stopped (an error). How a host feeds a
// graph by what has come out of it. `tick` runs every 100 ms while it
// waits -- a job's progress, its cancel -- and false gives up (Cancelled:
// the command is closed, the tap forgets it).
Result<TapCount> tap_wait(vpipe::StageHandle tap, std::uint64_t frames,
                          const std::function<bool()>& tick = {});

class SourceWriter {
public:
  explicit SourceWriter(vpipe::StageHandle stage) : _stage(stage) {}

  // A beat being filled. commit() -- or destruction -- closes the
  // command, which emits the beat; with no copy, because the Lease keeps
  // no view of its own (a kept view would make the stage copy).
  class Lease {
  public:
    Lease() = default;
    Lease(Lease&&) noexcept = default;
    Lease& operator=(Lease&&) noexcept = default;

    std::uint8_t* data() const { return _data; }
    std::size_t   size() const { return _size; }
    // In-process: the MTL::Buffer* to write on the GPU (null when the
    // beat is CPU memory), and the byte offset of data() in it.
    void*         mtl_buffer() const { return _mtl; }
    std::size_t   mtl_offset() const { return _mtl_offset; }
    std::uint64_t seq() const { return _seq; }

    void commit() { _cmd.close(); }

  private:
    friend class SourceWriter;
    vpipe::CommandHandle _cmd;
    std::uint8_t*        _data = nullptr;
    std::size_t          _size = 0;
    void*                _mtl = nullptr;
    std::size_t          _mtl_offset = 0;
    std::uint64_t        _seq = 0;
  };

  // `storage`: shared (a Metal buffer, the default) or CPU memory.
  Result<Lease> lease(Tensor::DType, const std::vector<std::int64_t>& shape,
                      const Json& sideband = Json::object(),
                      bool shared = true);
  // A copy of `t` (any strides) as one beat; returns once the source's
  // oport has taken it.
  Status push(const Tensor& t, const Json& sideband = Json::object());
  Status emit(const Json& value);
  Status finish();

private:
  vpipe::StageHandle _stage;
};

}

#endif
