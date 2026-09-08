// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Lifecycle and state enumerations.
//
// Every enumeration keeps an explicit unknown member. The helper predicates
// below exist so that callers cannot accidentally treat "unknown" as a
// positive statement.

#ifndef RACK_FABRIC_LIFECYCLE_HPP
#define RACK_FABRIC_LIFECYCLE_HPP

#include <cstdint>
#include <string_view>

namespace rack_fabric {

/// Lifecycle of the rack as a whole.
///
/// The current lifecycle is always derived from canonical state plus the
/// configured readiness contract; it is never stored as an independent flag,
/// so it cannot drift from the evidence that justifies it.
enum class RackLifecycle : std::uint8_t {
  /// No rack has been declared in this instance.
  Undeclared = 0,
  /// The rack identity is declared but no member evidence has arrived.
  Declared = 1,
  /// Member evidence is arriving; the readiness contract is not satisfied.
  Discovering = 2,
  /// The rack is deliberately or accidentally incomplete. Unknown facts
  /// remain unknown; a partial rack is not treated as invalid.
  Partial = 3,
  /// The configured readiness contract is satisfied and no member is
  /// unhealthy or unavailable.
  Ready = 4,
  /// The readiness contract is satisfied, but at least one member is
  /// degraded, unhealthy or unavailable.
  Degraded = 5,
  /// Some recovered or orphaned evidence must be republished by a current
  /// authority before the rack can be trusted again.
  RevalidationRequired = 6,
  /// The rack has been retired. Retired racks do not accept mutations.
  Retired = 7,
};

[[nodiscard]] std::string_view to_string(RackLifecycle value) noexcept;
[[nodiscard]] std::optional<RackLifecycle> rack_lifecycle_from_string(std::string_view) noexcept;
/// True only for READY. DEGRADED is explicitly not READY.
[[nodiscard]] constexpr bool is_operational(RackLifecycle value) noexcept {
  return value == RackLifecycle::Ready;
}
/// True when the rack can still be mutated by an authorized publisher.
[[nodiscard]] constexpr bool accepts_mutations(RackLifecycle value) noexcept {
  return value != RackLifecycle::Retired;
}

/// Lifecycle of one rack member.
enum class MemberLifecycle : std::uint8_t {
  Unknown = 0,
  /// Declared but not yet observed as present.
  Declared = 1,
  /// Observed present under current authority.
  Present = 2,
  /// Explicitly marked unavailable. Distinct from absent: the identity is
  /// still known and the member is expected to return.
  Unavailable = 3,
  /// Replaced by a newer generation of the same identity.
  Superseded = 4,
  /// Withdrawn from the rack.
  Removed = 5,
  /// Permanently retired. A retired identity cannot be resurrected.
  Retired = 6,
};

[[nodiscard]] std::string_view to_string(MemberLifecycle value) noexcept;
[[nodiscard]] std::optional<MemberLifecycle> member_lifecycle_from_string(std::string_view) noexcept;

/// True when the member is part of the current rack composition.
[[nodiscard]] constexpr bool is_current_member(MemberLifecycle value) noexcept {
  return value == MemberLifecycle::Present || value == MemberLifecycle::Unavailable;
}

/// State of a registered publisher (agent process) at the coordinator.
enum class PublisherState : std::uint8_t {
  Unknown = 0,
  /// REGISTER accepted; no publication yet.
  Registered = 1,
  /// Publishing under a current boot identity and live lease.
  Active = 2,
  /// The process is gone. Its authority is revoked.
  Lost = 3,
  /// Explicitly fenced by the coordinator. A fenced boot identity can never
  /// reacquire authority.
  Fenced = 4,
};

[[nodiscard]] std::string_view to_string(PublisherState value) noexcept;

enum class HealthState : std::uint8_t {
  Unknown = 0,
  Healthy = 1,
  Degraded = 2,
  Unhealthy = 3,
  NotApplicable = 4,
};

[[nodiscard]] std::string_view to_string(HealthState value) noexcept;
[[nodiscard]] constexpr bool is_positive_health(HealthState value) noexcept {
  return value == HealthState::Healthy || value == HealthState::NotApplicable;
}

enum class ReadinessState : std::uint8_t {
  Unknown = 0,
  Ready = 1,
  NotReady = 2,
};

[[nodiscard]] std::string_view to_string(ReadinessState value) noexcept;

enum class ReachabilityState : std::uint8_t {
  Unknown = 0,
  Reachable = 1,
  Unreachable = 2,
};

[[nodiscard]] std::string_view to_string(ReachabilityState value) noexcept;

enum class ThrottleState : std::uint8_t {
  Unknown = 0,
  None = 1,
  Throttled = 2,
  SeverelyThrottled = 3,
};

[[nodiscard]] std::string_view to_string(ThrottleState value) noexcept;

/// Reason a member or rack requires revalidation.
enum class RevalidationReason : std::uint8_t {
  None = 0,
  OwnerProcessLost = 1,
  CoordinatorRestarted = 2,
  EvidenceExpired = 3,
  Superseded = 4,
};

[[nodiscard]] std::string_view to_string(RevalidationReason value) noexcept;

}  // namespace rack_fabric

#endif  // RACK_FABRIC_LIFECYCLE_HPP
