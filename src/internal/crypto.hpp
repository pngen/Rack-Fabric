// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Integrity primitives used by the persistence format and by snapshot
// digests. First-party and dependency free.

#ifndef RACK_FABRIC_INTERNAL_CRYPTO_HPP
#define RACK_FABRIC_INTERNAL_CRYPTO_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace rack_fabric::internal {

/// CRC-32C (Castagnoli), used for fast frame and file integrity checks.
[[nodiscard]] std::uint32_t crc32c(const std::byte* data, std::size_t size, std::uint32_t seed = 0) noexcept;
[[nodiscard]] std::uint32_t crc32c(std::string_view data, std::uint32_t seed = 0) noexcept;

/// SHA-256, used for snapshot digests and persistence integrity.
class Sha256 {
 public:
  Sha256() noexcept;
  void update(const std::byte* data, std::size_t size) noexcept;
  void update(std::string_view data) noexcept;
  /// Finalizes and returns the 32-byte digest. The object must not be used
  /// afterwards except by calling reset().
  [[nodiscard]] std::string finish();
  void reset() noexcept;

  [[nodiscard]] static std::string hash(const std::byte* data, std::size_t size);

 private:
  void transform(const std::byte* block) noexcept;

  std::uint32_t state_[8];
  std::uint64_t bit_count_;
  std::byte buffer_[64];
  std::size_t buffer_length_;
};

/// Lowercase hexadecimal rendering of a byte string.
[[nodiscard]] std::string to_hex(const std::byte* data, std::size_t size);

}  // namespace rack_fabric::internal

#endif  // RACK_FABRIC_INTERNAL_CRYPTO_HPP
