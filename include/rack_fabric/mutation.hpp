// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Mutation requests and typed outcomes.
//
// Rack Fabric never reduces a failure to a boolean. Every mutation returns a
// typed outcome plus a deterministic explanation that names the controlling
// invariant, the affected identity and, where relevant, the expected and
// current generation.

#ifndef RACK_FABRIC_MUTATION_HPP
#define RACK_FABRIC_MUTATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/authority.hpp"
#include "rack_fabric/envelope.hpp"
#include "rack_fabric/explanation.hpp"
#include "rack_fabric/failure_domain.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/lifecycle.hpp"
#include "rack_fabric/member.hpp"
#include "rack_fabric/topology.hpp"

namespace rack_fabric {

enum class MutationOutcome : std::uint8_t {
  /// The mutation changed canonical state.
  Accepted = 0,
  /// The mutation was semantically identical to current state; nothing was
  /// written and no generation was advanced.
  NoChange = 1,
  /// The mutation carried a coordinator epoch that is not current.
  RejectStaleCoordinatorEpoch = 2,
  /// The mutation carried a boot identity that has been fenced, or a boot
  /// identity that is not the registered owner of the target record.
  RejectStaleWorkerBoot = 3,
  /// The mutation carried a generation older than the current one.
  RejectStaleGeneration = 4,
  /// The mutation was not authorized for the target record.
  RejectNotAuthorized = 5,
  /// The mutation conflicts with current state and did not ask to supersede.
  RejectConflict = 6,
  /// The relationship is structurally invalid (self-link, unknown endpoint,
  /// forbidden cycle).
  RejectInvalidRelationship = 7,
  /// The referenced parent member does not exist.
  RejectUnknownParent = 8,
  /// The target identity has been retired and cannot be resurrected.
  RejectRetired = 9,
  /// The mutation is well formed but the state it targets must be revalidated
  /// by its current authority first.
  RevalidationRequired = 10,
  /// The request was structurally invalid: malformed identity, impossible
  /// numeric value, missing required field, or a bound exceeded.
  RejectInvalidInput = 11,
  /// A configured resource bound would be exceeded.
  RejectLimitExceeded = 12,
  /// No rack is declared, or the mutation names a different rack.
  RejectUnknownRack = 13,
  /// The publisher is not registered under the presented boot identity.
  RejectNotRegistered = 14,
  /// The referenced member does not exist.
  RejectUnknownMember = 15,
  /// The rack itself is retired.
  RejectRackRetired = 16,
  /// The mutation is not supported for this record kind.
  RejectUnsupported = 17,
};

[[nodiscard]] std::string_view to_string(MutationOutcome value) noexcept;
[[nodiscard]] constexpr bool is_accepted(MutationOutcome value) noexcept {
  return value == MutationOutcome::Accepted;
}
[[nodiscard]] constexpr bool is_rejection(MutationOutcome value) noexcept {
  return !is_accepted(value) && value != MutationOutcome::NoChange &&
         value != MutationOutcome::RevalidationRequired;
}

struct MutationResult {
  MutationOutcome outcome = MutationOutcome::RejectInvalidInput;
  Explanation explanation;
  /// Generations in effect after the mutation attempt.
  GenerationSet generations_after;

  [[nodiscard]] bool accepted() const noexcept { return is_accepted(outcome); }
  [[nodiscard]] bool rejected() const noexcept { return is_rejection(outcome); }
  [[nodiscard]] bool no_change() const noexcept { return outcome == MutationOutcome::NoChange; }

  friend bool operator==(const MutationResult&, const MutationResult&) = default;
};

/// Which evidence attached to a member is being withdrawn.
enum class WithdrawScope : std::uint8_t {
  All = 0,
  Health = 1,
  Readiness = 2,
  Reachability = 3,
  Capabilities = 4,
  Details = 5,
};

[[nodiscard]] std::string_view to_string(WithdrawScope value) noexcept;

struct RegisterPublisherRequest {
  std::optional<RackId> rack;
  std::optional<WorkerId> worker;
  std::optional<AgentBootId> boot;
  CoordinatorEpoch coordinator_epoch;
  std::optional<std::string> label;
};

struct DeclareRackRequest {
  AuthorityToken authority;
  std::optional<RackEpochId> epoch;
  std::optional<std::string> label;
  /// Explicitly re-declare an existing rack under a new epoch. Without this,
  /// declaring a rack that already exists is rejected as a conflict.
  bool redeclare = false;
};

struct PublishMemberRequest {
  AuthorityToken authority;
  MemberRecord record;
  /// Optimistic concurrency: when present, the mutation is accepted only if
  /// the current member generation equals this value.
  std::optional<MemberGeneration> expected_generation;
  /// Explicitly supersede a conflicting current generation.
  bool supersede = false;
};

struct PublishRelationshipRequest {
  AuthorityToken authority;
  RelationshipRecord record;
  std::optional<TopologyGeneration> expected_generation;
  bool supersede = false;
};

struct PublishFailureDomainRequest {
  AuthorityToken authority;
  FailureDomainRecord record;
  std::optional<FailureDomainGeneration> expected_generation;
  bool supersede = false;
};

struct PublishPowerEnvelopeRequest {
  AuthorityToken authority;
  PowerEnvelopeRecord record;
  std::optional<PowerEnvelopeGeneration> expected_generation;
  bool supersede = false;
};

struct PublishCoolingEnvelopeRequest {
  AuthorityToken authority;
  CoolingEnvelopeRecord record;
  std::optional<CoolingEnvelopeGeneration> expected_generation;
  bool supersede = false;
};

struct PublishHealthRequest {
  AuthorityToken authority;
  MemberKey member;
  EvidenceValue<HealthState> health;
  EvidenceValue<ReadinessState> readiness;
  bool publish_readiness = false;
  std::optional<MemberGeneration> expected_generation;
};

struct PublishCapabilityRequest {
  AuthorityToken authority;
  MemberKey member;
  std::optional<CapabilityRef> capability;
  std::optional<MemberGeneration> expected_generation;
};

struct WithdrawEvidenceRequest {
  AuthorityToken authority;
  MemberKey member;
  WithdrawScope scope = WithdrawScope::All;
  std::optional<MemberGeneration> expected_generation;
};

struct WithdrawRelationshipRequest {
  AuthorityToken authority;
  RelationshipKey key;
  std::optional<TopologyGeneration> expected_generation;
};

struct MarkUnavailableRequest {
  AuthorityToken authority;
  MemberKey member;
  RevalidationReason reason = RevalidationReason::OwnerProcessLost;
  std::optional<MemberGeneration> expected_generation;
};

struct RetireMemberRequest {
  AuthorityToken authority;
  MemberKey member;
  std::optional<MemberGeneration> expected_generation;
};

struct RetireRackRequest {
  AuthorityToken authority;
};

struct RevalidateMembersRequest {
  AuthorityToken authority;
  std::vector<MemberKey> members;
  /// Revalidate every member that currently requires revalidation.
  bool all = false;
};

struct FencePublisherRequest {
  CoordinatorEpoch coordinator_epoch;
  std::optional<AgentBootId> boot;
  RevalidationReason reason = RevalidationReason::OwnerProcessLost;
};

/// A liveness assertion from a registered publisher. It carries no state of
/// its own: it only proves that the process behind a boot identity is still
/// alive, so that its lease does not expire.
struct HeartbeatRequest {
  AuthorityToken authority;
  PublicationGeneration publication_generation;
};

struct SnapshotRequest {
  AuthorityToken authority;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_MUTATION_HPP
