// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Minimal structured logging for the coordinator and agent executables.
// Logging is local only. Rack Fabric never transmits telemetry.

#ifndef RACK_FABRIC_INTERNAL_LOG_HPP
#define RACK_FABRIC_INTERNAL_LOG_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <string>
#include <string_view>

namespace rack_fabric::internal {

enum class LogLevel : std::uint8_t { Error = 0, Warn = 1, Info = 2, Debug = 3 };

class Logger {
 public:
  Logger() = delete;

  static void set_level(LogLevel level) noexcept;
  [[nodiscard]] static LogLevel level() noexcept;
  static void set_stream(std::ostream* stream) noexcept;

  static void write(LogLevel level, std::string_view component, std::string_view message);

  static void error(std::string_view component, std::string_view message) {
    write(LogLevel::Error, component, message);
  }
  static void warn(std::string_view component, std::string_view message) {
    write(LogLevel::Warn, component, message);
  }
  static void info(std::string_view component, std::string_view message) {
    write(LogLevel::Info, component, message);
  }
  static void debug(std::string_view component, std::string_view message) {
    write(LogLevel::Debug, component, message);
  }

 private:
  static std::atomic<LogLevel> level_;
  static std::mutex mutex_;
  static std::ostream* stream_;
};

}  // namespace rack_fabric::internal

#endif  // RACK_FABRIC_INTERNAL_LOG_HPP
