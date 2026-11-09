#include "guide-clean.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace valtz::engine::vp {

std::int64_t
TensorFile::count() const
{
  std::int64_t n = dims.empty() ? 0 : 1;
  for (const auto d : dims) {
    n *= d;
  }
  return n;
}

std::size_t
TensorFile::sample_bytes() const
{
  return dtype == kF32 ? 4 : dtype == kF16 ? 2 : 0;
}

Result<TensorFile>
read_tensor_file(const fs::path& path)
{
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return make_error(Code::Io, std::format("cannot read {}",
                                            path.string()));
  }
  char magic[8] = {};
  std::uint32_t head[4] = {};
  std::uint64_t count = 0;
  if (!f.read(magic, 8) || std::string_view(magic, 8) != "VPTENSOR" ||
      !f.read(reinterpret_cast<char*>(head), sizeof head) ||
      !f.read(reinterpret_cast<char*>(&count), sizeof count) ||
      head[2] == 0 || head[2] > 8) {
    return make_error(Code::Corrupt, std::format(
        "{} is not a tensor file", path.filename().string()));
  }
  TensorFile t;
  t.version = head[0];
  t.dtype = head[1];
  t.dims.resize(head[2]);
  if (!f.read(reinterpret_cast<char*>(t.dims.data()),
              static_cast<std::streamsize>(t.dims.size() * 8)) ||
      t.sample_bytes() == 0 ||
      static_cast<std::uint64_t>(t.count()) != count) {
    return make_error(Code::Corrupt, std::format(
        "{}: a tensor of another kind (dtype {})", path.filename().string(),
        t.dtype));
  }
  t.bytes.resize(count * t.sample_bytes());
  if (!f.read(reinterpret_cast<char*>(t.bytes.data()),
              static_cast<std::streamsize>(t.bytes.size()))) {
    return make_error(Code::Corrupt, std::format(
        "{} is cut short", path.filename().string()));
  }
  t.sideband.assign(std::istreambuf_iterator<char>(f),
                    std::istreambuf_iterator<char>());
  return t;
}

Status
write_tensor_file(const fs::path& path, const TensorFile& t)
{
  if (t.sample_bytes() == 0 ||
      t.bytes.size() != static_cast<std::size_t>(t.count()) *
                            t.sample_bytes()) {
    return make_error(Code::InvalidArgument, std::format(
        "{}: its samples do not fill its shape", path.filename().string()));
  }
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  const std::uint32_t head[4] = {t.version, t.dtype,
                                 static_cast<std::uint32_t>(t.dims.size()),
                                 0};
  const std::uint64_t count = static_cast<std::uint64_t>(t.count());
  f.write("VPTENSOR", 8);
  f.write(reinterpret_cast<const char*>(head), sizeof head);
  f.write(reinterpret_cast<const char*>(&count), sizeof count);
  f.write(reinterpret_cast<const char*>(t.dims.data()),
          static_cast<std::streamsize>(t.dims.size() * 8));
  f.write(reinterpret_cast<const char*>(t.bytes.data()),
          static_cast<std::streamsize>(t.bytes.size()));
  f.write(t.sideband.data(), static_cast<std::streamsize>(t.sideband.size()));
  if (!f) {
    return make_error(Code::Io, std::format("cannot write {}",
                                            path.string()));
  }
  return ok_status();
}

std::vector<double>
guide_ramp_weights(std::int64_t latents)
{
  std::vector<double> mid;
  std::int64_t frame = 0;
  for (std::int64_t i = 0; i < latents; ++i) {
    const std::int64_t n = i % 5 == 0 ? 1 : 4;
    mid.push_back(static_cast<double>(frame) + (n - 1) / 2.0);
    frame += n;
  }
  std::vector<double> w(mid.size(), 1.0);
  if (mid.size() > 1 && mid.back() > 0) {
    for (std::size_t i = 0; i < mid.size(); ++i) {
      w[i] = mid[i] / mid.back();
    }
  }
  return w;
}

Status
ramp_guide(const fs::path& kept, std::int64_t start, const fs::path& cleaned,
           const fs::path& out, bool ramp)
{
  VALTZ_ASSIGN(TensorFile a, read_tensor_file(kept));
  VALTZ_ASSIGN(TensorFile b, read_tensor_file(cleaned));
  if (a.dtype != TensorFile::kF32 || b.dtype != TensorFile::kF32 ||
      a.dims.size() != 4 || b.dims.size() != 4 || a.dims[0] != b.dims[0] ||
      a.dims[2] != b.dims[2] || a.dims[3] != b.dims[3] || start < 0 ||
      start + b.dims[1] > a.dims[1]) {
    return make_error(Code::InvalidArgument, std::format(
        "the cleaned guide's latents ({}) do not lie on the kept ones' "
        "window",
        b.dims.size() == 4
            ? std::format("{} x {} x {} x {}", b.dims[0], b.dims[1],
                          b.dims[2], b.dims[3])
            : std::string("of another rank")));
  }
  const std::int64_t C = b.dims[0], T = b.dims[1], HW = b.dims[2] *
                                                         b.dims[3];
  const std::vector<double> w =
      ramp ? guide_ramp_weights(T) : std::vector<double>(T, 1.0);
  const auto* ka = reinterpret_cast<const float*>(a.bytes.data());
  auto* cb = reinterpret_cast<float*>(b.bytes.data());
  for (std::int64_t c = 0; c < C; ++c) {
    for (std::int64_t t = 0; t < T; ++t) {
      const float* k = ka + (c * a.dims[1] + start + t) * HW;
      float* o = cb + (c * T + t) * HW;
      const auto wt = static_cast<float>(w[static_cast<std::size_t>(t)]);
      for (std::int64_t i = 0; i < HW; ++i) {
        o[i] = (1.0f - wt) * k[i] + wt * o[i];
      }
    }
  }
  return write_tensor_file(out, b);
}

namespace {

// The widths of three box blurs that stand for a Gaussian of `sigma`
// (Kutskir's "boxes for Gauss"): odd, the smaller first.
std::vector<int>
box_widths(double sigma)
{
  constexpr int n = 3;
  const double ideal = std::sqrt(12.0 * sigma * sigma / n + 1.0);
  int wl = static_cast<int>(std::floor(ideal));
  if (wl % 2 == 0) {
    --wl;
  }
  const int wu = wl + 2;
  const double m_ideal =
      (12.0 * sigma * sigma - n * wl * wl - 4.0 * n * wl - 3.0 * n) /
      (-4.0 * wl - 4.0);
  const int m = static_cast<int>(std::lround(m_ideal));
  std::vector<int> w;
  for (int i = 0; i < n; ++i) {
    w.push_back(i < m ? wl : wu);
  }
  return w;
}

// One box blur of odd width `w` along a line of `n` samples `stride`
// apart, the edges extended; `tmp` holds a line.
void
box_line(float* line, std::int64_t n, std::int64_t stride, int w,
         std::vector<float>& tmp)
{
  const std::int64_t r = w / 2;
  tmp.resize(static_cast<std::size_t>(n));
  for (std::int64_t i = 0; i < n; ++i) {
    tmp[static_cast<std::size_t>(i)] = line[i * stride];
  }
  auto at = [&](std::int64_t i) {
    return tmp[static_cast<std::size_t>(std::clamp<std::int64_t>(i, 0,
                                                                 n - 1))];
  };
  double sum = 0;
  for (std::int64_t i = -r; i <= r; ++i) {
    sum += at(i);
  }
  for (std::int64_t i = 0; i < n; ++i) {
    line[i * stride] = static_cast<float>(sum / w);
    sum += at(i + r + 1) - at(i - r);
  }
}

}  // namespace

void
gaussian_blur(std::span<float> plane, int width, int height, double sigma)
{
  if (sigma <= 0 || width <= 0 || height <= 0 ||
      plane.size() < static_cast<std::size_t>(width) * height) {
    return;
  }
  std::vector<float> tmp;
  for (const int w : box_widths(sigma)) {
    for (int y = 0; y < height; ++y) {
      box_line(plane.data() + static_cast<std::ptrdiff_t>(y) * width, width,
               1, w, tmp);
    }
    for (int x = 0; x < width; ++x) {
      box_line(plane.data() + x, height, width, w, tmp);
    }
  }
}

void
tone_transfer(std::span<float> detail, std::span<const float> tone,
              int width, int height, double sigma)
{
  const std::size_t plane = static_cast<std::size_t>(width) * height;
  if (detail.size() < 3 * plane || tone.size() < 3 * plane) {
    return;
  }
  std::vector<float> bd(plane), bt(plane);
  for (std::size_t c = 0; c < 3; ++c) {
    float* d = detail.data() + c * plane;
    const float* t = tone.data() + c * plane;
    std::memcpy(bd.data(), d, plane * sizeof(float));
    std::memcpy(bt.data(), t, plane * sizeof(float));
    gaussian_blur(bd, width, height, sigma);
    gaussian_blur(bt, width, height, sigma);
    for (std::size_t i = 0; i < plane; ++i) {
      d[i] = std::clamp(d[i] - bd[i] + bt[i], 0.0f, 1.0f);
    }
  }
}

}  // namespace valtz::engine::vp
