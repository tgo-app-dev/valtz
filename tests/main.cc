#include "testing.h"

#include <cstring>
#include <unistd.h>

namespace valtz::test {

namespace {
int         g_failures = 0;
bool        g_skipped = false;
std::string g_skip_reason;
std::filesystem::path g_root;
}

std::vector<Case>&
registry()
{
  static std::vector<Case> r;
  return r;
}

void
record_failure(const char* file, int line, const std::string& what)
{
  ++g_failures;
  std::fprintf(stderr, "    FAILED %s:%d: %s\n", file, line, what.c_str());
}

void
record_skip(const std::string& why)
{
  g_skipped = true;
  g_skip_reason = why;
}

std::filesystem::path
temp_dir(const std::string& tag)
{
  static int n = 0;
  auto p = g_root / (tag + "-" + std::to_string(++n));
  std::filesystem::create_directories(p);
  return p;
}

}

int
main(int argc, char** argv)
{
  using namespace valtz::test;
  const char* filter = argc > 2 && std::strcmp(argv[1], "--filter") == 0
                           ? argv[2]
                           : nullptr;
  g_root = std::filesystem::temp_directory_path() /
           ("valtz-test-" + std::to_string(getpid()));
  std::filesystem::create_directories(g_root);

  int ran = 0, failed = 0, skipped = 0;
  for (const auto& c : registry()) {
    std::string full = std::string(c.suite) + "." + c.name;
    if (filter && full.find(filter) == std::string::npos) {
      continue;
    }
    int before = g_failures;
    g_skipped = false;
    std::fprintf(stderr, "[ RUN  ] %s\n", full.c_str());
    c.fn();
    ++ran;
    if (g_failures != before) {
      ++failed;
      std::fprintf(stderr, "[ FAIL ] %s\n", full.c_str());
    } else if (g_skipped) {
      ++skipped;
      std::fprintf(stderr, "[ SKIP ] %s (%s)\n", full.c_str(),
                   g_skip_reason.c_str());
    } else {
      std::fprintf(stderr, "[  OK  ] %s\n", full.c_str());
    }
  }
  std::error_code ec;
  std::filesystem::remove_all(g_root, ec);
  std::fprintf(stderr, "\n%d ran, %d passed, %d failed, %d skipped\n", ran,
               ran - failed - skipped, failed, skipped);
  return failed ? 1 : 0;
}
