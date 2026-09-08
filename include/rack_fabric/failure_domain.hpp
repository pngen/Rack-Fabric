// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Failure domains.
//
// A failure domain is a set of rack components that can be expected to fail
// together. Domains may be nested and may overlap: a component can belong to
// a node domain, a chassis domain, a power-feed domain and a cooling-zone
// domain at the same time. Rack Fabric deliberately does not assume that
// failure domains form a single tree.
//
// Rack Fabric exposes failure-domain facts. It does not implement workload
// replication policy or placement.

#ifndef RACK_FABRIC_FAILURE_DOMAIN_HPP
#define RACK_FABRIC_FAILURE_DOMAIN_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/lifecycle.hpp"

namespace rack_fabric {

enum class FailureDomainKind : std::uint8_t {
  Unknown = 0,
  Node = 1,
  Chassis = 2,
  PowerFeed = 3,
  Pdu = 4,
  Switch = 5,
  NetworkPath = 6,
  CoolingZone = 7,
  StorageEnclosure = 8,
  Rack = 9,
  Other = 10,
};

[[nodiscard]] std::string_view to_string(FailureDomainKind value) noexcept;
[[nodiscard]] std::optional<FailureDomainKind> failure_domain_kind_from_string(std::string_view) noexcept;

struct FailureDomainRecord {
  FailureDomainId id;
  FailureDomainKind kind = FailureDomainKind::Unknown;
  MemberLifecycle lifecycle = MemberLifecycle::Declared;

  /// Parent domains. Empty means the domain has no known parent. Parents form
  /// a directed acyclic graph: a cycle is rejected at publication time.
  std::vector<FailureDomainId> parents;

  FailureDomainGeneration generation;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
  bool revalidation_required = false;
  std::optional<WorkerId> owner_worker;
  std::optional<AgentBootId> owner_boot;

  std::optional<std::string> label;
  std::optional<std::string> source;

  [[nodiscard]] Freshness freshness_at(Timestamp now) const noexcept;
  [[nodiscard]] bool is_current_at(Timestamp now) const noexcept;

  friend bool operator==(const FailureDomainRecord&, const FailureDomainRecord&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_FAILURE_DOMAIN_HPP
