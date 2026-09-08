// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The framed rack protocol.
//
// The protocol is bounded, versioned and integrity checked. Every length is
// validated against a configured bound before memory is reserved for it, all
// length arithmetic is checked, and a malformed frame is rejected without
// touching authoritative state.
//
// Frame layout (little endian):
//
//   offset  size  field
//   0       4     magic 'R' 'K' 'F' '1'
//   4       2     protocol version
//   6       2     message type
//   8       2     flags (bit 0: response)
//   10      2     reserved (must be zero)
//   12      8     correlation id
//   20      4     payload length
//   24      N     payload
//   24+N    4     CRC-32C of bytes 0..24+N-1
//
// A frame whose declared payload length exceeds the configured maximum is
// rejected before any buffer is allocated.

#ifndef RACK_FABRIC_PROTOCOL_HPP
#define RACK_FABRIC_PROTOCOL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rack_fabric/limits.hpp"
#include "rack_fabric/readiness.hpp"
#include "rack_fabric/member.hpp"
#include "rack_fabric/mutation.hpp"
#include "rack_fabric/snapshot.hpp"
#include "rack_fabric/version.hpp"

namespace rack_fabric {

inline constexpr std::uint8_t kFrameMagic[4] = {'R', 'K', 'F', '1'};
inline constexpr std::size_t kFrameHeaderBytes = 24;
inline constexpr std::size_t kFrameTrailerBytes = 4;
inline constexpr std::size_t kFrameOverheadBytes = kFrameHeaderBytes + kFrameTrailerBytes;

enum class MessageType : std::uint16_t {
  Invalid = 0,
  Hello = 1,
  HelloAck = 2,
  Register = 3,
  RegisterAck = 4,
  DeclareRack = 5,
  PublishNode = 6,
  PublishDevice = 7,
  PublishLink = 8,
  PublishFailureDomain = 9,
  PublishPower = 10,
  PublishCooling = 11,
  PublishHealth = 12,
  PublishCapability = 13,
  Withdraw = 14,
  RetireMember = 15,
  Revalidate = 16,
  Heartbeat = 17,
  Query = 18,
  SnapshotRequest = 19,
  SnapshotResponse = 20,
  Result = 21,
  Error = 22,
};

[[nodiscard]] std::string_view to_string(MessageType value) noexcept;
[[nodiscard]] std::optional<MessageType> message_type_from_value(std::uint16_t value) noexcept;

/// Stable error codes reported by the Error message. They mirror the
/// MutationOutcome space and the framing failures.
enum class ProtocolError : std::uint16_t {
  None = 0,
  BadMagic = 1,
  UnsupportedVersion = 2,
  UnknownMessageType = 3,
  OversizedFrame = 4,
  TruncatedFrame = 5,
  ChecksumMismatch = 6,
  MalformedPayload = 7,
  TrailingGarbage = 8,
  InvalidIdentity = 9,
  CollectionTooLarge = 10,
  StringTooLong = 11,
  NotRegistered = 12,
  StaleCoordinatorEpoch = 13,
  StaleWorkerBoot = 14,
  StaleGeneration = 15,
  NotAuthorized = 16,
  Conflict = 17,
  LimitExceeded = 18,
  Unsupported = 19,
  Internal = 20,
};

[[nodiscard]] std::string_view to_string(ProtocolError value) noexcept;

struct Frame {
  std::uint16_t version = kProtocolVersion;
  MessageType type = MessageType::Invalid;
  std::uint16_t flags = 0;
  std::uint64_t correlation = 0;
  std::vector<std::byte> payload;

  [[nodiscard]] bool is_response() const noexcept { return (flags & 0x0001U) != 0; }
  [[nodiscard]] static constexpr std::uint16_t response_flag() noexcept { return 0x0001U; }

  friend bool operator==(const Frame&, const Frame&) = default;
};

enum class DecodeStatus : std::uint8_t {
  Ok = 0,
  BadMagic = 1,
  UnsupportedVersion = 2,
  UnknownMessageType = 3,
  OversizedFrame = 4,
  TruncatedFrame = 5,
  ChecksumMismatch = 6,
  ReservedBitsSet = 7,
  TrailingGarbage = 8,
  /// The frame was well formed but its payload could not be decoded.
  MalformedPayload = 9,
};

[[nodiscard]] std::string_view to_string(DecodeStatus value) noexcept;

struct DecodeResult {
  DecodeStatus status = DecodeStatus::Ok;
  Frame frame;
  /// Number of bytes consumed from the input.
  std::size_t consumed = 0;
  /// Human-readable detail; never used for control flow.
  std::string detail;

  [[nodiscard]] bool ok() const noexcept { return status == DecodeStatus::Ok; }
};

/// Encodes a frame. Returns std::nullopt when the payload exceeds the bound.
[[nodiscard]] std::optional<std::vector<std::byte>> encode_frame(const Frame& frame,
                                                                const ResourceLimits& limits);

/// Decodes exactly one frame from the front of the buffer.
[[nodiscard]] DecodeResult decode_frame(const std::byte* data, std::size_t size,
                                        const ResourceLimits& limits);

// ---------------------------------------------------------------------------
// Payload codec
// ---------------------------------------------------------------------------

class PayloadWriter {
 public:
  explicit PayloadWriter(const ResourceLimits& limits = ResourceLimits{}) : limits_(limits) {}

  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void f64(double value);
  void boolean(bool value);
  void string(std::string_view value);
  void optional_string(const std::optional<std::string>& value);
  void bytes(const std::vector<std::byte>& value);
  void collection_size(std::size_t count);

  [[nodiscard]] const std::vector<std::byte>& data() const noexcept { return buffer_; }
  [[nodiscard]] std::vector<std::byte> take() && { return std::move(buffer_); }
  /// False when a bound was exceeded while encoding. Callers must not send a
  /// payload produced by a failed writer.
  [[nodiscard]] bool ok() const noexcept { return ok_; }

 private:
  void fail() noexcept { ok_ = false; }
  ResourceLimits limits_;
  std::vector<std::byte> buffer_;
  bool ok_ = true;
};

class PayloadReader {
 public:
  PayloadReader(const std::byte* data, std::size_t size, const ResourceLimits& limits)
      : data_(data), size_(size), limits_(limits) {}

  [[nodiscard]] bool ok() const noexcept { return ok_; }
  [[nodiscard]] ProtocolError error() const noexcept { return error_; }
  [[nodiscard]] std::size_t remaining() const noexcept { return size_ - offset_; }
  [[nodiscard]] bool at_end() const noexcept { return offset_ == size_; }

  std::uint8_t u8();
  std::uint16_t u16();
  std::uint32_t u32();
  std::uint64_t u64();
  std::int64_t i64();
  double f64();
  bool boolean();
  std::optional<std::string> string();
  std::optional<std::string> optional_string();
  std::vector<std::byte> bytes();
  std::size_t collection_size();

  /// Reads an enumeration encoded as one byte. A value above the highest
  /// enumerator is a decode error, never a silently coerced value.
  template <class Enum>
  [[nodiscard]] Enum enum8(Enum maximum) {
    const std::uint8_t value = u8();
    if (!ok_ || value > static_cast<std::uint8_t>(maximum)) {
      fail(ProtocolError::MalformedPayload);
      return static_cast<Enum>(0);
    }
    return static_cast<Enum>(value);
  }

  /// True when every byte was consumed. A payload with trailing bytes is
  /// malformed.
  [[nodiscard]] bool consumed_exactly() const noexcept { return ok_ && offset_ == size_; }

  /// Marks the payload as malformed. Public so that codec helpers can reject
  /// a payload without exposing the reader's internals.
  void fail(ProtocolError error) noexcept {
    if (ok_) {
      ok_ = false;
      error_ = error;
    }
  }

 private:
  [[nodiscard]] bool need(std::size_t count) noexcept;

  const std::byte* data_;
  std::size_t size_;
  std::size_t offset_ = 0;
  bool ok_ = true;
  ProtocolError error_ = ProtocolError::None;
  ResourceLimits limits_;
};

// ---------------------------------------------------------------------------
// Message payloads
// ---------------------------------------------------------------------------

struct HelloMessage {
  std::uint16_t protocol_version = kProtocolVersion;
  CoordinatorEpoch coordinator_epoch;
  std::string agent_version;
};

struct HelloAckMessage {
  std::uint16_t protocol_version = kProtocolVersion;
  CoordinatorEpoch coordinator_epoch;
  std::string coordinator_version;
  std::optional<RackId> rack;
  bool accepts_mutations = true;
};

struct RegisterMessage {
  std::optional<RackId> rack;
  std::optional<WorkerId> worker;
  std::optional<AgentBootId> boot;
  CoordinatorEpoch coordinator_epoch;
  std::optional<std::string> label;
};

struct RegisterAckMessage {
  bool accepted = false;
  CoordinatorEpoch coordinator_epoch;
  GenerationSet generations;
  PublicationGeneration publication_generation;
};

struct DeclareRackMessage {
  AuthorityToken authority;
  RackEpochId epoch;
  std::optional<std::string> label;
  bool redeclare = false;
};

struct PublishMemberMessage {
  AuthorityToken authority;
  MemberRecord record;
  std::optional<MemberGeneration> expected_generation;
  bool supersede = false;
};

struct PublishLinkMessage {
  AuthorityToken authority;
  RelationshipRecord record;
  std::optional<TopologyGeneration> expected_generation;
  bool supersede = false;
};

struct PublishFailureDomainMessage {
  AuthorityToken authority;
  FailureDomainRecord record;
  std::optional<FailureDomainGeneration> expected_generation;
  bool supersede = false;
};

struct PublishPowerMessage {
  AuthorityToken authority;
  PowerEnvelopeRecord record;
  std::optional<PowerEnvelopeGeneration> expected_generation;
  bool supersede = false;
};

struct PublishCoolingMessage {
  AuthorityToken authority;
  CoolingEnvelopeRecord record;
  std::optional<CoolingEnvelopeGeneration> expected_generation;
  bool supersede = false;
};

struct PublishHealthMessage {
  AuthorityToken authority;
  MemberKey member;
  EvidenceValue<HealthState> health;
  EvidenceValue<ReadinessState> readiness;
  bool publish_readiness = false;
  std::optional<MemberGeneration> expected_generation;
};

struct PublishCapabilityMessage {
  AuthorityToken authority;
  MemberKey member;
  CapabilityRef capability;
  std::optional<MemberGeneration> expected_generation;
};

struct WithdrawMessage {
  AuthorityToken authority;
  MemberKey member;
  WithdrawScope scope = WithdrawScope::All;
  std::optional<MemberGeneration> expected_generation;
};

struct RetireMemberMessage {
  AuthorityToken authority;
  MemberKey member;
  std::optional<MemberGeneration> expected_generation;
};

struct RevalidateMessage {
  AuthorityToken authority;
  std::vector<MemberKey> members;
  bool all = false;
};

struct HeartbeatMessage {
  AuthorityToken authority;
  PublicationGeneration publication_generation;
};

struct QueryMessage {
  std::uint16_t query_kind = 0;
  std::optional<MemberKey> member;
  std::optional<FailureDomainId> failure_domain;
};

enum class QueryKind : std::uint16_t {
  Summary = 0,
  Members = 1,
  Member = 2,
  Relationships = 3,
  FailureDomains = 4,
  PowerEnvelope = 5,
  CoolingEnvelope = 6,
  Publishers = 7,
  Readiness = 8,
  Generations = 9,
};

struct SnapshotRequestMessage {
  AuthorityToken authority;
  bool create = true;
};

struct ResultMessage {
  bool accepted = false;
  MutationOutcome outcome = MutationOutcome::RejectInvalidInput;
  GenerationSet generations;
  std::string code;
  std::string detail;
};

struct ErrorMessage {
  ProtocolError error = ProtocolError::None;
  std::string detail;
};

/// Deterministic payload codec for every message type. Encoding never
/// produces a payload larger than the configured bound; decoding validates
/// every length and never allocates from an unvalidated count.
struct MessageCodec {
  static std::optional<std::vector<std::byte>> encode(const HelloMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, HelloMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const HelloAckMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, HelloAckMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const RegisterMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, RegisterMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const RegisterAckMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, RegisterAckMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const DeclareRackMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, DeclareRackMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const PublishMemberMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, PublishMemberMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const PublishLinkMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, PublishLinkMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const PublishFailureDomainMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size,
                             PublishFailureDomainMessage& out, const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const PublishPowerMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, PublishPowerMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const PublishCoolingMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, PublishCoolingMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const PublishHealthMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, PublishHealthMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const PublishCapabilityMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, PublishCapabilityMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const WithdrawMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, WithdrawMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const RetireMemberMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, RetireMemberMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const RevalidateMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, RevalidateMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const HeartbeatMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, HeartbeatMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const QueryMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, QueryMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const SnapshotRequestMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, SnapshotRequestMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const ResultMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, ResultMessage& out,
                             const ResourceLimits& limits = {});

  static std::optional<std::vector<std::byte>> encode(const ErrorMessage& message,
                                                     const ResourceLimits& limits = {});
  static DecodeStatus decode(const std::byte* data, std::size_t size, ErrorMessage& out,
                             const ResourceLimits& limits = {});

  /// Encodes a snapshot as a self-describing deterministic byte string for
  /// the SnapshotResponse message.
  static std::optional<std::vector<std::byte>> encode_snapshot_payload(const RackSnapshot& snapshot,
                                                                      const ResourceLimits& limits);
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_PROTOCOL_HPP
