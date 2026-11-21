#include "far-compute.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <thread>
#include <vector>

namespace valtz::fleet {

namespace fw = far_wire;

namespace {

// vpipe's FP4 expert kernels (gpu-kernels/metal/ops/deepseek_v41.metal,
// dsv41_moe_swiglu_fp4 and dsv41_moe_down_fp4), the same steps and lane
// order, so the outputs are theirs to the bit; and a sum of a row's
// outputs in call order. T is the element type (VPIPE_ELT).
constexpr const char* kKernels = R"(
#include <metal_stdlib>
using namespace metal;
typedef VPIPE_ELT T;

inline float e8m0_(uchar e) { return as_type<float>((uint)e << 23); }

inline half2 fp4x2_(uint v)
{
  return as_type<half2>(ushort2((ushort)(((v & 0x8u) << 12) |
                                         ((v & 0x7u) << 9)),
                                (ushort)(((v & 0x80u) << 8) |
                                         ((v & 0x70u) << 5))));
}

inline float fp4_dot8_(uint u, thread const float* x)
{
  const half2 h0 = fp4x2_(u), h1 = fp4x2_(u >> 8);
  const half2 h2 = fp4x2_(u >> 16), h3 = fp4x2_(u >> 24);
  float s = 0.0f;
  s += (float)h0.x * x[0];
  s += (float)h0.y * x[1];
  s += (float)h1.x * x[2];
  s += (float)h1.y * x[3];
  s += (float)h2.x * x[4];
  s += (float)h2.y * x[5];
  s += (float)h3.x * x[6];
  s += (float)h3.y * x[7];
  return s * 16384.0f;
}

// one expert (its parts at the buffers' offsets), one row: grid {32,
// I / 4 * 2, 1}, tg {32, 2, 1}
kernel void far_swiglu_fp4(
    const device uchar* w1  [[buffer(0)]],
    const device uchar* s1  [[buffer(1)]],
    const device uchar* w3  [[buffer(2)]],
    const device uchar* s3  [[buffer(3)]],
    const device T*     x   [[buffer(4)]],
    device T*           act [[buffer(5)]],
    constant float&     w   [[buffer(6)]],
    constant int&       K   [[buffer(7)]],
    constant int&       I   [[buffer(8)]],
    constant float&     lim [[buffer(9)]],
    uint3 tg   [[threadgroup_position_in_grid]],
    uint  sg   [[simdgroup_index_in_threadgroup]],
    uint  lane [[thread_index_in_simdgroup]])
{
  const int f0 = ((int)tg.y * 2 + (int)sg) * 2;
  if (f0 >= I) { return; }
  const int KB = K / 2, KS = K / 32;
  float g[2] = {0.0f, 0.0f}, u[2] = {0.0f, 0.0f};
  for (int k = (int)lane * 8; k < K; k += 256) {
    float xv[8];
    for (int j = 0; j < 8; ++j) { xv[j] = (float)x[k + j]; }
    for (int r = 0; r < 2; ++r) {
      const long row = f0 + r;
      const uint c1 = *reinterpret_cast<const device uint*>(
          w1 + row * KB + k / 2);
      const uint c3 = *reinterpret_cast<const device uint*>(
          w3 + row * KB + k / 2);
      g[r] += e8m0_(s1[row * KS + k / 32]) * fp4_dot8_(c1, xv);
      u[r] += e8m0_(s3[row * KS + k / 32]) * fp4_dot8_(c3, xv);
    }
  }
  for (int r = 0; r < 2; ++r) {
    g[r] = simd_sum(g[r]);
    u[r] = simd_sum(u[r]);
  }
  if (lane == 0) {
    for (int r = 0; r < 2; ++r) {
      float gg = g[r], uu = u[r];
      if (lim > 0.0f) {
        uu = clamp(uu, -lim, lim);
        gg = min(gg, lim);
      }
      const float a = gg / (1.0f + exp(-gg)) * uu;
      act[f0 + r] = (T)(w * a);
    }
  }
}

// grid {32, H / 8 * 2, 1}, tg {32, 2, 1}
kernel void far_down_fp4(
    const device uchar* w2  [[buffer(0)]],
    const device uchar* s2  [[buffer(1)]],
    const device T*     act [[buffer(2)]],
    device T*           out [[buffer(3)]],
    constant int&       I   [[buffer(4)]],
    constant int&       H   [[buffer(5)]],
    uint3 tg   [[threadgroup_position_in_grid]],
    uint  sg   [[simdgroup_index_in_threadgroup]],
    uint  lane [[thread_index_in_simdgroup]])
{
  const int r0 = ((int)tg.y * 2 + (int)sg) * 4;
  if (r0 >= H) { return; }
  const int KB = I / 2, KS = I / 32;
  float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  for (int k = (int)lane * 8; k < I; k += 256) {
    float xv[8];
    for (int j = 0; j < 8; ++j) { xv[j] = (float)act[k + j]; }
    for (int r = 0; r < 4; ++r) {
      const long row = r0 + r;
      const uint c = *reinterpret_cast<const device uint*>(
          w2 + row * KB + k / 2);
      acc[r] += e8m0_(s2[row * KS + k / 32]) * fp4_dot8_(c, xv);
    }
  }
  for (int r = 0; r < 4; ++r) { acc[r] = simd_sum(acc[r]); }
  if (lane == 0) {
    for (int r = 0; r < 4; ++r) { out[r0 + r] = (T)acc[r]; }
  }
}

)";

// vpipe's decode kernels for a GGUF's own blocks (deepseek_v41.metal's
// dsv41_moe_swiglu_iq5_s4_r2 / dsv41_moe_down_iq5_s4_r2 and the dots they
// call), copied as they are -- so a row is the decode's to the bit -- an
// expert given as slot 0 of its parts' own offsets.
constexpr const char* kKernelsIq = R"IQ(
constant constexpr uint kDsv41IqXXS = 16, kDsv41IqXS = 17;  // else IQ1_M

constant constexpr uint kDsv41Iq2S = 22, kDsv41Iq3XXS = 18;

constant constexpr uint kDsv41Grid2S = 3856, kDsv41Grid3XXS = 4880;

constant constexpr uint kDsv41GridXS = 256, kDsv41Grid1S = 768,
                        kDsv41Signs = 2816;

constant constexpr uint kDsv41Grid1N = 2832;   // in u64s

inline int dsv41_iq_block_bytes_(const uint type)
{
  return type == kDsv41IqXXS ? 66 : type == kDsv41IqXS ? 74
         : type == kDsv41Iq2S ? 82 : type == kDsv41Iq3XXS ? 98 : 56;
}

inline uint dsv41_u32_at2_(const device uchar* p)
{
  return uint(*(const device ushort*)p) |
         (uint(*(const device ushort*)(p + 2)) << 16);
}

inline float dsv41_iq_dot8_(const ulong g, const uint sg, const float4 y0,
                            const float4 y1)
{
  const float4 a = float4(as_type<uchar4>(uint(g)));
  const float4 b = float4(as_type<uchar4>(uint(g >> 32)));
  const uint4 m0 = ((uint4(sg) >> uint4(0, 1, 2, 3)) & 1u) << 31;
  const uint4 m1 = ((uint4(sg) >> uint4(4, 5, 6, 7)) & 1u) << 31;
  return dot(a, as_type<float4>(as_type<uint4>(y0) ^ m0)) +
         dot(b, as_type<float4>(as_type<uint4>(y1) ^ m1));
}

inline uint dsv41_ksign_(const uint i) { return i | ((popcount(i) & 1u) << 7); }

template <typename P>
inline float dsv41_iq32_dotf_(const device uchar* row, const int sub,
                              const uint type, P grid,
                              thread const float4* y)
{
  const int ib = sub % 8;
  if (type == kDsv41IqXXS) {
    const device uchar* blk = row + (long)(sub / 8) * 66;
    const packed_ushort4 q =
        *(const device packed_ushort4*)(blk + 2 + 8 * ib);
    const float d = float(as_type<half>(*(const device ushort*)blk));
    const uint a1 = uint(q[2]) | (uint(q[3]) << 16);
    const ulong g0 = grid[q[0] & 0xff], g1 = grid[q[0] >> 8];
    const ulong g2 = grid[q[1] & 0xff], g3 = grid[q[1] >> 8];
    float s = dsv41_iq_dot8_(g0, dsv41_ksign_(a1 & 127), y[0], y[1]);
    s += dsv41_iq_dot8_(g1, dsv41_ksign_((a1 >> 7) & 127), y[2], y[3]);
    s += dsv41_iq_dot8_(g2, dsv41_ksign_((a1 >> 14) & 127), y[4], y[5]);
    s += dsv41_iq_dot8_(g3, dsv41_ksign_((a1 >> 21) & 127), y[6], y[7]);
    return d * (0.5f + float(a1 >> 28)) * 0.25f * s;
  }
  if (type == kDsv41IqXS) {
    const device uchar* blk = row + (long)(sub / 8) * 74;
    const packed_ushort4 q =
        *(const device packed_ushort4*)(blk + 2 + 8 * ib);
    const float d = float(as_type<half>(*(const device ushort*)blk));
    const uint sc = blk[66 + ib];
    const float s0 =
        dsv41_iq_dot8_(grid[q[0] & 511], dsv41_ksign_(q[0] >> 9), y[0],
                       y[1]) +
        dsv41_iq_dot8_(grid[q[1] & 511], dsv41_ksign_(q[1] >> 9), y[2], y[3]);
    const float s1 =
        dsv41_iq_dot8_(grid[q[2] & 511], dsv41_ksign_(q[2] >> 9), y[4],
                       y[5]) +
        dsv41_iq_dot8_(grid[q[3] & 511], dsv41_ksign_(q[3] >> 9), y[6], y[7]);
    return d * 0.25f * ((0.5f + float(sc & 0xf)) * s0 +
                        (0.5f + float(sc >> 4)) * s1);
  }
  return 0.0f;   // dsv41_iq32_dot5_ routes the rest elsewhere
}

template <typename PU>
inline float dsv41_iq1m_dot_(const device uchar* row, const int sub, PU g4,
                             thread const float4* y, thread const float* ys)
{
  const int ib = sub % 8;
  const device uchar* blk = row + (long)(sub / 8) * 56;
  const uint qs = *(const device uint*)(blk + 4 * ib);
  const uint qh = *(const device ushort*)(blk + 32 + 2 * ib);
  const uint2 sc = *(const device uint2*)(blk + 48);
  const uint sc0 = sc.x & 0xffff, sc1 = sc.x >> 16, sc2 = sc.y & 0xffff,
             sc3 = sc.y >> 16;
  const ushort su = ushort((sc0 >> 12) | ((sc1 >> 8) & 0x00f0) |
                           ((sc2 >> 4) & 0x0f00) | (sc3 & 0xf000));
  const float d = float(as_type<half>(su));
  const uint scb = (ib / 2 == 0 ? sc0 : ib / 2 == 1 ? sc1 : ib / 2 == 2 ? sc2
                                                                       : sc3);
  const int sh = 6 * (ib % 2);
  float r = 0.0f;
  for (int hh = 0; hh < 2; ++hh) {
    const float dl = d * float(2 * ((scb >> (sh + 3 * hh)) & 0x7) + 1);
    const uint h = (qh >> (8 * hh)) & 0xff;
    float s = 0.0f;
    for (int t = 0; t < 2; ++t) {
      const int l = 2 * hh + t;
      const uint idx = ((qs >> (8 * l)) & 0xff) |
                       ((t == 0 ? (h << 8) : (h << 4)) & 0x700);
      const float delta = (h & (t == 0 ? 0x08u : 0x80u)) != 0 ? -1.125f
                                                              : -0.875f;
      const uint u = g4[idx];
      const float4 a = float4(as_type<uchar4>(u & 0x0f0f0f0fu));
      const float4 b = float4(as_type<uchar4>((u >> 4) & 0x0f0f0f0fu));
      s += dot(a, y[2 * l]) + dot(b, y[2 * l + 1]) + delta * ys[l];
    }
    r += dl * s;
  }
  return r;
}

template <typename P>
inline float dsv41_iq2s_dot_(const device uchar* row, const int sub, P grid,
                             thread const float4* y)
{
  const int ib = sub % 8;
  const device uchar* blk = row + (long)(sub / 8) * 82;
  const float d = float(as_type<half>(*(const device ushort*)blk));
  const device uchar* qs = blk + 2 + 4 * ib;
  const device uchar* sgn = blk + 2 + 32 + 4 * ib;
  const uint qh = blk[66 + ib], sc = blk[74 + ib];
  float s0 = 0.0f, s1 = 0.0f;
  for (int l = 0; l < 4; ++l) {
    const uint gi = uint(qs[l]) | ((qh << (8 - 2 * l)) & 0x300);
    const float s = dsv41_iq_dot8_(grid[gi], sgn[l], y[2 * l], y[2 * l + 1]);
    if (l < 2) { s0 += s; } else { s1 += s; }
  }
  return d * 0.25f * ((0.5f + float(sc & 0xf)) * s0 +
                      (0.5f + float(sc >> 4)) * s1);
}

template <typename PU>
inline float dsv41_iq3xxs_dot_(const device uchar* row, const int sub,
                               PU g3, thread const float4* y)
{
  const int ib = sub % 8;
  const device uchar* blk = row + (long)(sub / 8) * 98;
  const float d = float(as_type<half>(*(const device ushort*)blk));
  const device uchar* qs = blk + 2 + 8 * ib;
  const uint a = dsv41_u32_at2_(blk + 66 + 4 * ib);
  float s = 0.0f;
  for (int l = 0; l < 4; ++l) {
    const ulong g = ulong(g3[qs[2 * l]]) | (ulong(g3[qs[2 * l + 1]]) << 32);
    s += dsv41_iq_dot8_(g, dsv41_ksign_((a >> (7 * l)) & 127), y[2 * l],
                        y[2 * l + 1]);
  }
  return d * (0.5f + float(a >> 28)) * 0.5f * s;
}

template <typename P, typename PU>
inline float dsv41_iq32_dot5_(const device uchar* row, const int sub,
                              const uint type, P grid, PU g4,
                              thread const float4* y, thread const float* ys)
{
  if (type == kDsv41IqXXS || type == kDsv41IqXS) {
    return dsv41_iq32_dotf_(row, sub, type, grid, y);
  }
  if (type == kDsv41Iq2S) { return dsv41_iq2s_dot_(row, sub, grid, y); }
  if (type == kDsv41Iq3XXS) { return dsv41_iq3xxs_dot_(row, sub, g4, y); }
  return dsv41_iq1m_dot_(row, sub, g4, y, ys);
}

inline int dsv41_iq_tab_at5_(const uint type)
{
  return type == kDsv41IqXXS ? 0 : type == kDsv41IqXS ? (int)kDsv41GridXS
         : type == kDsv41Iq2S ? (int)kDsv41Grid2S
         : type == kDsv41Iq3XXS ? (int)kDsv41Grid3XXS : (int)kDsv41Grid1N;
}

inline int dsv41_iq_tab_n5_(const uint type)
{
  return type == kDsv41IqXXS ? 256 : type == kDsv41IqXS ? 512
         : type == kDsv41Iq2S ? 1024 : type == kDsv41Iq3XXS ? 128 : 1024;
}

inline void dsv41_y32_(const device T* x, thread float4* y, thread float* ys)
{
  const device vec<T, 4>* xp = (const device vec<T, 4>*)x;
  for (int q = 0; q < 8; ++q) { y[q] = float4(xp[q]); }
  for (int l = 0; l < 4; ++l) {
    const float4 v = y[2 * l] + y[2 * l + 1];
    ys[l] = v.x + v.y + v.z + v.w;
  }
}

template <int NS, int RS, bool TG>
kernel void dsv41_moe_swiglu_iq5_(
    const device uchar* w1 [[buffer(0)]],
    const device uchar* w3 [[buffer(1)]],
    const device ulong* tab [[buffer(2)]],
    const device T* x [[buffer(3)]], device T* act [[buffer(4)]],
    const device int* slot [[buffer(5)]],
    const device float* wt [[buffer(6)]], constant int& K [[buffer(7)]],
    constant int& I [[buffer(8)]], constant int& topk [[buffer(9)]],
    constant float& lim [[buffer(10)]], constant uint& t1 [[buffer(11)]],
    constant uint& t3 [[buffer(12)]], constant long& cw [[buffer(13)]],
    uint3 tg [[threadgroup_position_in_grid]],
    uint sg [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]])
{
  threadgroup ulong tt[1024];
  const device ulong* a1d = tab + dsv41_iq_tab_at5_(t1);
  const device ulong* a3d = tab + dsv41_iq_tab_at5_(t3);
  if (TG) {
    const int n = dsv41_iq_tab_n5_(t1);
    for (int i = (int)(sg * 32 + lane); i < n; i += 32 * NS) { tt[i] = a1d[i]; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  const bool same = t1 == t3;
  const int p = (int)tg.z;
  const long e = slot[p];
  const int f0 = ((int)tg.y * NS + (int)sg) * RS;
  const long rb1 = (long)(K / 256) * dsv41_iq_block_bytes_(t1);
  const long rb3 = (long)(K / 256) * dsv41_iq_block_bytes_(t3);
  const device uchar* b1 = w1 + e * cw;
  const device uchar* b3 = w3 + e * cw;
  const device T* xr = x + (long)(p / topk) * K;
  float g[RS], u[RS];
  for (int r = 0; r < RS; ++r) { g[r] = u[r] = 0.0f; }
  for (int sb = (int)lane; sb < K / 32; sb += 32) {
    float4 y[8];
    float ys[4];
    dsv41_y32_(xr + sb * 32, y, ys);
    for (int r = 0; r < RS; ++r) {
      const int f = f0 + r;
      if (f >= I) { break; }
      if (TG) {
        const threadgroup ulong* gt = tt;
        const threadgroup uint* gu = (const threadgroup uint*)tt;
        g[r] += dsv41_iq32_dot5_(b1 + f * rb1, sb, t1, gt, gu, y, ys);
        u[r] += same ? dsv41_iq32_dot5_(b3 + f * rb3, sb, t3, gt, gu, y, ys)
                     : dsv41_iq32_dot5_(b3 + f * rb3, sb, t3, a3d,
                                        (const device uint*)a3d, y, ys);
      } else {
        g[r] += dsv41_iq32_dot5_(b1 + f * rb1, sb, t1, a1d,
                                 (const device uint*)a1d, y, ys);
        u[r] += dsv41_iq32_dot5_(b3 + f * rb3, sb, t3, a3d,
                                 (const device uint*)a3d, y, ys);
      }
    }
  }
  for (int r = 0; r < RS; ++r) {
    float gg = simd_sum(g[r]), uu = simd_sum(u[r]);
    const int f = f0 + r;
    if (lane != 0 || f >= I) { continue; }
    if (lim > 0.0f) {
      uu = clamp(uu, -lim, lim);
      gg = min(gg, lim);
    }
    act[(long)p * I + f] = (T)(wt[p] * (gg / (1.0f + exp(-gg)) * uu));
  }
}

template <int NS, int RS, bool TG>
kernel void dsv41_moe_down_iq5_(
    const device uchar* w2 [[buffer(0)]],
    const device ulong* tab [[buffer(1)]],
    const device T* act [[buffer(2)]], device T* out [[buffer(3)]],
    const device int* slot [[buffer(4)]], constant int& I [[buffer(5)]],
    constant int& H [[buffer(6)]], constant uint& t2 [[buffer(7)]],
    constant long& cw [[buffer(8)]],
    uint3 tg [[threadgroup_position_in_grid]],
    uint sg [[simdgroup_index_in_threadgroup]],
    uint lane [[thread_index_in_simdgroup]])
{
  threadgroup ulong tt[1024];
  const device ulong* ad = tab + dsv41_iq_tab_at5_(t2);
  if (TG) {
    const int n = dsv41_iq_tab_n5_(t2);
    for (int i = (int)(sg * 32 + lane); i < n; i += 32 * NS) { tt[i] = ad[i]; }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  const int p = (int)tg.z;
  const long e = slot[p];
  const int o0 = ((int)tg.y * NS + (int)sg) * RS;
  const long rb = (long)(I / 256) * dsv41_iq_block_bytes_(t2);
  const device uchar* b2 = w2 + e * cw;
  const device T* ar = act + (long)p * I;
  float acc[RS];
  for (int r = 0; r < RS; ++r) { acc[r] = 0.0f; }
  for (int sb = (int)lane; sb < I / 32; sb += 32) {
    float4 y[8];
    float ys[4];
    dsv41_y32_(ar + sb * 32, y, ys);
    for (int r = 0; r < RS; ++r) {
      const int o = o0 + r;
      if (o >= H) { break; }
      if (TG) {
        acc[r] += dsv41_iq32_dot5_(b2 + o * rb, sb, t2,
                                   (const threadgroup ulong*)tt,
                                   (const threadgroup uint*)tt, y, ys);
      } else {
        acc[r] += dsv41_iq32_dot5_(b2 + o * rb, sb, t2, ad,
                                   (const device uint*)ad, y, ys);
      }
    }
  }
  for (int r = 0; r < RS; ++r) {
    const float s = simd_sum(acc[r]);
    const int o = o0 + r;
    if (lane == 0 && o < H) { out[(long)p * H + o] = (T)s; }
  }
}

template [[host_name("far_swiglu_iq")]] kernel void
dsv41_moe_swiglu_iq5_<4, 2, false>(
    const device uchar*, const device uchar*, const device ulong*,
    const device T*, device T*, const device int*, const device float*,
    constant int&, constant int&, constant int&, constant float&,
    constant uint&, constant uint&, constant long&, uint3, uint, uint);
template [[host_name("far_down_iq")]] kernel void
dsv41_moe_down_iq5_<4, 2, false>(
    const device uchar*, const device ulong*, const device T*, device T*,
    const device int*, constant int&, constant int&, constant uint&,
    constant long&, uint3, uint, uint);
)IQ";

using Clock = std::chrono::steady_clock;

// Seconds on the host clock Metal's GPUStartTime / GPUEndTime are read on
// (CACurrentMediaTime's: mach_absolute_time).
double
host_s()
{
  static const double k = [] {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (double)tb.numer / (double)tb.denom * 1e-9;
  }();
  return (double)mach_absolute_time() * k;
}

}  // namespace

struct FarCompute::Impl {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil, warm_queue = nil;
  id<MTLResidencySet> resident = nil;
  // per element type (0 f16, 1 bf16): swiglu, down; a GGUF's swiglu,
  // down
  id<MTLComputePipelineState> pso[2][4] = {};
  // the lent memory as buffers of at most the device's length, each
  // reaching kOverlap past the next one's start, so every part (smaller
  // than that) lies whole in one
  static constexpr std::uint64_t kOverlap = 64ull << 20;
  std::vector<id<MTLBuffer>> chunk;
  std::uint64_t step = 0, bytes = 0;
  // a call's working buffers (grown to fit)
  id<MTLBuffer> xb = nil, act = nil, outs = nil;
  id<MTLBuffer> misc = nil;   // a GGUF call's slot 0, then its weights
  std::size_t calls_cap = 0, hidden_cap = 0, inter_cap = 0;
  std::mutex mu;
  // keeping the GPU awake
  std::thread warm;
  std::mutex wmu;
  std::condition_variable wcv;
  bool quit = false;
  Clock::time_point last{};
  int warm_us = 20;
  bool spin = true;
  mutable std::mutex tmu;
  Timing tm;
  int warm_dtype = -1, warm_hidden = 0, warm_inter = 0;

  bool where_(std::uint64_t off, std::uint64_t len,
              id<MTLBuffer> __strong* b, std::uint64_t* at) const
  {
    if (off > bytes || len > bytes - off) { return false; }
    const std::size_t i = (std::size_t)std::min<std::uint64_t>(
        off / step, chunk.size() - 1);
    const std::uint64_t base = (std::uint64_t)i * step;
    if (off + len > base + chunk[i].length) { return false; }
    *b = chunk[i];
    *at = off - base;
    return true;
  }

  bool grow_(std::size_t n, std::size_t H, std::size_t I)
  {
    if (n <= calls_cap && H <= hidden_cap && I <= inter_cap) { return true; }
    calls_cap = std::max(calls_cap, n);
    hidden_cap = std::max(hidden_cap, H);
    inter_cap = std::max(inter_cap, I);
    const MTLResourceOptions o = MTLResourceStorageModeShared;
    xb = [device newBufferWithLength:hidden_cap * 2 options:o];
    act = [device newBufferWithLength:calls_cap * inter_cap * 2 options:o];
    outs = [device newBufferWithLength:calls_cap * hidden_cap * 2 options:o];
    misc = [device newBufferWithLength:(calls_cap + 4) * 4 options:o];
    return xb && act && outs && misc;
  }

  // A small piece of gate|up -- a few rows of the first chunk's bytes,
  // whatever they hold -- every warm_us while calls come.
  void warm_loop_()
  {
    const MTLResourceOptions so = MTLResourceStorageModeShared;
    id<MTLBuffer> wx = [device newBufferWithLength:1 << 16 options:so];
    id<MTLBuffer> wa = [device newBufferWithLength:1 << 16 options:so];
    std::unique_lock<std::mutex> lk(wmu);
    while (!quit) {
      const bool on = warm_dtype >= 0 &&
                      Clock::now() - last < std::chrono::seconds(1);
      if (!on) {
        wcv.wait_for(lk, std::chrono::milliseconds(50));
        continue;
      }
      const int d = warm_dtype, K = std::min(warm_hidden, 8192);
      const int I = 64;
      lk.unlock();
      @autoreleasepool {
        id<MTLCommandBuffer> cb = [warm_queue commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:pso[d][0]];
        for (int k = 0; k < 4; ++k) {
          [e setBuffer:chunk[0] offset:0 atIndex:(NSUInteger)k];
        }
        [e setBuffer:wx offset:0 atIndex:4];
        [e setBuffer:wa offset:0 atIndex:5];
        const float w = 0.0f, lim = 0.0f;
        [e setBytes:&w length:4 atIndex:6];
        [e setBytes:&K length:4 atIndex:7];
        [e setBytes:&I length:4 atIndex:8];
        [e setBytes:&lim length:4 atIndex:9];
        [e dispatchThreadgroups:MTLSizeMake(1, (NSUInteger)(I / 4), 1)
          threadsPerThreadgroup:MTLSizeMake(32, 2, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
      }
      std::this_thread::sleep_for(std::chrono::microseconds(warm_us));
      lk.lock();
    }
  }
};

FarCompute::FarCompute(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

FarCompute::~FarCompute()
{
  if (!_impl) { return; }
  {
    std::lock_guard<std::mutex> lk(_impl->wmu);
    _impl->quit = true;
  }
  _impl->wcv.notify_all();
  if (_impl->warm.joinable()) { _impl->warm.join(); }
  if (_impl->resident != nil) { [_impl->resident endResidency]; }
}

std::unique_ptr<FarCompute>
FarCompute::create(std::uint8_t* mem, std::uint64_t bytes, std::string* err)
{
  auto fail = [&](std::string m) -> std::unique_ptr<FarCompute> {
    if (err != nullptr) { *err = std::move(m); }
    return nullptr;
  };
  const std::uint64_t page = (std::uint64_t)getpagesize();
  if (mem == nullptr || bytes == 0 || bytes % page != 0 ||
      reinterpret_cast<std::uintptr_t>(mem) % page != 0) {
    return fail("the lent memory is not whole pages");
  }
  auto m = std::make_unique<Impl>();
  m->device = MTLCreateSystemDefaultDevice();
  if (m->device == nil) { return fail("no Metal device"); }
  m->queue = [m->device newCommandQueue];
  m->warm_queue = [m->device newCommandQueue];
  for (int d = 0; d < 2; ++d) {
    MTLCompileOptions* opts = [MTLCompileOptions new];
    opts.preprocessorMacros = @{@"VPIPE_ELT" : d == 1 ? @"bfloat" : @"half"};
    NSError* e = nil;
    const std::string all = std::string(kKernels) + kKernelsIq;
    id<MTLLibrary> lib = [m->device newLibraryWithSource:@(all.c_str())
                                                 options:opts
                                                   error:&e];
    if (lib == nil) {
      return fail(std::format("the expert kernels did not build: {}",
                              e ? e.localizedDescription.UTF8String : "?"));
    }
    NSString* names[4] = {@"far_swiglu_fp4", @"far_down_fp4",
                          @"far_swiglu_iq", @"far_down_iq"};
    for (int k = 0; k < 4; ++k) {
      id<MTLFunction> fn = [lib newFunctionWithName:names[k]];
      m->pso[d][k] = fn ? [m->device newComputePipelineStateWithFunction:fn
                                                                   error:&e]
                        : nil;
      if (m->pso[d][k] == nil) { return fail("an expert kernel did not load"); }
    }
  }
  // the memory in place, in buffers the device can take
  const std::uint64_t maxlen =
      (std::uint64_t)m->device.maxBufferLength / page * page;
  if (maxlen <= Impl::kOverlap) {
    return fail("Metal's buffers are too short");
  }
  m->bytes = bytes;
  m->step = bytes <= maxlen ? bytes : (maxlen - Impl::kOverlap) / page * page;
  MTLResidencySetDescriptor* rd = [MTLResidencySetDescriptor new];
  NSError* re = nil;
  m->resident = [m->device newResidencySetWithDescriptor:rd error:&re];
  for (std::uint64_t at = 0; at < bytes; at += m->step) {
    const std::uint64_t len = std::min(bytes - at, maxlen);
    id<MTLBuffer> b =
        [m->device newBufferWithBytesNoCopy:mem + at
                                     length:(NSUInteger)len
                                    options:MTLResourceStorageModeShared
                                deallocator:nil];
    if (b == nil) { return fail("the lent memory could not be mapped"); }
    m->chunk.push_back(b);
    if (m->resident != nil) { [m->resident addAllocation:b]; }
    if (at + len >= bytes) { break; }
  }
  if (m->resident != nil) {
    [m->resident commit];
    [m->resident requestResidency];
    [m->queue addResidencySet:m->resident];
    [m->warm_queue addResidencySet:m->resident];
  }
  if (const char* v = std::getenv("VALTZ_FAR_GPU_WARM_US")) {
    m->warm_us = std::max(0, std::atoi(v));
  }
  if (const char* v = std::getenv("VALTZ_FAR_GPU_SPIN")) {
    m->spin = std::atoi(v) != 0;
  }
  Impl* raw = m.get();
  if (m->warm_us > 0) {
    m->warm = std::thread([raw] { raw->warm_loop_(); });
  }
  return std::unique_ptr<FarCompute>(new FarCompute(std::move(m)));
}

FarCompute::Timing
FarCompute::timing() const
{
  std::lock_guard<std::mutex> lk(_impl->tmu);
  return _impl->tm;
}

bool
FarCompute::run(const fw::ComputeForm& form, const fw::ComputeCall* calls,
                std::size_t n, const void* x, void* out, std::string* err)
{
  Impl& m = *_impl;
  auto fail = [&](std::string why) {
    if (err != nullptr) { *err = std::move(why); }
    return false;
  };
  const int H = (int)form.hidden, I = (int)form.inter;
  const bool gguf = form.format == fw::kComputeGguf;
  if ((form.format != fw::kComputeFp4 && !gguf) || form.dtype > 1 ||
      n == 0 || H <= 0 || I <= 0 || H % 256 != 0 || I % 256 != 0) {
    return fail("a form these kernels do not run");
  }
  const std::uint32_t t1 = form.arg[2] & 0xff, t3 = (form.arg[2] >> 8) & 0xff,
                      t2 = (form.arg[2] >> 16) & 0xff;
  id<MTLBuffer> tab = nil;
  std::uint64_t tab_at = 0;
  if (gguf) {
    const std::uint64_t to =
        (std::uint64_t)form.arg[0] | ((std::uint64_t)form.arg[1] << 32);
    if (fw::gguf_block_bytes(t1) == 0 || fw::gguf_block_bytes(t3) == 0 ||
        fw::gguf_block_bytes(t2) == 0) {
      return fail("a GGUF type these kernels do not run");
    }
    if (!m.where_(to, fw::kGgufTablesHead + fw::kComputeGgufTableBytes,
                  &tab, &tab_at)) {
      return fail("the tables past the lent memory");
    }
    // their head: version 1's layout, the size these kernels read
    std::uint32_t head[4];
    std::memcpy(head, static_cast<const std::uint8_t*>(tab.contents) + tab_at,
                sizeof head);
    const std::uint64_t words = (std::uint64_t)head[2] |
                                ((std::uint64_t)head[3] << 32);
    if (head[0] != fw::kGgufTablesMagic || head[1] != 1 ||
        words * 8 != fw::kComputeGgufTableBytes) {
      return fail("the GGUF's tables are not version 1's");
    }
    tab_at += fw::kGgufTablesHead;
  }
  {
    std::lock_guard<std::mutex> lk(m.wmu);
    m.last = Clock::now();
    m.warm_dtype = (int)form.dtype;
    m.warm_hidden = H;
    m.warm_inter = I;
  }
  m.wcv.notify_all();
  std::lock_guard<std::mutex> lk(m.mu);
  if (!m.grow_(n, (std::size_t)H, (std::size_t)I)) {
    return fail("out of GPU memory");
  }
  std::memcpy(m.xb.contents, x, (std::size_t)H * 2);
  const int d = (int)form.dtype;
  if (gguf) {
    auto* mw = static_cast<std::int32_t*>(m.misc.contents);
    mw[0] = 0;
    for (std::size_t j = 0; j < n; ++j) {
      std::memcpy(mw + 4 + j, &calls[j].weight, 4);
    }
  }
  @autoreleasepool {
    id<MTLCommandBuffer> cb = [m.queue commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    for (std::size_t j = 0; j < n; ++j) {
      id<MTLBuffer> pb[6];
      std::uint64_t po[6];
      for (int k = 0; k < 6; ++k) {
        if (!m.where_(calls[j].part[k], fw::compute_part_bytes(form, k),
                      &pb[k], &po[k])) {
          [e endEncoding];
          return fail("an expert past the lent memory");
        }
      }
      if (gguf) {
        // w1, w3 [I, H]; w2 [H, I]: 4 simdgroups of 2 rows a threadgroup
        const int one = 1;
        const long cw = 0;
        const float lim = form.swiglu_limit;
        [e setComputePipelineState:m.pso[d][2]];
        [e setBuffer:pb[0] offset:(NSUInteger)po[0] atIndex:0];
        [e setBuffer:pb[2] offset:(NSUInteger)po[2] atIndex:1];
        [e setBuffer:tab offset:(NSUInteger)tab_at atIndex:2];
        [e setBuffer:m.xb offset:0 atIndex:3];
        [e setBuffer:m.act offset:j * (std::size_t)I * 2 atIndex:4];
        [e setBuffer:m.misc offset:0 atIndex:5];
        [e setBuffer:m.misc offset:(4 + j) * 4 atIndex:6];
        [e setBytes:&H length:4 atIndex:7];
        [e setBytes:&I length:4 atIndex:8];
        [e setBytes:&one length:4 atIndex:9];
        [e setBytes:&lim length:4 atIndex:10];
        [e setBytes:&t1 length:4 atIndex:11];
        [e setBytes:&t3 length:4 atIndex:12];
        [e setBytes:&cw length:8 atIndex:13];
        [e dispatchThreadgroups:MTLSizeMake(1, (NSUInteger)((I + 7) / 8), 1)
          threadsPerThreadgroup:MTLSizeMake(32, 4, 1)];
        [e setComputePipelineState:m.pso[d][3]];
        [e setBuffer:pb[4] offset:(NSUInteger)po[4] atIndex:0];
        [e setBuffer:tab offset:(NSUInteger)tab_at atIndex:1];
        [e setBuffer:m.act offset:j * (std::size_t)I * 2 atIndex:2];
        [e setBuffer:m.outs offset:j * (std::size_t)H * 2 atIndex:3];
        [e setBuffer:m.misc offset:0 atIndex:4];
        [e setBytes:&I length:4 atIndex:5];
        [e setBytes:&H length:4 atIndex:6];
        [e setBytes:&t2 length:4 atIndex:7];
        [e setBytes:&cw length:8 atIndex:8];
        [e dispatchThreadgroups:MTLSizeMake(1, (NSUInteger)((H + 7) / 8), 1)
          threadsPerThreadgroup:MTLSizeMake(32, 4, 1)];
        continue;
      }
      [e setComputePipelineState:m.pso[d][0]];
      for (int k = 0; k < 4; ++k) {
        [e setBuffer:pb[k] offset:(NSUInteger)po[k] atIndex:(NSUInteger)k];
      }
      [e setBuffer:m.xb offset:0 atIndex:4];
      [e setBuffer:m.act offset:j * (std::size_t)I * 2 atIndex:5];
      const float w = calls[j].weight, lim = form.swiglu_limit;
      [e setBytes:&w length:4 atIndex:6];
      [e setBytes:&H length:4 atIndex:7];
      [e setBytes:&I length:4 atIndex:8];
      [e setBytes:&lim length:4 atIndex:9];
      [e dispatchThreadgroups:MTLSizeMake(1, (NSUInteger)(I / 4), 1)
        threadsPerThreadgroup:MTLSizeMake(32, 2, 1)];
      [e setComputePipelineState:m.pso[d][1]];
      [e setBuffer:pb[4] offset:(NSUInteger)po[4] atIndex:0];
      [e setBuffer:pb[5] offset:(NSUInteger)po[5] atIndex:1];
      [e setBuffer:m.act offset:j * (std::size_t)I * 2 atIndex:2];
      [e setBuffer:m.outs offset:j * (std::size_t)H * 2 atIndex:3];
      [e setBytes:&I length:4 atIndex:4];
      [e setBytes:&H length:4 atIndex:5];
      [e dispatchThreadgroups:MTLSizeMake(1, (NSUInteger)(H / 8), 1)
        threadsPerThreadgroup:MTLSizeMake(32, 2, 1)];
    }
    [e endEncoding];
    const double c0 = host_s();
    [cb commit];
    // polled, not waited: a decode waits on this answer, and a thread
    // put to sleep wakes late (VALTZ_FAR_GPU_SPIN=0: waited)
    if (m.spin) {
      while (cb.status < MTLCommandBufferStatusCompleted) {}
    } else {
      [cb waitUntilCompleted];
    }
    const double c1 = host_s();
    if (cb.status == MTLCommandBufferStatusCompleted && cb.GPUStartTime > 0) {
      std::lock_guard<std::mutex> tl(m.tmu);
      ++m.tm.calls;
      m.tm.lag_us += std::max(0.0, cb.GPUStartTime - c0) * 1e6;
      m.tm.run_us += (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
      m.tm.notice_us += std::max(0.0, c1 - cb.GPUEndTime) * 1e6;
      m.tm.wall_us += (c1 - c0) * 1e6;
    }
    if (cb.status != MTLCommandBufferStatusCompleted) {
      return fail(std::format("the GPU: {}",
                              cb.error ? cb.error.localizedDescription
                                             .UTF8String
                                       : "failed"));
    }
  }
  // each call's row as its kernel rounded it (far-compute/2)
  std::memcpy(out, m.outs.contents, n * (std::size_t)H * 2);
  return true;
}

}
