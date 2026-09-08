// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The synthetic rack laboratory.
//
// This generator produces deterministic, explicitly SYNTHETIC rack shapes for
// architectural, property and adversarial testing. Every record it emits
// carries EvidenceProvenance::Synthetic. It is not a substitute for physical
// proof, and no synthetic rack can satisfy a readiness contract that requires
// physical provenance.

#ifndef RACK_FABRIC_SYNTHETIC_HPP
#define RACK_FABRIC_SYNTHETIC_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/envelope.hpp"
#include "rack_fabric/failure_domain.hpp"
#include "rack_fabric/member.hpp"
#include "rack_fabric/rack_fabric.hpp"
#include "rack_fabric/topology.hpp"

namespace rack_fabric {

struct SyntheticRackConfig {
  std::uint64_t seed = 1;
  std::optional<RackId> rack;
  std::optional<RackEpochId> rack_epoch;

  std::size_t node_count = 8;
  std::size_t min_accelerators_per_node = 0;
  std::size_t max_accelerators_per_node = 4;
  std::size_t min_nics_per_node = 1;
  std::size_t max_nics_per_node = 2;
  std::size_t dpu_count = 0;
  std::size_t switch_count = 2;
  std::size_t storage_endpoint_count = 2;
  std::size_t power_domain_count = 2;
  std::size_t cooling_domain_count = 2;
  std::size_t cpu_packages_per_node = 2;
  std::size_t memory_domains_per_node = 2;

  /// Probability in [0,1] that a node differs from the baseline shape.
  double heterogeneity = 0.35;
  /// Probability in [0,1] that an optional field is omitted, producing
  /// deliberate partial evidence.
  double missing_evidence = 0.15;
  /// Probability in [0,1] that a node is marked degraded or unhealthy.
  double degraded_fraction = 0.05;
  /// Probability in [0,1] that a link is omitted from the topology.
  double missing_links = 0.1;
  /// Emit synthetic accelerator-peer links. These are SYNTHETIC; they are not
  /// evidence of a real NVLink or NVSwitch fabric.
  bool synthetic_accelerator_peer_links = false;
  /// Include a synthetic power envelope.
  bool include_power_envelope = true;
  /// Include a synthetic cooling envelope.
  bool include_cooling_envelope = true;
};

struct SyntheticRack {
  RackId rack;
  RackEpochId rack_epoch;
  std::vector<MemberRecord> members;
  std::vector<RelationshipRecord> relationships;
  std::vector<FailureDomainRecord> failure_domains;
  std::optional<PowerEnvelopeRecord> power;
  std::optional<CoolingEnvelopeRecord> cooling;
  std::uint64_t seed = 0;
  /// Exact parameters that reproduce this rack. Printed on test failure.
  std::string reproduction_parameters;
};

[[nodiscard]] SyntheticRack generate_synthetic_rack(const SyntheticRackConfig& config);

/// Deterministic one-line reproduction parameters for a configuration.
[[nodiscard]] std::string synthetic_reproduction_parameters(const SyntheticRackConfig& config);

struct SyntheticPublishSummary {
  std::size_t members_accepted = 0;
  std::size_t relationships_accepted = 0;
  std::size_t failure_domains_accepted = 0;
  std::vector<MutationResult> rejections;
};

/// Publishes a generated rack into an instance using coordinator authority.
[[nodiscard]] SyntheticPublishSummary publish_synthetic_rack(RackFabric& fabric,
                                                            const SyntheticRack& rack,
                                                            const AuthorityToken& authority);

}  // namespace rack_fabric

#endif  // RACK_FABRIC_SYNTHETIC_HPP
