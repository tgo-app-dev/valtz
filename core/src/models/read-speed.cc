#include "valtz/models/read-speed.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <thread>

namespace valtz::models {

namespace fs = std::filesystem;

namespace {

using Clock = std::chrono::steady_clock;

struct Opened {
  fs::path      path;
  int           fd = -1;
  std::uint64_t size = 0;
};

// A stretch of one file to read: its start on a page, its length whole
// pages (a file's last one read short, at its end).
struct Span {
  int           file = 0;
  std::uint64_t off = 0;
  std::uint64_t len = 0;
};

std::uint64_t
round_up(std::uint64_t n, std::uint64_t page)
{
  return (n + page - 1) / page * page;
}

// The stretches of `f` not in memory, by its pages, and their bytes.
// mincore on a mapping of the file says which of its pages the buffer
// cache holds; a file it cannot say for is read whole.
std::vector<Span>
absent_spans(int index, const Opened& f, std::uint64_t page,
             std::uint64_t* absent)
{
  std::vector<Span> out;
  if (f.size == 0) {
    return out;
  }
  auto whole = [&] {
    out.assign(1, Span{index, 0, round_up(f.size, page)});
    *absent += f.size;
    return out;
  };
  void* map = ::mmap(nullptr, f.size, PROT_READ, MAP_SHARED, f.fd, 0);
  if (map == MAP_FAILED) {
    return whole();
  }
  const std::uint64_t pages = (f.size + page - 1) / page;
  std::vector<char> in(pages);
  const bool known = ::mincore(map, f.size, in.data()) == 0;
  ::munmap(map, f.size);
  if (!known) {
    return whole();
  }
  for (std::uint64_t p = 0; p < pages;) {
    if (in[p] & MINCORE_INCORE) {
      ++p;
      continue;
    }
    const std::uint64_t from = p;
    while (p < pages && !(in[p] & MINCORE_INCORE)) {
      ++p;
    }
    out.push_back({index, from * page, (p - from) * page});
    *absent += std::min(f.size, p * page) - from * page;
  }
  return out;
}

// `want` bytes of `spans` -- laid end to end, `total` of them -- in runs
// of `run`, spread evenly through them; all of them, in order, when they
// hold no more. Each run a list of pieces of whole pages.
std::vector<std::vector<Span>>
plan_runs(const std::vector<Span>& spans, std::uint64_t total,
          std::uint64_t want, std::uint64_t run, std::uint64_t page)
{
  std::vector<std::vector<Span>> runs;
  if (total == 0 || run == 0) {
    return runs;
  }
  const bool all = total <= want;
  const std::uint64_t n = all ? (total + run - 1) / run
                              : std::max<std::uint64_t>(1, want / run);
  std::size_t si = 0;          // the span `at` lies in...
  std::uint64_t base = 0;      // ...and where it starts, end to end
  for (std::uint64_t k = 0; k < n; ++k) {
    std::uint64_t at = all ? k * run : total / n * k;
    at -= at % page;
    while (si < spans.size() && base + spans[si].len <= at) {
      base += spans[si].len;
      ++si;
    }
    std::vector<Span> pieces;
    std::uint64_t left = std::min(run, total - at);
    std::uint64_t pos = at;
    std::size_t sj = si;
    std::uint64_t bj = base;
    while (left > 0 && sj < spans.size()) {
      const Span& s = spans[sj];
      const std::uint64_t into = pos - bj;
      const std::uint64_t take = std::min(left, s.len - into);
      pieces.push_back({s.file, s.off + into, take});
      left -= take;
      pos += take;
      if (into + take == s.len) {
        bj += s.len;
        ++sj;
      }
    }
    runs.push_back(std::move(pieces));
  }
  return runs;
}

struct Shared {
  std::atomic<std::size_t>   next{0};
  std::atomic<std::uint64_t> bytes{0};
  std::atomic<bool>          halt{false};
  std::atomic<bool>          stopped{false};
  std::mutex                 mu;
  std::string                error;
};

void
read_runs(const std::vector<Opened>& files,
          const std::vector<std::vector<Span>>& runs, Shared& sh,
          const ReadSpeedOptions& opt, Clock::time_point deadline,
          std::uint64_t page)
{
  void* buf = nullptr;
  if (::posix_memalign(&buf, page, round_up(opt.chunk, page)) != 0) {
    std::lock_guard lk(sh.mu);
    sh.error = "no memory for a read buffer";
    sh.halt = true;
    return;
  }
  const std::size_t chunk = round_up(opt.chunk, page);
  for (;;) {
    const std::size_t r = sh.next.fetch_add(1);
    if (r >= runs.size() || sh.halt) {
      break;
    }
    for (const Span& s : runs[r]) {
      std::uint64_t o = 0;
      while (o < s.len && !sh.halt) {
        if (Clock::now() >= deadline) {
          sh.halt = true;
          break;
        }
        if (opt.stop && opt.stop()) {
          sh.stopped = true;
          sh.halt = true;
          break;
        }
        const std::size_t n =
            static_cast<std::size_t>(std::min<std::uint64_t>(chunk,
                                                             s.len - o));
        const ssize_t got = ::pread(files[static_cast<std::size_t>(s.file)].fd,
                                    buf, n, static_cast<off_t>(s.off + o));
        if (got < 0) {
          if (errno == EINTR) {
            continue;
          }
          std::lock_guard lk(sh.mu);
          if (sh.error.empty()) {
            sh.error = std::format(
                "{}: {}", files[static_cast<std::size_t>(s.file)].path.string(),
                std::strerror(errno));
          }
          sh.halt = true;
          break;
        }
        sh.bytes += static_cast<std::uint64_t>(got);
        if (static_cast<std::size_t>(got) < n) {
          break;              // the file's end
        }
        o += n;
      }
    }
  }
  std::free(buf);
}

fs::path
common_folder(const std::vector<Opened>& files)
{
  if (files.empty()) {
    return {};
  }
  fs::path at = files.front().path.parent_path();
  while (!at.empty()) {
    const bool all = std::ranges::all_of(files, [&](const Opened& f) {
      return path_within(f.path, at);
    });
    if (all || at == at.root_path()) {
      break;
    }
    at = at.parent_path();
  }
  return at;
}

double
rate(std::uint64_t read, double seconds)
{
  return read > 0 && seconds > 0
             ? static_cast<double>(read) / seconds / 1e9 : 0.0;
}

}  // namespace

double
ReadSpeedPlace::gb_per_s() const
{
  return rate(read, seconds);
}

double
ReadSpeed::gb_per_s() const
{
  return rate(read, seconds);
}

Json
ReadSpeed::to_json() const
{
  Json ps = Json::array();
  for (const auto& p : places) {
    ps.push_back({
      {"path", p.path.string()},
      {"volume", {{"name", p.volume.name}, {"protocol", p.volume.protocol},
                  {"device", p.volume.device},
                  {"internal", p.volume.internal},
                  {"network", p.volume.network}}},
      {"total", p.total}, {"cached", p.cached}, {"read", p.read},
      {"seconds", p.seconds}, {"gbps", p.gb_per_s()}, {"files", p.files},
      {"whole", p.whole},
    });
  }
  return {{"total", total}, {"cached", cached}, {"read", read},
          {"seconds", seconds}, {"gbps", gb_per_s()}, {"files", files},
          {"whole", whole}, {"stopped", stopped}, {"error", error},
          {"places", ps}};
}

ReadSpeed
measure_read_speed(const std::vector<fs::path>& paths,
                   const ReadSpeedOptions& opt)
{
  ReadSpeed out;
  const auto page = static_cast<std::uint64_t>(::getpagesize());
  // The files by the drive they are on, in the order first met.
  std::map<dev_t, std::vector<Opened>> by_drive;
  std::vector<dev_t> order;
  for (const auto& p : paths) {
    const int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) {
      if (out.error.empty()) {
        out.error = std::format("{}: {}", p.string(), std::strerror(errno));
      }
      continue;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      ::close(fd);
      continue;
    }
    // Past the buffer cache: the drive serves each read, nothing stays.
    (void)::fcntl(fd, F_NOCACHE, 1);
    if (!by_drive.contains(st.st_dev)) {
      order.push_back(st.st_dev);
    }
    by_drive[st.st_dev].push_back(
        {p, fd, static_cast<std::uint64_t>(st.st_size)});
  }
  std::uint64_t all = 0;
  for (const auto& [dev, files] : by_drive) {
    for (const auto& f : files) {
      all += f.size;
    }
  }
  out.whole = true;
  for (const dev_t dev : order) {
    const auto& files = by_drive[dev];
    ReadSpeedPlace place;
    place.files = static_cast<int>(files.size());
    place.path = common_folder(files);
    place.volume = volume_info(place.path);
    std::vector<Span> spans;
    std::uint64_t absent = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
      place.total += files[i].size;
      for (const Span& s :
           absent_spans(static_cast<int>(i), files[i], page, &absent)) {
        spans.push_back(s);
      }
    }
    place.cached = place.total - absent;
    std::uint64_t planned = 0;
    for (const Span& s : spans) {
      planned += s.len;
    }
    // Several drives: each its share of the budget, two seconds and a
    // gigabyte at least.
    const double share = order.size() == 1 || all == 0
        ? 1.0 : static_cast<double>(place.total) / static_cast<double>(all);
    const double seconds = order.size() == 1
        ? opt.max_seconds : std::max(2.0, opt.max_seconds * share);
    const std::uint64_t bytes = order.size() == 1
        ? opt.max_bytes
        : std::max<std::uint64_t>(
              1'000'000'000ull,
              static_cast<std::uint64_t>(
                  static_cast<double>(opt.max_bytes) * share));
    const auto runs = plan_runs(spans, planned, bytes, opt.run, page);
    Shared sh;
    const int streams = std::max(
        1, std::min(opt.streams, static_cast<int>(runs.size())));
    const auto t0 = Clock::now();
    const auto deadline =
        t0 + std::chrono::duration_cast<Clock::duration>(
                 std::chrono::duration<double>(seconds));
    if (!runs.empty()) {
      std::vector<std::thread> workers;
      for (int w = 0; w < streams; ++w) {
        workers.emplace_back(read_runs, std::cref(files), std::cref(runs),
                             std::ref(sh), std::cref(opt), deadline, page);
      }
      for (auto& t : workers) {
        t.join();
      }
    }
    place.seconds = runs.empty()
        ? 0.0 : std::chrono::duration<double>(Clock::now() - t0).count();
    place.read = sh.bytes;
    place.whole = planned <= bytes && !sh.halt;
    if (!sh.error.empty() && out.error.empty()) {
      out.error = sh.error;
    }
    out.stopped = out.stopped || sh.stopped;
    out.total += place.total;
    out.cached += place.cached;
    out.read += place.read;
    out.seconds += place.seconds;
    out.files += place.files;
    out.whole = out.whole && place.whole;
    out.places.push_back(std::move(place));
    if (out.stopped) {
      break;
    }
  }
  for (auto& [dev, files] : by_drive) {
    for (const auto& f : files) {
      ::close(f.fd);
    }
  }
  return out;
}

std::vector<fs::path>
files_to_load(const InstallInfo& info)
{
  std::vector<fs::path> out;
  std::error_code ec;
  if (!info.file.empty()) {
    out.push_back(info.dir / info.file);
  } else if (!info.dir.empty()) {
    // Not what hides: a clone's .git (its LFS objects a second copy of
    // the weights), a fetcher's .cache.
    for (auto it = fs::recursive_directory_iterator(
             info.dir, fs::directory_options::follow_directory_symlink |
                           fs::directory_options::skip_permission_denied,
             ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      if (it->path().filename().string().starts_with(".")) {
        if (it->is_directory(ec)) {
          it.disable_recursion_pending();
        }
        continue;
      }
      if (it->is_regular_file(ec)) {
        out.push_back(it->path());
      }
    }
    std::ranges::sort(out);
  }
  for (const auto& [part, link] : info.parts) {
    if (link.present && std::ranges::find(out, link.file) == out.end()) {
      out.push_back(link.file);
    }
  }
  return out;
}

}  // namespace valtz::models
