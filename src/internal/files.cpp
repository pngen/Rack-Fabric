// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "internal/files.hpp"

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rack_fabric::internal {
namespace {

std::atomic<std::uint64_t> g_temp_counter{0};

[[nodiscard]] std::filesystem::path make_temp_path(const std::filesystem::path& destination) {
  const std::uint64_t counter = g_temp_counter.fetch_add(1, std::memory_order_relaxed);
#ifdef _WIN32
  const int pid = _getpid();
#else
  const int pid = static_cast<int>(::getpid());
#endif
  std::string name = destination.filename().string();
  name += ".tmp-";
  name += std::to_string(pid);
  name += '-';
  name += std::to_string(counter);
  return destination.parent_path() / name;
}

[[nodiscard]] bool flush_to_disk(std::FILE* file) {
#ifdef _WIN32
  return ::_commit(::_fileno(file)) == 0;
#else
  return ::fsync(::fileno(file)) == 0;
#endif
}

[[nodiscard]] bool replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
  return ::MoveFileExW(from.c_str(), to.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  std::error_code ec;
  std::filesystem::rename(from, to, ec);
  return !ec;
#endif
}

}  // namespace

std::string_view to_string(FileStatus status) noexcept {
  switch (status) {
    case FileStatus::Ok:
      return "OK";
    case FileStatus::NotFound:
      return "NOT_FOUND";
    case FileStatus::AccessDenied:
      return "ACCESS_DENIED";
    case FileStatus::IoError:
      return "IO_ERROR";
    case FileStatus::TooLarge:
      return "TOO_LARGE";
    case FileStatus::Interrupted:
      return "INTERRUPTED";
  }
  return "IO_ERROR";
}

FileStatus read_file(const std::filesystem::path& path, std::vector<std::byte>& out,
                     std::uint64_t max_bytes, std::string& detail) {
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    detail = "file does not exist";
    return FileStatus::NotFound;
  }
  const std::uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    detail = "cannot determine file size";
    return FileStatus::IoError;
  }
  if (size > max_bytes) {
    detail = "file exceeds the configured maximum size";
    return FileStatus::TooLarge;
  }
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    detail = "cannot open file for reading";
    return FileStatus::AccessDenied;
  }
  out.resize(static_cast<std::size_t>(size));
  if (size > 0) {
    stream.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size));
    if (stream.gcount() != static_cast<std::streamsize>(size)) {
      detail = "short read";
      out.clear();
      return FileStatus::Interrupted;
    }
  }
  return FileStatus::Ok;
}

FileStatus write_file_atomic(const std::filesystem::path& path, const std::byte* data,
                             std::size_t size, bool atomic_replace, std::string& detail) {
  const std::filesystem::path target = atomic_replace ? make_temp_path(path) : path;
  std::FILE* file = nullptr;
#ifdef _WIN32
  if (::_wfopen_s(&file, target.c_str(), L"wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(target.string().c_str(), "wb");
#endif
  if (file == nullptr) {
    detail = "cannot open destination for writing";
    return FileStatus::AccessDenied;
  }
  if (size > 0 && std::fwrite(data, 1, size, file) != size) {
    std::fclose(file);
    remove_file_quietly(target);
    detail = "short write";
    return FileStatus::IoError;
  }
  if (std::fflush(file) != 0 || !flush_to_disk(file)) {
    std::fclose(file);
    remove_file_quietly(target);
    detail = "cannot flush file to storage";
    return FileStatus::IoError;
  }
  if (std::fclose(file) != 0) {
    remove_file_quietly(target);
    detail = "cannot close file";
    return FileStatus::IoError;
  }
  if (atomic_replace && !replace_file(target, path)) {
    remove_file_quietly(target);
    detail = "cannot replace destination atomically";
    return FileStatus::IoError;
  }
  return FileStatus::Ok;
}

void remove_file_quietly(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
}

}  // namespace rack_fabric::internal
