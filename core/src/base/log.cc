#include "valtz/base/log.h"

#include "valtz/base/paths.h"

#include <os/log.h>

#include <atomic>
#include <cstdio>
#include <mutex>

namespace valtz {

namespace {

std::mutex             g_mu;
LogSink                g_sink;
std::atomic<LogLevel>  g_level{LogLevel::Info};

void
default_sink(LogLevel lvl, std::string_view cat, std::string_view msg)
{
  static os_log_t log = os_log_create(kBundleId, "core");
  os_log_type_t t = OS_LOG_TYPE_DEFAULT;
  switch (lvl) {
  case LogLevel::Debug: t = OS_LOG_TYPE_DEBUG; break;
  case LogLevel::Info:  t = OS_LOG_TYPE_INFO; break;
  case LogLevel::Warn:  t = OS_LOG_TYPE_DEFAULT; break;
  case LogLevel::Error: t = OS_LOG_TYPE_ERROR; break;
  }
  std::string line = std::format("[{}] {}", cat, msg);
  os_log_with_type(log, t, "%{public}s", line.c_str());
#ifndef NDEBUG
  std::fprintf(stderr, "valtz %-5s %s\n", to_str(lvl), line.c_str());
#endif
}

}

const char*
to_str(LogLevel l)
{
  switch (l) {
  case LogLevel::Debug: return "debug";
  case LogLevel::Info:  return "info";
  case LogLevel::Warn:  return "warn";
  case LogLevel::Error: return "error";
  }
  return "?";
}

void
set_log_sink(LogSink sink)
{
  std::lock_guard lk(g_mu);
  g_sink = std::move(sink);
}

void
set_log_level(LogLevel l)
{
  g_level.store(l, std::memory_order_relaxed);
}

LogLevel
log_level()
{
  return g_level.load(std::memory_order_relaxed);
}

void
log_message(LogLevel lvl, std::string_view cat, std::string_view msg)
{
  if (lvl < log_level()) {
    return;
  }
  LogSink sink;
  {
    std::lock_guard lk(g_mu);
    sink = g_sink;
  }
  if (sink) {
    sink(lvl, cat, msg);
  } else {
    default_sink(lvl, cat, msg);
  }
}

}
