// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Atomic file replacement and durable flushing.

#ifndef RACK_FABRIC_INTERNAL_FILES_HPP
#define RACK_FABRIC_INTERNAL_FILES_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace rack_fabric::internal {

enum class FileStatus {
  Ok = 0,
  NotFound,
  AccessDenied,
  IoError,
  TooLarge,
  Interrupted,
};

[[nodiscard]] std::string_view to_string(FileStatus status) noexcept;

/// Reads an entire file. Reports the exact byte count on success.
[[nodiscard]] FileStatus read_file(const std::filesystem::path& path, std::vector<std::byte>& out,
                                   std::uint64_t max_bytes, std::string& detail);

/// Writes bytes to a temporary file in the same directory, flushes it to the
/// storage device and then replaces the destination atomically. When
/// atomic_replace is false the destination is written directly.
[[nodiscard]] FileStatus write_file_atomic(const std::filesystem::path& path, const std::byte* data,
                                           std::size_t size, bool atomic_replace,
                                           std::string& detail);

/// Removes a file, ignoring absence. Used to clean temporary artifacts.
void remove_file_quietly(const std::filesystem::path& path);

}  // namespace rack_fabric::internal

#endif  // RACK_FABRIC_INTERNAL_FILES_HPP
