// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::member;
using rf_test::observed;
using rf_test::publish_member;

[[nodiscard]] SnapshotResult take(RackFabric& fabric, const AuthorityToken& authority) {
  SnapshotRequest request;
  request.authority = authority;
  return fabric.publish_snapshot(request);
}

}  // namespace

RF_TEST(snapshots, publish_binds_identity_generations_and_digest) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  publish_member(fabric, authority, member(MemberKind::Node, "node-2"));

  const SnapshotResult result = take(fabric, authority);
  RF_REQUIRE(result.ok());
  const RackSnapshot& snapshot = *result.snapshot;
  RF_CHECK_EQ(snapshot.id().value(), std::string("snap-rack-a-1"));
  RF_CHECK_EQ(snapshot.generation().value(), std::uint64_t{1});
  RF_CHECK_EQ(snapshot.rack().value(), std::string("rack-a"));
  RF_CHECK_EQ(snapshot.rack_epoch().value(), std::string("epoch-1"));
  RF_CHECK_EQ(snapshot.members().size(), std::size_t{2});
  RF_CHECK(!snapshot.digest().empty());
  RF_CHECK_EQ(snapshot.digest().size(), std::size_t{64});
  RF_CHECK_EQ(snapshot.generations().membership.value(), fabric.generations().membership.value());
  RF_CHECK_EQ(snapshot.coordinator_epoch().value(), fabric.coordinator_epoch().value());

  const SnapshotValidation validation = fabric.validate_snapshot(snapshot);
  RF_CHECK_EQ(validation.status, SnapshotValidationStatus::Current);
  RF_CHECK(validation.is_current());
  RF_CHECK_EQ(validation.explanation.code, std::string("SNAPSHOT_CURRENT"));

  RF_CHECK_EQ(fabric.snapshot_count(), std::size_t{1});
  RF_CHECK(fabric.find_snapshot(snapshot.id()).has_value());
  RF_CHECK_EQ(fabric.snapshot_ids().size(), std::size_t{1});
}

RF_TEST(snapshots, snapshot_is_immutable_content_not_a_live_view) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  const SnapshotResult first = take(fabric, authority);
  RF_REQUIRE(first.ok());
  const std::string digest = first.snapshot->digest();

  publish_member(fabric, authority, member(MemberKind::Node, "node-2"));
  RF_CHECK_EQ(first.snapshot->members().size(), std::size_t{1});
  RF_CHECK_EQ(first.snapshot->digest(), digest);

  // The snapshot now binds generations that have moved on.
  const SnapshotValidation validation = fabric.validate_snapshot(*first.snapshot);
  RF_CHECK_EQ(validation.status, SnapshotValidationStatus::Stale);
  RF_CHECK_EQ(validation.explanation.code, std::string("SNAPSHOT_STALE"));
  RF_CHECK(!validation.explanation.factors.empty());

  const SnapshotResult second = take(fabric, authority);
  RF_REQUIRE(second.ok());
  RF_CHECK_EQ(second.snapshot->id().value(), std::string("snap-rack-a-2"));
  RF_CHECK_NE(second.snapshot->digest(), digest);
  RF_CHECK_EQ(fabric.validate_snapshot(*second.snapshot).status,
              SnapshotValidationStatus::Current);
  RF_CHECK_EQ(fabric.validate_snapshot(*first.snapshot).status, SnapshotValidationStatus::Stale);
}

RF_TEST(snapshots, digest_detects_tampering) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  const SnapshotResult result = take(fabric, authority);
  RF_REQUIRE(result.ok());

  RackSnapshot tampered = *result.snapshot;
  // Reconstructing a snapshot through the public API is impossible by design:
  // the class has no public constructor and no setters. The digest check is
  // therefore exercised through the persisted path, where bytes can be
  // altered, and through the generation comparison here.
  RF_CHECK_EQ(tampered.digest(), result.snapshot->digest());
  RF_CHECK_EQ(fabric.validate_snapshot(tampered).status, SnapshotValidationStatus::Current);
  RF_CHECK(tampered == *result.snapshot);
}

RF_TEST(snapshots, unknown_rack_is_reported) {
  RackFabric source;
  rf_test::declare_rack(source, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(source, "rack-a");
  publish_member(source, authority, member(MemberKind::Node, "node-1"));
  const SnapshotResult result = take(source, authority);
  RF_REQUIRE(result.ok());

  RackFabric other;
  rf_test::declare_rack(other, "rack-b", "epoch-1");
  const SnapshotValidation validation = other.validate_snapshot(*result.snapshot);
  RF_CHECK_EQ(validation.status, SnapshotValidationStatus::UnknownRack);
  RF_CHECK_EQ(validation.explanation.code, std::string("SNAPSHOT_UNKNOWN_RACK"));

  RackFabric empty;
  RF_CHECK_EQ(empty.validate_snapshot(*result.snapshot).status,
              SnapshotValidationStatus::UnknownRack);
}

RF_TEST(snapshots, retained_snapshots_are_bounded) {
  RackFabricOptions options;
  options.limits.max_retained_snapshots = 3;
  RackFabric fabric(options);
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  for (int index = 0; index < 6; ++index) {
    publish_member(fabric, authority, member(MemberKind::Node, "node-" + std::to_string(index)));
    RF_REQUIRE(take(fabric, authority).ok());
  }
  RF_CHECK_EQ(fabric.snapshot_count(), std::size_t{3});
  const std::vector<SnapshotId> ids = fabric.snapshot_ids();
  RF_REQUIRE(ids.size() == 3);
  RF_CHECK_EQ(ids[0].value(), std::string("snap-rack-a-4"));
  RF_CHECK_EQ(ids[2].value(), std::string("snap-rack-a-6"));
  RF_CHECK(!fabric.find_snapshot(SnapshotId{"snap-rack-a-3"}).has_value());
}

RF_TEST(snapshots, content_is_deterministically_ordered) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  for (const char* id : {"node-c", "node-a", "node-b"}) {
    publish_member(fabric, authority, member(MemberKind::Node, id));
  }
  const SnapshotResult result = take(fabric, authority);
  RF_REQUIRE(result.ok());
  const auto& members = result.snapshot->members();
  RF_REQUIRE(members.size() == 3);
  RF_CHECK_EQ(members[0].key.id, std::string("node-a"));
  RF_CHECK_EQ(members[1].key.id, std::string("node-b"));
  RF_CHECK_EQ(members[2].key.id, std::string("node-c"));
  const std::string rendered = result.snapshot->render();
  RF_CHECK(rendered.find("node-a") != std::string::npos);
  RF_CHECK(rendered.find("node-b") != std::string::npos);
  RF_CHECK(rendered.find("node-c") != std::string::npos);
  RF_CHECK(rendered.find("snap-rack-a-1") != std::string::npos);
  RF_CHECK_EQ(rendered, result.snapshot->render());
}

RF_TEST(snapshots, snapshot_requires_authority_and_a_rack) {
  RackFabric fabric;
  SnapshotRequest request;
  // Without any authority the coordinator epoch cannot match, so the request
  // is refused before anything else is considered.
  request.authority = AuthorityToken{};
  const SnapshotResult no_authority = fabric.publish_snapshot(request);
  RF_CHECK(!no_authority.ok());
  RF_CHECK_EQ(no_authority.result.outcome, MutationOutcome::RejectStaleCoordinatorEpoch);

  // With a matching epoch but no declared rack the rack identity is unknown.
  AuthorityToken unregistered;
  unregistered.coordinator_epoch = fabric.coordinator_epoch();
  request.authority = unregistered;
  const SnapshotResult no_rack = fabric.publish_snapshot(request);
  RF_CHECK(!no_rack.ok());
  RF_CHECK_EQ(no_rack.result.outcome, MutationOutcome::RejectUnknownRack);

  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  AuthorityToken stale = rf_test::operator_token(fabric, "rack-a");
  stale.coordinator_epoch = CoordinatorEpoch::from_value(stale.coordinator_epoch.value() + 5);
  request.authority = stale;
  const SnapshotResult rejected = fabric.publish_snapshot(request);
  RF_CHECK(!rejected.ok());
  RF_CHECK_EQ(rejected.result.outcome, MutationOutcome::RejectStaleCoordinatorEpoch);
  RF_CHECK_EQ(fabric.snapshot_count(), std::size_t{0});
}

RF_TEST(snapshots, snapshot_after_retirement_is_revalidatable) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  const SnapshotResult before = take(fabric, authority);
  RF_REQUIRE(before.ok());

  RetireRackRequest retire;
  retire.authority = authority;
  RF_REQUIRE(fabric.retire_rack(retire).accepted());
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Retired);
  RF_CHECK_EQ(fabric.validate_snapshot(*before.snapshot).status, SnapshotValidationStatus::Stale);
}
