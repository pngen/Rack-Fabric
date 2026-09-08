// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <chrono>
#include <string>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::member;
using rf_test::observed;
using rf_test::publish_member;

[[nodiscard]] Quantity quantity(double value, QuantityUnit unit,
                                EvidenceProvenance provenance = EvidenceProvenance::Measured,
                                std::chrono::milliseconds ttl = std::chrono::milliseconds{0}) {
  Quantity quantity;
  quantity.value = value;
  quantity.unit = unit;
  quantity.provenance = provenance;
  quantity.observed_at = observed();
  quantity.ttl = ttl;
  quantity.durability = ttl.count() == 0 ? Durability::Durable : Durability::Ephemeral;
  return quantity;
}

}  // namespace

RF_TEST(envelopes, power_envelope_derives_headroom_only_from_comparable_evidence) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");

  PublishPowerEnvelopeRequest request;
  request.authority = authority;
  request.record.provenance = EvidenceProvenance::Measured;
  request.record.observed_at = observed();
  request.record.rack_limit = quantity(12000.0, QuantityUnit::Watts);
  request.record.rack_observed_draw = quantity(4500.0, QuantityUnit::Watts);
  const MutationResult result = fabric.publish_power_envelope(request);
  RF_REQUIRE(result.accepted());
  const PowerEnvelopeRecord stored = *fabric.power_envelope();
  RF_REQUIRE(stored.rack_headroom.has_value());
  RF_CHECK_EQ(stored.rack_headroom->value, 7500.0);
  RF_CHECK_EQ(stored.rack_headroom->unit, QuantityUnit::Watts);
  RF_CHECK_EQ(stored.rack_headroom->provenance, EvidenceProvenance::Derived);
  RF_CHECK_EQ(stored.generation.value(), std::uint64_t{1});

  // A draw in a different unit is not comparable: no headroom is produced.
  PublishPowerEnvelopeRequest mixed = request;
  mixed.record.rack_observed_draw = quantity(4500.0, QuantityUnit::Amperes);
  RF_REQUIRE(fabric.publish_power_envelope(mixed).accepted());
  RF_CHECK(!fabric.power_envelope()->rack_headroom.has_value());

  // A draw larger than the limit is not reported as negative headroom.
  PublishPowerEnvelopeRequest over = request;
  over.record.rack_observed_draw = quantity(20000.0, QuantityUnit::Watts);
  RF_REQUIRE(fabric.publish_power_envelope(over).accepted());
  RF_CHECK(!fabric.power_envelope()->rack_headroom.has_value());
}

RF_TEST(envelopes, power_quantities_are_validated) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");

  PublishPowerEnvelopeRequest request;
  request.authority = authority;
  request.record.provenance = EvidenceProvenance::Measured;
  request.record.observed_at = observed();
  request.record.rack_limit = quantity(12000.0, QuantityUnit::None);
  const MutationResult unitless = fabric.publish_power_envelope(request);
  RF_CHECK_EQ(unitless.outcome, MutationOutcome::RejectInvalidInput);
  RF_CHECK_EQ(unitless.explanation.code, std::string("QUANTITY_WITHOUT_UNIT"));

  request.record.rack_limit = quantity(-5.0, QuantityUnit::Watts);
  const MutationResult negative = fabric.publish_power_envelope(request);
  RF_CHECK_EQ(negative.outcome, MutationOutcome::RejectInvalidInput);
  RF_CHECK_EQ(negative.explanation.code, std::string("QUANTITY_OUT_OF_RANGE"));

  request.record.rack_limit = quantity(12000.0, QuantityUnit::Watts);
  request.record.provenance = EvidenceProvenance::Unknown;
  request.record.observed_at = observed();
  const MutationResult inconsistent = fabric.publish_power_envelope(request);
  RF_CHECK_EQ(inconsistent.outcome, MutationOutcome::RejectInvalidInput);
  RF_CHECK_EQ(inconsistent.explanation.code, std::string("OBSERVATION_TIME_WITHOUT_PROVENANCE"));
}

RF_TEST(envelopes, power_envelope_generation_is_monotonic) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  PublishPowerEnvelopeRequest request;
  request.authority = authority;
  request.record.provenance = EvidenceProvenance::Measured;
  request.record.observed_at = observed();
  request.record.rack_limit = quantity(12000.0, QuantityUnit::Watts);
  RF_REQUIRE(fabric.publish_power_envelope(request).accepted());
  RF_REQUIRE(fabric.publish_power_envelope(request).accepted());
  RF_CHECK_EQ(fabric.generations().power.value(), std::uint64_t{2});
  RF_CHECK_EQ(fabric.power_envelope()->generation.value(), std::uint64_t{2});

  request.expected_generation = PowerEnvelopeGeneration::from_value(1);
  const MutationResult stale = fabric.publish_power_envelope(request);
  RF_CHECK_EQ(stale.outcome, MutationOutcome::RejectStaleGeneration);
  RF_CHECK_EQ(stale.explanation.code, std::string("STALE_POWER_ENVELOPE_GENERATION"));

  request.expected_generation.reset();
  request.record.generation = PowerEnvelopeGeneration::from_value(1);
  const MutationResult older = fabric.publish_power_envelope(request);
  RF_CHECK_EQ(older.outcome, MutationOutcome::RejectStaleGeneration);
}

RF_TEST(envelopes, cooling_zone_must_be_a_cooling_domain_member) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");

  PublishCoolingEnvelopeRequest request;
  request.authority = authority;
  request.record.provenance = EvidenceProvenance::Measured;
  request.record.observed_at = observed();
  CoolingZoneRecord zone;
  zone.zone = CoolingDomainId{"cooling-0"};
  zone.design_thermal_limit = quantity(20000.0, QuantityUnit::Watts);
  zone.cooling_capacity = quantity(24000.0, QuantityUnit::Watts);
  zone.observed_temperature = quantity(24.5, QuantityUnit::Celsius);
  EvidenceValue<ThrottleState> throttling;
  throttling.value = ThrottleState::None;
  throttling.provenance = EvidenceProvenance::Measured;
  throttling.observed_at = observed();
  throttling.ttl = std::chrono::milliseconds{1000};
  throttling.durability = Durability::Ephemeral;
  zone.throttling = throttling;
  request.record.zones.push_back(zone);
  const MutationResult unknown = fabric.publish_cooling_envelope(request);
  RF_CHECK_EQ(unknown.outcome, MutationOutcome::RejectUnknownParent);
  RF_CHECK_EQ(unknown.explanation.code, std::string("UNKNOWN_COOLING_DOMAIN"));

  publish_member(fabric, authority, member(MemberKind::CoolingDomain, "cooling-0"));
  RF_REQUIRE(fabric.publish_cooling_envelope(request).accepted());
  const CoolingEnvelopeRecord stored = *fabric.cooling_envelope();
  RF_REQUIRE(stored.zones.size() == 1);
  RF_REQUIRE(stored.zones[0].thermal_headroom.has_value());
  RF_CHECK_EQ(stored.zones[0].thermal_headroom->value, 4000.0);
  RF_CHECK_EQ(stored.zones[0].thermal_headroom->provenance, EvidenceProvenance::Derived);
  RF_CHECK(stored.zones[0].throttling.has_value());
  RF_CHECK_EQ(stored.zones[0].throttling->value, ThrottleState::None);

  // Throttling evidence without a lifetime cannot be current.
  CoolingEnvelopeRecord ephemeral = *fabric.cooling_envelope();
  ephemeral.zones[0].throttling->ttl = std::chrono::milliseconds{0};
  ephemeral.zones[0].throttling->durability = Durability::Ephemeral;
  request.record = ephemeral;
  const MutationResult invalid = fabric.publish_cooling_envelope(request);
  RF_CHECK_EQ(invalid.outcome, MutationOutcome::RejectInvalidInput);
  RF_CHECK_EQ(invalid.explanation.code, std::string("EPHEMERAL_EVIDENCE_REQUIRES_TTL"));
}

RF_TEST(envelopes, envelope_ownership_follows_authority) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  PublishPowerEnvelopeRequest request;
  request.authority = publisher.token;
  request.record.provenance = EvidenceProvenance::Measured;
  request.record.observed_at = observed();
  request.record.rack_limit = quantity(9000.0, QuantityUnit::Watts);
  RF_REQUIRE(fabric.publish_power_envelope(request).accepted());
  const PowerEnvelopeRecord stored = *fabric.power_envelope();
  RF_REQUIRE(stored.owner_boot.has_value());
  RF_CHECK_EQ(stored.owner_boot->value(), publisher.boot.value());
  RF_REQUIRE(stored.owner_worker.has_value());
  RF_CHECK_EQ(stored.owner_worker->value(), publisher.worker.value());
  RF_CHECK_EQ(fabric.generations().constraints.value() >= 1, true);
}

RF_TEST(envelopes, health_and_capability_evidence_are_member_scoped) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
  publish_member(fabric, authority, member(MemberKind::Node, "node-1"));

  PublishHealthRequest health;
  health.authority = authority;
  health.member = MemberKey{MemberKind::Node, "node-1"};
  health.health.value = HealthState::Healthy;
  health.health.provenance = EvidenceProvenance::Measured;
  health.health.observed_at = observed();
  health.health.ttl = std::chrono::milliseconds{1000};
  health.health.durability = Durability::Ephemeral;
  RF_REQUIRE(fabric.publish_health(health).accepted());
  const MemberRecord with_health = *fabric.find_member(health.member);
  RF_CHECK_EQ(with_health.health.value, HealthState::Healthy);
  RF_CHECK(with_health.health.is_current_at(observed()));

  PublishCapabilityRequest capability;
  capability.authority = authority;
  capability.member = health.member;
  capability.capability = CapabilityRef{CapabilityId{"rdma-rocev2"}};
  capability.capability->provenance = EvidenceProvenance::Reported;
  capability.capability->observed_at = observed();
  capability.capability->value = std::string("v2");
  RF_REQUIRE(fabric.publish_capability(capability).accepted());
  const MemberRecord with_capability = *fabric.find_member(health.member);
  RF_REQUIRE(with_capability.capabilities.size() == 1);
  RF_CHECK_EQ(with_capability.capabilities[0].id.value(), std::string("rdma-rocev2"));
  RF_CHECK_EQ(with_capability.capabilities[0].provenance, EvidenceProvenance::Reported);
  RF_CHECK_EQ(with_capability.capabilities[0].value->c_str(), std::string("v2"));

  PublishHealthRequest unknown_member = health;
  unknown_member.member = MemberKey{MemberKind::Node, "node-9"};
  const MutationResult missing = fabric.publish_health(unknown_member);
  RF_CHECK_EQ(missing.outcome, MutationOutcome::RejectUnknownMember);

  PublishCapabilityRequest missing_capability = capability;
  missing_capability.capability.reset();
  const MutationResult absent = fabric.publish_capability(missing_capability);
  RF_CHECK_EQ(absent.outcome, MutationOutcome::RejectInvalidInput);
  RF_CHECK_EQ(absent.explanation.code, std::string("CAPABILITY_REQUIRED"));
  RF_CHECK(fabric.check_invariants().ok());
}
