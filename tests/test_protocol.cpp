// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "rack_fabric/protocol.hpp"
#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;

[[nodiscard]] Frame make_frame(MessageType type, std::vector<std::byte> payload,
                               std::uint64_t correlation = 7) {
  Frame frame;
  frame.type = type;
  frame.correlation = correlation;
  frame.payload = std::move(payload);
  return frame;
}

[[nodiscard]] DecodeResult decode(const std::vector<std::byte>& bytes) {
  return decode_frame(bytes.data(), bytes.size(), ResourceLimits{});
}

/// A decoded message must re-encode to exactly the bytes it was decoded from.
/// The codec is canonical, so encode/decode/encode is a byte-identical fixed
/// point; this proves every field survived the round trip without requiring
/// each message type to define equality.
template <class Message>
void check_same_encoding(const Message& decoded, const std::vector<std::byte>& bytes,
                         const char* label) {
  const auto again = MessageCodec::encode(decoded, ResourceLimits{});
  if (!again.has_value()) {
    rftest::record_failure(__FILE__, __LINE__, std::string(label) + ": re-encode failed");
    return;
  }
  if (*again != bytes) {
    rftest::record_failure(__FILE__, __LINE__,
                           std::string(label) + ": re-encoded bytes differ from the original");
  }
}

}  // namespace

RF_TEST(protocol, frame_round_trip_preserves_every_field) {
  std::vector<std::byte> payload(64);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    payload[index] = static_cast<std::byte>(index * 7U);
  }
  Frame frame = make_frame(MessageType::PublishNode, payload, 0x0102030405060708ULL);
  frame.flags = Frame::response_flag();
  const auto encoded = encode_frame(frame, ResourceLimits{});
  RF_REQUIRE(encoded.has_value());
  RF_CHECK_EQ(encoded->size(), payload.size() + kFrameOverheadBytes);
  const DecodeResult result = decode(*encoded);
  RF_REQUIRE(result.ok());
  RF_CHECK_EQ(result.frame.version, kProtocolVersion);
  RF_CHECK_EQ(result.frame.type, MessageType::PublishNode);
  RF_CHECK_EQ(result.frame.flags, Frame::response_flag());
  RF_CHECK_EQ(result.frame.correlation, 0x0102030405060708ULL);
  RF_CHECK(result.frame.payload == payload);
  RF_CHECK_EQ(result.consumed, encoded->size());
  RF_CHECK(result.frame.is_response());

  // A frame followed by another frame is decoded one frame at a time.
  std::vector<std::byte> two = *encoded;
  const auto second = encode_frame(make_frame(MessageType::Heartbeat, {}, 9), ResourceLimits{});
  RF_REQUIRE(second.has_value());
  two.insert(two.end(), second->begin(), second->end());
  const DecodeResult first_of_two = decode_frame(two.data(), two.size(), ResourceLimits{});
  RF_REQUIRE(first_of_two.ok());
  RF_CHECK_EQ(first_of_two.consumed, encoded->size());
  const DecodeResult second_of_two =
      decode_frame(two.data() + first_of_two.consumed, two.size() - first_of_two.consumed,
                   ResourceLimits{});
  RF_REQUIRE(second_of_two.ok());
  RF_CHECK_EQ(second_of_two.frame.correlation, 9U);
}

RF_TEST(protocol, empty_and_maximum_payloads_are_bounded) {
  const auto empty = encode_frame(make_frame(MessageType::Heartbeat, {}), ResourceLimits{});
  RF_REQUIRE(empty.has_value());
  RF_CHECK_EQ(empty->size(), kFrameOverheadBytes);

  ResourceLimits limits;
  limits.max_frame_bytes = 128;
  limits.max_payload_bytes = 128 - kFrameOverheadBytes;
  std::vector<std::byte> oversized(limits.max_payload_bytes + 1);
  RF_CHECK(!encode_frame(make_frame(MessageType::PublishNode, oversized), limits).has_value());
  std::vector<std::byte> at_limit(limits.max_payload_bytes);
  RF_CHECK(encode_frame(make_frame(MessageType::PublishNode, at_limit), limits).has_value());
}

RF_TEST(protocol, malformed_frames_are_rejected_without_allocating) {
  const auto encoded = encode_frame(make_frame(MessageType::Query, {std::byte{1}, std::byte{2}}),
                                    ResourceLimits{});
  RF_REQUIRE(encoded.has_value());

  {
    std::vector<std::byte> bytes = *encoded;
    bytes[0] = std::byte{'X'};
    const DecodeResult result = decode(bytes);
    RF_CHECK_EQ(result.status, DecodeStatus::BadMagic);
    RF_CHECK_EQ(result.consumed, std::size_t{0});
  }
  {
    std::vector<std::byte> bytes = *encoded;
    bytes[4] = std::byte{99};
    RF_CHECK_EQ(decode(bytes).status, DecodeStatus::UnsupportedVersion);
  }
  {
    std::vector<std::byte> bytes = *encoded;
    bytes[6] = std::byte{0xEE};
    bytes[7] = std::byte{0xEE};
    RF_CHECK_EQ(decode(bytes).status, DecodeStatus::UnknownMessageType);
  }
  {
    std::vector<std::byte> bytes = *encoded;
    bytes[10] = std::byte{1};
    RF_CHECK_EQ(decode(bytes).status, DecodeStatus::ReservedBitsSet);
  }
  {
    std::vector<std::byte> bytes = *encoded;
    bytes[20] = std::byte{0xFF};
    bytes[21] = std::byte{0xFF};
    bytes[22] = std::byte{0xFF};
    bytes[23] = std::byte{0x7F};
    const DecodeResult result = decode(bytes);
    RF_CHECK_EQ(result.status, DecodeStatus::OversizedFrame);
    RF_CHECK_EQ(result.consumed, std::size_t{0});
  }
  {
    // A payload length inside the bound but beyond the received bytes is a
    // truncated frame, not an oversized one.
    std::vector<std::byte> bytes = *encoded;
    bytes[20] = std::byte{0x00};
    bytes[21] = std::byte{0x04};
    bytes[22] = std::byte{0x00};
    bytes[23] = std::byte{0x00};
    RF_CHECK_EQ(decode(bytes).status, DecodeStatus::TruncatedFrame);
  }
  {
    std::vector<std::byte> bytes(encoded->begin(), encoded->begin() + 10);
    RF_CHECK_EQ(decode(bytes).status, DecodeStatus::TruncatedFrame);
  }
  {
    std::vector<std::byte> bytes = *encoded;
    bytes[bytes.size() - 1] = static_cast<std::byte>(bytes[bytes.size() - 1] ^ std::byte{0x01});
    RF_CHECK_EQ(decode(bytes).status, DecodeStatus::ChecksumMismatch);
  }
  {
    // A corrupted header byte must be caught by the checksum, not acted on.
    std::vector<std::byte> bytes = *encoded;
    bytes[12] = static_cast<std::byte>(bytes[12] ^ std::byte{0x80});
    RF_CHECK_EQ(decode(bytes).status, DecodeStatus::ChecksumMismatch);
  }
}

RF_TEST(protocol, message_codecs_round_trip) {
  const ResourceLimits limits;

  HelloMessage hello;
  hello.coordinator_epoch = CoordinatorEpoch::from_value(4);
  hello.agent_version = "1.0.0";
  const auto hello_bytes = MessageCodec::encode(hello, limits);
  RF_REQUIRE(hello_bytes.has_value());
  HelloMessage hello_out;
  RF_CHECK_EQ(MessageCodec::decode(hello_bytes->data(), hello_bytes->size(), hello_out, limits),
              DecodeStatus::Ok);
  RF_CHECK_EQ(hello_out.coordinator_epoch.value(), std::uint64_t{4});
  RF_CHECK_EQ(hello_out.agent_version, std::string("1.0.0"));
  check_same_encoding(hello_out, *hello_bytes, "hello");

  RegisterMessage registration;
  registration.rack = RackId{"rack-a"};
  registration.worker = WorkerId{"worker-1"};
  registration.boot = AgentBootId{"boot-1"};
  registration.coordinator_epoch = CoordinatorEpoch::from_value(2);
  registration.label = "agent";
  const auto registration_bytes = MessageCodec::encode(registration, limits);
  RF_REQUIRE(registration_bytes.has_value());
  RegisterMessage registration_out;
  RF_CHECK_EQ(MessageCodec::decode(registration_bytes->data(), registration_bytes->size(),
                                   registration_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(registration_out, *registration_bytes, "register");
  RF_CHECK_EQ(registration_out.boot->value(), std::string("boot-1"));

  PublishMemberMessage publish;
  publish.authority.coordinator_epoch = CoordinatorEpoch::from_value(3);
  publish.authority.boot = AgentBootId{"boot-1"};
  publish.authority.rack = RackId{"rack-a"};
  publish.authority.publication_generation = PublicationGeneration::from_value(5);
  publish.record = rf_test::member(MemberKind::Node, "node-1");
  publish.record.failure_domains.push_back(FailureDomainId{"fd-node-1"});
  publish.record.power_domain = PowerDomainId{"pdu-0"};
  publish.record.cooling_domain = CoolingDomainId{"cooling-0"};
  publish.record.switch_domain = SwitchId{"switch-0"};
  publish.record.capabilities.push_back(CapabilityRef{CapabilityId{"rdma"}, EvidenceProvenance::Measured,
                                                      rf_test::observed(), std::chrono::milliseconds{0},
                                                      Durability::Durable, false,
                                                      std::string("v2")});
  publish.expected_generation = MemberGeneration::from_value(1);
  publish.supersede = true;
  const auto publish_bytes = MessageCodec::encode(publish, limits);
  RF_REQUIRE(publish_bytes.has_value());
  PublishMemberMessage publish_out;
  RF_CHECK_EQ(MessageCodec::decode(publish_bytes->data(), publish_bytes->size(), publish_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(publish_out, *publish_bytes, "publish_member");

  PublishLinkMessage link;
  link.authority = publish.authority;
  link.record.key = RelationshipKey::canonicalize(RelationshipClass::ConnectedTo,
                                                  MemberKey{MemberKind::Node, "node-1"},
                                                  MemberKey{MemberKind::Switch, "switch-0"});
  link.record.provenance = EvidenceProvenance::Reported;
  link.record.observed_at = rf_test::observed();
  link.record.link = LinkId{"link-0"};
  const auto link_bytes = MessageCodec::encode(link, limits);
  RF_REQUIRE(link_bytes.has_value());
  PublishLinkMessage link_out;
  RF_CHECK_EQ(MessageCodec::decode(link_bytes->data(), link_bytes->size(), link_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(link_out, *link_bytes, "publish_link");

  PublishFailureDomainMessage domain;
  domain.authority = publish.authority;
  domain.record = rf_test::failure_domain("fd-node-1", FailureDomainKind::Node);
  domain.record.parents.push_back(FailureDomainId{"fd-rack-0"});
  const auto domain_bytes = MessageCodec::encode(domain, limits);
  RF_REQUIRE(domain_bytes.has_value());
  PublishFailureDomainMessage domain_out;
  RF_CHECK_EQ(MessageCodec::decode(domain_bytes->data(), domain_bytes->size(), domain_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(domain_out, *domain_bytes, "publish_failure_domain");

  PublishPowerMessage power;
  power.authority = publish.authority;
  power.record.provenance = EvidenceProvenance::Measured;
  power.record.observed_at = rf_test::observed();
  Quantity limit;
  limit.value = 10000.0;
  limit.unit = QuantityUnit::Watts;
  limit.provenance = EvidenceProvenance::Measured;
  limit.observed_at = rf_test::observed();
  power.record.rack_limit = limit;
  const auto power_bytes = MessageCodec::encode(power, limits);
  RF_REQUIRE(power_bytes.has_value());
  PublishPowerMessage power_out;
  RF_CHECK_EQ(MessageCodec::decode(power_bytes->data(), power_bytes->size(), power_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(power_out, *power_bytes, "publish_power");

  PublishCoolingMessage cooling;
  cooling.authority = publish.authority;
  cooling.record.provenance = EvidenceProvenance::Measured;
  cooling.record.observed_at = rf_test::observed();
  CoolingZoneRecord zone;
  zone.zone = CoolingDomainId{"cooling-0"};
  zone.design_thermal_limit = limit;
  cooling.record.zones.push_back(zone);
  const auto cooling_bytes = MessageCodec::encode(cooling, limits);
  RF_REQUIRE(cooling_bytes.has_value());
  PublishCoolingMessage cooling_out;
  RF_CHECK_EQ(MessageCodec::decode(cooling_bytes->data(), cooling_bytes->size(), cooling_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(cooling_out, *cooling_bytes, "publish_cooling");

  PublishHealthMessage health;
  health.authority = publish.authority;
  health.member = MemberKey{MemberKind::Node, "node-1"};
  health.health.value = HealthState::Healthy;
  health.health.provenance = EvidenceProvenance::Measured;
  health.health.observed_at = rf_test::observed();
  health.publish_readiness = true;
  const auto health_bytes = MessageCodec::encode(health, limits);
  RF_REQUIRE(health_bytes.has_value());
  PublishHealthMessage health_out;
  RF_CHECK_EQ(MessageCodec::decode(health_bytes->data(), health_bytes->size(), health_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(health_out, *health_bytes, "publish_health");

  PublishCapabilityMessage capability;
  capability.authority = publish.authority;
  capability.member = health.member;
  capability.capability = CapabilityRef{CapabilityId{"cuda"}, EvidenceProvenance::Measured,
                                        rf_test::observed()};
  const auto capability_bytes = MessageCodec::encode(capability, limits);
  RF_REQUIRE(capability_bytes.has_value());
  PublishCapabilityMessage capability_out;
  RF_CHECK_EQ(MessageCodec::decode(capability_bytes->data(), capability_bytes->size(),
                                   capability_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(capability_out, *capability_bytes, "publish_capability");

  WithdrawMessage withdraw;
  withdraw.authority = publish.authority;
  withdraw.member = health.member;
  withdraw.scope = WithdrawScope::Health;
  const auto withdraw_bytes = MessageCodec::encode(withdraw, limits);
  RF_REQUIRE(withdraw_bytes.has_value());
  WithdrawMessage withdraw_out;
  RF_CHECK_EQ(MessageCodec::decode(withdraw_bytes->data(), withdraw_bytes->size(), withdraw_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(withdraw_out, *withdraw_bytes, "withdraw");

  RetireMemberMessage retire;
  retire.authority = publish.authority;
  retire.member = health.member;
  const auto retire_bytes = MessageCodec::encode(retire, limits);
  RF_REQUIRE(retire_bytes.has_value());
  RetireMemberMessage retire_out;
  RF_CHECK_EQ(MessageCodec::decode(retire_bytes->data(), retire_bytes->size(), retire_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(retire_out, *retire_bytes, "retire_member");

  RevalidateMessage revalidate;
  revalidate.authority = publish.authority;
  revalidate.members.push_back(health.member);
  revalidate.all = true;
  const auto revalidate_bytes = MessageCodec::encode(revalidate, limits);
  RF_REQUIRE(revalidate_bytes.has_value());
  RevalidateMessage revalidate_out;
  RF_CHECK_EQ(MessageCodec::decode(revalidate_bytes->data(), revalidate_bytes->size(),
                                   revalidate_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(revalidate_out, *revalidate_bytes, "revalidate");

  HeartbeatMessage heartbeat;
  heartbeat.authority = publish.authority;
  heartbeat.publication_generation = PublicationGeneration::from_value(3);
  const auto heartbeat_bytes = MessageCodec::encode(heartbeat, limits);
  RF_REQUIRE(heartbeat_bytes.has_value());
  HeartbeatMessage heartbeat_out;
  RF_CHECK_EQ(MessageCodec::decode(heartbeat_bytes->data(), heartbeat_bytes->size(), heartbeat_out,
                                   limits),
              DecodeStatus::Ok);
  check_same_encoding(heartbeat_out, *heartbeat_bytes, "heartbeat");

  QueryMessage query;
  query.query_kind = static_cast<std::uint16_t>(QueryKind::Member);
  query.member = health.member;
  query.failure_domain = FailureDomainId{"fd-rack-0"};
  const auto query_bytes = MessageCodec::encode(query, limits);
  RF_REQUIRE(query_bytes.has_value());
  QueryMessage query_out;
  RF_CHECK_EQ(MessageCodec::decode(query_bytes->data(), query_bytes->size(), query_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(query_out, *query_bytes, "query");

  SnapshotRequestMessage snapshot;
  snapshot.authority = publish.authority;
  snapshot.create = true;
  const auto snapshot_bytes = MessageCodec::encode(snapshot, limits);
  RF_REQUIRE(snapshot_bytes.has_value());
  SnapshotRequestMessage snapshot_out;
  RF_CHECK_EQ(MessageCodec::decode(snapshot_bytes->data(), snapshot_bytes->size(), snapshot_out,
                                   limits),
              DecodeStatus::Ok);
  check_same_encoding(snapshot_out, *snapshot_bytes, "snapshot_request");

  ResultMessage result;
  result.accepted = true;
  result.outcome = MutationOutcome::Accepted;
  result.generations.membership = MembershipGeneration::from_value(3);
  result.code = "MEMBER_ADDED";
  result.detail = "node:node-1";
  const auto result_bytes = MessageCodec::encode(result, limits);
  RF_REQUIRE(result_bytes.has_value());
  ResultMessage result_out;
  RF_CHECK_EQ(MessageCodec::decode(result_bytes->data(), result_bytes->size(), result_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(result_out, *result_bytes, "result");

  ErrorMessage error;
  error.error = ProtocolError::StaleWorkerBoot;
  error.detail = "boot fenced";
  const auto error_bytes = MessageCodec::encode(error, limits);
  RF_REQUIRE(error_bytes.has_value());
  ErrorMessage error_out;
  RF_CHECK_EQ(MessageCodec::decode(error_bytes->data(), error_bytes->size(), error_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(error_out, *error_bytes, "error");

  RegisterAckMessage ack;
  ack.accepted = true;
  ack.coordinator_epoch = CoordinatorEpoch::from_value(2);
  ack.publication_generation = PublicationGeneration::from_value(1);
  ack.generations.membership = MembershipGeneration::from_value(2);
  const auto ack_bytes = MessageCodec::encode(ack, limits);
  RF_REQUIRE(ack_bytes.has_value());
  RegisterAckMessage ack_out;
  RF_CHECK_EQ(MessageCodec::decode(ack_bytes->data(), ack_bytes->size(), ack_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(ack_out, *ack_bytes, "register_ack");

  HelloAckMessage hello_ack;
  hello_ack.coordinator_epoch = CoordinatorEpoch::from_value(2);
  hello_ack.coordinator_version = "1.0.0";
  hello_ack.rack = RackId{"rack-a"};
  const auto hello_ack_bytes = MessageCodec::encode(hello_ack, limits);
  RF_REQUIRE(hello_ack_bytes.has_value());
  HelloAckMessage hello_ack_out;
  RF_CHECK_EQ(MessageCodec::decode(hello_ack_bytes->data(), hello_ack_bytes->size(), hello_ack_out,
                                   limits),
              DecodeStatus::Ok);
  check_same_encoding(hello_ack_out, *hello_ack_bytes, "hello_ack");

  DeclareRackMessage declare;
  declare.authority = publish.authority;
  declare.epoch = RackEpochId{"epoch-1"};
  declare.label = "rack";
  declare.redeclare = true;
  const auto declare_bytes = MessageCodec::encode(declare, limits);
  RF_REQUIRE(declare_bytes.has_value());
  DeclareRackMessage declare_out;
  RF_CHECK_EQ(MessageCodec::decode(declare_bytes->data(), declare_bytes->size(), declare_out, limits),
              DecodeStatus::Ok);
  check_same_encoding(declare_out, *declare_bytes, "declare_rack");
}

RF_TEST(protocol, malformed_payloads_are_rejected) {
  const ResourceLimits limits;
  PublishMemberMessage publish;
  publish.authority.coordinator_epoch = CoordinatorEpoch::from_value(1);
  publish.record = rf_test::member(MemberKind::Node, "node-1");
  const auto bytes = MessageCodec::encode(publish, limits);
  RF_REQUIRE(bytes.has_value());

  {
    // Trailing bytes are not silently ignored.
    std::vector<std::byte> extended = *bytes;
    extended.push_back(std::byte{0});
    PublishMemberMessage out;
    RF_CHECK_EQ(MessageCodec::decode(extended.data(), extended.size(), out, limits),
                DecodeStatus::TrailingGarbage);
  }
  {
    PublishMemberMessage out;
    RF_CHECK_EQ(MessageCodec::decode(bytes->data(), 1, out, limits),
                DecodeStatus::MalformedPayload);
    RF_CHECK_EQ(MessageCodec::decode(nullptr, 0, out, limits), DecodeStatus::MalformedPayload);
  }
  {
    // A collection count larger than the bound is refused before allocation.
    std::vector<std::byte> truncated(bytes->begin(), bytes->begin() + 4);
    PublishMemberMessage out;
    RF_CHECK_EQ(MessageCodec::decode(truncated.data(), truncated.size(), out, limits),
                DecodeStatus::MalformedPayload);
  }
  {
    // A truncated identity string cannot be decoded.
    std::vector<std::byte> cut(bytes->begin(), bytes->end() - 3);
    PublishMemberMessage out;
    RF_CHECK_NE(MessageCodec::decode(cut.data(), cut.size(), out, limits), DecodeStatus::Ok);
  }
}

RF_TEST(protocol, decoding_arbitrary_bytes_never_crashes) {
  std::mt19937_64 generator(0x5EED1234ULL);
  std::uniform_int_distribution<int> length_distribution(0, 512);
  std::uniform_int_distribution<int> byte_distribution(0, 255);
  std::size_t decoded = 0;
  std::size_t rejected = 0;
  for (int iteration = 0; iteration < 20000; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(length_distribution(generator));
    std::vector<std::byte> bytes(length);
    for (std::size_t index = 0; index < length; ++index) {
      bytes[index] = static_cast<std::byte>(byte_distribution(generator));
    }
    const DecodeResult result = decode_frame(bytes.data(), bytes.size(), ResourceLimits{});
    if (result.ok()) {
      ++decoded;
      RF_CHECK(result.consumed <= bytes.size());
    } else {
      ++rejected;
      RF_CHECK(!result.detail.empty() || result.status != DecodeStatus::Ok);
    }
    // A frame that begins with valid magic is also exercised.
    if (length >= 4) {
      bytes[0] = std::byte{'R'};
      bytes[1] = std::byte{'K'};
      bytes[2] = std::byte{'F'};
      bytes[3] = std::byte{'1'};
      const DecodeResult magic_result = decode_frame(bytes.data(), bytes.size(), ResourceLimits{});
      RF_CHECK(magic_result.consumed <= bytes.size());
    }
  }
  RF_CHECK(rejected > 0);
  RF_CHECK(decoded + rejected == 20000);
}

RF_TEST(protocol, snapshot_payload_is_self_describing_and_bounded) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  rf_test::publish_member(fabric, authority, rf_test::member(MemberKind::Node, "node-1"));
  SnapshotRequest request;
  request.authority = authority;
  const SnapshotResult snapshot = fabric.publish_snapshot(request);
  RF_REQUIRE(snapshot.ok());
  const auto bytes = MessageCodec::encode_snapshot_payload(*snapshot.snapshot, ResourceLimits{});
  RF_REQUIRE(bytes.has_value());
  RF_CHECK(bytes->size() > 32);

  ResourceLimits tiny;
  tiny.max_payload_bytes = 16;
  RF_CHECK(!MessageCodec::encode_snapshot_payload(*snapshot.snapshot, tiny).has_value());
}
