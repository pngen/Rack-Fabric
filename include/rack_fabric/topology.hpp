// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Rack-local topology relationships.
//
// Rack Fabric represents relationships; it does not rank paths, build
// communication plans or schedule traffic. That belongs to Communication
// Planner.

#ifndef RACK_FABRIC_TOPOLOGY_HPP
#define RACK_FABRIC_TOPOLOGY_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/member.hpp"

namespace rack_fabric {

enum class RelationshipClass : std::uint8_t {
  /// Structural containment: the first endpoint contains the second.
  /// Directed and acyclic.
  Contains = 0,
  /// The first endpoint is attached to the second (device in a slot, card in
  /// a chassis).
  AttachedTo = 1,
  /// The two endpoints are connected. Symmetric.
  ConnectedTo = 2,
  /// The first endpoint can be reached through the second. Directed.
  ReachableThrough = 3,
  SameNode = 4,
  SameNumaDomain = 5,
  SameSwitchDomain = 6,
  SamePowerDomain = 7,
  SameCoolingDomain = 8,
  SameFailureDomain = 9,
  /// The first endpoint is storage local to the second. Directed.
  StorageLocal = 10,
  /// The first endpoint is a NIC local to the second. Directed.
  NicLocal = 11,
  /// The two endpoints are accelerator peers over an interconnect that was
  /// actually observed. Symmetric.
  AcceleratorPeer = 12,
};

[[nodiscard]] std::string_view to_string(RelationshipClass value) noexcept;
[[nodiscard]] std::optional<RelationshipClass> relationship_class_from_string(std::string_view) noexcept;

/// Symmetric classes carry no direction: their canonical form orders the two
/// endpoints so that publishing them in either order is idempotent.
[[nodiscard]] constexpr bool is_symmetric(RelationshipClass value) noexcept {
  switch (value) {
    case RelationshipClass::ConnectedTo:
    case RelationshipClass::SameNode:
    case RelationshipClass::SameNumaDomain:
    case RelationshipClass::SameSwitchDomain:
    case RelationshipClass::SamePowerDomain:
    case RelationshipClass::SameCoolingDomain:
    case RelationshipClass::SameFailureDomain:
    case RelationshipClass::AcceleratorPeer:
      return true;
    default:
      return false;
  }
}

/// Canonical identity of one relationship.
struct RelationshipKey {
  RelationshipClass cls = RelationshipClass::Contains;
  MemberKey from;
  MemberKey to;

  /// Normalizes symmetric classes so that (a,b) and (b,a) are the same key.
  [[nodiscard]] static RelationshipKey canonicalize(RelationshipClass cls, MemberKey from, MemberKey to);

  friend bool operator==(const RelationshipKey&, const RelationshipKey&) = default;
  friend auto operator<=>(const RelationshipKey&, const RelationshipKey&) = default;

  [[nodiscard]] std::string to_string() const;
};

struct RelationshipKeyHash {
  [[nodiscard]] std::size_t operator()(const RelationshipKey& key) const noexcept;
};

struct RelationshipRecord {
  RelationshipKey key;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
  bool revalidation_required = false;
  TopologyGeneration generation;
  /// Process that published this relationship. Absent for declarations made
  /// in-process by the coordinator operator.
  std::optional<WorkerId> owner_worker;
  std::optional<AgentBootId> owner_boot;
  /// Optional link identity for relationships that model a physical link.
  std::optional<LinkId> link;
  /// Optional link capacity as reported by the source. Absence means unknown.
  std::optional<Quantity> capacity;
  std::optional<std::string> source;

  [[nodiscard]] Freshness freshness_at(Timestamp now) const noexcept;
  [[nodiscard]] bool is_current_at(Timestamp now) const noexcept;

  friend bool operator==(const RelationshipRecord&, const RelationshipRecord&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_TOPOLOGY_HPP
