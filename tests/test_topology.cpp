// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <string>
#include <vector>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::member;
using rf_test::observed;
using rf_test::publish_member;

[[nodiscard]] RelationshipRecord relationship(RelationshipClass cls, const MemberKey& from,
                                              const MemberKey& to) {
  RelationshipRecord record;
  record.key = RelationshipKey::canonicalize(cls, from, to);
  record.provenance = EvidenceProvenance::Measured;
  record.observed_at = observed();
  record.ttl = std::chrono::milliseconds{0};
  record.durability = Durability::Durable;
  return record;
}

inline void publish_relationship(RackFabric& fabric, const AuthorityToken& authority,
                                 const RelationshipRecord& record) {
  PublishRelationshipRequest request;
  request.authority = authority;
  request.record = record;
  const MutationResult result = fabric.publish_relationship(request);
  RF_REQUIRE(result.accepted());
}

}  // namespace

RF_TEST(topology, symmetric_classes_are_canonicalized) {
  const MemberKey a{MemberKind::Node, "node-a"};
  const MemberKey b{MemberKind::Node, "node-b"};
  const RelationshipKey forward = RelationshipKey::canonicalize(RelationshipClass::ConnectedTo, a, b);
  const RelationshipKey reverse = RelationshipKey::canonicalize(RelationshipClass::ConnectedTo, b, a);
  RF_CHECK_EQ(forward, reverse);
  RF_CHECK_EQ(forward.from, a);
  RF_CHECK_EQ(forward.to, b);

  // Directed classes keep their direction.
  const RelationshipKey contains = RelationshipKey::canonicalize(RelationshipClass::Contains, a, b);
  const RelationshipKey contains_reverse = RelationshipKey::canonicalize(RelationshipClass::Contains,
                                                                         b, a);
  RF_CHECK_NE(contains, contains_reverse);
  RF_CHECK(is_symmetric(RelationshipClass::AcceleratorPeer));
  RF_CHECK(!is_symmetric(RelationshipClass::Contains));
  RF_CHECK(!is_symmetric(RelationshipClass::ReachableThrough));
}

RF_TEST(topology, publish_requires_existing_endpoints) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));

  PublishRelationshipRequest request;
  request.authority = authority;
  request.record = relationship(RelationshipClass::ConnectedTo, MemberKey{MemberKind::Node, "node-1"},
                                MemberKey{MemberKind::Switch, "switch-1"});
  const MutationResult unknown = fabric.publish_relationship(request);
  RF_CHECK_EQ(unknown.outcome, MutationOutcome::RejectUnknownParent);
  RF_CHECK_EQ(unknown.explanation.code, std::string("UNKNOWN_RELATIONSHIP_ENDPOINT"));

  request.record = relationship(RelationshipClass::ConnectedTo, MemberKey{MemberKind::Node, "node-1"},
                                MemberKey{MemberKind::Node, "node-1"});
  const MutationResult self = fabric.publish_relationship(request);
  RF_CHECK_EQ(self.outcome, MutationOutcome::RejectInvalidRelationship);
  RF_CHECK_EQ(self.explanation.code, std::string("RELATIONSHIP_SELF_LINK"));
}

RF_TEST(topology, containment_edges_are_acyclic) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  publish_member(fabric, authority, member(MemberKind::Node, "node-2"));
  publish_relationship(fabric, authority,
                       relationship(RelationshipClass::Contains, MemberKey{MemberKind::Node, "node-1"},
                                    MemberKey{MemberKind::Node, "node-2"}));
  PublishRelationshipRequest request;
  request.authority = authority;
  request.record = relationship(RelationshipClass::Contains, MemberKey{MemberKind::Node, "node-2"},
                                MemberKey{MemberKind::Node, "node-1"});
  const MutationResult cycle = fabric.publish_relationship(request);
  RF_CHECK_EQ(cycle.outcome, MutationOutcome::RejectInvalidRelationship);
  RF_CHECK_EQ(cycle.explanation.code, std::string("CONTAINMENT_CYCLE"));
  RF_CHECK(fabric.check_invariants().ok());
}

RF_TEST(topology, republishing_identical_relationship_is_a_no_change) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  publish_member(fabric, authority, member(MemberKind::Node, "node-2"));
  const RelationshipRecord record = relationship(RelationshipClass::ConnectedTo,
                                                 MemberKey{MemberKind::Node, "node-1"},
                                                 MemberKey{MemberKind::Node, "node-2"});
  publish_relationship(fabric, authority, record);
  const TopologyGeneration after_first = fabric.generations().topology;

  PublishRelationshipRequest repeat;
  repeat.authority = authority;
  repeat.record = record;
  RF_CHECK_EQ(fabric.publish_relationship(repeat).outcome, MutationOutcome::NoChange);

  // The same edge published in the opposite order is the same edge.
  repeat.record = relationship(RelationshipClass::ConnectedTo, MemberKey{MemberKind::Node, "node-2"},
                               MemberKey{MemberKind::Node, "node-1"});
  RF_CHECK_EQ(fabric.publish_relationship(repeat).outcome, MutationOutcome::NoChange);
  RF_CHECK_EQ(fabric.generations().topology.value(), after_first.value());
  RF_CHECK_EQ(fabric.relationships().size(), std::size_t{1});
}

RF_TEST(topology, relationship_content_changes_advance_the_generation) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  publish_member(fabric, authority, member(MemberKind::Node, "node-2"));
  RelationshipRecord record = relationship(RelationshipClass::ConnectedTo,
                                           MemberKey{MemberKind::Node, "node-1"},
                                           MemberKey{MemberKind::Node, "node-2"});
  publish_relationship(fabric, authority, record);

  PublishRelationshipRequest request;
  request.authority = authority;
  request.record = record;
  request.record.link = LinkId{"link-1"};
  Quantity capacity;
  capacity.value = 400.0;
  capacity.unit = QuantityUnit::GigabitsPerSecond;
  capacity.provenance = EvidenceProvenance::Measured;
  capacity.observed_at = observed();
  request.record.capacity = capacity;
  const MutationResult updated = fabric.publish_relationship(request);
  RF_CHECK(updated.accepted());
  const RelationshipRecord stored = *fabric.find_relationship(record.key);
  RF_CHECK(stored.link.has_value());
  RF_CHECK_EQ(stored.link->value(), std::string("link-1"));
  RF_CHECK(stored.capacity.has_value());
  RF_CHECK_EQ(stored.capacity->unit, QuantityUnit::GigabitsPerSecond);
  RF_CHECK_EQ(stored.generation.value(), std::uint64_t{2});

  // A capacity with no unit is not a measurement and is refused.
  request.record.capacity->unit = QuantityUnit::None;
  const MutationResult unitless = fabric.publish_relationship(request);
  RF_CHECK_EQ(unitless.outcome, MutationOutcome::RejectInvalidInput);
  RF_CHECK_EQ(unitless.explanation.code, std::string("QUANTITY_WITHOUT_UNIT"));
}

RF_TEST(topology, traversal_and_withdrawal) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  for (int index = 0; index < 3; ++index) {
    publish_member(fabric, authority, member(MemberKind::Node, "node-" + std::to_string(index)));
  }
  publish_relationship(fabric, authority,
                       relationship(RelationshipClass::ConnectedTo, MemberKey{MemberKind::Node, "node-0"},
                                    MemberKey{MemberKind::Node, "node-1"}));
  publish_relationship(fabric, authority,
                       relationship(RelationshipClass::ReachableThrough,
                                    MemberKey{MemberKind::Node, "node-0"},
                                    MemberKey{MemberKind::Node, "node-2"}));
  RF_CHECK_EQ(fabric.relationships_touching(MemberKey{MemberKind::Node, "node-0"}).size(),
              std::size_t{2});
  RF_CHECK_EQ(fabric.relationships_touching(MemberKey{MemberKind::Node, "node-1"}).size(),
              std::size_t{1});
  RF_CHECK_EQ(fabric.relationships_touching(MemberKey{MemberKind::Node, "node-2"}).size(),
              std::size_t{1});

  WithdrawRelationshipRequest withdraw;
  withdraw.authority = authority;
  withdraw.key = RelationshipKey::canonicalize(RelationshipClass::ConnectedTo,
                                               MemberKey{MemberKind::Node, "node-0"},
                                               MemberKey{MemberKind::Node, "node-1"});
  RF_CHECK(fabric.withdraw_relationship(withdraw).accepted());
  RF_CHECK_EQ(fabric.relationships().size(), std::size_t{1});
  RF_CHECK_EQ(fabric.relationships_touching(MemberKey{MemberKind::Node, "node-1"}).size(),
              std::size_t{0});
  // Withdrawing an already-withdrawn relationship is a no-change, not an error.
  RF_CHECK_EQ(fabric.withdraw_relationship(withdraw).outcome, MutationOutcome::NoChange);
  RF_CHECK(fabric.check_invariants().ok());
}

RF_TEST(topology, relationship_limit_is_enforced) {
  RackFabricOptions options;
  options.limits.max_relationships = 1;
  RackFabric fabric(options);
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  for (int index = 0; index < 3; ++index) {
    publish_member(fabric, authority, member(MemberKind::Node, "node-" + std::to_string(index)));
  }
  publish_relationship(fabric, authority,
                       relationship(RelationshipClass::ConnectedTo, MemberKey{MemberKind::Node, "node-0"},
                                    MemberKey{MemberKind::Node, "node-1"}));
  PublishRelationshipRequest request;
  request.authority = authority;
  request.record = relationship(RelationshipClass::ConnectedTo,
                                MemberKey{MemberKind::Node, "node-1"},
                                MemberKey{MemberKind::Node, "node-2"});
  const MutationResult result = fabric.publish_relationship(request);
  RF_CHECK_EQ(result.outcome, MutationOutcome::RejectLimitExceeded);
  RF_CHECK_EQ(result.explanation.code, std::string("RELATIONSHIP_LIMIT_EXCEEDED"));
}

RF_TEST(topology, relationship_freshness_is_reported) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
  publish_member(fabric, authority, member(MemberKind::Node, "node-2"));
  RelationshipRecord record = relationship(RelationshipClass::ConnectedTo,
                                           MemberKey{MemberKind::Node, "node-1"},
                                           MemberKey{MemberKind::Node, "node-2"});
  record.durability = Durability::Ephemeral;
  record.ttl = std::chrono::milliseconds{500};
  publish_relationship(fabric, authority, record);
  const RelationshipRecord stored = *fabric.find_relationship(record.key);
  const Timestamp soon = Timestamp::from_unix_millis(rf_test::kObservationMillis + 100);
  const Timestamp later = Timestamp::from_unix_millis(rf_test::kObservationMillis + 900);
  RF_CHECK_EQ(stored.freshness_at(soon), Freshness::Fresh);
  RF_CHECK_EQ(stored.freshness_at(later), Freshness::Stale);
  RF_CHECK(!stored.is_current_at(later));
  RF_CHECK_EQ(stored.provenance, EvidenceProvenance::Measured);
}
