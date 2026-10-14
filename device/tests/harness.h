#pragma once

// Tiny single-header test framework.
//
// No third-party test dependency: the board cross-build has no package manager,
// and the desktop build should need nothing but a compiler.
//
// Usage:
//   #include "harness.h"
//   HV_TEST(queue_rejects_when_full) {
//     hv::BoundedQueue<int> q(2, hv::OverflowPolicy::Reject);
//     ...
//     HV_CHECK_EQ(q.stats().depth, size_t{2});
//   }
// Register `int main()` via tests/test_main.cpp.

#include <cmath>
#include <cstdio>
#include <exception>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace hv::test {

struct Case {
  const char* name;
  void (*fn)();
};

// One instance across translation units: inline function, local static.
inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

struct Registerer {
  Registerer(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

struct State {
  std::string current;
  int checks = 0;
  int failures = 0;
  std::vector<std::pair<std::string, int>> failed_cases;
};

inline State& state() {
  static State s;
  return s;
}

inline void report_failure(const char* file, int line, const std::string& what) {
  state().failures++;
  std::fprintf(stderr, "  FAIL %s (%s:%d): %s\n", state().current.c_str(), file,
               line, what.c_str());
}

template <class T, class = void>
struct is_streamable : std::false_type {};

template <class T>
struct is_streamable<
    T, std::void_t<decltype(std::declval<std::ostream&>()
                            << std::declval<const T&>())>> : std::true_type {};

// Enums (and anything else not streamable) are printed as their underlying
// integer so failure messages always compile.
template <class T>
std::string describe(const T& value) {
  std::ostringstream oss;
  if constexpr (is_streamable<T>::value) {
    oss << value;
  } else if constexpr (std::is_enum_v<T>) {
    oss << static_cast<long long>(static_cast<std::underlying_type_t<T>>(value));
  } else {
    oss << "<unprintable>";
  }
  return oss.str();
}

inline int run_all() {
  auto& st = state();
  int failed_cases = 0;
  for (const auto& c : registry()) {
    st.current = c.name;
    const int before = st.failures;
    try {
      c.fn();
    } catch (const std::exception& e) {
      report_failure(__FILE__, __LINE__,
                     std::string("uncaught exception: ") + e.what());
    } catch (...) {
      report_failure(__FILE__, __LINE__, "uncaught non-std exception");
    }
    if (st.failures > before) {
      failed_cases++;
      st.failed_cases.push_back({c.name, st.failures - before});
    }
  }

  std::printf("\n%d checks, %d case(s) run, %d case(s) failed\n", st.checks,
              static_cast<int>(registry().size()), failed_cases);
  for (const auto& f : st.failed_cases) {
    std::printf("  failed: %s (%d checks)\n", f.first.c_str(), f.second);
  }
  return failed_cases == 0 ? 0 : 1;
}

}  // namespace hv::test

#define HV_TEST(name)                                                       \
  static void hv_test_##name();                                             \
  static ::hv::test::Registerer hv_test_reg_##name(#name, &hv_test_##name); \
  static void hv_test_##name()

#define HV_CHECK(cond)                                                     \
  do {                                                                     \
    ::hv::test::state().checks++;                                          \
    if (!(cond)) {                                                         \
      ::hv::test::report_failure(__FILE__, __LINE__, #cond);               \
    }                                                                      \
  } while (0)

#define HV_CHECK_TRUE(cond) HV_CHECK(cond)
#define HV_CHECK_FALSE(cond) HV_CHECK(!(cond))

namespace hv::test {

template <class A, class B>
inline void check_eq(const A& a, const B& b, const char* file, int line,
                     const char* ea, const char* eb) {
  ::hv::test::state().checks++;
  if (a == b) return;
  ::hv::test::report_failure(
      file, line,
      std::string(ea) + " == " + eb + " (got " + ::hv::test::describe(a) +
          ", want " + ::hv::test::describe(b) + ")");
}

template <class A, class B>
inline void check_ne(const A& a, const B& b, const char* file, int line,
                     const char* ea, const char* eb) {
  ::hv::test::state().checks++;
  if (!(a == b)) return;
  ::hv::test::report_failure(
      file, line, std::string(ea) + " != " + eb + " (both " +
                      ::hv::test::describe(a) + ")");
}

inline void check_near(double a, double b, double epsilon, const char* file,
                       int line, const char* ea, const char* eb) {
  ::hv::test::state().checks++;
  if (std::fabs(a - b) <= epsilon) return;
  std::ostringstream oss;
  oss << ea << " ~= " << eb << " (got " << a << ", want " << b << ", eps "
      << epsilon << ")";
  ::hv::test::report_failure(file, line, oss.str());
}

}  // namespace hv::test

#define HV_CHECK_EQ(a, b) \
  ::hv::test::check_eq((a), (b), __FILE__, __LINE__, #a, #b)
#define HV_CHECK_NE(a, b) \
  ::hv::test::check_ne((a), (b), __FILE__, __LINE__, #a, #b)
#define HV_CHECK_STR_EQ(a, b)                                                \
  ::hv::test::check_eq(std::string(a), std::string(b), __FILE__, __LINE__,   \
                       #a, #b)
#define HV_CHECK_NEAR(a, b, eps) \
  ::hv::test::check_near((a), (b), (eps), __FILE__, __LINE__, #a, #b)
