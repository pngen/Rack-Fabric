// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <string>
#include <vector>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::declare_rack;
using rf_test::member;
using rf_test::observed;
using rf_test::publish_domain;
using rf_test::publish_member;
using rf_test::publish_rack_with_nodes;

[[nodiscard]] std::optional<MemberRecord> find(const RackFabric& fabric, MemberKind kind,
                                               const std::string& id) {
  return fabric.find_member(MemberKey{kind, id});
}

}  // namespace

RF_TEST(membership, register_publisher_is_idempotent_and_identity_bound) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  RF_CHECK(fabric.find_publisher(publisher.boot).has_value());
  RF_CHECK_EQ(fabric.publishers().size(), std::size_t{1});
  RF_CHECK_EQ(fabric.find_publisher(publisher.boot)->state, PublisherState::Active);

  // Registering the same boot identity for the same worker is a no-op.
  RegisterPublisherRequest again;
  again.rack = RackId{"rack-a"};
  again.worker = publisher.worker;
  again.boot = publisher.boot;
  again.coordinator_epoch = fabric.coordinator_epoch();
  RF_CHECK_EQ(fabric.register_publisher(again).outcome, MutationOutcome::NoChange);

  // The same boot identity cannot be claimed by another worker.
  RegisterPublisherRequest stolen = again;
  stolen.worker = WorkerId{"worker-2"};
  const MutationResult conflict = fabric.register_publisher(stolen);
  RF_CHECK_EQ(conflict.outcome, MutationOutcome::RejectNotAuthorized);
  RF_CHECK_EQ(conflict.explanation.code, std::string("WORKER_IDENTITY_CHANGED"));
}

RF_TEST(membership, registration_requires_a_declared_rack) {
  RackFabric fabric;
  RegisterPublisherRequest request;
  request.rack = RackId{"rack-a"};
  request.worker = WorkerId{"worker-1"};
  request.boot = AgentBootId{"boot-1"};
  request.coordinator_epoch = fabric.coordinator_epoch();
  const MutationResult result = fabric.register_publisher(request);
  RF_CHECK_EQ(result.outcome, MutationOutcome::RejectUnknownRack);
  RF_CHECK_EQ(result.explanation.code, std::string("RACK_NOT_DECLARED"));
}

RF_TEST(membership, declaring_a_second_rack_is_refused_without_redeclaration) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  DeclareRackRequest request;
  request.authority = rf_test::operator_token(fabric, "rack-b");
  request.epoch = RackEpochId{"epoch-1"};
  const MutationResult result = fabric.declare_rack(request);
  RF_CHECK_EQ(result.outcome, MutationOutcome::RejectConflict);
  RF_CHECK_EQ(fabric.rack_id()->value(), std::string("rack-a"));

  DeclareRackRequest same = request;
  same.authority = rf_test::operator_token(fabric, "rack-a");
  same.epoch = RackEpochId{"epoch-2"};
  RF_CHECK_EQ(fabric.declare_rack(same).outcome, MutationOutcome::RejectConflict);

  const MembershipGeneration membership_before = fabric.generations().membership;
  same.redeclare = true;
  const MutationResult redeclared = fabric.declare_rack(same);
  RF_CHECK(redeclared.accepted());
  RF_CHECK_EQ(fabric.rack_epoch()->value(), std::string("epoch-2"));
  // Redeclaration changes the epoch, never the membership generation.
  RF_CHECK_EQ(fabric.generations().membership.value(), membership_before.value());
}

RF_TEST(membership, publish_requires_a_rack_and_valid_records) {
  RackFabric fabric;
  PublishMemberRequest request;
  // Without authority the coordinator epoch cannot match, so the request is
  // refused before any rack is considered.
  request.authority = AuthorityToken{};
  request.record = member(MemberKind::Node, "node-1");
  const MutationResult no_authority = fabric.publish_member(request);
  RF_CHECK_EQ(no_authority.outcome, MutationOutcome::RejectStaleCoordinatorEpoch);

  // With a matching epoch but no declared rack, the rack is unknown.
  AuthorityToken unregistered;
  unregistered.coordinator_epoch = fabric.coordinator_epoch();
  request.authority = unregistered;
  const MutationResult no_rack = fabric.publish_member(request);
  RF_CHECK_EQ(no_rack.outcome, MutationOutcome::RejectUnknownRack);

  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");

  PublishMemberRequest invalid = request;
  invalid.authority = authority;
  invalid.record.key.id = "bad id with spaces";
  RF_CHECK_EQ(fabric.publish_member(invalid).outcome, MutationOutcome::RejectInvalidInput);

  PublishMemberRequest mismatched = request;
  mismatched.authority = authority;
  mismatched.record = member(MemberKind::Node, "node-1");
  NicDetails details;
  details.vendor = "vendor";
  mismatched.record.details = details;
  const MutationResult mismatch = fabric.publish_member(mismatched);
  RF_CHECK_EQ(mismatch.outcome, MutationOutcome::RejectInvalidInput);
  RF_CHECK_EQ(mismatch.explanation.code, std::string("DETAILS_DO_NOT_MATCH_KIND"));
}

RF_TEST(membership, every_member_kind_is_representable) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_domain(fabric, authority, rf_test::failure_domain("fd-node-0", FailureDomainKind::Node));
  publish_member(fabric, authority, member(MemberKind::PowerDomain, "pdu-0"));
  publish_member(fabric, authority, member(MemberKind::CoolingDomain, "cooling-0"));

  const MemberKind kinds[] = {MemberKind::Node,      MemberKind::Accelerator,
                              MemberKind::CpuPackage, MemberKind::MemoryDomain,
                              MemberKind::Nic,        MemberKind::Dpu,
                              MemberKind::Switch,     MemberKind::StorageEndpoint,
                              MemberKind::PowerDomain, MemberKind::CoolingDomain};
  for (const MemberKind kind : kinds) {
    if (kind == MemberKind::PowerDomain || kind == MemberKind::CoolingDomain) {
      continue;
    }
    MemberRecord record = member(kind, "member-" + std::string(to_string(kind)));
    if (kind == MemberKind::Node) {
      record.failure_domains.push_back(FailureDomainId{"fd-node-0"});
      record.power_domain = PowerDomainId{"pdu-0"};
      record.cooling_domain = CoolingDomainId{"cooling-0"};
    }
    publish_member(fabric, authority, record);
  }
  const RackSummary summary = fabric.summary();
  RF_CHECK_EQ(summary.node_count, std::size_t{1});
  RF_CHECK_EQ(summary.accelerator_count, std::size_t{1});
  RF_CHECK_EQ(summary.cpu_package_count, std::size_t{1});
  RF_CHECK_EQ(summary.memory_domain_count, std::size_t{1});
  RF_CHECK_EQ(summary.nic_count, std::size_t{1});
  RF_CHECK_EQ(summary.dpu_count, std::size_t{1});
  RF_CHECK_EQ(summary.switch_count, std::size_t{1});
  RF_CHECK_EQ(summary.storage_endpoint_count, std::size_t{1});
  RF_CHECK_EQ(summary.power_domain_count, std::size_t{1});
  RF_CHECK_EQ(summary.cooling_domain_count, std::size_t{1});
  RF_CHECK_EQ(summary.member_count, std::size_t{10});
  RF_CHECK(fabric.check_invariants().ok());
}

RF_TEST(membership, parent_must_exist_and_containment_must_be_acyclic) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");

  MemberRecord orphan = member(MemberKind::Accelerator, "acc-0");
  orphan.parent = MemberKey{MemberKind::Node, "node-9"};
  PublishMemberRequest request;
  request.authority = authority;
  request.record = orphan;
  const MutationResult unknown = fabric.publish_member(request);
  RF_CHECK_EQ(unknown.outcome, MutationOutcome::RejectUnknownParent);
  RF_CHECK_EQ(unknown.explanation.code, std::string("UNKNOWN_PARENT_MEMBER"));

  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  MemberRecord self = member(MemberKind::Node, "node-1");
  self.parent = MemberKey{MemberKind::Node, "node-1"};
  request.record = self;
  const MutationResult self_parent = fabric.publish_member(request);
  RF_CHECK_EQ(self_parent.outcome, MutationOutcome::RejectInvalidRelationship);
  RF_CHECK_EQ(self_parent.explanation.code, std::string("MEMBER_SELF_PARENT"));

  // A containment cycle is refused: node-1 contains node-2 contains node-1.
  MemberRecord child = member(MemberKind::Node, "node-2");
  child.parent = MemberKey{MemberKind::Node, "node-1"};
  publish_member(fabric, authority, child);
  MemberRecord loop = member(MemberKind::Node, "node-1");
  loop.parent = MemberKey{MemberKind::Node, "node-2"};
  request.record = loop;
  const MutationResult cycle = fabric.publish_member(request);
  RF_CHECK_EQ(cycle.outcome, MutationOutcome::RejectInvalidRelationship);
  RF_CHECK_EQ(cycle.explanation.code, std::string("CONTAINMENT_CYCLE"));
  RF_CHECK(fabric.check_invariants().ok());
}

RF_TEST(membership, identical_publication_is_a_no_change) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  const MemberRecord record = member(MemberKind::Node, "node-1");
  publish_member(fabric, publisher.token, record);
  const MembershipGeneration after_first = fabric.generations().membership;

  // Republishing exactly what the coordinator already holds is a no-change.
  const MemberRecord stored = *fabric.find_member(record.key);
  PublishMemberRequest repeat;
  repeat.authority = publisher.token;
  repeat.record = stored;
  const MutationResult result = fabric.publish_member(repeat);
  RF_CHECK_EQ(result.outcome, MutationOutcome::NoChange);
  RF_CHECK_EQ(result.explanation.code, std::string("MEMBER_UNCHANGED"));
  RF_CHECK_EQ(fabric.generations().membership.value(), after_first.value());
  // The helper advances the publisher token with its accepted publication, so
  // the publisher's recorded generation is 2 and the repeat is a no-change.
  RF_CHECK_EQ(fabric.find_publisher(publisher.boot)->publication_generation.value(), std::uint64_t{2});

  // Different content at the current generation is a conflict, not an overwrite.
  MemberRecord changed = stored;
  changed.lifecycle = MemberLifecycle::Unavailable;
  repeat.record = changed;
  const MutationResult conflict = fabric.publish_member(repeat);
  RF_CHECK_EQ(conflict.outcome, MutationOutcome::RejectConflict);
  RF_CHECK_EQ(conflict.explanation.code, std::string("CONFLICTING_MEMBER_PUBLICATION"));
  RF_CHECK_EQ(fabric.find_member(record.key)->lifecycle, MemberLifecycle::Present);
}

RF_TEST(membership, generation_cas_and_supersession) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  RF_CHECK_EQ(find(fabric, MemberKind::Node, "node-1")->generation.value(), std::uint64_t{1});

  PublishMemberRequest request;
  request.authority = authority;
  request.record = member(MemberKind::Node, "node-1");
  request.record.lifecycle = MemberLifecycle::Unavailable;
  request.expected_generation = MemberGeneration::from_value(0);
  const MutationResult stale = fabric.publish_member(request);
  RF_CHECK_EQ(stale.outcome, MutationOutcome::RejectStaleGeneration);
  RF_CHECK_EQ(stale.explanation.code, std::string("STALE_MEMBER_GENERATION"));

  request.expected_generation = MemberGeneration::from_value(5);
  const MutationResult ahead = fabric.publish_member(request);
  RF_CHECK_EQ(ahead.outcome, MutationOutcome::RejectConflict);
  RF_CHECK_EQ(ahead.explanation.code, std::string("MEMBER_GENERATION_AHEAD"));

  request.expected_generation = MemberGeneration::from_value(1);
  const MutationResult ok = fabric.publish_member(request);
  RF_CHECK(ok.accepted());
  RF_CHECK_EQ(find(fabric, MemberKind::Node, "node-1")->generation.value(), std::uint64_t{2});

  // A jump beyond the next generation needs explicit supersession.
  request.expected_generation.reset();
  request.record.generation = MemberGeneration::from_value(10);
  const MutationResult jump = fabric.publish_member(request);
  RF_CHECK_EQ(jump.outcome, MutationOutcome::RejectConflict);
  RF_CHECK_EQ(jump.explanation.code, std::string("MEMBER_GENERATION_JUMP"));
  request.supersede = true;
  const MutationResult superseded = fabric.publish_member(request);
  RF_CHECK(superseded.accepted());
  RF_CHECK_EQ(find(fabric, MemberKind::Node, "node-1")->generation.value(), std::uint64_t{10});
}

RF_TEST(membership, owner_is_taken_from_authority_and_cannot_be_forged) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher first = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                               "boot-1");
  rf_test::Publisher second = rf_test::register_publisher(fabric, "rack-a", "worker-2",
                                                                "boot-2");

  MemberRecord record = member(MemberKind::Node, "node-1");
  record.owner_boot = second.boot;
  record.owner_worker = second.worker;
  PublishMemberRequest forged;
  forged.authority = first.token;
  forged.record = record;
  const MutationResult refused = fabric.publish_member(forged);
  RF_CHECK_EQ(refused.outcome, MutationOutcome::RejectNotAuthorized);
  RF_CHECK_EQ(refused.explanation.code, std::string("OWNERSHIP_CHANGE_REFUSED"));

  forged.record.owner_boot.reset();
  forged.record.owner_worker.reset();
  publish_member(fabric, first.token, forged.record);
  const MemberRecord stored = *find(fabric, MemberKind::Node, "node-1");
  RF_CHECK_EQ(stored.owner_boot->value(), first.boot.value());
  RF_CHECK_EQ(stored.owner_worker->value(), first.worker.value());

  // The second publisher cannot overwrite evidence owned by the first.
  PublishMemberRequest other;
  other.authority = second.token;
  other.record = forged.record;
  other.record.lifecycle = MemberLifecycle::Unavailable;
  const MutationResult owned = fabric.publish_member(other);
  RF_CHECK_EQ(owned.outcome, MutationOutcome::RejectNotAuthorized);
  RF_CHECK_EQ(owned.explanation.code, std::string("MEMBER_OWNED_BY_OTHER_PROCESS"));
  RF_CHECK_EQ(find(fabric, MemberKind::Node, "node-1")->lifecycle, MemberLifecycle::Present);
}

RF_TEST(membership, unregistered_process_cannot_mutate) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  AuthorityToken token = rf_test::operator_token(fabric, "rack-a");
  token.boot = AgentBootId{"boot-unregistered"};
  token.publication_generation = PublicationGeneration::first();
  PublishMemberRequest request;
  request.authority = token;
  request.record = member(MemberKind::Node, "node-1");
  const MutationResult result = fabric.publish_member(request);
  RF_CHECK_EQ(result.outcome, MutationOutcome::RejectNotRegistered);
  RF_CHECK_EQ(result.explanation.code, std::string("PUBLISHER_NOT_REGISTERED"));
}

RF_TEST(membership, member_limit_is_enforced) {
  RackFabricOptions options;
  options.limits.max_members = 2;
  RackFabric fabric(options);
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  publish_member(fabric, authority, member(MemberKind::Node, "node-2"));
  PublishMemberRequest request;
  request.authority = authority;
  request.record = member(MemberKind::Node, "node-3");
  const MutationResult result = fabric.publish_member(request);
  RF_CHECK_EQ(result.outcome, MutationOutcome::RejectLimitExceeded);
  RF_CHECK_EQ(result.explanation.code, std::string("MEMBER_LIMIT_EXCEEDED"));
  RF_CHECK_EQ(fabric.summary().member_count, std::size_t{2});
}

RF_TEST(membership, retire_is_permanent_and_withdraw_is_scoped) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  MemberRecord record = member(MemberKind::Node, "node-1");
  EvidenceValue<HealthState> health;
  health.value = HealthState::Healthy;
  health.provenance = EvidenceProvenance::Measured;
  health.observed_at = observed();
  health.ttl = std::chrono::milliseconds{0};
  health.durability = Durability::Durable;
  record.health = health;
  EvidenceValue<ReachabilityState> reachability;
  reachability.value = ReachabilityState::Reachable;
  reachability.provenance = EvidenceProvenance::Measured;
  reachability.observed_at = observed();
  reachability.ttl = std::chrono::milliseconds{1000};
  reachability.durability = Durability::Ephemeral;
  record.reachability = reachability;
  publish_member(fabric, authority, record);

  WithdrawEvidenceRequest withdraw;
  withdraw.authority = authority;
  withdraw.member = record.key;
  withdraw.scope = WithdrawScope::Health;
  RF_CHECK(fabric.withdraw_evidence(withdraw).accepted());
  const MemberRecord after = *find(fabric, MemberKind::Node, "node-1");
  RF_CHECK_EQ(after.health.provenance, EvidenceProvenance::Unknown);
  RF_CHECK_EQ(after.reachability.provenance, EvidenceProvenance::Measured);
  RF_CHECK_EQ(after.lifecycle, MemberLifecycle::Present);

  withdraw.scope = WithdrawScope::Reachability;
  RF_CHECK(fabric.withdraw_evidence(withdraw).accepted());
  RF_CHECK_EQ(find(fabric, MemberKind::Node, "node-1")->reachability.provenance,
              EvidenceProvenance::Unknown);

  RetireMemberRequest retire;
  retire.authority = authority;
  retire.member = record.key;
  RF_CHECK(fabric.retire_member(retire).accepted());
  RF_CHECK_EQ(find(fabric, MemberKind::Node, "node-1")->lifecycle, MemberLifecycle::Retired);

  PublishMemberRequest republish;
  republish.authority = authority;
  republish.record = record;
  const MutationResult resurrect = fabric.publish_member(republish);
  RF_CHECK_EQ(resurrect.outcome, MutationOutcome::RejectRetired);
  RF_CHECK_EQ(resurrect.explanation.code, std::string("MEMBER_RETIRED"));
  const InvariantReport report = fabric.check_invariants();
  RF_CHECK(report.ok());
  if (!report.ok()) {
    rftest::record_failure(__FILE__, __LINE__,
                           report.render() + " first=" + report.violations.front().code + " " +
                               report.violations.front().detail);
  }
}

RF_TEST(membership, instances_are_isolated) {
  RackFabric first;
  RackFabric second;
  rf_test::declare_rack(first, "rack-a", "epoch-1");
  rf_test::declare_rack(second, "rack-b", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(first, "rack-a");
  publish_member(first, authority, member(MemberKind::Node, "node-1"));

  RF_CHECK_EQ(first.summary().member_count, std::size_t{1});
  RF_CHECK_EQ(second.summary().member_count, std::size_t{0});
  RF_CHECK_EQ(second.rack_id()->value(), std::string("rack-b"));
  RF_CHECK(!find(second, MemberKind::Node, "node-1").has_value());
  RF_CHECK_EQ(first.lifecycle(), RackLifecycle::Ready);
  RF_CHECK_EQ(second.lifecycle(), RackLifecycle::Declared);
}

RF_TEST(membership, unknown_evidence_never_becomes_present) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  MemberRecord declared = member(MemberKind::Node, "node-1", EvidenceProvenance::Unknown,
                                 MemberLifecycle::Declared);
  publish_member(fabric, authority, declared);
  const MemberRecord stored = *find(fabric, MemberKind::Node, "node-1");
  RF_CHECK_EQ(stored.lifecycle, MemberLifecycle::Declared);
  RF_CHECK_EQ(stored.provenance, EvidenceProvenance::Unknown);
  RF_CHECK(!stored.is_authoritatively_current_at(observed()));
  RF_CHECK_EQ(fabric.summary().present_member_count, std::size_t{0});
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Discovering);
  const Explanation explanation = fabric.explain_member(stored.key);
  RF_CHECK(!explanation.ok);
  RF_CHECK_EQ(explanation.code, std::string("MEMBER_NOT_CURRENT"));
  RF_CHECK(!explanation.factors.empty());
}
