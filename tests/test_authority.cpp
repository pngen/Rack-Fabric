// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <memory>
#include <string>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::member;
using rf_test::observed;
using rf_test::publish_member;

[[nodiscard]] MutationResult publish(RackFabric& fabric, const AuthorityToken& authority,
                                     const std::string& id,
                                     MemberLifecycle lifecycle = MemberLifecycle::Present) {
  PublishMemberRequest request;
  request.authority = authority;
  request.record = member(MemberKind::Node, id, EvidenceProvenance::Measured, lifecycle);
  return fabric.publish_member(request);
}

}  // namespace

RF_TEST(authority, stale_coordinator_epoch_is_refused) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken current = rf_test::operator_token(fabric, "rack-a");
  RF_CHECK(publish(fabric, current, "node-1").accepted());

  AuthorityToken stale = current;
  stale.coordinator_epoch = CoordinatorEpoch::from_value(current.coordinator_epoch.value() + 1);
  const MutationResult result = publish(fabric, stale, "node-2");
  RF_CHECK_EQ(result.outcome, MutationOutcome::RejectStaleCoordinatorEpoch);
  RF_CHECK_EQ(result.explanation.code, std::string("STALE_COORDINATOR_EPOCH"));
  RF_CHECK(!result.explanation.factors.empty());
  RF_CHECK_EQ(result.explanation.factors[0].generation_name.value(), std::string("coordinator_epoch"));
  RF_CHECK(!fabric.find_member(MemberKey{MemberKind::Node, "node-2"}).has_value());
}

RF_TEST(authority, publication_generation_is_a_cas) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  AuthorityToken token = publisher.token;
  RF_CHECK(publish(fabric, token, "node-1").accepted());
  RF_CHECK_EQ(fabric.find_publisher(publisher.boot)->publication_generation.value(),
              std::uint64_t{2});

  // Replaying the first publication generation is stale.
  const MutationResult replayed = publish(fabric, token, "node-2");
  RF_CHECK_EQ(replayed.outcome, MutationOutcome::RejectStaleGeneration);
  RF_CHECK_EQ(replayed.explanation.code, std::string("STALE_PUBLICATION_GENERATION"));

  // Claiming a future generation is a conflict, not an acceptance.
  token.publication_generation = PublicationGeneration::from_value(99);
  const MutationResult ahead = publish(fabric, token, "node-3");
  RF_CHECK_EQ(ahead.outcome, MutationOutcome::RejectConflict);
  RF_CHECK_EQ(ahead.explanation.code, std::string("PUBLICATION_GENERATION_AHEAD"));

  // The publisher's own next generation is accepted.
  token.publication_generation = PublicationGeneration::from_value(2);
  RF_CHECK(publish(fabric, token, "node-4").accepted());
}

RF_TEST(authority, lease_expiry_fences_the_publisher_and_its_evidence) {
  auto clock = std::make_shared<ManualClock>(rf_test::kObservationMillis);
  RackFabricOptions options;
  options.clock = clock;
  options.limits.publisher_lease_millis = 1000;
  RackFabric fabric(options);
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");

  MemberRecord record = member(MemberKind::Node, "node-1");
  EvidenceValue<ReachabilityState> reachability;
  reachability.value = ReachabilityState::Reachable;
  reachability.provenance = EvidenceProvenance::Measured;
  reachability.observed_at = observed();
  reachability.ttl = std::chrono::milliseconds{100000};
  reachability.durability = Durability::Ephemeral;
  record.reachability = reachability;
  publish_member(fabric, publisher.token, record);

  // Within the lease, a heartbeat keeps the publisher current.
  clock->advance(std::chrono::milliseconds{500});
  HeartbeatRequest heartbeat;
  heartbeat.authority = publisher.token;
  RF_CHECK(fabric.heartbeat(heartbeat).accepted());
  RF_CHECK_EQ(fabric.expire_publishers(), std::size_t{0});

  // Without a heartbeat the lease expires.
  clock->advance(std::chrono::milliseconds{1500});
  RF_CHECK_EQ(fabric.expire_publishers(), std::size_t{1});
  RF_CHECK_EQ(fabric.find_publisher(publisher.boot)->state, PublisherState::Fenced);
  RF_CHECK(fabric.is_boot_fenced(publisher.boot));
  RF_CHECK_EQ(fabric.fenced_boots().size(), std::size_t{1});
  RF_CHECK_EQ(fabric.fenced_boots()[0].reason, RevalidationReason::OwnerProcessLost);

  // The dead process can never publish again with the same boot identity.
  const MutationResult replayed = publish(fabric, publisher.token, "node-2");
  RF_CHECK_EQ(replayed.outcome, MutationOutcome::RejectStaleWorkerBoot);
  RF_CHECK_EQ(replayed.explanation.code, std::string("BOOT_FENCED"));

  // Its live observation is no longer current and is flagged for revalidation.
  const MemberRecord stored = *fabric.find_member(record.key);
  RF_CHECK(stored.revalidation_required);
  RF_CHECK_EQ(stored.revalidation_reason, RevalidationReason::OwnerProcessLost);
  RF_CHECK_EQ(stored.reachability.freshness_at(clock->now()), Freshness::RevalidationRequired);
  RF_CHECK(!stored.reachability.is_current_at(clock->now()));
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::RevalidationRequired);
}

RF_TEST(authority, fencing_a_boot_advances_member_generations_and_marks_revalidation) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  publish_member(fabric, publisher.token, member(MemberKind::Node, "node-1"));
  publish_member(fabric, publisher.token, member(MemberKind::Accelerator, "acc-1"));
  const MemberGeneration before =
      fabric.find_member(MemberKey{MemberKind::Node, "node-1"})->generation;

  FencePublisherRequest fence;
  fence.coordinator_epoch = fabric.coordinator_epoch();
  fence.boot = publisher.boot;
  fence.reason = RevalidationReason::OwnerProcessLost;
  const MutationResult result = fabric.fence_publisher(fence);
  RF_REQUIRE(result.accepted());
  RF_CHECK_EQ(result.explanation.code, std::string("PUBLISHER_FENCED"));
  RF_CHECK_EQ(fabric.find_publisher(publisher.boot)->state, PublisherState::Fenced);
  RF_CHECK(fabric.is_boot_fenced(publisher.boot));
  RF_CHECK_EQ(fabric.fenced_boots()[0].worker->value(), std::string("worker-1"));

  const MemberRecord stored = *fabric.find_member(MemberKey{MemberKind::Node, "node-1"});
  RF_CHECK(stored.revalidation_required);
  RF_CHECK(stored.generation > before);
  RF_CHECK_EQ(stored.lifecycle, MemberLifecycle::Present);
  RF_CHECK_EQ(stored.provenance, EvidenceProvenance::Measured);

  // Fencing is idempotent and a fenced identity cannot re-register.
  RF_CHECK_EQ(fabric.fence_publisher(fence).outcome, MutationOutcome::NoChange);
  RegisterPublisherRequest again;
  again.rack = RackId{"rack-a"};
  again.worker = publisher.worker;
  again.boot = publisher.boot;
  again.coordinator_epoch = fabric.coordinator_epoch();
  const MutationResult refused = fabric.register_publisher(again);
  RF_CHECK_EQ(refused.outcome, MutationOutcome::RejectStaleWorkerBoot);
  RF_CHECK_EQ(refused.explanation.code, std::string("BOOT_FENCED"));

  // Revalidation by current authority clears the flag.
  RevalidateMembersRequest revalidate;
  revalidate.authority = rf_test::operator_token(fabric, "rack-a");
  revalidate.all = true;
  const MutationResult revalidated = fabric.revalidate_members(revalidate);
  RF_REQUIRE(revalidated.accepted());
  RF_CHECK(!fabric.find_member(MemberKey{MemberKind::Node, "node-1"})->revalidation_required);
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Ready);
}

RF_TEST(authority, reincarnation_gets_fresh_authority_and_the_old_boot_stays_dead) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher first = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                               "boot-1");
  publish_member(fabric, first.token, member(MemberKind::Node, "node-1"));
  FencePublisherRequest fence;
  fence.coordinator_epoch = fabric.coordinator_epoch();
  fence.boot = first.boot;
  RF_REQUIRE(fabric.fence_publisher(fence).accepted());

  rf_test::Publisher second = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                "boot-2");
  RF_CHECK_EQ(second.token.publication_generation->value(), std::uint64_t{1});
  const MutationResult published = publish(fabric, second.token, "node-2");
  RF_REQUIRE(published.accepted());
  RF_CHECK_EQ(fabric.find_member(MemberKey{MemberKind::Node, "node-2"})->owner_boot->value(),
              std::string("boot-2"));
  RF_CHECK(fabric.is_boot_fenced(first.boot));
  RF_CHECK(!fabric.is_boot_fenced(second.boot));

  const MutationResult ghost = publish(fabric, first.token, "node-3");
  RF_CHECK_EQ(ghost.outcome, MutationOutcome::RejectStaleWorkerBoot);
  RF_CHECK_EQ(fabric.summary().publisher_count, std::size_t{2});
  RF_CHECK_EQ(fabric.summary().active_publisher_count, std::size_t{1});
  RF_CHECK_EQ(fabric.summary().fenced_boot_count, std::size_t{1});
  RF_CHECK(fabric.check_invariants().ok());
}

RF_TEST(authority, heartbeat_requires_a_registered_process) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  HeartbeatRequest heartbeat;
  heartbeat.authority = rf_test::operator_token(fabric, "rack-a");
  const MutationResult operator_heartbeat = fabric.heartbeat(heartbeat);
  RF_CHECK_EQ(operator_heartbeat.outcome, MutationOutcome::RejectNotRegistered);
  RF_CHECK_EQ(operator_heartbeat.explanation.code, std::string("HEARTBEAT_REQUIRES_BOOT"));

  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  heartbeat.authority = publisher.token;
  RF_CHECK(fabric.heartbeat(heartbeat).accepted());
  RF_CHECK_EQ(fabric.find_publisher(publisher.boot)->state, PublisherState::Active);
}

RF_TEST(authority, retiring_the_rack_revokes_every_process_authority) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  publish_member(fabric, publisher.token, member(MemberKind::Node, "node-1"));

  RetireRackRequest retire;
  retire.authority = rf_test::operator_token(fabric, "rack-a");
  RF_REQUIRE(fabric.retire_rack(retire).accepted());
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Retired);
  RF_CHECK_EQ(fabric.find_member(MemberKey{MemberKind::Node, "node-1"})->lifecycle,
              MemberLifecycle::Unavailable);
  RF_CHECK(fabric.is_boot_fenced(publisher.boot));
  RF_CHECK_EQ(fabric.find_publisher(publisher.boot)->state, PublisherState::Lost);

  // A retired rack refuses every process, fenced or not, before any other
  // authority consideration.
  const MutationResult after = publish(fabric, publisher.token, "node-2");
  RF_CHECK_EQ(after.outcome, MutationOutcome::RejectRackRetired);
  RF_CHECK_EQ(after.explanation.code, std::string("RACK_RETIRED"));
  const MutationResult operator_after = publish(fabric, rf_test::operator_token(fabric, "rack-a"),
                                                "node-3");
  RF_CHECK_EQ(operator_after.outcome, MutationOutcome::RejectRackRetired);
  RF_CHECK_EQ(operator_after.explanation.code, std::string("RACK_RETIRED"));
  RF_CHECK_EQ(fabric.retire_rack(retire).outcome, MutationOutcome::NoChange);
}

RF_TEST(authority, wrong_rack_identity_is_refused) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  AuthorityToken token = rf_test::operator_token(fabric, "rack-b");
  PublishMemberRequest request;
  request.authority = token;
  request.record = member(MemberKind::Node, "node-1");
  const MutationResult result = fabric.publish_member(request);
  RF_CHECK_EQ(result.outcome, MutationOutcome::RejectUnknownRack);
  RF_CHECK_EQ(result.explanation.code, std::string("RACK_IDENTITY_MISMATCH"));
}
