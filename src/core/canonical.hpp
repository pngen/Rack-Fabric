// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Canonical binary encoding of rack records.
//
// The same encoding backs both the snapshot digest and the persistence
// format, so a digest and a stored file can never disagree about the meaning
// of a record. Encoding is deterministic: records are written in canonical
// order and no unordered container is ever iterated.
//
// Decoding is bounded and checked. Every length is validated against the
// configured limit before memory is reserved, every count is checked against
// the remaining bytes, and an invalid enumeration value or identity is a
// decode error rather than a silently coerced value.

#ifndef RACK_FABRIC_INTERNAL_CANONICAL_HPP
#define RACK_FABRIC_INTERNAL_CANONICAL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/authority.hpp"
#include "rack_fabric/envelope.hpp"
#include "rack_fabric/failure_domain.hpp"
#include "rack_fabric/limits.hpp"
#include "rack_fabric/member.hpp"
#include "rack_fabric/topology.hpp"

namespace rack_fabric::internal {

enum class CodecError : std::uint8_t {
  None = 0,
  Truncated = 1,
  LengthTooLarge = 2,
  CountTooLarge = 3,
  InvalidEnum = 4,
  InvalidIdentity = 5,
  InvalidNumber = 6,
  TrailingData = 7,
  InvalidValue = 8,
};

[[nodiscard]] std::string_view to_string(CodecError value) noexcept;

class ByteWriter {
 public:
  explicit ByteWriter(const ResourceLimits& limits = ResourceLimits{}) : limits_(limits) {}
  /// Encodes with an explicit total-size bound. Persisted state is bounded by
  /// the persistence limit, not by the protocol frame limit, so the two must
  /// not share one number.
  ByteWriter(const ResourceLimits& limits, std::size_t max_bytes)
      : limits_(limits), max_bytes_(max_bytes) {}

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void f64(double value);
  void boolean(bool value);
  void string(std::string_view value);
  void optional_string(const std::optional<std::string>& value);
  void optional_double(const std::optional<double>& value);
  void optional_u32(const std::optional<std::uint32_t>& value);
  void optional_u64(const std::optional<std::uint64_t>& value);
  void optional_i64(const std::optional<std::int64_t>& value);
  /// Encodes a collection count bounded by max_collection_items.
  void count(std::size_t value) { count(value, limits_.max_collection_items); }
  /// Encodes a collection count bounded by an explicit limit. Durable state is
  /// bounded by the limit of the collection it encodes, not by the per-message
  /// collection limit.
  void count(std::size_t value, std::size_t maximum);
  void timestamp(Timestamp value);
  void ttl(std::chrono::milliseconds value);
  void raw(const std::byte* data, std::size_t size);

  [[nodiscard]] const ResourceLimits& limits() const noexcept { return limits_; }
  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(buffer_); }
  /// False when a bound was exceeded while encoding. A producer must never
  /// emit bytes that a consumer is required to reject, so every caller that
  /// turns a writer into a payload checks this.
  [[nodiscard]] bool ok() const noexcept { return ok_; }
  /// The reason the writer stopped accepting bytes. None while ok() is true.
  [[nodiscard]] CodecError error() const noexcept { return error_; }

 private:
  void fail(CodecError error = CodecError::LengthTooLarge) noexcept {
    if (ok_) {
      ok_ = false;
      error_ = error;
    }
  }
  ResourceLimits limits_;
  std::size_t max_bytes_ = limits_.max_payload_bytes;
  std::vector<std::byte> buffer_;
  bool ok_ = true;
  CodecError error_ = CodecError::None;
};

class ByteReader {
 public:
  ByteReader(const std::byte* data, std::size_t size, const ResourceLimits& limits = ResourceLimits{})
      : data_(data), size_(size), limits_(limits) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] CodecError error() const noexcept { return error_; }
  [[nodiscard]] const ResourceLimits& limits() const noexcept { return limits_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == size_; }

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int64_t i64();
  double f64();
  bool boolean();
  std::string string();
  std::optional<std::string> optional_string();
  std::optional<double> optional_double();
  std::optional<std::uint32_t> optional_u32();
  std::optional<std::uint64_t> optional_u64();
  std::optional<std::int64_t> optional_i64();
  /// Reads a collection count bounded by max_collection_items.
  std::size_t count() { return count(limits_.max_collection_items); }
  /// Reads a collection count bounded by an explicit limit.
  std::size_t count(std::size_t maximum);
  Timestamp timestamp();
  std::chrono::milliseconds ttl();
  bool raw(std::byte* out, std::size_t size);

  template <class Enum>
  [[nodiscard]] Enum enum8(Enum maximum) {
    const std::uint8_t value = u8();
    if (!ok_ || value > static_cast<std::uint8_t>(maximum)) {
      fail(CodecError::InvalidEnum);
      return static_cast<Enum>(0);
    }
    return static_cast<Enum>(value);
  }

  void fail(CodecError error) noexcept {
    if (ok_) {
      ok_ = false;
      error_ = error;
    }
  }

 private:
  [[nodiscard]] bool need(std::size_t count_bytes) noexcept;

  const std::byte* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
  bool ok_ = true;
  CodecError error_ = CodecError::None;
  ResourceLimits limits_;
};

// -- record codecs ----------------------------------------------------------

void encode_member(ByteWriter& writer, const MemberRecord& record);
void decode_member(ByteReader& reader, MemberRecord& out);

void encode_relationship(ByteWriter& writer, const RelationshipRecord& record);
void decode_relationship(ByteReader& reader, RelationshipRecord& out);

void encode_failure_domain(ByteWriter& writer, const FailureDomainRecord& record);
void decode_failure_domain(ByteReader& reader, FailureDomainRecord& out);

void encode_power_envelope(ByteWriter& writer, const PowerEnvelopeRecord& record);
void decode_power_envelope(ByteReader& reader, PowerEnvelopeRecord& out);

void encode_cooling_envelope(ByteWriter& writer, const CoolingEnvelopeRecord& record);
void decode_cooling_envelope(ByteReader& reader, CoolingEnvelopeRecord& out);

void encode_generation_set(ByteWriter& writer, const GenerationSet& value);
void decode_generation_set(ByteReader& reader, GenerationSet& out);

void encode_member_details(ByteWriter& writer, const MemberDetails& details);
void decode_member_details(ByteReader& reader, MemberDetails& out);

void encode_member_key(ByteWriter& writer, const MemberKey& key);
void decode_member_key(ByteReader& reader, MemberKey& out);

void encode_authority(ByteWriter& writer, const AuthorityToken& token);
void decode_authority(ByteReader& reader, AuthorityToken& out);

}  // namespace rack_fabric::internal

#endif  // RACK_FABRIC_INTERNAL_CANONICAL_HPP
