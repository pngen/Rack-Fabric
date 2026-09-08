// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The Rack Fabric public API.
//
// Rack Fabric owns the authoritative description of one rack: its identity,
// membership, relationships, locality, failure domains, power and cooling
// envelopes, evidence provenance, generations and lifecycle, and the
// generation-bound snapshots other runtimes consume.
//
// Rack Fabric does not own cluster composition, workload placement,
// communication planning, collectives, resource reservation, capacity
// prediction, congestion control, power optimisation, thermal control, device
// health remediation, runtime-service discovery or the canonical hardware
// capability database. See the README for the exact systems boundary.

#ifndef RACK_FABRIC_RACK_FABRIC_HPP
#define RACK_FABRIC_RACK_FABRIC_HPP

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/authority.hpp"
#include "rack_fabric/envelope.hpp"
#include "rack_fabric/explanation.hpp"
#include "rack_fabric/failure_domain.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/invariant.hpp"
#include "rack_fabric/limits.hpp"
#include "rack_fabric/lifecycle.hpp"
#include "rack_fabric/member.hpp"
#include "rack_fabric/mutation.hpp"
#include "rack_fabric/persistence.hpp"
#include "rack_fabric/readiness.hpp"
#include "rack_fabric/snapshot.hpp"
#include "rack_fabric/topology.hpp"
#include "rack_fabric/version.hpp"

namespace rack_fabric {

struct RackFabricOptions {
  ResourceLimits limits;
  ReadinessContract readiness;
  /// Observation clock. Null selects the process default clock.
  std::shared_ptr<const Clock> clock;
  /// When true, a mutation that presents a boot identity must be registered
  /// as a publisher before it can mutate state.
  bool require_registered_publisher = true;
  /// Free-form label of this instance, used in rendered diagnostics only.
  std::string instance_label = "rack-fabric";
};

struct SnapshotResult {
  MutationResult result;
  std::optional<RackSnapshot> snapshot;

  [[nodiscard]] bool ok() const noexcept { return result.accepted() && snapshot.has_value(); }
};

/// One authoritative rack runtime instance.
///
/// The class owns no process-global mutable state: independent instances in
/// one process are fully isolated, and no operation of one instance can be
/// observed through another.
class RackFabric {
 public:
  explicit RackFabric(RackFabricOptions options = {});
  ~RackFabric();

  RackFabric(const RackFabric&) = delete;
  RackFabric& operator=(const RackFabric&) = delete;
  RackFabric(RackFabric&&) = delete;
  RackFabric& operator=(RackFabric&&) = delete;

  // -- configuration ------------------------------------------------------

  [[nodiscard]] const RackFabricOptions& options() const;
  [[nodiscard]] const Clock& clock() const noexcept;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept;

  // -- mutation surface ---------------------------------------------------

  MutationResult register_publisher(const RegisterPublisherRequest& request);
  MutationResult declare_rack(const DeclareRackRequest& request);
  MutationResult publish_member(const PublishMemberRequest& request);
  MutationResult publish_relationship(const PublishRelationshipRequest& request);
  MutationResult publish_failure_domain(const PublishFailureDomainRequest& request);
  MutationResult publish_power_envelope(const PublishPowerEnvelopeRequest& request);
  MutationResult publish_cooling_envelope(const PublishCoolingEnvelopeRequest& request);
  MutationResult publish_health(const PublishHealthRequest& request);
  MutationResult publish_capability(const PublishCapabilityRequest& request);
  MutationResult withdraw_evidence(const WithdrawEvidenceRequest& request);
  MutationResult withdraw_relationship(const WithdrawRelationshipRequest& request);
  MutationResult mark_unavailable(const MarkUnavailableRequest& request);
  MutationResult retire_member(const RetireMemberRequest& request);
  MutationResult retire_rack(const RetireRackRequest& request);
  MutationResult revalidate_members(const RevalidateMembersRequest& request);
  MutationResult fence_publisher(const FencePublisherRequest& request);

  /// Records that a registered publisher is still alive. A publisher whose
  /// lease expires without a heartbeat is fenced, and its boot identity can
  /// never reacquire authority.
  MutationResult heartbeat(const HeartbeatRequest& request);

  /// Fences every publisher whose lease has expired at the current clock.
  /// Returns the number of publishers fenced by this call.
  std::size_t expire_publishers();

  // -- inspection ---------------------------------------------------------

  [[nodiscard]] RackLifecycle lifecycle() const;
  [[nodiscard]] GenerationSet generations() const;
  [[nodiscard]] RackSummary summary() const;
  [[nodiscard]] std::optional<RackId> rack_id() const;
  [[nodiscard]] std::optional<RackEpochId> rack_epoch() const;

  [[nodiscard]] std::optional<MemberRecord> find_member(const MemberKey& key) const;
  [[nodiscard]] std::vector<MemberRecord> members(std::optional<MemberKind> kind = std::nullopt) const;
  [[nodiscard]] std::vector<RelationshipRecord> relationships() const;
  [[nodiscard]] std::vector<RelationshipRecord> relationships_touching(const MemberKey& key) const;
  [[nodiscard]] std::optional<RelationshipRecord> find_relationship(const RelationshipKey& key) const;

  [[nodiscard]] std::vector<FailureDomainRecord> failure_domains() const;
  [[nodiscard]] std::optional<FailureDomainRecord> find_failure_domain(const FailureDomainId& id) const;
  /// Every failure domain the member belongs to, including transitive parents.
  [[nodiscard]] std::vector<FailureDomainId> failure_domains_of(const MemberKey& key) const;
  /// Members that belong to a failure domain. When include_nested is true the
  /// result also contains members of nested domains that declare this domain
  /// as an ancestor.
  [[nodiscard]] std::vector<MemberKey> members_in_failure_domain(const FailureDomainId& id,
                                                                bool include_nested = true) const;
  /// Members that would be affected if the given domain failed: the members of
  /// the domain itself plus every member of its nested descendant domains.
  [[nodiscard]] std::vector<MemberKey> members_affected_by(const FailureDomainId& id) const;
  [[nodiscard]] bool shares_failure_domain(const MemberKey& lhs, const MemberKey& rhs) const;

  [[nodiscard]] std::optional<PowerEnvelopeRecord> power_envelope() const;
  [[nodiscard]] std::optional<CoolingEnvelopeRecord> cooling_envelope() const;

  [[nodiscard]] std::vector<PublisherRecord> publishers() const;
  [[nodiscard]] std::optional<PublisherRecord> find_publisher(const AgentBootId& boot) const;
  [[nodiscard]] std::vector<FencedBootRecord> fenced_boots() const;
  [[nodiscard]] bool is_boot_fenced(const AgentBootId& boot) const;

  [[nodiscard]] ReadinessEvaluation evaluate_readiness() const;

  // -- explanations -------------------------------------------------------

  [[nodiscard]] Explanation explain_readiness() const;
  [[nodiscard]] Explanation explain_member(const MemberKey& key) const;
  [[nodiscard]] Explanation explain_relationship(const RelationshipKey& key) const;
  [[nodiscard]] Explanation explain_publication(const MutationResult& result) const;

  // -- snapshots ----------------------------------------------------------

  [[nodiscard]] SnapshotResult publish_snapshot(const SnapshotRequest& request);
  [[nodiscard]] SnapshotValidation validate_snapshot(const RackSnapshot& snapshot) const;
  [[nodiscard]] std::optional<RackSnapshot> find_snapshot(const SnapshotId& id) const;
  [[nodiscard]] std::vector<SnapshotId> snapshot_ids() const;
  [[nodiscard]] std::size_t snapshot_count() const;

  // -- invariants ---------------------------------------------------------

  [[nodiscard]] InvariantReport check_invariants() const;

  // -- persistence --------------------------------------------------------

  [[nodiscard]] PersistenceResult save_state(const std::filesystem::path& path,
                                             const PersistenceOptions& options = {}) const;
  [[nodiscard]] PersistenceResult load_state(const std::filesystem::path& path);

  /// Opaque implementation type. It is public only so that the persistence
  /// translation unit can share its definition; it exposes no interface and
  /// is not part of the supported API surface.
  class Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_RACK_FABRIC_HPP
