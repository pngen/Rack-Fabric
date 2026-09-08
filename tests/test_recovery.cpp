// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <filesystem>
#include <process.h>
#include <string>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::member;
using rf_test::observed;
using rf_test::publish_member;

class TempDir {
 public:
  TempDir() {
    static std::uint64_t counter = 0;
    path_ = std::filesystem::temp_directory_path() /
            ("rack_fabric_recovery_" + std::to_string(_getpid()) + "_" + std::to_string(++counter));
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

/// Publishes a rack with one durable node and one ephemeral observation.
void populate_mixed(RackFabric& fabric) {
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  MemberRecord node = member(MemberKind::Node, "node-1");
  EvidenceValue<ReachabilityState> reachability;
  reachability.value = ReachabilityState::Reachable;
  reachability.provenance = EvidenceProvenance::Measured;
  reachability.observed_at = observed();
  reachability.ttl = std::chrono::milliseconds{5000};
  reachability.durability = Durability::Ephemeral;
  node.reachability = reachability;
  publish_member(fabric, publisher.token, node);
  MemberRecord ephemeral = member(MemberKind::Nic, "nic-1", EvidenceProvenance::Measured,
                                  MemberLifecycle::Present);
  ephemeral.parent = MemberKey{MemberKind::Node, "node-1"};
  ephemeral.durability = Durability::Ephemeral;
  ephemeral.ttl = std::chrono::milliseconds{5000};
  publish_member(fabric, publisher.token, ephemeral);
}

}  // namespace

RF_TEST(recovery, restart_advances_the_epoch_and_keeps_durable_identity) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric first;
  populate_mixed(first);
  const CoordinatorEpoch epoch_before = first.coordinator_epoch();
  RF_REQUIRE(first.save_state(path).ok());

  RackFabric second;
  RF_REQUIRE(second.load_state(path).ok());
  RF_CHECK_EQ(second.coordinator_epoch().value(), epoch_before.value() + 1);
  RF_CHECK_EQ(second.rack_id()->value(), std::string("rack-a"));
  RF_CHECK_EQ(second.rack_epoch()->value(), std::string("epoch-1"));
  RF_CHECK(second.find_member(MemberKey{MemberKind::Node, "node-1"}).has_value());
  RF_CHECK(second.find_member(MemberKey{MemberKind::Nic, "nic-1"}).has_value());
  RF_CHECK(second.check_invariants().ok());

  // The recovered runtime refuses mutations issued under the dead epoch.
  AuthorityToken stale = rf_test::operator_token(second, "rack-a");
  stale.coordinator_epoch = epoch_before;
  PublishMemberRequest request;
  request.authority = stale;
  request.record = member(MemberKind::Node, "node-2");
  const MutationResult rejected = second.publish_member(request);
  RF_CHECK_EQ(rejected.outcome, MutationOutcome::RejectStaleCoordinatorEpoch);
  RF_CHECK(!second.find_member(MemberKey{MemberKind::Node, "node-2"}).has_value());
}

RF_TEST(recovery, live_observations_are_not_current_after_a_restart) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric first;
  populate_mixed(first);
  RF_REQUIRE(first.save_state(path).ok());
  RackFabric second;
  RF_REQUIRE(second.load_state(path).ok());

  const MemberRecord node = *second.find_member(MemberKey{MemberKind::Node, "node-1"});
  RF_CHECK_EQ(node.provenance, EvidenceProvenance::Measured);
  RF_CHECK_EQ(node.lifecycle, MemberLifecycle::Present);
  RF_CHECK(node.revalidation_required);
  RF_CHECK_EQ(node.revalidation_reason, RevalidationReason::CoordinatorRestarted);
  RF_CHECK_EQ(node.reachability.provenance, EvidenceProvenance::Unknown);
  RF_CHECK(!node.is_authoritatively_current_at(second.clock().now()));

  const MemberRecord nic = *second.find_member(MemberKey{MemberKind::Nic, "nic-1"});
  RF_CHECK(nic.revalidation_required);
  RF_CHECK_EQ(nic.lifecycle, MemberLifecycle::Present);
  RF_CHECK_EQ(second.lifecycle(), RackLifecycle::RevalidationRequired);

  const Explanation explanation = second.explain_readiness();
  RF_CHECK(!explanation.ok);
  RF_CHECK(!explanation.factors.empty());
}

RF_TEST(recovery, publishers_are_lost_and_cannot_resume_after_restart) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric first;
  populate_mixed(first);
  RF_REQUIRE(first.save_state(path).ok());
  RackFabric second;
  RF_REQUIRE(second.load_state(path).ok());
  RF_REQUIRE(second.publishers().size() == 1);
  RF_CHECK_EQ(second.publishers()[0].state, PublisherState::Lost);
  RF_CHECK(!second.is_boot_fenced(second.publishers()[0].boot));

  // The recovered publisher record carries the old epoch, so a mutation that
  // presents it is refused even if the boot identity is presented again.
  AuthorityToken resumed;
  resumed.coordinator_epoch = second.coordinator_epoch();
  resumed.boot = second.publishers()[0].boot;
  resumed.rack = RackId{"rack-a"};
  resumed.publication_generation = second.publishers()[0].publication_generation;
  PublishMemberRequest request;
  request.authority = resumed;
  request.record = member(MemberKind::Node, "node-3");
  const MutationResult rejected = second.publish_member(request);
  RF_CHECK_EQ(rejected.outcome, MutationOutcome::RejectStaleWorkerBoot);
  RF_CHECK_EQ(rejected.explanation.code, std::string("PUBLISHER_NOT_CURRENT"));
}

RF_TEST(recovery, readiness_is_re_earned_by_the_contract_after_restart) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabricOptions options;
  options.readiness.min_nodes = 2;
  options.readiness.require_power_envelope = false;
  options.readiness.require_cooling_envelope = false;
  options.readiness.require_all_members_current = true;

  RackFabric first(options);
  rf_test::declare_rack(first, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(first, "rack-a", "worker-1",
                                                                   "boot-1");
  publish_member(first, publisher.token, member(MemberKind::Node, "node-1"));
  publish_member(first, publisher.token, member(MemberKind::Node, "node-2"));
  RF_CHECK_EQ(first.lifecycle(), RackLifecycle::Ready);
  RF_REQUIRE(first.save_state(path).ok());

  RackFabric second(options);
  RF_REQUIRE(second.load_state(path).ok());
  RF_CHECK_EQ(second.lifecycle(), RackLifecycle::RevalidationRequired);
  RF_CHECK(!second.evaluate_readiness().satisfied);

  RevalidateMembersRequest revalidate;
  revalidate.authority = rf_test::operator_token(second, "rack-a");
  revalidate.all = true;
  RF_REQUIRE(second.revalidate_members(revalidate).accepted());
  RF_CHECK_EQ(second.lifecycle(), RackLifecycle::Ready);
  RF_CHECK(second.evaluate_readiness().satisfied);
  RF_CHECK_EQ(second.evaluate_readiness().qualifying_nodes, std::size_t{2});
}

RF_TEST(recovery, fenced_boots_survive_a_restart) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric first;
  rf_test::declare_rack(first, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(first, "rack-a", "worker-1",
                                                                   "boot-1");
  publish_member(first, publisher.token, member(MemberKind::Node, "node-1"));
  FencePublisherRequest fence;
  fence.coordinator_epoch = first.coordinator_epoch();
  fence.boot = publisher.boot;
  RF_REQUIRE(first.fence_publisher(fence).accepted());
  RF_REQUIRE(first.save_state(path).ok());

  RackFabric second;
  RF_REQUIRE(second.load_state(path).ok());
  RF_CHECK_EQ(second.fenced_boots().size(), std::size_t{1});
  RF_CHECK(second.is_boot_fenced(publisher.boot));
  RegisterPublisherRequest again;
  again.rack = RackId{"rack-a"};
  again.worker = publisher.worker;
  again.boot = publisher.boot;
  again.coordinator_epoch = second.coordinator_epoch();
  const MutationResult refused = second.register_publisher(again);
  RF_CHECK_EQ(refused.outcome, MutationOutcome::RejectStaleWorkerBoot);
  RF_CHECK_EQ(refused.explanation.code, std::string("BOOT_FENCED"));
}

RF_TEST(recovery, repeated_restarts_are_monotonic_and_bounded) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric seed;
  populate_mixed(seed);
  RF_REQUIRE(seed.save_state(path).ok());
  std::uint64_t previous_epoch = seed.coordinator_epoch().value();
  const std::uint64_t membership = seed.generations().membership.value();
  for (int cycle = 0; cycle < 5; ++cycle) {
    RackFabric next;
    RF_REQUIRE(next.load_state(path).ok());
    RF_CHECK_EQ(next.coordinator_epoch().value(), previous_epoch + 1);
    RF_CHECK_EQ(next.summary().member_count, std::size_t{2});
    RF_CHECK_EQ(next.generations().membership.value(), membership);
    RF_CHECK(next.check_invariants().ok());
    previous_epoch = next.coordinator_epoch().value();
    RF_REQUIRE(next.save_state(path).ok());
  }
  RF_CHECK_EQ(previous_epoch, seed.coordinator_epoch().value() + 5);
}

RF_TEST(recovery, snapshots_survive_but_do_not_become_current) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric first;
  populate_mixed(first);
  SnapshotRequest request;
  request.authority = rf_test::operator_token(first, "rack-a");
  const SnapshotResult snapshot = first.publish_snapshot(request);
  RF_REQUIRE(snapshot.ok());
  RF_REQUIRE(first.save_state(path).ok());

  RackFabric second;
  RF_REQUIRE(second.load_state(path).ok());
  RF_CHECK_EQ(second.snapshot_count(), std::size_t{1});
  const auto recovered = second.find_snapshot(snapshot.snapshot->id());
  RF_REQUIRE(recovered.has_value());
  RF_CHECK_EQ(recovered->digest(), snapshot.snapshot->digest());
  // The coordinator epoch advanced, so the snapshot no longer binds the
  // current authority.
  RF_CHECK_EQ(second.validate_snapshot(*recovered).status, SnapshotValidationStatus::Stale);
}

RF_TEST(recovery, reincarnated_agent_republishes_and_the_rack_recovers) {
  const TempDir dir;
  const std::filesystem::path path = dir.file("state.rkf");
  RackFabric first;
  rf_test::declare_rack(first, "rack-a", "epoch-1");
  rf_test::Publisher original = rf_test::register_publisher(first, "rack-a", "worker-1",
                                                                  "boot-1");
  publish_member(first, original.token, member(MemberKind::Node, "node-1"));
  RF_REQUIRE(first.save_state(path).ok());

  RackFabric second;
  RF_REQUIRE(second.load_state(path).ok());
  rf_test::Publisher reincarnated = rf_test::register_publisher(second, "rack-a", "worker-1",
                                                                      "boot-2");
  publish_member(second, reincarnated.token, member(MemberKind::Node, "node-1"));
  const MemberRecord node = *second.find_member(MemberKey{MemberKind::Node, "node-1"});
  RF_CHECK(!node.revalidation_required);
  RF_CHECK_EQ(node.owner_boot->value(), std::string("boot-2"));
  RF_CHECK_EQ(second.lifecycle(), RackLifecycle::Ready);
  RF_CHECK(second.check_invariants().ok());
}
