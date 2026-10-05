#pragma once

// Minimal assertions for host tests. A test is a plain main() that calls CHECK
// macros and ends with `return tinta_test::result();`.

#include <cmath>
#include <cstdio>
#include <cstring>

namespace tinta_test {

inline int& failures() {
  static int count = 0;
  return count;
}

inline int result() {
  if (failures() > 0) std::printf("  %d check(s) failed\n", failures());
  return failures() > 0 ? 1 : 0;
}

}  // namespace tinta_test

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  %s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++tinta_test::failures();                                              \
    }                                                                        \
  } while (0)

#define CHECK_EQ(actual, expected)                                                                        \
  do {                                                                                                    \
    const long long tinta_a = static_cast<long long>(actual);                                             \
    const long long tinta_e = static_cast<long long>(expected);                                           \
    if (tinta_a != tinta_e) {                                                                             \
      std::printf("  %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, #actual, tinta_a, tinta_e); \
      ++tinta_test::failures();                                                                           \
    }                                                                                                     \
  } while (0)

#define CHECK_STR_EQ(actual, expected)                                                                        \
  do {                                                                                                        \
    const char* tinta_a = (actual);                                                                           \
    const char* tinta_e = (expected);                                                                         \
    if (std::strcmp(tinta_a, tinta_e) != 0) {                                                                 \
      std::printf("  %s:%d: %s == \"%s\", expected \"%s\"\n", __FILE__, __LINE__, #actual, tinta_a, tinta_e); \
      ++tinta_test::failures();                                                                               \
    }                                                                                                         \
  } while (0)

#define CHECK_NEAR(actual, expected, tolerance)                                                            \
  do {                                                                                                     \
    const double tinta_a = static_cast<double>(actual);                                                    \
    const double tinta_e = static_cast<double>(expected);                                                  \
    if (std::fabs(tinta_a - tinta_e) > (tolerance)) {                                                      \
      std::printf("  %s:%d: %s == %g, expected %g (±%g)\n", __FILE__, __LINE__, #actual, tinta_a, tinta_e, \
                  static_cast<double>(tolerance));                                                         \
      ++tinta_test::failures();                                                                            \
    }                                                                                                      \
  } while (0)
