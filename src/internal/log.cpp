// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "internal/log.hpp"

#include <iostream>

namespace rack_fabric::internal {

std::atomic<LogLevel> Logger::level_{LogLevel::Info};
std::mutex Logger::mutex_;
std::ostream* Logger::stream_ = nullptr;

void Logger::set_level(LogLevel level) noexcept { level_.store(level, std::memory_order_relaxed); }

LogLevel Logger::level() noexcept { return level_.load(std::memory_order_relaxed); }

void Logger::set_stream(std::ostream* stream) noexcept {
  std::lock_guard<std::mutex> guard(mutex_);
  stream_ = stream;
}

void Logger::write(LogLevel level, std::string_view component, std::string_view message) {
  if (static_cast<std::uint8_t>(level) > static_cast<std::uint8_t>(level_.load(std::memory_order_relaxed))) {
    return;
  }
  const char* label = "INFO";
  switch (level) {
    case LogLevel::Error:
      label = "ERROR";
      break;
    case LogLevel::Warn:
      label = "WARN";
      break;
    case LogLevel::Info:
      label = "INFO";
      break;
    case LogLevel::Debug:
      label = "DEBUG";
      break;
  }
  std::lock_guard<std::mutex> guard(mutex_);
  std::ostream& out = stream_ != nullptr ? *stream_ : std::cerr;
  out << '[' << label << "] [" << component << "] " << message << '\n';
  out.flush();
}

}  // namespace rack_fabric::internal
