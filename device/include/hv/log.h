#pragma once

#include <sstream>
#include <string>

namespace hv {

enum class LogLevel { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/** Process-wide level; messages below it are skipped without formatting cost. */
LogLevel log_level();
void set_log_level(LogLevel level);

/** Writes one line to stderr: "<level> <HH:MM:SS.mmm> <msg> (file:line)". */
void log_message(LogLevel level, const char* file, int line,
                 const std::string& message);

}  // namespace hv

#define HV_LOG_AT(level, expr)                                              \
  do {                                                                      \
    if (static_cast<int>(level) >= static_cast<int>(::hv::log_level())) {   \
      std::ostringstream hv_log_stream;                                     \
      hv_log_stream << expr;                                                \
      ::hv::log_message((level), __FILE__, __LINE__, hv_log_stream.str());  \
    }                                                                       \
  } while (0)

#define HV_DEBUG(expr) HV_LOG_AT(::hv::LogLevel::Debug, expr)
#define HV_INFO(expr) HV_LOG_AT(::hv::LogLevel::Info, expr)
#define HV_WARN(expr) HV_LOG_AT(::hv::LogLevel::Warn, expr)
#define HV_ERROR(expr) HV_LOG_AT(::hv::LogLevel::Error, expr)
