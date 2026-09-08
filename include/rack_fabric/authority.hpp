// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Authority, process incarnation and fencing.
//
// Every process-originated mutation must carry current authority: the epoch
// of the coordinator that is currently serving, the boot identity of the
// process that produced the mutation, and the publication generation of that
// process. A mutation produced under a dead coordinator epoch or a dead boot
// identity is rejected before it can touch canonical state.

#ifndef RACK_FABRIC_AUTHORITY_HPP
#define RACK_FABRIC_AUTHORITY_HPP

#include <cstdint>
#include <optional>
#include <string>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/lifecycle.hpp"

namespace rack_fabric {

/// Authority attached to one mutation.
struct AuthorityToken {
  /// Epoch of the coordinator that must still be current for this mutation.
  CoordinatorEpoch coordinator_epoch;
  /// Boot identity of the process that produced the mutation. Absent for
  /// mutations issued in-process by the coordinator operator.
  std::optional<AgentBootId> boot;
  /// Publication generation of the producing process. Absent for in-process
  /// operator mutations, which are not sequenced by a publisher.
  std::optional<PublicationGeneration> publication_generation;
  /// Rack the mutation applies to. Required for every rack-scoped mutation.
  std::optional<RackId> rack;

  [[nodiscard]] bool has_process_authority() const noexcept { return boot.has_value(); }

  friend bool operator==(const AuthorityToken&, const AuthorityToken&) = default;
};

/// Coordinator-side record of one registered publisher process.
struct PublisherRecord {
  WorkerId worker;
  AgentBootId boot;
  CoordinatorEpoch coordinator_epoch;
  PublicationGeneration publication_generation;
  PublisherState state = PublisherState::Unknown;
  Timestamp registered_at = Timestamp::unknown();
  Timestamp last_seen_at = Timestamp::unknown();
  std::int64_t lease_millis = 0;
  std::optional<std::string> label;
  std::size_t accepted_mutations = 0;
  std::size_t rejected_mutations = 0;

  friend bool operator==(const PublisherRecord&, const PublisherRecord&) = default;
};

/// A boot identity that has been fenced. A fenced identity can never
/// reacquire authority, even if a process presents it again.
struct FencedBootRecord {
  AgentBootId boot;
  std::optional<WorkerId> worker;
  CoordinatorEpoch fenced_under_epoch;
  Timestamp fenced_at = Timestamp::unknown();
  RevalidationReason reason = RevalidationReason::None;

  friend bool operator==(const FencedBootRecord&, const FencedBootRecord&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_AUTHORITY_HPP
