#pragma once

// Minimal result type shared by every device module.
//
// Rule of thumb used across the device code:
//   - setup / fatal misconfiguration throws std::runtime_error (fail fast, logged by main)
//   - runtime paths return Result, Code or a bool plus last_error(), and never throw

#include <string>
#include <utility>

namespace hv {

enum class Code {
  Ok = 0,
  Timeout,
  Io,
  Protocol,
  Conflict,
  NotFound,
  InsufficientStorage,
  Invalid,
  Internal,
};

const char* to_string(Code code);

class Result {
 public:
  Result() = default;
  Result(Code code, std::string message)
      : code_(code), message_(std::move(message)) {}

  static Result success() { return Result{}; }
  static Result error(Code code, std::string message) {
    return Result(code, std::move(message));
  }

  bool ok() const { return code_ == Code::Ok; }
  explicit operator bool() const { return ok(); }
  Code code() const { return code_; }
  const std::string& message() const { return message_; }

  /** "Ok" or "Invalid: hub_url is empty" — for logs and test failure messages. */
  std::string describe() const;

 private:
  Code code_ = Code::Ok;
  std::string message_;
};

}  // namespace hv
