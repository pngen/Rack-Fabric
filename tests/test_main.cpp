// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "test_framework.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

namespace rftest {
namespace {

std::vector<Failure> g_failures;
std::mutex g_failure_mutex;
std::string g_current_test;
std::atomic<std::size_t> g_current_test_index{0};

}  // namespace

std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

int register_test(const char* suite, const char* name, void (*function)()) {
  registry().push_back(TestCase{suite, name, function});
  return 0;
}

void record_failure(const char* file, int line, const std::string& message) {
  // Tests are allowed to assert from worker threads, so failure recording is
  // serialised. A failure is never dropped and never interleaves with another.
  Failure failure;
  failure.test = g_current_test;
  failure.where = std::string(file) + ":" + std::to_string(line);
  failure.message = message;
  const std::lock_guard<std::mutex> lock(g_failure_mutex);
  // Report before moving the record: a moved-from string renders as empty,
  // which would hide the very detail a failure needs to carry.
  std::cout << "  FAIL " << g_current_test << " at " << failure.where << ": " << failure.message
            << "\n";
  std::cout.flush();
  g_failures.push_back(std::move(failure));
}

std::string describe(const std::string& value) { return "\"" + value + "\""; }
std::string describe(const char* value) {
  return value == nullptr ? std::string("<null>") : "\"" + std::string(value) + "\"";
}
std::string describe(bool value) { return value ? "true" : "false"; }

int run_all(const std::vector<std::string>& filters, std::size_t repeat, bool list_only) {
  std::vector<TestCase> selected;
  for (const auto& test : registry()) {
    if (filters.empty()) {
      selected.push_back(test);
      continue;
    }
    for (const auto& filter : filters) {
      if (test.name.find(filter) != std::string::npos ||
          test.suite.find(filter) != std::string::npos) {
        selected.push_back(test);
        break;
      }
    }
  }
  std::sort(selected.begin(), selected.end(), [](const TestCase& lhs, const TestCase& rhs) {
    if (lhs.suite != rhs.suite) {
      return lhs.suite < rhs.suite;
    }
    return lhs.name < rhs.name;
  });
  if (list_only) {
    for (const auto& test : selected) {
      std::cout << test.suite << "." << test.name << "\n";
    }
    return 0;
  }
  if (selected.empty()) {
    std::cerr << "no tests matched the filter\n";
    return 2;
  }

  std::size_t total_failures = 0;
  const std::size_t repeats = std::max<std::size_t>(1, repeat);
  const auto started = std::chrono::steady_clock::now();
  for (std::size_t round = 0; round < repeats; ++round) {
    if (repeats > 1) {
      std::cout << "-- round " << (round + 1) << " of " << repeats << "\n";
    }
    for (const auto& test : selected) {
      g_current_test = test.suite + "." + test.name;
      const std::size_t before = g_failures.size();
      std::cout << "RUN  " << g_current_test << "\n";
      std::cout.flush();
      try {
        test.function();
      } catch (const FatalFailure&) {
        // Already recorded.
      } catch (const std::exception& error) {
        record_failure(__FILE__, __LINE__,
                       std::string("unhandled exception: ") + error.what());
      } catch (...) {
        record_failure(__FILE__, __LINE__, "unhandled non-standard exception");
      }
      const std::size_t after = g_failures.size();
      if (after == before) {
        std::cout << "PASS " << g_current_test << "\n";
      } else {
        total_failures += after - before;
        std::cout << "FAIL " << g_current_test << " (" << (after - before) << " checks)\n";
      }
      std::cout.flush();
    }
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  std::cout << "\n" << selected.size() << " tests, " << total_failures << " failed checks in "
            << elapsed.count() << " ms\n";
  std::cout.flush();
  return total_failures == 0 ? 0 : 1;
}

}  // namespace rftest

int main(int argc, char** argv) {
  std::vector<std::string> filters;
  std::size_t repeat = 1;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--list") {
      list_only = true;
    } else if (argument == "--repeat" && index + 1 < argc) {
      repeat = static_cast<std::size_t>(std::stoull(argv[++index]));
    } else if (argument.rfind("--filter=", 0) == 0) {
      filters.push_back(argument.substr(9));
    } else if (argument == "--filter" && index + 1 < argc) {
      filters.push_back(argv[++index]);
    } else {
      filters.push_back(argument);
    }
  }
  return rftest::run_all(filters, repeat, list_only);
}
