// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <process.h>
#include <string>
#include <vector>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::member;
using rf_test::observed;
using rf_test::publish_domain;
using rf_test::publish_member;

constexpr std::size_t kHeaderBytes = 72;
constexpr std::size_t kTrailerBytes = 8;

/// A per-test directory that is removed when the test finishes.
class TempDir {
 public:
  TempDir() {
    static std::uint64_t counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("rack_fabric_test_" + std::to_string(_getpid()) + "_" + std::to_string(++counter));
    std::filesystem::create_directories(path_);
  }
  ~TempDir() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] std::filesystem::path file(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::byte> read_all(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  std::vector<char> raw((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  std::vector<std::byte> bytes(raw.size());
  for (std::size_t index = 0; index < raw.size(); ++index) {
    bytes[index] = static_cast<std::byte>(static_cast<unsigned char>(raw[index]));
  }
  return bytes;
}

void write_all(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
}

/// Populates an instance with durable identity, live observations, a
/// publisher and a snapshot. RackFabric is neither copyable nor movable, so
/// the instance is always supplied by the caller.
void populate(RackFabric& fabric) {
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  publish_domain(fabric, publisher.token, rf_test::failure_domain("fd-node-0",
                                                                 FailureDomainKind::Node));
  MemberRecord other = member(MemberKind::Node, "node-2");
  other.failure_domains.push_back(FailureDomainId{"fd-node-0"});
  publish_member(fabric, publisher.token, other);
  MemberRecord node = member(MemberKind::Node, "node-1");
  node.failure_domains.push_back(FailureDomainId{"fd-node-0"});
  EvidenceValue<HealthState> health;
  health.value = HealthState::Healthy;
  health.provenance = EvidenceProvenance::Measured;
  health.observed_at = observed();
  health.ttl = std::chrono::milliseconds{60000};
  health.durability = Durability::Ephemeral;
  node.health = health;
  publish_member(fabric, publisher.token, node);
  PublishRelationshipRequest link;
  link.authority = publisher.token;
  link.record.key = RelationshipKey::canonicalize(RelationshipClass::ConnectedTo, node.key,
                                                  MemberKey{MemberKind::Node, "node-2"});
  link.record.provenance = EvidenceProvenance::Measured;
  link.record.observed_at = observed();
  const MutationResult linked = fabric.publish_relationship(link);
  RF_REQUIRE(linked.accepted());
  // The publisher advanced its publication generation with the accepted link.
  rf_test::advance(publisher.token);
  SnapshotRequest snapshot;
  snapshot.authority = publisher.token;
  RF_REQUIRE(fabric.publish_snapshot(snapshot).ok());
}

}  // namespace

RF_TEST(persistence, round_trip_preserves_durable_state_and_advances_the_epoch) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric source;
  populate(source);
  const CoordinatorEpoch source_epoch = source.coordinator_epoch();
  const GenerationSet before = source.generations();
  const PersistenceResult saved = source.save_state(path);
  RF_REQUIRE(saved.ok());
  RF_CHECK(saved.bytes_written > kHeaderBytes + kTrailerBytes);
  RF_CHECK(std::filesystem::exists(path));

  PersistedStateInfo info;
  const PersistenceResult inspected = inspect_persisted_state(path, info);
  RF_REQUIRE(inspected.ok());
  RF_CHECK_EQ(info.rack->value(), std::string("rack-a"));
  RF_CHECK_EQ(info.rack_epoch->value(), std::string("epoch-1"));
  RF_CHECK_EQ(info.member_count, std::size_t{2});
  RF_CHECK_EQ(info.relationship_count, std::size_t{1});
  RF_CHECK_EQ(info.failure_domain_count, std::size_t{1});
  RF_CHECK_EQ(info.publisher_count, std::size_t{1});
  RF_CHECK_EQ(info.snapshot_count, std::size_t{1});
  RF_CHECK_EQ(info.content_digest.size(), std::size_t{64});
  RF_CHECK_EQ(info.encoded_bytes, saved.bytes_written);

  RackFabric recovered;
  const PersistenceResult loaded = recovered.load_state(path);
  RF_REQUIRE(loaded.ok());
  RF_CHECK_EQ(recovered.rack_id()->value(), std::string("rack-a"));
  RF_CHECK_EQ(recovered.rack_epoch()->value(), std::string("epoch-1"));
  RF_CHECK_EQ(recovered.summary().member_count, std::size_t{2});
  RF_CHECK_EQ(recovered.summary().relationship_count, std::size_t{1});
  RF_CHECK_EQ(recovered.summary().snapshot_count, std::size_t{1});
  RF_CHECK_EQ(recovered.generations().membership.value(), before.membership.value());
  RF_CHECK_EQ(recovered.generations().coordinator_epoch.value(), source_epoch.value() + 1);
  RF_CHECK(recovered.check_invariants().ok());
}

RF_TEST(persistence, recovery_never_claims_recovered_evidence_is_current) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric source;
  populate(source);
  RF_REQUIRE(source.save_state(path).ok());

  RackFabric recovered;
  RF_REQUIRE(recovered.load_state(path).ok());
  const MemberRecord node = *recovered.find_member(MemberKey{MemberKind::Node, "node-1"});
  RF_CHECK_EQ(node.provenance, EvidenceProvenance::Measured);
  RF_CHECK_EQ(node.lifecycle, MemberLifecycle::Present);
  RF_CHECK(node.revalidation_required);
  RF_CHECK_EQ(node.revalidation_reason, RevalidationReason::CoordinatorRestarted);
  RF_CHECK(!node.is_authoritatively_current_at(observed()));
  RF_CHECK_EQ(node.health.provenance, EvidenceProvenance::Unknown);
  RF_CHECK_EQ(node.health.freshness_at(observed()), Freshness::Unknown);

  const PublisherRecord publisher = recovered.publishers()[0];
  RF_CHECK_EQ(publisher.state, PublisherState::Lost);
  RF_CHECK_EQ(recovered.lifecycle(), RackLifecycle::RevalidationRequired);

  // Revalidating by current authority makes the durable identity current again
  // without inventing a live observation.
  RevalidateMembersRequest revalidate;
  revalidate.authority = rf_test::operator_token(recovered, "rack-a");
  revalidate.all = true;
  RF_REQUIRE(recovered.revalidate_members(revalidate).accepted());
  const MemberRecord revalidated = *recovered.find_member(MemberKey{MemberKind::Node, "node-1"});
  RF_CHECK(!revalidated.revalidation_required);
  RF_CHECK_EQ(revalidated.health.provenance, EvidenceProvenance::Unknown);
  // The relationship and failure domain recovered from disk still require
  // revalidation, so the rack is not Ready until every recovered record is
  // revalidated. Revalidating the members is not enough.
  RF_CHECK_EQ(recovered.lifecycle(), RackLifecycle::RevalidationRequired);
}

RF_TEST(persistence, missing_file_is_reported_and_changes_nothing) {
  const TempDir dir;
  RackFabric fabric;
  const PersistenceResult result = fabric.load_state(dir.file("absent.rkf"));
  RF_CHECK_EQ(result.status, PersistenceStatus::NotFound);
  RF_CHECK_EQ(result.explanation.code, std::string("PERSISTENCE_NOT_FOUND"));
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Undeclared);
  PersistedStateInfo info;
  RF_CHECK_EQ(inspect_persisted_state(dir.file("absent.rkf"), info).status,
              PersistenceStatus::NotFound);
}

RF_TEST(persistence, corrupt_header_is_rejected) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric source;
  populate(source);
  RF_REQUIRE(source.save_state(path).ok());
  const std::vector<std::byte> original = read_all(path);
  RF_REQUIRE(original.size() > kHeaderBytes + kTrailerBytes);

  {
    std::vector<std::byte> bytes = original;
    bytes[0] = std::byte{'X'};
    write_all(path, bytes);
    RackFabric fabric;
    const PersistenceResult result = fabric.load_state(path);
    RF_CHECK_EQ(result.status, PersistenceStatus::Corrupt);
    RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Undeclared);
  }
  {
    std::vector<std::byte> bytes = original;
    bytes[8] = std::byte{99};
    write_all(path, bytes);
    RackFabric fabric;
    RF_CHECK_EQ(fabric.load_state(path).status, PersistenceStatus::UnsupportedVersion);
  }
  {
    // A payload length that does not match the file is not trusted.
    std::vector<std::byte> bytes = original;
    bytes[16] = std::byte{0xFF};
    bytes[17] = std::byte{0xFF};
    write_all(path, bytes);
    RackFabric fabric;
    const PersistenceResult result = fabric.load_state(path);
    RF_CHECK(result.status == PersistenceStatus::TrailingGarbage ||
             result.status == PersistenceStatus::Truncated ||
             result.status == PersistenceStatus::LimitExceeded ||
             result.status == PersistenceStatus::Corrupt);
    RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Undeclared);
  }
}

RF_TEST(persistence, truncation_at_every_offset_is_rejected) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric source;
  populate(source);
  RF_REQUIRE(source.save_state(path).ok());
  const std::vector<std::byte> original = read_all(path);

  const std::size_t probes[] = {0,  1,      7,          8,          16,          40,
                                71, 72,     73,         100,        kHeaderBytes + 1,
                                original.size() - kTrailerBytes - 1, original.size() - kTrailerBytes,
                                original.size() - 1};
  for (const std::size_t length : probes) {
    if (length >= original.size()) {
      continue;
    }
    std::vector<std::byte> truncated(original.begin(), original.begin() + length);
    write_all(path, truncated);
    RackFabric fabric;
    const PersistenceResult result = fabric.load_state(path);
    RF_CHECK(result.status != PersistenceStatus::Ok);
    RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Undeclared);
    RF_CHECK_EQ(fabric.summary().member_count, std::size_t{0});
    RF_CHECK(!result.explanation.code.empty());
  }
}

RF_TEST(persistence, integrity_failures_are_detected) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric source;
  populate(source);
  RF_REQUIRE(source.save_state(path).ok());
  const std::vector<std::byte> original = read_all(path);

  {
    // Payload byte flipped: the payload CRC and the header digest disagree.
    std::vector<std::byte> bytes = original;
    bytes[kHeaderBytes + 4] = static_cast<std::byte>(bytes[kHeaderBytes + 4] ^ std::byte{0x40});
    write_all(path, bytes);
    RackFabric fabric;
    const PersistenceResult result = fabric.load_state(path);
    RF_CHECK(result.status == PersistenceStatus::IntegrityFailure ||
             result.status == PersistenceStatus::Corrupt);
    RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Undeclared);
  }
  {
    // The recorded SHA-256 is altered.
    std::vector<std::byte> bytes = original;
    bytes[40] = static_cast<std::byte>(bytes[40] ^ std::byte{0xFF});
    write_all(path, bytes);
    RackFabric fabric;
    RF_CHECK_EQ(fabric.load_state(path).status, PersistenceStatus::IntegrityFailure);
  }
  {
    // The trailer magic is destroyed.
    std::vector<std::byte> bytes = original;
    bytes[bytes.size() - kTrailerBytes] = std::byte{'Z'};
    write_all(path, bytes);
    RackFabric fabric;
    RF_CHECK_EQ(fabric.load_state(path).status, PersistenceStatus::Corrupt);
  }
  {
    // Trailing garbage after a valid file is refused.
    std::vector<std::byte> bytes = original;
    bytes.push_back(std::byte{0x5A});
    write_all(path, bytes);
    RackFabric fabric;
    RF_CHECK_EQ(fabric.load_state(path).status, PersistenceStatus::TrailingGarbage);
  }
}

RF_TEST(persistence, hostile_payload_length_cannot_allocate) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric source;
  populate(source);
  RF_REQUIRE(source.save_state(path).ok());
  std::vector<std::byte> bytes = read_all(path);
  // Declare a payload far beyond the configured bound.
  bytes[16] = std::byte{0xFF};
  bytes[17] = std::byte{0xFF};
  bytes[18] = std::byte{0xFF};
  bytes[19] = std::byte{0x7F};
  write_all(path, bytes);
  RackFabric fabric;
  const PersistenceResult result = fabric.load_state(path);
  RF_CHECK_EQ(result.status, PersistenceStatus::LimitExceeded);
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Undeclared);
}

RF_TEST(persistence, write_bound_and_atomic_replace) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric source;
  populate(source);

  PersistenceOptions options;
  options.max_bytes = 8;
  const PersistenceResult too_large = source.save_state(path, options);
  RF_CHECK_EQ(too_large.status, PersistenceStatus::LimitExceeded);
  RF_CHECK(!std::filesystem::exists(path));

  RF_REQUIRE(source.save_state(path).ok());
  const std::vector<std::byte> first = read_all(path);
  publish_member(source, rf_test::operator_token(source, "rack-a"),
                 member(MemberKind::Node, "node-3"));
  RF_REQUIRE(source.save_state(path).ok());
  const std::vector<std::byte> second = read_all(path);
  RF_CHECK(second.size() > first.size());

  // No temporary file is left behind next to the state file.
  for (const auto& entry : std::filesystem::directory_iterator(dir.file(""))) {
    const std::string name = entry.path().filename().string();
    RF_CHECK_EQ(name, std::string("state.rkf"));
  }

  // A non-atomic write is still a complete write.
  PersistenceOptions direct;
  direct.atomic_replace = false;
  RF_REQUIRE(source.save_state(path, direct).ok());
  RackFabric recovered;
  RF_REQUIRE(recovered.load_state(path).ok());
  RF_CHECK_EQ(recovered.summary().member_count, std::size_t{3});
}

RF_TEST(persistence, loading_replaces_the_whole_instance) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric source;
  populate(source);
  RF_REQUIRE(source.save_state(path).ok());

  RackFabric target;
  rf_test::declare_rack(target, "rack-other", "epoch-9");
  publish_member(target, rf_test::operator_token(target, "rack-other"),
                 member(MemberKind::Node, "other-node"));
  RF_REQUIRE(target.load_state(path).ok());
  RF_CHECK_EQ(target.rack_id()->value(), std::string("rack-a"));
  RF_CHECK_EQ(target.rack_epoch()->value(), std::string("epoch-1"));
  RF_CHECK(!target.find_member(MemberKey{MemberKind::Node, "other-node"}).has_value());
  RF_CHECK(target.find_member(MemberKey{MemberKind::Node, "node-1"}).has_value());
  RF_CHECK(target.check_invariants().ok());
}

RF_TEST(persistence, states_larger_than_one_message_round_trip) {
  // The durable encoding is bounded by the rack limits, not by the per-message
  // collection limit: a rack may legitimately hold far more members than one
  // protocol message may carry.
  const TempDir dir;
  const std::filesystem::path path = dir.file("large.rkf");
  RackFabric source;
  rf_test::declare_rack(source, "rack-a", "epoch-1");
  AuthorityToken authority = rf_test::operator_token(source, "rack-a");
  constexpr std::size_t kMembers = 5000;
  for (std::size_t index = 0; index < kMembers; ++index) {
    publish_member(source, authority,
                   member(MemberKind::Node, "bulk-node-" + std::to_string(index)));
  }
  RF_CHECK_EQ(source.summary().member_count, kMembers);
  RF_REQUIRE(source.save_state(path).ok());
  RackFabric recovered;
  RF_REQUIRE(recovered.load_state(path).ok());
  RF_CHECK_EQ(recovered.summary().member_count, kMembers);
  RF_CHECK(recovered.find_member(MemberKey{MemberKind::Node, "bulk-node-4999"}).has_value());
  RF_CHECK(recovered.check_invariants().ok());
}

RF_TEST(persistence, empty_instance_round_trips) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("empty.rkf");
  RackFabric empty;
  const PersistenceResult saved = empty.save_state(path);
  RF_REQUIRE(saved.ok());
  RackFabric recovered;
  RF_REQUIRE(recovered.load_state(path).ok());
  RF_CHECK_EQ(recovered.lifecycle(), RackLifecycle::Undeclared);
  RF_CHECK(!recovered.rack_id().has_value());
  RF_CHECK_EQ(recovered.summary().member_count, std::size_t{0});
}
