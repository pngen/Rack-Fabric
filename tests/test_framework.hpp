// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small test framework.
//
// It has no third-party dependencies, it never terminates the process on a
// failed assertion (so a failure reports the whole picture rather than a
// crash dialog), and it imposes no timeout of any kind. A test that hangs is
// a defect to be diagnosed, not a test to be killed.

#ifndef RACK_FABRIC_TESTS_TEST_FRAMEWORK_HPP
#define RACK_FABRIC_TESTS_TEST_FRAMEWORK_HPP

#include <atomic>
#include <chrono>
#include <cstddef>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace rftest {

struct TestCase {
  std::string suite;
  std::string name;
  void (*function)();
};

struct Failure {
  std::string test;
  std::string where;
  std::string message;
};

[[nodiscard]] std::vector<TestCase>& registry();

int register_test(const char* suite, const char* name, void (*function)());

/// Records a failure for the currently running test.
void record_failure(const char* file, int line, const std::string& message);

/// Records a fatal failure and stops the current test.
struct FatalFailure {};

namespace detail {

/// Detects a library to_string overload found by argument-dependent lookup, so
/// that every strongly typed enum in Rack Fabric is rendered by name instead
/// of failing to compile.
template <class T, class = void>
struct has_to_string : std::false_type {};

template <class T>
struct has_to_string<T, std::void_t<decltype(to_string(std::declval<const T&>()))>>
    : std::true_type {};

/// Detects a member to_string(), used by record and key types.
template <class T, class = void>
struct has_member_to_string : std::false_type {};

template <class T>
struct has_member_to_string<T,
                            std::void_t<decltype(std::declval<const T&>().to_string())>>
    : std::true_type {};

}  // namespace detail

[[nodiscard]] std::string describe(const std::string& value);
[[nodiscard]] std::string describe(const char* value);
[[nodiscard]] std::string describe(bool value);
template <class T>
[[nodiscard]] std::string describe(const T& value) {
  if constexpr (detail::has_to_string<T>::value) {
    return std::string(to_string(value));
  } else if constexpr (detail::has_member_to_string<T>::value) {
    return std::string(value.to_string());
  } else {
    std::ostringstream out;
    out << value;
    return out.str();
  }
}

int run_all(const std::vector<std::string>& filters, std::size_t repeat, bool list_only);

}  // namespace rftest

#define RF_TEST(suite, name)                                                              \
  static void suite##_##name##_body();                                                    \
  static const int suite##_##name##_registration =                                        \
      rftest::register_test(#suite, #name, suite##_##name##_body);                        \
  static void suite##_##name##_body()

#define RF_CHECK(expression)                                                              \
  do {                                                                                    \
    if (!(expression)) {                                                                  \
      rftest::record_failure(__FILE__, __LINE__, "RF_CHECK failed: " #expression);         \
    }                                                                                     \
  } while (false)

#define RF_CHECK_EQ(expected, actual)                                                     \
  do {                                                                                    \
    const auto rf_expected_value = (expected);                                            \
    const auto rf_actual_value = (actual);                                                \
    if (!(rf_expected_value == rf_actual_value)) {                                         \
      rftest::record_failure(__FILE__, __LINE__,                                          \
                             std::string("RF_CHECK_EQ failed: " #expected " == " #actual) + \
                                 " expected=" + rftest::describe(rf_expected_value) +       \
                                 " actual=" + rftest::describe(rf_actual_value));           \
    }                                                                                     \
  } while (false)

#define RF_CHECK_NE(left, right)                                                          \
  do {                                                                                    \
    if ((left) == (right)) {                                                              \
      rftest::record_failure(__FILE__, __LINE__, "RF_CHECK_NE failed: " #left " != " #right); \
    }                                                                                     \
  } while (false)

#define RF_REQUIRE(expression)                                                            \
  do {                                                                                    \
    if (!(expression)) {                                                                  \
      rftest::record_failure(__FILE__, __LINE__, "RF_REQUIRE failed: " #expression);       \
      return;                                                                             \
    }                                                                                     \
  } while (false)

#endif  // RACK_FABRIC_TESTS_TEST_FRAMEWORK_HPP
