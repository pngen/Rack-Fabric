// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Deterministic, machine-readable explanations.
//
// Callers must never have to reverse-engineer rack state from log text. Every
// decision that Rack Fabric makes about readiness, availability, staleness,
// relationship validity or publication rejection is available as a structured
// record with a stable code, the identity it concerns and the controlling
// factors in a deterministic order.

#ifndef RACK_FABRIC_EXPLANATION_HPP
#define RACK_FABRIC_EXPLANATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/identity.hpp"

namespace rack_fabric {

/// One controlling factor behind a decision.
struct ExplanationFactor {
  /// Stable machine-readable code, for example "MEMBER_EVIDENCE_STALE".
  std::string code;
  /// Human-readable statement of the factor.
  std::string detail;
  /// The member this factor concerns, when the factor is member-scoped.
  std::optional<std::string> member_id;
  std::optional<std::string> member_kind;
  /// Name of the generation that controls the factor, for example
  /// "topology_generation".
  std::optional<std::string> generation_name;
  /// Generation the caller supplied or the record carries.
  std::optional<std::uint64_t> expected_generation;
  /// Generation that is currently authoritative.
  std::optional<std::uint64_t> current_generation;
  /// Freshness or lifecycle value, rendered as its stable name.
  std::optional<std::string> state;

  friend bool operator==(const ExplanationFactor&, const ExplanationFactor&) = default;
};

/// The result of a decision, with the factors that produced it.
struct Explanation {
  /// True when the decision was affirmative.
  bool ok = false;
  /// Stable top-level code, for example "RACK_READY" or "SNAPSHOT_STALE".
  std::string code;
  /// The identity the decision concerns, as a stable string.
  std::string subject;
  /// Controlling factors, in deterministic order.
  std::vector<ExplanationFactor> factors;

  [[nodiscard]] static Explanation success(std::string code, std::string subject);
  [[nodiscard]] static Explanation failure(std::string code, std::string subject,
                                           std::vector<ExplanationFactor> factors);

  /// Renders the explanation as deterministic text.
  [[nodiscard]] std::string render() const;

  friend bool operator==(const Explanation&, const Explanation&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_EXPLANATION_HPP
