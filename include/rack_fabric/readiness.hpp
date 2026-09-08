// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The rack readiness contract.
//
// READY is not a side effect of a worker registering. READY means that an
// explicitly configured evidence contract has been satisfied under current
// authority. The contract is inspectable, testable and reported through a
// deterministic explanation.

#ifndef RACK_FABRIC_READINESS_HPP
#define RACK_FABRIC_READINESS_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "rack_fabric/explanation.hpp"

namespace rack_fabric {

struct ReadinessContract {
  /// Minimum number of nodes that must be PRESENT with current evidence.
  std::size_t min_nodes = 1;
  /// Minimum number of accelerators that must be PRESENT with current
  /// evidence.
  std::size_t min_accelerators = 0;
  /// Minimum number of distinct failure domains covered by the members that
  /// satisfy the node requirement.
  std::size_t min_distinct_failure_domains = 0;
  /// Require at least one topology relationship to exist.
  bool require_topology = false;
  /// Require a failure-domain model to exist.
  bool require_failure_domains = false;
  /// Require a current power envelope.
  bool require_power_envelope = false;
  /// Require a current cooling envelope.
  bool require_cooling_envelope = false;
  /// Require every member that is part of the current composition to carry
  /// current, non-stale evidence.
  bool require_all_members_current = true;
  /// Require health evidence for every member in the current composition.
  bool require_member_health = false;
  /// Require that the rack has at least one active publisher.
  bool require_active_publisher = false;
  /// Only evidence with physical provenance (MEASURED, REPORTED, DERIVED)
  /// may satisfy the member requirements. Synthetic evidence can describe a
  /// rack, but it can never make a physical rack READY.
  bool require_physical_provenance = true;

  friend bool operator==(const ReadinessContract&, const ReadinessContract&) = default;
};

/// A single unmet requirement, in deterministic order.
struct ReadinessDeficit {
  std::string code;
  std::string detail;
  std::size_t required = 0;
  std::size_t observed = 0;

  friend bool operator==(const ReadinessDeficit&, const ReadinessDeficit&) = default;
};

/// Outcome of evaluating the readiness contract.
struct ReadinessEvaluation {
  bool satisfied = false;
  std::vector<ReadinessDeficit> deficits;
  std::size_t qualifying_nodes = 0;
  std::size_t qualifying_accelerators = 0;
  std::size_t qualifying_failure_domains = 0;

  friend bool operator==(const ReadinessEvaluation&, const ReadinessEvaluation&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_READINESS_HPP
