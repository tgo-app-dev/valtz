// Logging.
//
// One process-wide sink. The default writes to the unified log
// (subsystem "com.tgous.valtz", viewable in Console.app and `log stream`) and,
// in debug builds, to stderr. The controller installs a sink that also
// forwards warnings and errors to the UI's event stream.

#ifndef VALTZ_BASE_LOG_H
#define VALTZ_BASE_LOG_H

#include <format>
#include <functional>
#include <string>
#include <string_view>

namespace valtz {

enum class LogLevel { Debug, Info, Warn, Error };

const char* to_str(LogLevel);

using LogSink = std::function<void(LogLevel, std::string_view category,
                                   std::string_view message)>;

// Replace the sink (nullptr restores the default). Thread-safe.
void set_log_sink(LogSink);
void set_log_level(LogLevel);
LogLevel log_level();

void log_message(LogLevel, std::string_view category, std::string_view);

template <class... Args>
void
log_at(LogLevel lvl, std::string_view category,
       std::format_string<Args...> fmt, Args&&... args)
{
  if (lvl < log_level()) {
    return;
  }
  log_message(lvl, category, std::format(fmt, std::forward<Args>(args)...));
}

#define VALTZ_LOG_DEBUG(cat, ...) \
  ::valtz::log_at(::valtz::LogLevel::Debug, cat, __VA_ARGS__)
#define VALTZ_LOG_INFO(cat, ...) \
  ::valtz::log_at(::valtz::LogLevel::Info, cat, __VA_ARGS__)
#define VALTZ_LOG_WARN(cat, ...) \
  ::valtz::log_at(::valtz::LogLevel::Warn, cat, __VA_ARGS__)
#define VALTZ_LOG_ERROR(cat, ...) \
  ::valtz::log_at(::valtz::LogLevel::Error, cat, __VA_ARGS__)

}

#endif
