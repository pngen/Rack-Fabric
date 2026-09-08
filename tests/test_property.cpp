// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Property tests with fixed seeds. Every seed is printed in the failure
// message so any failure is exactly reproducible.

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "rack_fabric/rack_fabric.hpp"
#include "rack_fabric/synthetic.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::member;
using rf_test::observed;

constexpr std::uint64_t kSeeds[] = {1, 2, 3, 17, 12345, 999983, 0x5EED1234ULL, 0xFFFFFFFFULL};

[[nodiscard]] MemberKind random_kind(std::mt19937_64& generator) {
  std::uniform_int_distribution<int> distribution(0, 9);
  return static_cast<MemberKind>(distribution(generator));
}

}  // namespace

RF_TEST(property, random_mutation_sequences_preserve_invariants) {
  for (const std::uint64_t seed : kSeeds) {
    std::mt19937_64 generator(seed);
    std::uniform_int_distribution<int> action_distribution(0, 8);
    std::uniform_int_distribution<int> id_distribution(0, 11);
    std::uniform_int_distribution<int> lifecycle_distribution(0, 6);

    RackFabric fabric;
    rf_test::declare_rack(fabric, "rack-a", "epoch-1");
    rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                     "boot-1");
    AuthorityToken token = publisher.token;
    std::vector<MemberKey> known;
    std::vector<FailureDomainId> domains;

    for (int step = 0; step < 400; ++step) {
      const int action = action_distribution(generator);
      const std::string id = "member-" + std::to_string(id_distribution(generator));
      MutationResult result;
      switch (action) {
        case 0: {
          PublishFailureDomainRequest request;
          request.authority = token;
          request.record = rf_test::failure_domain("fd-" + id, FailureDomainKind::Node);
          result = fabric.publish_failure_domain(request);
          if (result.accepted()) {
            domains.push_back(FailureDomainId{"fd-" + id});
          }
          break;
        }
        case 1:
        case 2:
        case 3: {
          const MemberKind kind = random_kind(generator);
          const MemberLifecycle lifecycle =
              static_cast<MemberLifecycle>(lifecycle_distribution(generator));
          if (lifecycle == MemberLifecycle::Unknown) {
            break;
          }
          MemberRecord record = member(kind, id, EvidenceProvenance::Measured, lifecycle);
          if (!domains.empty() && kind != MemberKind::PowerDomain &&
              kind != MemberKind::CoolingDomain) {
            record.failure_domains.push_back(domains[generator() % domains.size()]);
          }
          if (kind != MemberKind::Node && kind != MemberKind::PowerDomain &&
              kind != MemberKind::CoolingDomain && !known.empty()) {
            record.parent = known[generator() % known.size()];
          }
          if (kind == MemberKind::Node && !known.empty()) {
            record.power_domain = PowerDomainId{"pdu-0"};
          }
          PublishMemberRequest request;
          request.authority = token;
          request.record = record;
          result = fabric.publish_member(request);
          if (result.accepted()) {
            known.push_back(record.key);
            token.publication_generation = PublicationGeneration::from_value(
                token.publication_generation->value() + 1);
          }
          break;
        }
        case 4: {
          if (known.size() < 2) {
            break;
          }
          PublishRelationshipRequest request;
          request.authority = token;
          request.record.key = RelationshipKey::canonicalize(
              RelationshipClass::ConnectedTo, known[generator() % known.size()],
              known[generator() % known.size()]);
          request.record.provenance = EvidenceProvenance::Measured;
          request.record.observed_at = observed();
          result = fabric.publish_relationship(request);
          if (result.accepted()) {
            token.publication_generation = PublicationGeneration::from_value(
                token.publication_generation->value() + 1);
          }
          break;
        }
        case 5: {
          if (known.empty()) {
            break;
          }
          WithdrawEvidenceRequest request;
          request.authority = token;
          request.member = known[generator() % known.size()];
          request.scope = static_cast<WithdrawScope>(generator() % 6);
          result = fabric.withdraw_evidence(request);
          break;
        }
        case 6: {
          if (known.empty()) {
            break;
          }
          RetireMemberRequest request;
          request.authority = token;
          request.member = known[generator() % known.size()];
          result = fabric.retire_member(request);
          break;
        }
        case 7: {
          if (known.empty()) {
            break;
          }
          MarkUnavailableRequest request;
          request.authority = token;
          request.member = known[generator() % known.size()];
          request.reason = RevalidationReason::OwnerProcessLost;
          result = fabric.mark_unavailable(request);
          break;
        }
        default: {
          PublishPowerEnvelopeRequest request;
          request.authority = token;
          request.record.provenance = EvidenceProvenance::Measured;
          request.record.observed_at = observed();
          Quantity limit;
          limit.value = static_cast<double>(1000 + (generator() % 9000));
          limit.unit = QuantityUnit::Watts;
          limit.provenance = EvidenceProvenance::Measured;
          limit.observed_at = observed();
          request.record.rack_limit = limit;
          result = fabric.publish_power_envelope(request);
          if (result.accepted()) {
            token.publication_generation = PublicationGeneration::from_value(
                token.publication_generation->value() + 1);
          }
          break;
        }
      }
      (void)result;
      const InvariantReport report = fabric.check_invariants();
      if (!report.ok()) {
        rftest::record_failure(__FILE__, __LINE__,
                               "invariant violation at seed " + std::to_string(seed) + " step " +
                                   std::to_string(step) + ": " + report.render());
        break;
      }
      // The summary must always be internally consistent.
      const RackSummary summary = fabric.summary();
      RF_CHECK(summary.present_member_count <= summary.member_count);
      RF_CHECK(summary.stale_member_count <= summary.member_count);
      RF_CHECK(summary.retired_member_count <= summary.member_count);
      RF_CHECK(summary.active_publisher_count <= summary.publisher_count);
      // UNKNOWN evidence is never counted as present.
      for (const MemberRecord& record : fabric.members()) {
        if (record.provenance == EvidenceProvenance::Unknown &&
            record.lifecycle == MemberLifecycle::Present) {
          RF_CHECK(false);
          break;
        }
      }
    }
  }
}

RF_TEST(property, generations_never_move_backwards) {
  for (const std::uint64_t seed : kSeeds) {
    std::mt19937_64 generator(seed);
    RackFabric fabric;
    rf_test::declare_rack(fabric, "rack-a", "epoch-1");
    const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
    GenerationSet previous = fabric.generations();
    for (int step = 0; step < 200; ++step) {
      const std::string id = "node-" + std::to_string(generator() % 16);
      PublishMemberRequest request;
      request.authority = authority;
      request.record = member(MemberKind::Node, id, EvidenceProvenance::Measured,
                              static_cast<MemberLifecycle>(1 + generator() % 5));
      (void)fabric.publish_member(request);
      const GenerationSet current = fabric.generations();
      RF_CHECK(current.rack.value() >= previous.rack.value());
      RF_CHECK(current.membership.value() >= previous.membership.value());
      RF_CHECK(current.topology.value() >= previous.topology.value());
      RF_CHECK(current.failure_domains.value() >= previous.failure_domains.value());
      RF_CHECK(current.constraints.value() >= previous.constraints.value());
      RF_CHECK(current.power.value() >= previous.power.value());
      RF_CHECK(current.cooling.value() >= previous.cooling.value());
      RF_CHECK(current.health.value() >= previous.health.value());
      RF_CHECK(current.capabilities.value() >= previous.capabilities.value());
      RF_CHECK(current.coordinator_epoch.value() >= previous.coordinator_epoch.value());
      previous = current;
    }
  }
}

RF_TEST(property, a_fresh_snapshot_is_always_current) {
  for (const std::uint64_t seed : kSeeds) {
    SyntheticRackConfig config;
    config.seed = seed;
    config.node_count = 6;
    config.max_accelerators_per_node = 3;
    config.synthetic_accelerator_peer_links = true;
    RackFabric fabric;
    const SyntheticRack rack = generate_synthetic_rack(config);
    const AuthorityToken authority = rf_test::operator_token(fabric, rack.rack.value());
    const SyntheticPublishSummary summary = publish_synthetic_rack(fabric, rack, authority);
    RF_CHECK_EQ(summary.rejections.size(), std::size_t{0});

    SnapshotRequest request;
    request.authority = authority;
    const SnapshotResult snapshot = fabric.publish_snapshot(request);
    RF_REQUIRE(snapshot.ok());
    RF_CHECK_EQ(fabric.validate_snapshot(*snapshot.snapshot).status,
                SnapshotValidationStatus::Current);
    RF_CHECK_EQ(snapshot.snapshot->members().size(), fabric.summary().member_count);

    // Any mutation of the same instance makes the snapshot stale, never
    // silently current.
    PublishMemberRequest mutation;
    mutation.authority = authority;
    mutation.record = member(MemberKind::Node, "extra-node");
    RF_REQUIRE(fabric.publish_member(mutation).accepted());
    RF_CHECK_EQ(fabric.validate_snapshot(*snapshot.snapshot).status,
                SnapshotValidationStatus::Stale);
  }
}

RF_TEST(property, identical_operation_sequences_produce_identical_digests) {
  const auto run = [](std::uint64_t seed) {
    std::mt19937_64 generator(seed);
    RackFabric fabric;
    rf_test::declare_rack(fabric, "rack-a", "epoch-1");
    const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
    std::string trace;
    for (int step = 0; step < 100; ++step) {
      PublishMemberRequest request;
      request.authority = authority;
      request.record = member(MemberKind::Node, "node-" + std::to_string(generator() % 24));
      const MutationResult result = fabric.publish_member(request);
      trace.append(to_string(result.outcome));
      trace.append(":");
      trace.append(result.explanation.subject);
      trace.append(";");
    }
    return std::make_pair(fabric.summary().digest, trace);
  };
  for (const std::uint64_t seed : kSeeds) {
    const auto first = run(seed);
    const auto second = run(seed);
    RF_CHECK_EQ(first.first, second.first);
    RF_CHECK_EQ(first.second, second.second);
  }
  // Different seeds touch different identities in a different order, so the
  // recorded outcome trace must differ even though the final summary may not.
  RF_CHECK_NE(run(1).second, run(2).second);
}

RF_TEST(property, synthetic_racks_are_deterministic_and_never_physical) {
  for (const std::uint64_t seed : kSeeds) {
    SyntheticRackConfig config;
    config.seed = seed;
    config.node_count = 5;
    config.synthetic_accelerator_peer_links = true;
    const SyntheticRack first = generate_synthetic_rack(config);
    const SyntheticRack second = generate_synthetic_rack(config);
    RF_CHECK_EQ(first.members.size(), second.members.size());
    RF_CHECK_EQ(first.relationships.size(), second.relationships.size());
    RF_CHECK_EQ(first.failure_domains.size(), second.failure_domains.size());
    RF_CHECK_EQ(first.reproduction_parameters, second.reproduction_parameters);
    for (const MemberRecord& record : first.members) {
      // A synthetic rack declares identities it has no observation about, so
      // UNKNOWN is expected for those; nothing may claim physical provenance.
      RF_CHECK(record.provenance == EvidenceProvenance::Synthetic ||
               record.provenance == EvidenceProvenance::Unknown);
      RF_CHECK(!is_physical_provenance(record.provenance));
      RF_CHECK(record.key.id != std::string());
    }
    for (const RelationshipRecord& record : first.relationships) {
      RF_CHECK_EQ(record.provenance, EvidenceProvenance::Synthetic);
    }
    for (const FailureDomainRecord& record : first.failure_domains) {
      RF_CHECK_EQ(record.provenance, EvidenceProvenance::Synthetic);
    }

    // A contract that requires physical provenance can never be satisfied by
    // a synthetic rack.
    RackFabricOptions options;
    options.readiness.require_physical_provenance = true;
    options.readiness.require_power_envelope = false;
    options.readiness.require_cooling_envelope = false;
    RackFabric fabric(options);
    const AuthorityToken authority = rf_test::operator_token(fabric, first.rack.value());
    (void)publish_synthetic_rack(fabric, first, authority);
    RF_CHECK(!fabric.evaluate_readiness().satisfied);
    RF_CHECK_NE(fabric.lifecycle(), RackLifecycle::Ready);
  }
}
