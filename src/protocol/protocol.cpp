// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Framed protocol codec.

#include "rack_fabric/protocol.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "core/canonical.hpp"
#include "internal/crypto.hpp"

namespace rack_fabric {
namespace {

void store_u16(std::byte* out, std::uint16_t value) { std::memcpy(out, &value, sizeof(value)); }
void store_u32(std::byte* out, std::uint32_t value) { std::memcpy(out, &value, sizeof(value)); }
void store_u64(std::byte* out, std::uint64_t value) { std::memcpy(out, &value, sizeof(value)); }

[[nodiscard]] std::uint16_t load_u16(const std::byte* in) {
  std::uint16_t value = 0;
  std::memcpy(&value, in, sizeof(value));
  return value;
}

[[nodiscard]] std::uint32_t load_u32(const std::byte* in) {
  std::uint32_t value = 0;
  std::memcpy(&value, in, sizeof(value));
  return value;
}

[[nodiscard]] std::uint64_t load_u64(const std::byte* in) {
  std::uint64_t value = 0;
  std::memcpy(&value, in, sizeof(value));
  return value;
}

void write_optional_identity(PayloadWriter& writer, const std::optional<std::string>& value) {
  writer.optional_string(value);
}

[[nodiscard]] std::optional<std::string> read_identity(PayloadReader& reader) {
  const auto text = reader.string();
  if (!text.has_value()) {
    return std::nullopt;
  }
  if (!validate_identity(*text).ok()) {
    reader.fail(ProtocolError::InvalidIdentity);
    return std::nullopt;
  }
  return text;
}

template <class Id>
[[nodiscard]] std::optional<Id> read_typed_id(PayloadReader& reader) {
  const auto text = reader.string();
  if (!text.has_value()) {
    return std::nullopt;
  }
  const auto parsed = Id::parse(*text);
  if (!parsed.has_value()) {
    reader.fail(ProtocolError::InvalidIdentity);
    return std::nullopt;
  }
  return parsed;
}

void write_member_key(PayloadWriter& writer, const MemberKey& key) {
  writer.u8(static_cast<std::uint8_t>(key.kind));
  writer.string(key.id);
}

[[nodiscard]] bool read_member_key(PayloadReader& reader, MemberKey& out) {
  out.kind = reader.enum8(MemberKind::CoolingDomain);
  const auto id = reader.string();
  if (!id.has_value()) {
    return false;
  }
  if (!validate_identity(*id).ok()) {
    reader.fail(ProtocolError::InvalidIdentity);
    return false;
  }
  out.id = *id;
  return true;
}

void write_generation_set(PayloadWriter& writer, const GenerationSet& generations) {
  writer.u64(generations.rack.value());
  writer.u64(generations.membership.value());
  writer.u64(generations.topology.value());
  writer.u64(generations.failure_domains.value());
  writer.u64(generations.constraints.value());
  writer.u64(generations.power.value());
  writer.u64(generations.cooling.value());
  writer.u64(generations.health.value());
  writer.u64(generations.capabilities.value());
  writer.u64(generations.coordinator_epoch.value());
}

void read_generation_set(PayloadReader& reader, GenerationSet& out) {
  out.rack = RackGeneration::from_value(reader.u64());
  out.membership = MembershipGeneration::from_value(reader.u64());
  out.topology = TopologyGeneration::from_value(reader.u64());
  out.failure_domains = FailureDomainGeneration::from_value(reader.u64());
  out.constraints = ConstraintGeneration::from_value(reader.u64());
  out.power = PowerEnvelopeGeneration::from_value(reader.u64());
  out.cooling = CoolingEnvelopeGeneration::from_value(reader.u64());
  out.health = HealthGeneration::from_value(reader.u64());
  out.capabilities = CapabilityGeneration::from_value(reader.u64());
  out.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
}

void write_authority(PayloadWriter& writer, const AuthorityToken& token) {
  writer.u64(token.coordinator_epoch.value());
  writer.optional_string(token.boot.has_value() ? std::optional<std::string>(token.boot->value())
                                                : std::nullopt);
  if (token.publication_generation.has_value()) {
    writer.u8(1);
    writer.u64(token.publication_generation->value());
  } else {
    writer.u8(0);
  }
  writer.optional_string(token.rack.has_value() ? std::optional<std::string>(token.rack->value())
                                                : std::nullopt);
}

[[nodiscard]] bool read_authority(PayloadReader& reader, AuthorityToken& out) {
  out.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
  const auto boot = reader.optional_string();
  if (boot.has_value()) {
    const auto parsed = AgentBootId::parse(*boot);
    if (!parsed.has_value()) {
      reader.fail(ProtocolError::InvalidIdentity);
      return false;
    }
    out.boot = *parsed;
  }
  if (reader.u8() == 1U) {
    out.publication_generation = PublicationGeneration::from_value(reader.u64());
  }
  const auto rack = reader.optional_string();
  if (rack.has_value()) {
    const auto parsed = RackId::parse(*rack);
    if (!parsed.has_value()) {
      reader.fail(ProtocolError::InvalidIdentity);
      return false;
    }
    out.rack = *parsed;
  }
  return reader.ok();
}

void write_evidence_header(PayloadWriter& writer, EvidenceProvenance provenance,
                           Timestamp observed_at, std::chrono::milliseconds ttl,
                           Durability durability) {
  writer.u8(static_cast<std::uint8_t>(provenance));
  writer.i64(observed_at.millis());
  writer.i64(static_cast<std::int64_t>(ttl.count()));
  writer.u8(static_cast<std::uint8_t>(durability));
}

struct EvidenceHeader {
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
};

[[nodiscard]] EvidenceHeader read_evidence_header(PayloadReader& reader) {
  EvidenceHeader header;
  header.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
  header.observed_at = Timestamp::from_unix_millis(reader.i64());
  header.ttl = std::chrono::milliseconds{reader.i64()};
  header.durability = reader.enum8(Durability::Ephemeral);
  return header;
}

void write_quantity(PayloadWriter& writer, const Quantity& quantity) {
  writer.f64(quantity.value);
  writer.u8(static_cast<std::uint8_t>(quantity.unit));
  if (quantity.uncertainty.has_value()) {
    writer.u8(1);
    writer.f64(*quantity.uncertainty);
  } else {
    writer.u8(0);
  }
  write_evidence_header(writer, quantity.provenance, quantity.observed_at, quantity.ttl,
                        quantity.durability);
}

void read_quantity(PayloadReader& reader, Quantity& out) {
  out.value = reader.f64();
  out.unit = reader.enum8(QuantityUnit::Amperes);
  if (reader.u8() == 1U) {
    out.uncertainty = reader.f64();
  }
  const EvidenceHeader header = read_evidence_header(reader);
  out.provenance = header.provenance;
  out.observed_at = header.observed_at;
  out.ttl = header.ttl;
  out.durability = header.durability;
}

}  // namespace

// ---------------------------------------------------------------------------
// Payload codec
// ---------------------------------------------------------------------------

void PayloadWriter::u8(std::uint8_t value) {
  if (!ok_) return;
  buffer_.push_back(static_cast<std::byte>(value));
}

void PayloadWriter::u16(std::uint16_t value) {
  if (!ok_) return;
  std::byte bytes[2];
  store_u16(bytes, value);
  buffer_.insert(buffer_.end(), bytes, bytes + 2);
}

void PayloadWriter::u32(std::uint32_t value) {
  if (!ok_) return;
  std::byte bytes[4];
  store_u32(bytes, value);
  buffer_.insert(buffer_.end(), bytes, bytes + 4);
}

void PayloadWriter::u64(std::uint64_t value) {
  if (!ok_) return;
  std::byte bytes[8];
  store_u64(bytes, value);
  buffer_.insert(buffer_.end(), bytes, bytes + 8);
}

void PayloadWriter::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void PayloadWriter::f64(double value) {
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  u64(bits);
}

void PayloadWriter::boolean(bool value) { u8(value ? 1U : 0U); }

void PayloadWriter::string(std::string_view value) {
  if (!ok_) return;
  if (value.size() > limits_.max_string_bytes) {
    fail();
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (!ok_) return;
  buffer_.insert(buffer_.end(), reinterpret_cast<const std::byte*>(value.data()),
                 reinterpret_cast<const std::byte*>(value.data()) + value.size());
}

void PayloadWriter::optional_string(const std::optional<std::string>& value) {
  if (!ok_) return;
  if (!value.has_value()) {
    u8(0);
    return;
  }
  u8(1);
  string(*value);
}

void PayloadWriter::bytes(const std::vector<std::byte>& value) {
  if (!ok_) return;
  if (value.size() > limits_.max_payload_bytes) {
    fail();
    return;
  }
  u32(static_cast<std::uint32_t>(value.size()));
  if (!ok_) return;
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void PayloadWriter::collection_size(std::size_t count) {
  if (!ok_) return;
  if (count > limits_.max_collection_items) {
    fail();
    return;
  }
  u32(static_cast<std::uint32_t>(count));
}

bool PayloadReader::need(std::size_t count) noexcept {
  if (!ok_) {
    return false;
  }
  if (count > size_ - offset_) {
    fail(ProtocolError::MalformedPayload);
    return false;
  }
  return true;
}

std::uint8_t PayloadReader::u8() {
  if (!need(1)) {
    return 0;
  }
  return static_cast<std::uint8_t>(data_[offset_++]);
}

std::uint16_t PayloadReader::u16() {
  if (!need(2)) {
    return 0;
  }
  const std::uint16_t value = load_u16(data_ + offset_);
  offset_ += 2;
  return value;
}

std::uint32_t PayloadReader::u32() {
  if (!need(4)) {
    return 0;
  }
  const std::uint32_t value = load_u32(data_ + offset_);
  offset_ += 4;
  return value;
}

std::uint64_t PayloadReader::u64() {
  if (!need(8)) {
    return 0;
  }
  const std::uint64_t value = load_u64(data_ + offset_);
  offset_ += 8;
  return value;
}

std::int64_t PayloadReader::i64() { return static_cast<std::int64_t>(u64()); }

double PayloadReader::f64() {
  const std::uint64_t bits = u64();
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

bool PayloadReader::boolean() {
  const std::uint8_t value = u8();
  if (!ok_) {
    return false;
  }
  if (value > 1U) {
    fail(ProtocolError::MalformedPayload);
    return false;
  }
  return value == 1U;
}

std::optional<std::string> PayloadReader::string() {
  const std::uint32_t length = u32();
  if (!ok_) {
    return std::nullopt;
  }
  if (length > limits_.max_string_bytes) {
    fail(ProtocolError::StringTooLong);
    return std::nullopt;
  }
  if (!need(length)) {
    return std::nullopt;
  }
  std::string value(reinterpret_cast<const char*>(data_ + offset_), length);
  offset_ += length;
  return value;
}

std::optional<std::string> PayloadReader::optional_string() {
  const std::uint8_t flag = u8();
  if (!ok_) {
    return std::nullopt;
  }
  if (flag == 0) {
    return std::nullopt;
  }
  if (flag != 1U) {
    fail(ProtocolError::MalformedPayload);
    return std::nullopt;
  }
  return string();
}

std::vector<std::byte> PayloadReader::bytes() {
  const std::uint32_t length = u32();
  if (!ok_) {
    return {};
  }
  if (length > limits_.max_payload_bytes || !need(length)) {
    if (ok_) {
      fail(ProtocolError::CollectionTooLarge);
    }
    return {};
  }
  std::vector<std::byte> out(data_ + offset_, data_ + offset_ + length);
  offset_ += length;
  return out;
}

std::size_t PayloadReader::collection_size() {
  const std::uint32_t count = u32();
  if (!ok_) {
    return 0;
  }
  if (count > limits_.max_collection_items || count > remaining()) {
    fail(ProtocolError::CollectionTooLarge);
    return 0;
  }
  return count;
}

// ---------------------------------------------------------------------------
// Frame codec
// ---------------------------------------------------------------------------

std::optional<std::vector<std::byte>> encode_frame(const Frame& frame, const ResourceLimits& limits) {
  if (frame.payload.size() > limits.max_payload_bytes) {
    return std::nullopt;
  }
  // Checked arithmetic: both operands are bounded, so the sum cannot wrap.
  const std::size_t total = kFrameOverheadBytes + frame.payload.size();
  if (total > limits.max_frame_bytes) {
    return std::nullopt;
  }
  std::vector<std::byte> out(total);
  std::memcpy(out.data(), kFrameMagic, sizeof(kFrameMagic));
  store_u16(out.data() + 4, frame.version);
  store_u16(out.data() + 6, static_cast<std::uint16_t>(frame.type));
  store_u16(out.data() + 8, frame.flags);
  store_u16(out.data() + 10, 0);
  store_u64(out.data() + 12, frame.correlation);
  store_u32(out.data() + 20, static_cast<std::uint32_t>(frame.payload.size()));
  if (!frame.payload.empty()) {
    std::memcpy(out.data() + kFrameHeaderBytes, frame.payload.data(), frame.payload.size());
  }
  store_u32(out.data() + kFrameHeaderBytes + frame.payload.size(),
            internal::crc32c(out.data(), kFrameHeaderBytes + frame.payload.size()));
  return out;
}

DecodeResult decode_frame(const std::byte* data, std::size_t size, const ResourceLimits& limits) {
  DecodeResult result;
  if (data == nullptr) {
    result.status = DecodeStatus::TruncatedFrame;
    result.detail = "null buffer";
    return result;
  }
  if (size < kFrameOverheadBytes) {
    result.status = DecodeStatus::TruncatedFrame;
    result.detail = "buffer is shorter than the minimum frame length";
    return result;
  }
  if (std::memcmp(data, kFrameMagic, sizeof(kFrameMagic)) != 0) {
    result.status = DecodeStatus::BadMagic;
    result.detail = "frame magic does not match";
    return result;
  }
  const std::uint16_t version = load_u16(data + 4);
  if (version != kProtocolVersion) {
    result.status = DecodeStatus::UnsupportedVersion;
    result.detail = "unsupported protocol version";
    return result;
  }
  const std::uint16_t type_value = load_u16(data + 6);
  const auto type = message_type_from_value(type_value);
  if (!type.has_value()) {
    result.status = DecodeStatus::UnknownMessageType;
    result.detail = "unknown message type";
    return result;
  }
  const std::uint16_t flags = load_u16(data + 8);
  if ((flags & ~Frame::response_flag()) != 0U) {
    result.status = DecodeStatus::ReservedBitsSet;
    result.detail = "reserved flag bits are set";
    return result;
  }
  if (load_u16(data + 10) != 0U) {
    result.status = DecodeStatus::ReservedBitsSet;
    result.detail = "reserved header field is set";
    return result;
  }
  const std::uint32_t payload_length = load_u32(data + 20);
  if (payload_length > limits.max_payload_bytes) {
    result.status = DecodeStatus::OversizedFrame;
    result.detail = "declared payload length exceeds the configured bound";
    return result;
  }
  const std::size_t total = kFrameOverheadBytes + static_cast<std::size_t>(payload_length);
  if (total > limits.max_frame_bytes) {
    result.status = DecodeStatus::OversizedFrame;
    result.detail = "declared frame length exceeds the configured bound";
    return result;
  }
  if (size < total) {
    result.status = DecodeStatus::TruncatedFrame;
    result.detail = "buffer is shorter than the declared frame length";
    return result;
  }
  const std::uint32_t stored_crc = load_u32(data + kFrameHeaderBytes + payload_length);
  if (stored_crc != internal::crc32c(data, kFrameHeaderBytes + payload_length)) {
    result.status = DecodeStatus::ChecksumMismatch;
    result.detail = "frame checksum does not match";
    return result;
  }
  result.frame.version = version;
  result.frame.type = *type;
  result.frame.flags = flags;
  result.frame.correlation = load_u64(data + 12);
  result.frame.payload.assign(data + kFrameHeaderBytes, data + total - kFrameTrailerBytes);
  result.status = DecodeStatus::Ok;
  result.consumed = total;
  return result;
}

// ---------------------------------------------------------------------------
// Message codecs
// ---------------------------------------------------------------------------

#define RACK_FABRIC_ENCODE_BEGIN(name, type)                                            \
  std::optional<std::vector<std::byte>> MessageCodec::encode(const name& message,        \
                                                             const ResourceLimits& limits) { \
    PayloadWriter writer(limits);                                                       \
    (void)writer;

#define RACK_FABRIC_ENCODE_END()        \
  if (!writer.ok()) {                   \
    return std::nullopt;                \
  }                                     \
  return std::move(writer).take();      \
  }

#define RACK_FABRIC_DECODE_BEGIN(name)                                                       \
  DecodeStatus MessageCodec::decode(const std::byte* data, std::size_t size, name& out,       \
                                    const ResourceLimits& limits) {                           \
    PayloadReader reader(data, size, limits);                                                 \
    (void)reader;

#define RACK_FABRIC_DECODE_END()                    \
  if (!reader.ok()) {                               \
    return reader.error() == ProtocolError::StringTooLong ? DecodeStatus::TrailingGarbage       \
                                                         : DecodeStatus::MalformedPayload;      \
  }                                                 \
  if (!reader.consumed_exactly()) {                 \
    return DecodeStatus::TrailingGarbage;           \
  }                                                 \
  return DecodeStatus::Ok;                          \
  }

RACK_FABRIC_ENCODE_BEGIN(HelloMessage, Hello)
  writer.u16(message.protocol_version);
  writer.u64(message.coordinator_epoch.value());
  writer.string(message.agent_version);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(HelloMessage)
  out.protocol_version = reader.u16();
  out.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
  const auto version = reader.string();
  if (version.has_value()) {
    out.agent_version = *version;
  }
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(HelloAckMessage, HelloAck)
  writer.u16(message.protocol_version);
  writer.u64(message.coordinator_epoch.value());
  writer.string(message.coordinator_version);
  writer.optional_string(message.rack.has_value() ? std::optional<std::string>(message.rack->value())
                                                  : std::nullopt);
  writer.boolean(message.accepts_mutations);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(HelloAckMessage)
  out.protocol_version = reader.u16();
  out.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
  const auto version = reader.string();
  if (version.has_value()) {
    out.coordinator_version = *version;
  }
  const auto rack = reader.optional_string();
  if (rack.has_value()) {
    const auto parsed = RackId::parse(*rack);
    if (!parsed.has_value()) {
      reader.fail(ProtocolError::InvalidIdentity);
    } else {
      out.rack = *parsed;
    }
  }
  out.accepts_mutations = reader.boolean();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(RegisterMessage, Register)
  writer.optional_string(message.rack.has_value() ? std::optional<std::string>(message.rack->value())
                                                  : std::nullopt);
  writer.optional_string(message.worker.has_value()
                             ? std::optional<std::string>(message.worker->value())
                             : std::nullopt);
  writer.optional_string(message.boot.has_value() ? std::optional<std::string>(message.boot->value())
                                                  : std::nullopt);
  writer.u64(message.coordinator_epoch.value());
  writer.optional_string(message.label);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(RegisterMessage)
  const auto rack = reader.optional_string();
  if (rack.has_value()) {
    const auto parsed = RackId::parse(*rack);
    if (!parsed.has_value()) {
      reader.fail(ProtocolError::InvalidIdentity);
    } else {
      out.rack = *parsed;
    }
  }
  const auto worker = reader.optional_string();
  if (worker.has_value()) {
    const auto parsed = WorkerId::parse(*worker);
    if (!parsed.has_value()) {
      reader.fail(ProtocolError::InvalidIdentity);
    } else {
      out.worker = *parsed;
    }
  }
  const auto boot = reader.optional_string();
  if (boot.has_value()) {
    const auto parsed = AgentBootId::parse(*boot);
    if (!parsed.has_value()) {
      reader.fail(ProtocolError::InvalidIdentity);
    } else {
      out.boot = *parsed;
    }
  }
  out.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
  out.label = reader.optional_string();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(RegisterAckMessage, RegisterAck)
  writer.boolean(message.accepted);
  writer.u64(message.coordinator_epoch.value());
  write_generation_set(writer, message.generations);
  writer.u64(message.publication_generation.value());
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(RegisterAckMessage)
  out.accepted = reader.boolean();
  out.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
  read_generation_set(reader, out.generations);
  out.publication_generation = PublicationGeneration::from_value(reader.u64());
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(DeclareRackMessage, DeclareRack)
  write_authority(writer, message.authority);
  writer.string(message.epoch.value());
  writer.optional_string(message.label);
  writer.boolean(message.redeclare);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(DeclareRackMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  const auto epoch = read_typed_id<RackEpochId>(reader);
  if (!epoch.has_value()) {
    return DecodeStatus::MalformedPayload;
  }
  out.epoch = *epoch;
  out.label = reader.optional_string();
  out.redeclare = reader.boolean();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(PublishMemberMessage, PublishMember)
  write_authority(writer, message.authority);
  writer.u8(static_cast<std::uint8_t>(message.record.key.kind));
  writer.string(message.record.key.id);
  writer.u8(static_cast<std::uint8_t>(message.record.lifecycle));
  writer.u64(message.record.generation.value());
  write_evidence_header(writer, message.record.provenance, message.record.observed_at,
                        message.record.ttl, message.record.durability);
  if (message.record.parent.has_value()) {
    writer.u8(1);
    write_member_key(writer, *message.record.parent);
  } else {
    writer.u8(0);
  }
  writer.collection_size(message.record.failure_domains.size());
  for (const auto& domain : message.record.failure_domains) {
    writer.string(domain.value());
  }
  writer.optional_string(message.record.power_domain.has_value()
                             ? std::optional<std::string>(message.record.power_domain->value())
                             : std::nullopt);
  writer.optional_string(message.record.cooling_domain.has_value()
                             ? std::optional<std::string>(message.record.cooling_domain->value())
                             : std::nullopt);
  writer.optional_string(message.record.switch_domain.has_value()
                             ? std::optional<std::string>(message.record.switch_domain->value())
                             : std::nullopt);
  // Member details are carried as a canonical sub-payload so that the wire
  // format stays identical to the persisted representation.
  {
    internal::ByteWriter detail_writer(limits);
    internal::encode_member_details(detail_writer, message.record.details);
    writer.bytes(std::move(detail_writer).take());
  }
  writer.collection_size(message.record.capabilities.size());
  for (const auto& capability : message.record.capabilities) {
    writer.string(capability.id.value());
    write_evidence_header(writer, capability.provenance, capability.observed_at, capability.ttl,
                          capability.durability);
    writer.optional_string(capability.value);
  }
  write_evidence_header(writer, message.record.health.provenance, message.record.health.observed_at,
                        message.record.health.ttl, message.record.health.durability);
  writer.u8(static_cast<std::uint8_t>(message.record.health.value));
  write_evidence_header(writer, message.record.readiness.provenance,
                        message.record.readiness.observed_at, message.record.readiness.ttl,
                        message.record.readiness.durability);
  writer.u8(static_cast<std::uint8_t>(message.record.readiness.value));
  write_evidence_header(writer, message.record.reachability.provenance,
                        message.record.reachability.observed_at, message.record.reachability.ttl,
                        message.record.reachability.durability);
  writer.u8(static_cast<std::uint8_t>(message.record.reachability.value));
  writer.optional_string(message.record.source);
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
  writer.boolean(message.supersede);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(PublishMemberMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  out.record.key.kind = reader.enum8(MemberKind::CoolingDomain);
  const auto id = reader.string();
  if (!id.has_value()) {
    return DecodeStatus::MalformedPayload;
  }
  if (!validate_identity(*id).ok()) {
    return DecodeStatus::MalformedPayload;
  }
  out.record.key.id = *id;
  out.record.lifecycle = reader.enum8(MemberLifecycle::Retired);
  out.record.generation = MemberGeneration::from_value(reader.u64());
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.record.provenance = header.provenance;
    out.record.observed_at = header.observed_at;
    out.record.ttl = header.ttl;
    out.record.durability = header.durability;
  }
  if (reader.u8() == 1U) {
    MemberKey parent;
    if (!read_member_key(reader, parent)) {
      return DecodeStatus::MalformedPayload;
    }
    out.record.parent = parent;
  }
  {
    const std::size_t count = reader.collection_size();
    for (std::size_t i = 0; i < count; ++i) {
      const auto domain = read_typed_id<FailureDomainId>(reader);
      if (!domain.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      out.record.failure_domains.push_back(*domain);
    }
  }
  {
    const auto power = reader.optional_string();
    if (power.has_value()) {
      const auto parsed = PowerDomainId::parse(*power);
      if (!parsed.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      out.record.power_domain = *parsed;
    }
    const auto cooling = reader.optional_string();
    if (cooling.has_value()) {
      const auto parsed = CoolingDomainId::parse(*cooling);
      if (!parsed.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      out.record.cooling_domain = *parsed;
    }
    const auto switch_domain = reader.optional_string();
    if (switch_domain.has_value()) {
      const auto parsed = SwitchId::parse(*switch_domain);
      if (!parsed.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      out.record.switch_domain = *parsed;
    }
  }
  {
    const std::vector<std::byte> details = reader.bytes();
    if (!reader.ok()) {
      return DecodeStatus::MalformedPayload;
    }
    internal::ByteReader detail_reader(details.data(), details.size(), limits);
    internal::decode_member_details(detail_reader, out.record.details);
    if (!detail_reader.ok() || !detail_reader.at_end()) {
      return DecodeStatus::MalformedPayload;
    }
  }
  {
    const std::size_t count = reader.collection_size();
    for (std::size_t i = 0; i < count; ++i) {
      const auto capability_id = read_typed_id<CapabilityId>(reader);
      if (!capability_id.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      CapabilityRef capability{*capability_id};
      const EvidenceHeader header = read_evidence_header(reader);
      capability.provenance = header.provenance;
      capability.observed_at = header.observed_at;
      capability.ttl = header.ttl;
      capability.durability = header.durability;
      capability.value = reader.optional_string();
      if (!reader.ok()) {
        return DecodeStatus::MalformedPayload;
      }
      out.record.capabilities.push_back(std::move(capability));
    }
  }
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.record.health.provenance = header.provenance;
    out.record.health.observed_at = header.observed_at;
    out.record.health.ttl = header.ttl;
    out.record.health.durability = header.durability;
    out.record.health.value = reader.enum8(HealthState::NotApplicable);
  }
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.record.readiness.provenance = header.provenance;
    out.record.readiness.observed_at = header.observed_at;
    out.record.readiness.ttl = header.ttl;
    out.record.readiness.durability = header.durability;
    out.record.readiness.value = reader.enum8(ReadinessState::NotReady);
  }
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.record.reachability.provenance = header.provenance;
    out.record.reachability.observed_at = header.observed_at;
    out.record.reachability.ttl = header.ttl;
    out.record.reachability.durability = header.durability;
    out.record.reachability.value = reader.enum8(ReachabilityState::Unreachable);
  }
  out.record.source = reader.optional_string();
  if (reader.u8() == 1U) {
    out.expected_generation = MemberGeneration::from_value(reader.u64());
  }
  out.supersede = reader.boolean();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(PublishLinkMessage, PublishLink)
  write_authority(writer, message.authority);
  writer.u8(static_cast<std::uint8_t>(message.record.key.cls));
  write_member_key(writer, message.record.key.from);
  write_member_key(writer, message.record.key.to);
  write_evidence_header(writer, message.record.provenance, message.record.observed_at,
                        message.record.ttl, message.record.durability);
  writer.optional_string(message.record.link.has_value()
                             ? std::optional<std::string>(message.record.link->value())
                             : std::nullopt);
  if (message.record.capacity.has_value()) {
    writer.u8(1);
    write_quantity(writer, *message.record.capacity);
  } else {
    writer.u8(0);
  }
  writer.optional_string(message.record.source);
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
  writer.boolean(message.supersede);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(PublishLinkMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  out.record.key.cls = reader.enum8(RelationshipClass::AcceleratorPeer);
  if (!read_member_key(reader, out.record.key.from)) {
    return DecodeStatus::MalformedPayload;
  }
  if (!read_member_key(reader, out.record.key.to)) {
    return DecodeStatus::MalformedPayload;
  }
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.record.provenance = header.provenance;
    out.record.observed_at = header.observed_at;
    out.record.ttl = header.ttl;
    out.record.durability = header.durability;
  }
  {
    const auto link = reader.optional_string();
    if (link.has_value()) {
      const auto parsed = LinkId::parse(*link);
      if (!parsed.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      out.record.link = *parsed;
    }
  }
  if (reader.u8() == 1U) {
    Quantity quantity;
    read_quantity(reader, quantity);
    out.record.capacity = quantity;
  }
  out.record.source = reader.optional_string();
  if (reader.u8() == 1U) {
    out.expected_generation = TopologyGeneration::from_value(reader.u64());
  }
  out.supersede = reader.boolean();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(PublishFailureDomainMessage, PublishFailureDomain)
  write_authority(writer, message.authority);
  writer.string(message.record.id.value());
  writer.u8(static_cast<std::uint8_t>(message.record.kind));
  writer.u8(static_cast<std::uint8_t>(message.record.lifecycle));
  writer.collection_size(message.record.parents.size());
  for (const auto& parent : message.record.parents) {
    writer.string(parent.value());
  }
  write_evidence_header(writer, message.record.provenance, message.record.observed_at,
                        message.record.ttl, message.record.durability);
  writer.optional_string(message.record.label);
  writer.optional_string(message.record.source);
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
  writer.boolean(message.supersede);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(PublishFailureDomainMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  const auto id = read_typed_id<FailureDomainId>(reader);
  if (!id.has_value()) {
    return DecodeStatus::MalformedPayload;
  }
  out.record.id = *id;
  out.record.kind = reader.enum8(FailureDomainKind::Other);
  out.record.lifecycle = reader.enum8(MemberLifecycle::Retired);
  {
    const std::size_t count = reader.collection_size();
    for (std::size_t i = 0; i < count; ++i) {
      const auto parent = read_typed_id<FailureDomainId>(reader);
      if (!parent.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      out.record.parents.push_back(*parent);
    }
  }
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.record.provenance = header.provenance;
    out.record.observed_at = header.observed_at;
    out.record.ttl = header.ttl;
    out.record.durability = header.durability;
  }
  out.record.label = reader.optional_string();
  out.record.source = reader.optional_string();
  if (reader.u8() == 1U) {
    out.expected_generation = FailureDomainGeneration::from_value(reader.u64());
  }
  out.supersede = reader.boolean();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(PublishPowerMessage, PublishPower)
  write_authority(writer, message.authority);
  if (message.record.rack_limit.has_value()) {
    writer.u8(1);
    write_quantity(writer, *message.record.rack_limit);
  } else {
    writer.u8(0);
  }
  if (message.record.rack_observed_draw.has_value()) {
    writer.u8(1);
    write_quantity(writer, *message.record.rack_observed_draw);
  } else {
    writer.u8(0);
  }
  writer.collection_size(message.record.domain_budgets.size());
  for (const auto& budget : message.record.domain_budgets) {
    writer.string(budget.domain.value());
    if (budget.limit.has_value()) {
      writer.u8(1);
      write_quantity(writer, *budget.limit);
    } else {
      writer.u8(0);
    }
    if (budget.observed_draw.has_value()) {
      writer.u8(1);
      write_quantity(writer, *budget.observed_draw);
    } else {
      writer.u8(0);
    }
  }
  write_evidence_header(writer, message.record.provenance, message.record.observed_at,
                        message.record.ttl, message.record.durability);
  writer.optional_string(message.record.source);
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
  writer.boolean(message.supersede);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(PublishPowerMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  if (reader.u8() == 1U) {
    Quantity quantity;
    read_quantity(reader, quantity);
    out.record.rack_limit = quantity;
  }
  if (reader.u8() == 1U) {
    Quantity quantity;
    read_quantity(reader, quantity);
    out.record.rack_observed_draw = quantity;
  }
  {
    const std::size_t count = reader.collection_size();
    for (std::size_t i = 0; i < count; ++i) {
      const auto domain = read_typed_id<PowerDomainId>(reader);
      if (!domain.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      PowerDomainBudget budget{*domain};
      if (reader.u8() == 1U) {
        Quantity quantity;
        read_quantity(reader, quantity);
        budget.limit = quantity;
      }
      if (reader.u8() == 1U) {
        Quantity quantity;
        read_quantity(reader, quantity);
        budget.observed_draw = quantity;
      }
      out.record.domain_budgets.push_back(std::move(budget));
    }
  }
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.record.provenance = header.provenance;
    out.record.observed_at = header.observed_at;
    out.record.ttl = header.ttl;
    out.record.durability = header.durability;
  }
  out.record.source = reader.optional_string();
  if (reader.u8() == 1U) {
    out.expected_generation = PowerEnvelopeGeneration::from_value(reader.u64());
  }
  out.supersede = reader.boolean();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(PublishCoolingMessage, PublishCooling)
  write_authority(writer, message.authority);
  writer.collection_size(message.record.zones.size());
  for (const auto& zone : message.record.zones) {
    writer.string(zone.zone.value());
    if (zone.design_thermal_limit.has_value()) {
      writer.u8(1);
      write_quantity(writer, *zone.design_thermal_limit);
    } else {
      writer.u8(0);
    }
    if (zone.observed_temperature.has_value()) {
      writer.u8(1);
      write_quantity(writer, *zone.observed_temperature);
    } else {
      writer.u8(0);
    }
    if (zone.cooling_capacity.has_value()) {
      writer.u8(1);
      write_quantity(writer, *zone.cooling_capacity);
    } else {
      writer.u8(0);
    }
    if (zone.throttling.has_value()) {
      writer.u8(1);
      write_evidence_header(writer, zone.throttling->provenance, zone.throttling->observed_at,
                            zone.throttling->ttl, zone.throttling->durability);
      writer.u8(static_cast<std::uint8_t>(zone.throttling->value));
    } else {
      writer.u8(0);
    }
  }
  write_evidence_header(writer, message.record.provenance, message.record.observed_at,
                        message.record.ttl, message.record.durability);
  writer.optional_string(message.record.source);
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
  writer.boolean(message.supersede);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(PublishCoolingMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  {
    const std::size_t count = reader.collection_size();
    for (std::size_t i = 0; i < count; ++i) {
      const auto zone = read_typed_id<CoolingDomainId>(reader);
      if (!zone.has_value()) {
        return DecodeStatus::MalformedPayload;
      }
      CoolingZoneRecord record{*zone};
      if (reader.u8() == 1U) {
        Quantity quantity;
        read_quantity(reader, quantity);
        record.design_thermal_limit = quantity;
      }
      if (reader.u8() == 1U) {
        Quantity quantity;
        read_quantity(reader, quantity);
        record.observed_temperature = quantity;
      }
      if (reader.u8() == 1U) {
        Quantity quantity;
        read_quantity(reader, quantity);
        record.cooling_capacity = quantity;
      }
      if (reader.u8() == 1U) {
        EvidenceValue<ThrottleState> throttling;
        const EvidenceHeader header = read_evidence_header(reader);
        throttling.provenance = header.provenance;
        throttling.observed_at = header.observed_at;
        throttling.ttl = header.ttl;
        throttling.durability = header.durability;
        throttling.value = reader.enum8(ThrottleState::SeverelyThrottled);
        record.throttling = throttling;
      }
      out.record.zones.push_back(std::move(record));
    }
  }
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.record.provenance = header.provenance;
    out.record.observed_at = header.observed_at;
    out.record.ttl = header.ttl;
    out.record.durability = header.durability;
  }
  out.record.source = reader.optional_string();
  if (reader.u8() == 1U) {
    out.expected_generation = CoolingEnvelopeGeneration::from_value(reader.u64());
  }
  out.supersede = reader.boolean();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(PublishHealthMessage, PublishHealth)
  write_authority(writer, message.authority);
  write_member_key(writer, message.member);
  write_evidence_header(writer, message.health.provenance, message.health.observed_at,
                        message.health.ttl, message.health.durability);
  writer.u8(static_cast<std::uint8_t>(message.health.value));
  writer.boolean(message.publish_readiness);
  write_evidence_header(writer, message.readiness.provenance, message.readiness.observed_at,
                        message.readiness.ttl, message.readiness.durability);
  writer.u8(static_cast<std::uint8_t>(message.readiness.value));
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(PublishHealthMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  if (!read_member_key(reader, out.member)) {
    return DecodeStatus::MalformedPayload;
  }
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.health.provenance = header.provenance;
    out.health.observed_at = header.observed_at;
    out.health.ttl = header.ttl;
    out.health.durability = header.durability;
    out.health.value = reader.enum8(HealthState::NotApplicable);
  }
  out.publish_readiness = reader.boolean();
  {
    const EvidenceHeader header = read_evidence_header(reader);
    out.readiness.provenance = header.provenance;
    out.readiness.observed_at = header.observed_at;
    out.readiness.ttl = header.ttl;
    out.readiness.durability = header.durability;
    out.readiness.value = reader.enum8(ReadinessState::NotReady);
  }
  if (reader.u8() == 1U) {
    out.expected_generation = MemberGeneration::from_value(reader.u64());
  }
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(PublishCapabilityMessage, PublishCapability)
  write_authority(writer, message.authority);
  write_member_key(writer, message.member);
  writer.string(message.capability.id.value());
  write_evidence_header(writer, message.capability.provenance, message.capability.observed_at,
                        message.capability.ttl, message.capability.durability);
  writer.optional_string(message.capability.value);
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(PublishCapabilityMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  if (!read_member_key(reader, out.member)) {
    return DecodeStatus::MalformedPayload;
  }
  const auto capability_id = read_typed_id<CapabilityId>(reader);
  if (!capability_id.has_value()) {
    return DecodeStatus::MalformedPayload;
  }
  CapabilityRef capability{*capability_id};
  {
    const EvidenceHeader header = read_evidence_header(reader);
    capability.provenance = header.provenance;
    capability.observed_at = header.observed_at;
    capability.ttl = header.ttl;
    capability.durability = header.durability;
  }
  capability.value = reader.optional_string();
  out.capability = std::move(capability);
  if (reader.u8() == 1U) {
    out.expected_generation = MemberGeneration::from_value(reader.u64());
  }
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(WithdrawMessage, Withdraw)
  write_authority(writer, message.authority);
  write_member_key(writer, message.member);
  writer.u8(static_cast<std::uint8_t>(message.scope));
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(WithdrawMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  if (!read_member_key(reader, out.member)) {
    return DecodeStatus::MalformedPayload;
  }
  out.scope = reader.enum8(WithdrawScope::Details);
  if (reader.u8() == 1U) {
    out.expected_generation = MemberGeneration::from_value(reader.u64());
  }
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(RetireMemberMessage, RetireMember)
  write_authority(writer, message.authority);
  write_member_key(writer, message.member);
  if (message.expected_generation.has_value()) {
    writer.u8(1);
    writer.u64(message.expected_generation->value());
  } else {
    writer.u8(0);
  }
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(RetireMemberMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  if (!read_member_key(reader, out.member)) {
    return DecodeStatus::MalformedPayload;
  }
  if (reader.u8() == 1U) {
    out.expected_generation = MemberGeneration::from_value(reader.u64());
  }
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(RevalidateMessage, Revalidate)
  write_authority(writer, message.authority);
  writer.boolean(message.all);
  writer.collection_size(message.members.size());
  for (const auto& member : message.members) {
    write_member_key(writer, member);
  }
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(RevalidateMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  out.all = reader.boolean();
  {
    const std::size_t count = reader.collection_size();
    for (std::size_t i = 0; i < count; ++i) {
      MemberKey key;
      if (!read_member_key(reader, key)) {
        return DecodeStatus::MalformedPayload;
      }
      out.members.push_back(std::move(key));
    }
  }
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(HeartbeatMessage, Heartbeat)
  write_authority(writer, message.authority);
  writer.u64(message.publication_generation.value());
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(HeartbeatMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  out.publication_generation = PublicationGeneration::from_value(reader.u64());
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(QueryMessage, Query)
  writer.u16(message.query_kind);
  if (message.member.has_value()) {
    writer.u8(1);
    write_member_key(writer, *message.member);
  } else {
    writer.u8(0);
  }
  writer.optional_string(message.failure_domain.has_value()
                             ? std::optional<std::string>(message.failure_domain->value())
                             : std::nullopt);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(QueryMessage)
  out.query_kind = reader.u16();
  if (reader.u8() == 1U) {
    MemberKey key;
    if (!read_member_key(reader, key)) {
      return DecodeStatus::MalformedPayload;
    }
    out.member = std::move(key);
  }
  const auto domain = reader.optional_string();
  if (domain.has_value()) {
    const auto parsed = FailureDomainId::parse(*domain);
    if (!parsed.has_value()) {
      return DecodeStatus::MalformedPayload;
    }
    out.failure_domain = *parsed;
  }
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(SnapshotRequestMessage, SnapshotRequest)
  write_authority(writer, message.authority);
  writer.boolean(message.create);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(SnapshotRequestMessage)
  if (!read_authority(reader, out.authority)) {
    return DecodeStatus::MalformedPayload;
  }
  out.create = reader.boolean();
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(ResultMessage, Result)
  writer.boolean(message.accepted);
  writer.u16(static_cast<std::uint16_t>(message.outcome));
  write_generation_set(writer, message.generations);
  writer.string(message.code);
  writer.string(message.detail);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(ResultMessage)
  out.accepted = reader.boolean();
  const std::uint16_t outcome = reader.u16();
  if (outcome > static_cast<std::uint16_t>(MutationOutcome::RejectUnsupported)) {
    return DecodeStatus::MalformedPayload;
  }
  out.outcome = static_cast<MutationOutcome>(outcome);
  read_generation_set(reader, out.generations);
  const auto code = reader.string();
  if (code.has_value()) {
    out.code = *code;
  }
  const auto detail = reader.string();
  if (detail.has_value()) {
    out.detail = *detail;
  }
RACK_FABRIC_DECODE_END()

RACK_FABRIC_ENCODE_BEGIN(ErrorMessage, Error)
  writer.u16(static_cast<std::uint16_t>(message.error));
  writer.string(message.detail);
RACK_FABRIC_ENCODE_END()

RACK_FABRIC_DECODE_BEGIN(ErrorMessage)
  const std::uint16_t error = reader.u16();
  if (error > static_cast<std::uint16_t>(ProtocolError::Internal)) {
    return DecodeStatus::MalformedPayload;
  }
  out.error = static_cast<ProtocolError>(error);
  const auto detail = reader.string();
  if (detail.has_value()) {
    out.detail = *detail;
  }
RACK_FABRIC_DECODE_END()

std::optional<std::vector<std::byte>> MessageCodec::encode_snapshot_payload(
    const RackSnapshot& snapshot, const ResourceLimits& limits) {
  internal::ByteWriter writer(limits);
  writer.string(snapshot.id().value());
  writer.u64(snapshot.generation().value());
  writer.u64(snapshot.publication_generation().value());
  writer.timestamp(snapshot.created_at());
  writer.string(snapshot.rack().value());
  writer.string(snapshot.rack_epoch().value());
  internal::encode_generation_set(writer, snapshot.generations());
  writer.u8(static_cast<std::uint8_t>(snapshot.lifecycle()));
  writer.string(snapshot.digest());
  if (!writer.ok()) {
    return std::nullopt;
  }
  return std::move(writer).take();
}

#undef RACK_FABRIC_ENCODE_BEGIN
#undef RACK_FABRIC_ENCODE_END
#undef RACK_FABRIC_DECODE_BEGIN
#undef RACK_FABRIC_DECODE_END

}  // namespace rack_fabric
