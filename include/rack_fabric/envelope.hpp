// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Power and cooling/thermal envelope representation.
//
// Rack Fabric owns the authoritative representation of rack power and cooling
// envelopes and their current generation. It does not optimize clocks, it
// does not allocate power, it does not throttle, it does not derate workloads
// and it does not implement energy policy. Those belong to Power Fabric and
// the Thermal Governor.

#ifndef RACK_FABRIC_ENVELOPE_HPP
#define RACK_FABRIC_ENVELOPE_HPP

#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/lifecycle.hpp"

namespace rack_fabric {

/// Power budget and observation for one power domain.
struct PowerDomainBudget {
  PowerDomainId domain;
  /// Declared or reported budget for the domain.
  std::optional<Quantity> limit;
  /// Observed or reported draw for the domain.
  std::optional<Quantity> observed_draw;
  /// Derived headroom, present only when limit and draw are both known,
  /// current and expressed in the same unit.
  std::optional<Quantity> headroom;

  friend bool operator==(const PowerDomainBudget&, const PowerDomainBudget&) = default;
};

struct PowerEnvelopeRecord {
  PowerEnvelopeGeneration generation;
  std::optional<Quantity> rack_limit;
  std::optional<Quantity> rack_observed_draw;
  std::optional<Quantity> rack_headroom;
  std::vector<PowerDomainBudget> domain_budgets;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
  bool revalidation_required = false;
  std::optional<WorkerId> owner_worker;
  std::optional<AgentBootId> owner_boot;
  std::optional<std::string> source;

  [[nodiscard]] Freshness freshness_at(Timestamp now) const noexcept;

  friend bool operator==(const PowerEnvelopeRecord&, const PowerEnvelopeRecord&) = default;
};

/// Cooling/thermal state of one cooling domain.
struct CoolingZoneRecord {
  CoolingDomainId zone;
  /// Declared or design thermal limit for the zone.
  std::optional<Quantity> design_thermal_limit;
  /// Observed temperature.
  std::optional<Quantity> observed_temperature;
  /// Reported cooling capacity for the zone.
  std::optional<Quantity> cooling_capacity;
  /// Derived headroom, present only when capacity and load are both known.
  std::optional<Quantity> thermal_headroom;
  /// Observed throttling evidence. Unknown when the platform exposes none.
  std::optional<EvidenceValue<ThrottleState>> throttling;

  friend bool operator==(const CoolingZoneRecord&, const CoolingZoneRecord&) = default;
};

struct CoolingEnvelopeRecord {
  CoolingEnvelopeGeneration generation;
  std::vector<CoolingZoneRecord> zones;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
  bool revalidation_required = false;
  std::optional<WorkerId> owner_worker;
  std::optional<AgentBootId> owner_boot;
  std::optional<std::string> source;

  [[nodiscard]] Freshness freshness_at(Timestamp now) const noexcept;

  friend bool operator==(const CoolingEnvelopeRecord&, const CoolingEnvelopeRecord&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_ENVELOPE_HPP
