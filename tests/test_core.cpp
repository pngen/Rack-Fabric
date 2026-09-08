// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"

namespace {

using namespace rack_fabric;

[[nodiscard]] MemberRecord make_node(const std::string& id, EvidenceProvenance provenance,
                                     MemberLifecycle lifecycle,
                                     std::chrono::milliseconds ttl = std::chrono::milliseconds{0},
                                     Durability durability = Durability::Durable) {
  MemberRecord record;
  record.key = *MemberKey::parse(MemberKind::Node, id);
  record.lifecycle = lifecycle;
  record.provenance = provenance;
  record.observed_at = provenance == EvidenceProvenance::Unknown ? Timestamp::unknown()
                                                                 : Timestamp::from_unix_millis(1000);
  record.ttl = ttl;
  record.durability = durability;
  NodeDetails details;
  details.host_name = id;
  details.role = NodeRole::Compute;
  record.details = details;
  return record;
}

[[nodiscard]] AuthorityToken operator_authority(const RackFabric& fabric) {
  AuthorityToken authority;
  authority.coordinator_epoch = fabric.coordinator_epoch();
  return authority;
}

void declare(RackFabric& fabric, const std::string& rack_id, const std::string& epoch) {
  DeclareRackRequest request;
  request.authority = operator_authority(fabric);
  request.authority.rack = *RackId::parse(rack_id);
  request.epoch = *RackEpochId::parse(epoch);
  const MutationResult result = fabric.declare_rack(request);
  RF_CHECK(result.accepted());
}

[[nodiscard]] AuthorityToken rack_authority(const RackFabric& fabric, const std::string& rack_id) {
  AuthorityToken authority = operator_authority(fabric);
  authority.rack = *RackId::parse(rack_id);
  return authority;
}

}  // namespace

RF_TEST(identity, validation_rules) {
  RF_CHECK(validate_identity("node-01").ok());
  RF_CHECK(validate_identity("node_01.example:9000").ok());
  RF_CHECK(validate_identity("a+b@c").ok());
  RF_CHECK(!validate_identity("").ok());
  RF_CHECK(!validate_identity(".").ok());
  RF_CHECK(!validate_identity("..").ok());
  RF_CHECK(!validate_identity("has space").ok());
  RF_CHECK(!validate_identity("slash/path").ok());
  RF_CHECK(!validate_identity("back\\slash").ok());
  RF_CHECK(!validate_identity(std::string(kMaxIdentityLength + 1, 'a')).ok());
  RF_CHECK(validate_identity(std::string(kMaxIdentityLength, 'a')).ok());
  RF_CHECK(!validate_identity("semi;colon").ok());
  // An embedded NUL must be rejected. The literal has to carry its length:
  // a bare string literal would be truncated at the NUL by the caller.
  const char embedded_nul[] = {'n', 'u', 'l', '\0', 'b', 'y', 't', 'e'};
  RF_CHECK(!validate_identity(std::string_view{embedded_nul, sizeof(embedded_nul)}).ok());
}

RF_TEST(identity, strong_ids_are_distinct_and_hashable) {
  const auto node = NodeId::parse("node-01");
  RF_REQUIRE(node.has_value());
  const auto other = NodeId::parse("node-02");
  RF_REQUIRE(other.has_value());
  RF_CHECK_NE(*node, *other);
  RF_CHECK_EQ(node->value(), std::string("node-01"));
  RF_CHECK(!NodeId::parse("bad id").has_value());
  std::unordered_set<NodeId> set;
  set.insert(*node);
  set.insert(*other);
  set.insert(*node);
  RF_CHECK_EQ(set.size(), std::size_t{2});
  const auto worker_boot = AgentBootId::parse("boot-1");
  RF_REQUIRE(worker_boot.has_value());
  const WorkerBootId alias = *worker_boot;
  RF_CHECK_EQ(alias.value(), std::string("boot-1"));
}

RF_TEST(generation, arithmetic_is_checked) {
  MemberGeneration generation;
  RF_CHECK(generation.is_unset());
  RF_CHECK(!generation.is_set());
  generation = MemberGeneration::first();
  RF_CHECK_EQ(generation.value(), std::uint64_t{1});
  const auto next = generation.next();
  RF_REQUIRE(next.has_value());
  RF_CHECK_EQ(next->value(), std::uint64_t{2});
  MemberGeneration maximum = MemberGeneration::from_value(std::numeric_limits<std::uint64_t>::max());
  RF_CHECK(!maximum.next().has_value());
  RF_CHECK(MemberGeneration::from_value(7) > MemberGeneration::from_value(6));
  RF_CHECK(MemberGeneration::from_value(0).is_unset());
}

RF_TEST(evidence, unknown_is_first_class) {
  EvidenceValue<HealthState> health;
  RF_CHECK(!health.has_evidence());
  RF_CHECK_EQ(health.freshness_at(Timestamp::from_unix_millis(1000)), Freshness::Unknown);
  RF_CHECK(!health.is_current_at(Timestamp::from_unix_millis(1000)));

  EvidenceValue<HealthState> durable;
  durable.value = HealthState::Healthy;
  durable.provenance = EvidenceProvenance::Measured;
  durable.observed_at = Timestamp::from_unix_millis(1000);
  durable.ttl = std::chrono::milliseconds{0};
  durable.durability = Durability::Durable;
  RF_CHECK_EQ(durable.freshness_at(Timestamp::from_unix_millis(999999)), Freshness::Fresh);

  EvidenceValue<HealthState> ephemeral_without_ttl = durable;
  ephemeral_without_ttl.durability = Durability::Ephemeral;
  RF_CHECK_EQ(ephemeral_without_ttl.freshness_at(Timestamp::from_unix_millis(1000)),
              Freshness::Stale);

  EvidenceValue<HealthState> ephemeral = durable;
  ephemeral.durability = Durability::Ephemeral;
  ephemeral.ttl = std::chrono::milliseconds{100};
  RF_CHECK_EQ(ephemeral.freshness_at(Timestamp::from_unix_millis(1050)), Freshness::Fresh);
  RF_CHECK_EQ(ephemeral.freshness_at(Timestamp::from_unix_millis(1200)), Freshness::Stale);
  RF_CHECK_EQ(ephemeral.freshness_at(Timestamp::from_unix_millis(900)), Freshness::Stale);

  EvidenceValue<HealthState> future = ephemeral;
  future.observed_at = Timestamp::from_unix_millis(5000);
  RF_CHECK_EQ(future.freshness_at(Timestamp::from_unix_millis(1000)), Freshness::Stale);
}

RF_TEST(evidence, revalidation_overrides_everything) {
  EvidenceValue<HealthState> health;
  health.value = HealthState::Healthy;
  health.provenance = EvidenceProvenance::Measured;
  health.observed_at = Timestamp::from_unix_millis(1000);
  health.ttl = std::chrono::milliseconds{0};
  health.durability = Durability::Durable;
  health.revalidation_required = true;
  RF_CHECK_EQ(health.freshness_at(Timestamp::from_unix_millis(1000)),
              Freshness::RevalidationRequired);
  RF_CHECK(!health.is_current_at(Timestamp::from_unix_millis(1000)));
}

RF_TEST(evidence, provenance_classes_are_not_interchangeable) {
  RF_CHECK(is_physical_provenance(EvidenceProvenance::Measured));
  RF_CHECK(is_physical_provenance(EvidenceProvenance::Reported));
  RF_CHECK(!is_physical_provenance(EvidenceProvenance::Synthetic));
  // DERIVED is physical: it is computed from physical evidence, so it may
  // satisfy a physical readiness contract. ESTIMATED and SYNTHETIC may not.
  RF_CHECK(is_physical_provenance(EvidenceProvenance::Derived));
  RF_CHECK(!is_physical_provenance(EvidenceProvenance::Estimated));
  RF_CHECK(!is_physical_provenance(EvidenceProvenance::Reconstructed));
  RF_CHECK(!is_physical_provenance(EvidenceProvenance::Unknown));
  RF_CHECK_EQ(to_string(EvidenceProvenance::Reconstructed), std::string_view("RECONSTRUCTED"));
  RF_CHECK_EQ(to_string(EvidenceProvenance::Synthetic), std::string_view("SYNTHETIC"));
}

RF_TEST(lifecycle, undeclared_then_declared_then_ready) {
  RackFabric fabric;
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Undeclared);
  RF_CHECK(!fabric.rack_id().has_value());
  declare(fabric, "rack-a", "epoch-1");
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Declared);
  RF_CHECK_EQ(fabric.rack_id()->value(), std::string("rack-a"));

  ReadinessContract contract;
  contract.min_nodes = 2;
  contract.require_power_envelope = false;
  contract.require_cooling_envelope = false;
  RackFabricOptions options;
  options.readiness = contract;
  RackFabric strict(options);
  declare(strict, "rack-a", "epoch-1");
  const AuthorityToken authority = rack_authority(strict, "rack-a");

  PublishMemberRequest first;
  first.authority = authority;
  first.record = make_node("node-1", EvidenceProvenance::Measured, MemberLifecycle::Present);
  RF_CHECK(strict.publish_member(first).accepted());
  RF_CHECK_EQ(strict.lifecycle(), RackLifecycle::Partial);
  RF_CHECK(!strict.evaluate_readiness().satisfied);

  PublishMemberRequest second;
  second.authority = authority;
  second.record = make_node("node-2", EvidenceProvenance::Measured, MemberLifecycle::Present);
  RF_CHECK(strict.publish_member(second).accepted());
  RF_CHECK_EQ(strict.lifecycle(), RackLifecycle::Ready);
  RF_CHECK(strict.evaluate_readiness().satisfied);
}

RF_TEST(lifecycle, declared_but_unknown_evidence_is_not_present) {
  RackFabric fabric;
  declare(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rack_authority(fabric, "rack-a");
  PublishMemberRequest request;
  request.authority = authority;
  request.record = make_node("node-1", EvidenceProvenance::Unknown, MemberLifecycle::Declared);
  const MutationResult result = fabric.publish_member(request);
  RF_CHECK(result.accepted());
  const auto stored = fabric.find_member(*MemberKey::parse(MemberKind::Node, "node-1"));
  RF_REQUIRE(stored.has_value());
  RF_CHECK_EQ(stored->lifecycle, MemberLifecycle::Declared);
  RF_CHECK_EQ(stored->provenance, EvidenceProvenance::Unknown);
  RF_CHECK(!stored->is_authoritatively_current_at(default_clock().now()));
  RF_CHECK_EQ(fabric.lifecycle(), RackLifecycle::Discovering);
  const Explanation explanation = fabric.explain_member(stored->key);
  RF_CHECK(!explanation.ok);
  RF_CHECK_EQ(explanation.code, std::string("MEMBER_NOT_CURRENT"));
}

RF_TEST(lifecycle, present_without_evidence_is_rejected) {
  RackFabric fabric;
  declare(fabric, "rack-a", "epoch-1");
  PublishMemberRequest request;
  request.authority = rack_authority(fabric, "rack-a");
  request.record = make_node("node-1", EvidenceProvenance::Unknown, MemberLifecycle::Present);
  const MutationResult result = fabric.publish_member(request);
  RF_CHECK_EQ(result.outcome, MutationOutcome::RejectInvalidInput);
  RF_CHECK_EQ(result.explanation.code, std::string("PRESENT_WITHOUT_EVIDENCE"));
  RF_CHECK(!fabric.find_member(*MemberKey::parse(MemberKind::Node, "node-1")).has_value());
}

RF_TEST(lifecycle, evidence_without_provenance_or_time_is_rejected) {
  RackFabric fabric;
  declare(fabric, "rack-a", "epoch-1");
  {
    PublishMemberRequest request;
    request.authority = rack_authority(fabric, "rack-a");
    request.record = make_node("node-1", EvidenceProvenance::Measured, MemberLifecycle::Present);
    request.record.observed_at = Timestamp::unknown();
    const MutationResult result = fabric.publish_member(request);
    RF_CHECK_EQ(result.outcome, MutationOutcome::RejectInvalidInput);
    RF_CHECK_EQ(result.explanation.code, std::string("EVIDENCE_WITHOUT_OBSERVATION_TIME"));
  }
  {
    PublishMemberRequest request;
    request.authority = rack_authority(fabric, "rack-a");
    request.record = make_node("node-2", EvidenceProvenance::Unknown, MemberLifecycle::Declared);
    request.record.observed_at = Timestamp::from_unix_millis(1000);
    const MutationResult result = fabric.publish_member(request);
    RF_CHECK_EQ(result.outcome, MutationOutcome::RejectInvalidInput);
    RF_CHECK_EQ(result.explanation.code, std::string("OBSERVATION_TIME_WITHOUT_PROVENANCE"));
  }
  {
    PublishMemberRequest request;
    request.authority = rack_authority(fabric, "rack-a");
    request.record = make_node("node-3", EvidenceProvenance::Measured, MemberLifecycle::Present);
    request.record.durability = Durability::Ephemeral;
    request.record.ttl = std::chrono::milliseconds{0};
    const MutationResult result = fabric.publish_member(request);
    RF_CHECK_EQ(result.outcome, MutationOutcome::RejectInvalidInput);
    RF_CHECK_EQ(result.explanation.code, std::string("EPHEMERAL_EVIDENCE_REQUIRES_TTL"));
  }
  {
    PublishMemberRequest request;
    request.authority = rack_authority(fabric, "rack-a");
    request.record = make_node("node-4", EvidenceProvenance::Measured, MemberLifecycle::Present);
    request.record.ttl = std::chrono::milliseconds{100'000'000};
    const MutationResult result = fabric.publish_member(request);
    RF_CHECK_EQ(result.outcome, MutationOutcome::RejectInvalidInput);
    RF_CHECK_EQ(result.explanation.code, std::string("TTL_OUT_OF_RANGE"));
  }
}

RF_TEST(readiness, contract_deficits_are_reported) {
  RackFabricOptions options;
  options.readiness.min_nodes = 3;
  options.readiness.min_accelerators = 2;
  options.readiness.min_distinct_failure_domains = 2;
  options.readiness.require_topology = true;
  options.readiness.require_physical_provenance = true;
  RackFabric fabric(options);
  declare(fabric, "rack-a", "epoch-1");
  const ReadinessEvaluation evaluation = fabric.evaluate_readiness();
  RF_CHECK(!evaluation.satisfied);
  RF_CHECK(!evaluation.deficits.empty());
  const Explanation explanation = fabric.explain_readiness();
  RF_CHECK(!explanation.ok);
  RF_CHECK_EQ(explanation.code, std::string("RACK_NOT_READY"));
  RF_CHECK(!explanation.factors.empty());
}

RF_TEST(invariants, empty_instance_is_consistent) {
  RackFabric fabric;
  const InvariantReport report = fabric.check_invariants();
  RF_CHECK(report.ok());
  RF_CHECK(report.checks_run > 0);
  RF_CHECK_EQ(report.violations.size(), std::size_t{0});
}

RF_TEST(summary, digest_is_stable_and_changes_with_state) {
  RackFabric fabric;
  declare(fabric, "rack-a", "epoch-1");
  const RackSummary first = fabric.summary();
  const RackSummary second = fabric.summary();
  RF_CHECK_EQ(first.digest, second.digest);
  RF_CHECK_EQ(first.member_count, std::uint64_t{0});

  PublishMemberRequest request;
  request.authority = rack_authority(fabric, "rack-a");
  request.record = make_node("node-1", EvidenceProvenance::Measured, MemberLifecycle::Present);
  RF_CHECK(fabric.publish_member(request).accepted());
  const RackSummary third = fabric.summary();
  RF_CHECK_NE(first.digest, third.digest);
  RF_CHECK_EQ(third.member_count, std::uint64_t{1});
  RF_CHECK_EQ(third.node_count, std::uint64_t{1});
  RF_CHECK_EQ(third.present_member_count, std::uint64_t{1});
}
