#include "valtz/media/reverb.h"

#include "valtz/base/log.h"

#import <Accelerate/Accelerate.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <format>
#include <mutex>
#include <numbers>

namespace valtz::media {

double
reverb_length(const ReverbVoice& v)
{
  return std::clamp(std::max(0.0, v.pre_delay) + std::max(0.0, v.decay),
                    0.0, kMaxReverbLength);
}

std::vector<float>
reverb_impulse(const ReverbVoice& v, double size, int channel, int rate)
{
  const double fs = std::max(1, rate);
  const auto n = std::max<std::size_t>(
      1, static_cast<std::size_t>(std::ceil(reverb_length(v) * fs)));
  const auto pre = std::min<std::size_t>(
      n - 1, static_cast<std::size_t>(std::llround(
                 std::clamp(v.pre_delay, 0.0, kMaxPreDelay) * fs)));
  std::vector<float> h(n, 0.0f);
  // The room's echoes, where they fall and their signs: its own sequence
  // (xorshift64*), one a channel -- the same whatever the decay.
  std::uint64_t state = 0x9E3779B97F4A7C15ull ^
      (static_cast<std::uint64_t>(channel) + 1) * 0xD1B54A32D192ED03ull;
  auto uniform = [&] {
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return static_cast<double>((state * 0x2545F4914F6CDD1Dull) >> 11) *
           0x1.0p-53;
  };
  // How many echoes a second: a few at first (the larger the room, the
  // fewer), every sample one once they have filled in.
  const double s = std::clamp(size, 0.0, 1.0);
  const double first = 6000.0 * std::pow(100.0 / 6000.0, s);
  const double fill = 0.02 * std::pow(15.0, s);
  const double decay = std::max(1e-3, v.decay);
  // -60 dB at the decay; the highs' corner falling from 20 kHz, to
  // 500 Hz by then at full damping.
  const double fade = std::exp(-std::log(1000.0) / (decay * fs));
  const double beta = std::clamp(v.damping, 0.0, 1.0) * std::log(40.0) /
                      decay;
  const double nyquist = 0.45 * fs;
  double env = 1;
  double y1 = 0, y2 = 0, a = 1;
  for (std::size_t i = pre; i < n; ++i) {
    const double t = static_cast<double>(i - pre) / fs;
    const double f = std::min(1.0, (t / fill) * (t / fill));
    const double p = std::min(1.0, (first + (fs - first) * f) / fs);
    double x = 0;
    if (uniform() < p) {
      x = (uniform() < 0.5 ? -1.0 : 1.0) / std::sqrt(p);
    }
    x *= env;
    env *= fade;
    if ((i - pre) % 32 == 0) {
      const double fc = std::clamp(20000.0 * std::exp(-beta * t), 150.0,
                                   nyquist);
      a = 1 - std::exp(-2 * std::numbers::pi * fc / fs);
    }
    y1 += a * (x - y1);
    y2 += a * (y1 - y2);
    h[i] = static_cast<float>(y2);
  }
  double e = 0;
  for (float x : h) {
    e += double(x) * x;
  }
  if (e > 0) {
    const auto k = static_cast<float>(1 / std::sqrt(e));
    for (float& x : h) {
      x *= k;
    }
  }
  return h;
}

namespace {

struct Complex {
  float re, im;
};

struct Pick {
  std::uint32_t a, b;
  float         w;
  std::uint32_t pad;
};

struct Params {
  std::uint32_t bins, parts, ring, first, count, in_blocks;
};

// The sums of products, output block by block: for each output block
// `m` (a row), bin and channel, every send block `k` that reaches it
// times the piece `m - k` of the response it hears -- two mixed by its
// pick's weight.
const char* const kReverbKernel = R"(
#include <metal_stdlib>
using namespace metal;

struct Pick { uint a; uint b; float w; uint pad; };
struct Params { uint bins; uint parts; uint ring; uint first; uint count;
                uint in_blocks; };

kernel void reverb_sum(device const float2* X [[buffer(0)]],
                       device const float2* H [[buffer(1)]],
                       device const Pick* picks [[buffer(2)]],
                       device float2* Y [[buffer(3)]],
                       constant Params& p [[buffer(4)]],
                       uint3 g [[thread_position_in_grid]])
{
  const uint bin = g.x, row = g.y, c = g.z;
  if (bin >= p.bins || row >= p.count) { return; }
  const uint m = p.first + row;
  const uint q0 = m >= p.in_blocks ? m - p.in_blocks + 1 : 0;
  const uint q1 = min(p.parts, m + 1);
  float2 acc = float2(0.0f);
  for (uint q = q0; q < q1; ++q) {
    const uint k = m - q;
    const float2 x = X[(k % p.ring) * p.bins + bin];
    const Pick pk = picks[k];
    float2 h = H[((pk.a * 2 + c) * p.parts + q) * p.bins + bin];
    if (pk.w > 0.0f) {
      const float2 h1 = H[((pk.b * 2 + c) * p.parts + q) * p.bins + bin];
      h = mix(h, h1, pk.w);
    }
    acc += float2(x.x * h.x - x.y * h.y, x.x * h.y + x.y * h.x);
  }
  Y[(c * p.count + row) * p.bins + bin] = acc;
}
)";

// The kernel and a queue, made once (compiling takes a moment); nil
// where Metal cannot.
struct Gpu {
  id<MTLDevice>               device = nil;
  id<MTLCommandQueue>         queue = nil;
  id<MTLComputePipelineState> pipeline = nil;
};

Gpu*
reverb_gpu()
{
  static std::once_flag once;
  static Gpu* made = nullptr;
  std::call_once(once, [] {
    auto* g = new Gpu;  // for the process: kept
    g->device = MTLCreateSystemDefaultDevice();
    if (g->device) {
      NSError* err = nil;
      MTLCompileOptions* opts = [MTLCompileOptions new];
      id<MTLLibrary> lib = [g->device newLibraryWithSource:@(kReverbKernel)
                                                   options:opts
                                                     error:&err];
      id<MTLFunction> fn = [lib newFunctionWithName:@"reverb_sum"];
      g->pipeline = fn ? [g->device newComputePipelineStateWithFunction:fn
                                                                  error:&err]
                       : nil;
      if (!g->pipeline) {
        VALTZ_LOG_WARN("media", "the reverb's kernel did not compile: {}",
                       err ? err.localizedDescription.UTF8String : "?");
      }
      g->queue = g->pipeline ? [g->device newCommandQueue] : nil;
    }
    made = g;
  });
  return made->queue ? made : nullptr;
}

// vDSP's real FFT of 2 * kReverbBlock frames, its bins unpacked: DC and
// Nyquist apart (their imaginary parts 0), kReverbBlock + 1 in all.
class Fft {
public:
  static constexpr std::size_t kB = kReverbBlock;
  static constexpr std::size_t kN = 2 * kB;
  static constexpr std::size_t kBins = kB + 1;

  Fft()
  {
    _setup = vDSP_create_fftsetup(kLog2n, kFFTRadix2);
    _re.resize(kB);
    _im.resize(kB);
    _time.resize(kN);
  }
  ~Fft() {
    if (_setup) { vDSP_destroy_fftsetup(_setup); }
  }
  Fft(const Fft&) = delete;
  Fft& operator=(const Fft&) = delete;

  bool ok() const { return _setup != nullptr; }

  // `n` frames of `x` (the rest of the frame zeros), times `scale`.
  void
  forward(const float* x, std::size_t n, float scale, Complex* out)
  {
    std::fill(_time.begin(), _time.end(), 0.0f);
    std::copy_n(x, std::min(n, kN), _time.begin());
    DSPSplitComplex z{_re.data(), _im.data()};
    vDSP_ctoz(reinterpret_cast<const DSPComplex*>(_time.data()), 2, &z, 1,
              kB);
    vDSP_fft_zrip(_setup, &z, 1, kLog2n, kFFTDirection_Forward);
    out[0] = {_re[0] * scale, 0};
    out[kB] = {_im[0] * scale, 0};
    for (std::size_t k = 1; k < kB; ++k) {
      out[k] = {_re[k] * scale, _im[k] * scale};
    }
  }

  // The 2 * kB frames whose spectrum `in` is (as forward makes it, its
  // scale left to the caller).
  void
  inverse(const Complex* in, float* out)
  {
    _re[0] = in[0].re;
    _im[0] = in[kB].re;
    for (std::size_t k = 1; k < kB; ++k) {
      _re[k] = in[k].re;
      _im[k] = in[k].im;
    }
    DSPSplitComplex z{_re.data(), _im.data()};
    vDSP_fft_zrip(_setup, &z, 1, kLog2n, kFFTDirection_Inverse);
    vDSP_ztoc(&z, 1, reinterpret_cast<DSPComplex*>(out), 2, kB);
  }

private:
  static constexpr vDSP_Length kLog2n = 13;
  static_assert((std::size_t{1} << kLog2n) == kN);
  FFTSetup           _setup = nullptr;
  std::vector<float> _re, _im, _time;
};

}

Result<std::array<std::vector<float>, 2>>
convolve_reverb(const float* send, std::size_t frames,
                const std::vector<std::array<std::vector<float>, 2>>&
                    responses,
                const std::vector<ReverbPick>& picks,
                std::size_t out_frames, bool gpu, ReverbStats* stats)
{
  @autoreleasepool {
    using Clock = std::chrono::steady_clock;
    const auto began = Clock::now();
    constexpr std::size_t B = Fft::kB;
    constexpr std::size_t bins = Fft::kBins;
    std::array<std::vector<float>, 2> out;
    if (responses.empty() || out_frames == 0) {
      out[0].assign(out_frames, 0.0f);
      out[1].assign(out_frames, 0.0f);
      return out;
    }
    Fft fft;
    if (!fft.ok()) {
      return make_error(Code::Internal, "the reverb's FFT is not there");
    }
    std::size_t longest = 1;
    for (const auto& r : responses) {
      longest = std::max({longest, r[0].size(), r[1].size()});
    }
    const std::size_t parts = (longest + B - 1) / B;
    const std::size_t in_blocks = (frames + B - 1) / B;
    const std::size_t out_blocks = (out_frames + B - 1) / B;
    const std::size_t S = responses.size();
    // vDSP's forward gives twice the DFT and its inverse N times the
    // inverse: a product of two forwards comes back 4N times too large.
    const float scale = 1.0f / (4.0f * static_cast<float>(Fft::kN));

    Gpu* g = gpu ? reverb_gpu() : nullptr;
    // Every buffer the GPU reads is shared memory, written in place by
    // the CPU (the CPU's own vectors where Metal cannot run).
    auto buffer = [&](std::size_t bytes) -> id<MTLBuffer> {
      return [g->device newBufferWithLength:std::max<std::size_t>(16, bytes)
                                    options:MTLResourceStorageModeShared];
    };
    std::vector<Complex> h_cpu, x_cpu, y_cpu;
    std::vector<Pick> pick_cpu;
    id<MTLBuffer> hb = nil, xb = nil, yb = nil, pb = nil;
    constexpr std::size_t kChunk = 64;
    const std::size_t ring = parts + kChunk;
    const std::size_t h_count = S * 2 * parts * bins;
    if (g) {
      hb = buffer(h_count * sizeof(Complex));
      xb = buffer(ring * bins * sizeof(Complex));
      yb = buffer(2 * kChunk * bins * sizeof(Complex));
      pb = buffer(std::max<std::size_t>(1, in_blocks) * sizeof(Pick));
      if (!hb || !xb || !yb || !pb) {
        g = nullptr;
      }
    }
    if (!g) {
      h_cpu.assign(h_count, {0, 0});
      x_cpu.assign(ring * bins, {0, 0});
      y_cpu.assign(2 * kChunk * bins, {0, 0});
      pick_cpu.assign(std::max<std::size_t>(1, in_blocks), {});
    }
    auto* H = g ? static_cast<Complex*>(hb.contents) : h_cpu.data();
    auto* X = g ? static_cast<Complex*>(xb.contents) : x_cpu.data();
    auto* Y = g ? static_cast<Complex*>(yb.contents) : y_cpu.data();
    auto* P = g ? static_cast<Pick*>(pb.contents) : pick_cpu.data();

    // The responses' pieces, each FFT'd once (scaled for the round trip).
    for (std::size_t s = 0; s < S; ++s) {
      for (int c = 0; c < 2; ++c) {
        const auto& h = responses[s][c];
        for (std::size_t q = 0; q < parts; ++q) {
          Complex* to = H + ((s * 2 + c) * parts + q) * bins;
          const std::size_t at = q * B;
          if (at >= h.size()) {
            std::fill_n(to, bins, Complex{0, 0});
            continue;
          }
          fft.forward(h.data() + at, std::min(B, h.size() - at), scale, to);
        }
      }
    }
    for (std::size_t k = 0; k < in_blocks; ++k) {
      ReverbPick pk = picks.empty() ? ReverbPick{}
                                    : picks[std::min(k, picks.size() - 1)];
      pk.a = std::min<std::uint32_t>(pk.a, static_cast<std::uint32_t>(S - 1));
      pk.b = std::min<std::uint32_t>(pk.b, static_cast<std::uint32_t>(S - 1));
      P[k] = {pk.a, pk.b, std::clamp(pk.w, 0.0f, 1.0f), 0};
    }

    out[0].assign(out_blocks * B + B, 0.0f);
    out[1].assign(out_blocks * B + B, 0.0f);
    std::vector<float> seg(Fft::kN);
    double gpu_seconds = 0;
    for (std::size_t m0 = 0; m0 < out_blocks; m0 += kChunk) {
      const std::size_t count = std::min(kChunk, out_blocks - m0);
      // The send blocks this chunk adds; the older ones are in the ring.
      for (std::size_t k = m0; k < m0 + count && k < in_blocks; ++k) {
        const std::size_t at = k * B;
        fft.forward(send + at, std::min(B, frames - at), 1.0f,
                    X + (k % ring) * bins);
      }
      const Params prm{static_cast<std::uint32_t>(bins),
                       static_cast<std::uint32_t>(parts),
                       static_cast<std::uint32_t>(ring),
                       static_cast<std::uint32_t>(m0),
                       static_cast<std::uint32_t>(count),
                       static_cast<std::uint32_t>(in_blocks)};
      if (g) {
        const auto t0 = Clock::now();
        id<MTLCommandBuffer> cb = [g->queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:g->pipeline];
        [enc setBuffer:xb offset:0 atIndex:0];
        [enc setBuffer:hb offset:0 atIndex:1];
        [enc setBuffer:pb offset:0 atIndex:2];
        [enc setBuffer:yb offset:0 atIndex:3];
        [enc setBytes:&prm length:sizeof prm atIndex:4];
        const NSUInteger tw = g->pipeline.threadExecutionWidth;
        [enc dispatchThreads:MTLSizeMake(bins, count, 2)
            threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        gpu_seconds +=
            std::chrono::duration<double>(Clock::now() - t0).count();
        if (cb.status != MTLCommandBufferStatusCompleted) {
          return make_error(Code::Internal, std::format(
              "the reverb's kernel failed: {}",
              cb.error ? cb.error.localizedDescription.UTF8String : "?"));
        }
      } else {
        for (std::size_t row = 0; row < count; ++row) {
          const std::size_t m = m0 + row;
          const std::size_t q0 = m >= in_blocks ? m - in_blocks + 1 : 0;
          const std::size_t q1 = std::min(parts, m + 1);
          for (int c = 0; c < 2; ++c) {
            Complex* acc = Y + (c * count + row) * bins;
            std::fill_n(acc, bins, Complex{0, 0});
            for (std::size_t q = q0; q < q1; ++q) {
              const std::size_t k = m - q;
              const Complex* x = X + (k % ring) * bins;
              const Pick& pk = P[k];
              const Complex* ha = H + ((pk.a * 2 + c) * parts + q) * bins;
              const Complex* hb2 = H + ((pk.b * 2 + c) * parts + q) * bins;
              for (std::size_t i = 0; i < bins; ++i) {
                Complex h = ha[i];
                if (pk.w > 0) {
                  h = {h.re + (hb2[i].re - h.re) * pk.w,
                       h.im + (hb2[i].im - h.im) * pk.w};
                }
                acc[i].re += x[i].re * h.re - x[i].im * h.im;
                acc[i].im += x[i].re * h.im + x[i].im * h.re;
              }
            }
          }
        }
      }
      // Each output block's 2B frames over this block and the next.
      for (std::size_t row = 0; row < count; ++row) {
        const std::size_t at = (m0 + row) * B;
        for (int c = 0; c < 2; ++c) {
          fft.inverse(Y + (c * count + row) * bins, seg.data());
          vDSP_vadd(out[c].data() + at, 1, seg.data(), 1,
                    out[c].data() + at, 1, Fft::kN);
        }
      }
    }
    out[0].resize(out_frames);
    out[1].resize(out_frames);
    if (stats) {
      stats->gpu = g != nullptr;
      stats->gpu_seconds = gpu_seconds;
      stats->seconds =
          std::chrono::duration<double>(Clock::now() - began).count();
    }
    return out;
  }
}

}
