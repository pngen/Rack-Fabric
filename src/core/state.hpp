// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Canonical rack state and its derived indexes.
//
// The ordered maps below are the authoritative records. Every index is a
// derived acceleration structure and is verified against the canonical
// records by check_invariants().

#ifndef RACK_FABRIC_INTERNAL_STATE_HPP
#define RACK_FABRIC_INTERNAL_STATE_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "rack_fabric/authority.hpp"
#include "rack_fabric/envelope.hpp"
#include "rack_fabric/failure_domain.hpp"
#include "rack_fabric/invariant.hpp"
#include "rack_fabric/limits.hpp"
#include "rack_fabric/member.hpp"
#include "rack_fabric/readiness.hpp"
#include "rack_fabric/snapshot.hpp"
#include "rack_fabric/topology.hpp"

namespace rack_fabric {

struct RackRecord {
  RackId id;
  RackEpochId epoch;
  RackGeneration generation;
  bool retired = false;
  std::optional<std::string> label;
  Timestamp declared_at = Timestamp::unknown();

  friend bool operator==(const RackRecord&, const RackRecord&) = default;
};

/// Authoritative state of one rack, plus derived indexes.
struct RackState {
  std::optional<RackRecord> rack;

  std::map<MemberKey, MemberRecord> members;
  std::map<RelationshipKey, RelationshipRecord> relationships;
  std::map<FailureDomainId, FailureDomainRecord> failure_domains;
  std::optional<PowerEnvelopeRecord> power;
  std::optional<CoolingEnvelopeRecord> cooling;

  std::map<AgentBootId, PublisherRecord> publishers;
  std::map<AgentBootId, FencedBootRecord> fenced_boots;
  std::map<SnapshotGeneration, RackSnapshot> snapshots;

  GenerationSet generations;
  SnapshotGeneration snapshot_generation;
  PublicationGeneration operator_publication_generation;

  // -- derived indexes ----------------------------------------------------
  std::map<MemberKey, std::set<RelationshipKey>> relationships_by_member;
  std::map<MemberKind, std::set<MemberKey>> members_by_kind;
  std::map<FailureDomainId, std::set<MemberKey>> members_by_failure_domain;
  std::map<PowerDomainId, std::set<MemberKey>> members_by_power_domain;
  std::map<CoolingDomainId, std::set<MemberKey>> members_by_cooling_domain;
  std::map<AgentBootId, std::set<MemberKey>> members_by_owner_boot;
  std::map<MemberKey, std::set<MemberKey>> children_by_parent;
  std::map<FailureDomainId, std::set<FailureDomainId>> children_by_domain;

  ResourceLimits limits;
  ReadinessContract contract;

  // -- index maintenance --------------------------------------------------
  void rebuild_indexes();
  void index_add_member(const MemberRecord& record);
  void index_remove_member(const MemberRecord& record);
  void index_add_relationship(const RelationshipRecord& record);
  void index_remove_relationship(const RelationshipRecord& record);
  void index_add_failure_domain(const FailureDomainRecord& record);
  void index_remove_failure_domain(const FailureDomainRecord& record);

  // -- derived queries ----------------------------------------------------
  [[nodiscard]] RackLifecycle derive_lifecycle(Timestamp now) const;
  [[nodiscard]] ReadinessEvaluation evaluate_readiness(Timestamp now) const;
  [[nodiscard]] std::vector<FailureDomainId> failure_domains_of(const MemberKey& key) const;
  [[nodiscard]] std::vector<MemberKey> members_in_failure_domain(const FailureDomainId& id,
                                                                bool include_nested) const;
  [[nodiscard]] std::vector<MemberKey> members_affected_by(const FailureDomainId& id) const;
  [[nodiscard]] bool shares_failure_domain(const MemberKey& lhs, const MemberKey& rhs) const;
  [[nodiscard]] bool contains_cycle(const MemberKey& parent, const MemberKey& child) const;
  /// Cycle check over the published CONTAINS relationship graph. A member's
  /// parent field is only one source of containment; relationships are the
  /// other, and both must stay acyclic.
  [[nodiscard]] bool relationship_contains_cycle(const MemberKey& parent,
                                                 const MemberKey& child) const;
  [[nodiscard]] bool domain_contains_cycle(const FailureDomainId& parent,
                                           const FailureDomainId& child) const;
  [[nodiscard]] std::vector<MemberKey> descendants(const MemberKey& parent) const;

  // -- invariants ---------------------------------------------------------
  [[nodiscard]] InvariantReport check_invariants(Timestamp now) const;

  // -- snapshots ----------------------------------------------------------
  [[nodiscard]] RackSnapshot build_snapshot(const SnapshotId& id, SnapshotGeneration generation,
                                            PublicationGeneration publication, Timestamp now) const;

  // -- envelope derivation ------------------------------------------------
  /// Recomputes derived headroom fields. Derived values are only produced
  /// when both operands are known, current and expressed in the same unit.
  void recompute_power_derived(Timestamp now);
  void recompute_cooling_derived(Timestamp now);
};

/// True when a member record carries any live observation that must not be
/// treated as current after the owning process dies or the coordinator
/// restarts.
[[nodiscard]] bool member_has_ephemeral_evidence(const MemberRecord& record) noexcept;

/// SHA-256 digest of the deterministic encoding of a snapshot's content.
[[nodiscard]] std::string compute_snapshot_digest(const RackSnapshot& snapshot,
                                                  const ResourceLimits& limits);

/// Deterministic serialization of durable state for persistence. Returns
/// std::nullopt when a configured bound is exceeded, so a truncated or
/// oversized state can never be written.
/// Encodes the durable state. When the encoder refuses to produce bytes,
/// p failure receives the codec reason (for example "LENGTH_TOO_LARGE").
[[nodiscard]] std::optional<std::vector<std::byte>> encode_state_payload(
    const RackState& state, std::string* failure = nullptr);

/// Decodes a payload into a candidate state. Returns false and sets error on
/// any structural problem; the caller must discard the candidate.
[[nodiscard]] bool decode_state_payload(const std::byte* data, std::size_t size, RackState& out,
                                        std::string& error);

}  // namespace rack_fabric

#endif  // RACK_FABRIC_INTERNAL_STATE_HPP
