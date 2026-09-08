// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "internal/crypto.hpp"

#include <array>
#include <cstring>

namespace rack_fabric::internal {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  constexpr std::uint32_t polynomial = 0x82F63B78U;  // reflected Castagnoli
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t crc = i;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ polynomial : crc >> 1U;
    }
    table[i] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

constexpr std::array<std::uint32_t, 64> make_sha256_constants() noexcept {
  std::array<std::uint32_t, 64> k{};
  // Fractional parts of the cube roots of the first 64 primes.
  const std::uint32_t values[64] = {
      0x428A2F98U, 0x71374491U, 0xB5C0FBCFU, 0xE9B5DBA5U, 0x3956C25BU, 0x59F111F1U, 0x923F82A4U,
      0xAB1C5ED5U, 0xD807AA98U, 0x12835B01U, 0x243185BEU, 0x550C7DC3U, 0x72BE5D74U, 0x80DEB1FEU,
      0x9BDC06A7U, 0xC19BF174U, 0xE49B69C1U, 0xEFBE4786U, 0x0FC19DC6U, 0x240CA1CCU, 0x2DE92C6FU,
      0x4A7484AAU, 0x5CB0A9DCU, 0x76F988DAU, 0x983E5152U, 0xA831C66DU, 0xB00327C8U, 0xBF597FC7U,
      0xC6E00BF3U, 0xD5A79147U, 0x06CA6351U, 0x14292967U, 0x27B70A85U, 0x2E1B2138U, 0x4D2C6DFCU,
      0x53380D13U, 0x650A7354U, 0x766A0ABBU, 0x81C2C92EU, 0x92722C85U, 0xA2BFE8A1U, 0xA81A664BU,
      0xC24B8B70U, 0xC76C51A3U, 0xD192E819U, 0xD6990624U, 0xF40E3585U, 0x106AA070U, 0x19A4C116U,
      0x1E376C08U, 0x2748774CU, 0x34B0BCB5U, 0x391C0CB3U, 0x4ED8AA4AU, 0x5B9CCA4FU, 0x682E6FF3U,
      0x748F82EEU, 0x78A5636FU, 0x84C87814U, 0x8CC70208U, 0x90BEFFFAU, 0xA4506CEBU, 0xBEF9A3F7U,
      0xC67178F2U};
  for (std::size_t i = 0; i < 64; ++i) {
    k[i] = values[i];
  }
  return k;
}

constexpr std::array<std::uint32_t, 64> kSha256Constants = make_sha256_constants();

[[nodiscard]] constexpr std::uint32_t rotr(std::uint32_t value, unsigned count) noexcept {
  return (value >> count) | (value << (32U - count));
}

[[nodiscard]] constexpr std::uint32_t big_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}
[[nodiscard]] constexpr std::uint32_t big_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}
[[nodiscard]] constexpr std::uint32_t small_sigma0(std::uint32_t x) noexcept {
  return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3U);
}
[[nodiscard]] constexpr std::uint32_t small_sigma1(std::uint32_t x) noexcept {
  return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10U);
}

}  // namespace

std::uint32_t crc32c(const std::byte* data, std::size_t size, std::uint32_t seed) noexcept {
  std::uint32_t crc = ~seed;
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t byte = static_cast<std::uint8_t>(data[i]);
    crc = kCrc32cTable[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
  }
  return ~crc;
}

std::uint32_t crc32c(std::string_view data, std::uint32_t seed) noexcept {
  return crc32c(reinterpret_cast<const std::byte*>(data.data()), data.size(), seed);
}

Sha256::Sha256() noexcept { reset(); }

void Sha256::reset() noexcept {
  state_[0] = 0x6A09E667U;
  state_[1] = 0xBB67AE85U;
  state_[2] = 0x3C6EF372U;
  state_[3] = 0xA54FF53AU;
  state_[4] = 0x510E527FU;
  state_[5] = 0x9B05688CU;
  state_[6] = 0x1F83D9ABU;
  state_[7] = 0x5BE0CD19U;
  bit_count_ = 0;
  buffer_length_ = 0;
  std::memset(buffer_, 0, sizeof(buffer_));
}

void Sha256::transform(const std::byte* block) noexcept {
  std::uint32_t w[64];
  for (std::size_t i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(static_cast<std::uint8_t>(block[i * 4])) << 24U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(block[i * 4 + 1])) << 16U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(block[i * 4 + 2])) << 8U) |
           (static_cast<std::uint32_t>(static_cast<std::uint8_t>(block[i * 4 + 3])));
  }
  for (std::size_t i = 16; i < 64; ++i) {
    w[i] = small_sigma1(w[i - 2]) + w[i - 7] + small_sigma0(w[i - 15]) + w[i - 16];
  }

  std::uint32_t a = state_[0];
  std::uint32_t b = state_[1];
  std::uint32_t c = state_[2];
  std::uint32_t d = state_[3];
  std::uint32_t e = state_[4];
  std::uint32_t f = state_[5];
  std::uint32_t g = state_[6];
  std::uint32_t h = state_[7];

  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + big_sigma1(e) + ((e & f) ^ (~e & g)) + kSha256Constants[i] + w[i];
    const std::uint32_t t2 = big_sigma0(a) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }

  state_[0] += a;
  state_[1] += b;
  state_[2] += c;
  state_[3] += d;
  state_[4] += e;
  state_[5] += f;
  state_[6] += g;
  state_[7] += h;
}

void Sha256::update(const std::byte* data, std::size_t size) noexcept {
  bit_count_ += static_cast<std::uint64_t>(size) * 8U;
  while (size > 0) {
    const std::size_t take = size < (64 - buffer_length_) ? size : (64 - buffer_length_);
    std::memcpy(buffer_ + buffer_length_, data, take);
    buffer_length_ += take;
    data += take;
    size -= take;
    if (buffer_length_ == 64) {
      transform(buffer_);
      buffer_length_ = 0;
    }
  }
}

void Sha256::update(std::string_view data) noexcept {
  update(reinterpret_cast<const std::byte*>(data.data()), data.size());
}

std::string Sha256::finish() {
  const std::uint64_t bit_count = bit_count_;
  const std::byte padding = std::byte{0x80};
  update(&padding, 1);
  const std::byte zero = std::byte{0x00};
  while (buffer_length_ != 56) {
    update(&zero, 1);
  }
  std::byte length_bytes[8];
  for (std::size_t i = 0; i < 8; ++i) {
    length_bytes[i] = static_cast<std::byte>((bit_count >> ((7U - i) * 8U)) & 0xFFU);
  }
  update(length_bytes, 8);

  std::byte digest[32];
  for (std::size_t i = 0; i < 8; ++i) {
    digest[i * 4] = static_cast<std::byte>((state_[i] >> 24U) & 0xFFU);
    digest[i * 4 + 1] = static_cast<std::byte>((state_[i] >> 16U) & 0xFFU);
    digest[i * 4 + 2] = static_cast<std::byte>((state_[i] >> 8U) & 0xFFU);
    digest[i * 4 + 3] = static_cast<std::byte>(state_[i] & 0xFFU);
  }
  return to_hex(digest, sizeof(digest));
}

std::string Sha256::hash(const std::byte* data, std::size_t size) {
  Sha256 hasher;
  hasher.update(data, size);
  return hasher.finish();
}

std::string to_hex(const std::byte* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    const std::uint8_t byte = static_cast<std::uint8_t>(data[i]);
    out.push_back(kDigits[byte >> 4U]);
    out.push_back(kDigits[byte & 0x0FU]);
  }
  return out;
}

}  // namespace rack_fabric::internal
