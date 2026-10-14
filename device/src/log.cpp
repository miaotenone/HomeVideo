#include "hv/log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>

#include "hv/status.h"

namespace hv {

namespace {

std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};

const char* level_name(LogLevel level) {
  switch (level) {
    case LogLevel::Debug:
      return "DEBUG";
    case LogLevel::Info:
      return "INFO ";
    case LogLevel::Warn:
      return "WARN ";
    case LogLevel::Error:
      return "ERROR";
  }
  return "?????";
}

}  // namespace

const char* to_string(Code code) {
  switch (code) {
    case Code::Ok:
      return "Ok";
    case Code::Timeout:
      return "Timeout";
    case Code::Io:
      return "Io";
    case Code::Protocol:
      return "Protocol";
    case Code::Conflict:
      return "Conflict";
    case Code::NotFound:
      return "NotFound";
    case Code::InsufficientStorage:
      return "InsufficientStorage";
    case Code::Invalid:
      return "Invalid";
    case Code::Internal:
      return "Internal";
  }
  return "Unknown";
}

std::string Result::describe() const {
  if (ok()) return "Ok";
  if (message_.empty()) return to_string(code_);
  return std::string(to_string(code_)) + ": " + message_;
}

LogLevel log_level() {
  return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

void set_log_level(LogLevel level) {
  g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

void log_message(LogLevel level, const char* file, int line,
                 const std::string& message) {
  using clock = std::chrono::system_clock;
  const auto now = clock::now();
  const auto ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) %
      1000;
  const std::time_t seconds = clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &seconds);
#else
  gmtime_r(&seconds, &tm);
#endif
  char stamp[32];
  std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d", tm.tm_hour,
                tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));

  // Single fprintf keeps the line intact when threads log concurrently.
  std::fprintf(stderr, "%s %s %s (%s:%d)\n", level_name(level), stamp,
               message.c_str(), file, line);
  std::fflush(stderr);
}

}  // namespace hv
