// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Immutable rack snapshots.
//
// A snapshot binds the rack composition to the generations that were
// authoritative when it was produced. A snapshot is a statement about a
// particular generation set; it is never silently reinterpreted as current
// after the underlying generations move.

#ifndef RACK_FABRIC_SNAPSHOT_HPP
#define RACK_FABRIC_SNAPSHOT_HPP

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/envelope.hpp"
#include "rack_fabric/explanation.hpp"
#include "rack_fabric/failure_domain.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/lifecycle.hpp"
#include "rack_fabric/member.hpp"
#include "rack_fabric/topology.hpp"

namespace rack_fabric {

/// Deterministic summary of a rack.
struct RackSummary {
  std::optional<RackId> rack;
  std::optional<RackEpochId> rack_epoch;
  RackLifecycle lifecycle = RackLifecycle::Undeclared;
  GenerationSet generations;

  std::size_t member_count = 0;
  std::size_t node_count = 0;
  std::size_t accelerator_count = 0;
  std::size_t cpu_package_count = 0;
  std::size_t memory_domain_count = 0;
  std::size_t nic_count = 0;
  std::size_t dpu_count = 0;
  std::size_t switch_count = 0;
  std::size_t storage_endpoint_count = 0;
  std::size_t power_domain_count = 0;
  std::size_t cooling_domain_count = 0;

  std::size_t present_member_count = 0;
  std::size_t unavailable_member_count = 0;
  std::size_t retired_member_count = 0;
  std::size_t revalidation_required_member_count = 0;
  std::size_t stale_member_count = 0;

  std::size_t relationship_count = 0;
  std::size_t failure_domain_count = 0;
  std::size_t publisher_count = 0;
  std::size_t active_publisher_count = 0;
  std::size_t fenced_boot_count = 0;

  bool power_envelope_known = false;
  bool cooling_envelope_known = false;
  std::size_t snapshot_count = 0;

  /// Deterministic digest of the summary content.
  std::string digest;

  friend bool operator==(const RackSummary&, const RackSummary&) = default;
};

/// Result of validating a snapshot against current authoritative state.
enum class SnapshotValidationStatus : std::uint8_t {
  /// The snapshot binds exactly the current generations and epoch.
  Current = 0,
  /// The snapshot is internally valid but binds generations that are no
  /// longer current. The consumer must revalidate before acting.
  Stale = 1,
  /// The snapshot refers to a rack that this instance does not describe.
  UnknownRack = 2,
  /// The snapshot is internally inconsistent (digest mismatch or missing
  /// bound generation).
  Invalid = 3,
};

[[nodiscard]] std::string_view to_string(SnapshotValidationStatus value) noexcept;

struct SnapshotValidation {
  SnapshotValidationStatus status = SnapshotValidationStatus::Invalid;
  Explanation explanation;

  [[nodiscard]] bool is_current() const noexcept {
    return status == SnapshotValidationStatus::Current;
  }

  friend bool operator==(const SnapshotValidation&, const SnapshotValidation&) = default;
};

/// An immutable view of the rack at a bound generation set.
class RackSnapshot {
 public:
  RackSnapshot() = delete;
  RackSnapshot(const RackSnapshot&) = default;
  RackSnapshot(RackSnapshot&&) noexcept = default;
  RackSnapshot& operator=(const RackSnapshot&) = default;
  RackSnapshot& operator=(RackSnapshot&&) noexcept = default;
  ~RackSnapshot() = default;

  [[nodiscard]] const SnapshotId& id() const noexcept { return id_; }
  [[nodiscard]] SnapshotGeneration generation() const noexcept { return generation_; }
  [[nodiscard]] PublicationGeneration publication_generation() const noexcept {
    return publication_generation_;
  }
  [[nodiscard]] Timestamp created_at() const noexcept { return created_at_; }
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const noexcept { return generations_.coordinator_epoch; }
  [[nodiscard]] const GenerationSet& generations() const noexcept { return generations_; }
  [[nodiscard]] RackLifecycle lifecycle() const noexcept { return lifecycle_; }
  [[nodiscard]] const RackId& rack() const noexcept { return rack_; }
  [[nodiscard]] const RackEpochId& rack_epoch() const noexcept { return rack_epoch_; }

  [[nodiscard]] const std::vector<MemberRecord>& members() const noexcept { return members_; }
  [[nodiscard]] const std::vector<RelationshipRecord>& relationships() const noexcept {
    return relationships_;
  }
  [[nodiscard]] const std::vector<FailureDomainRecord>& failure_domains() const noexcept {
    return failure_domains_;
  }
  [[nodiscard]] const std::optional<PowerEnvelopeRecord>& power_envelope() const noexcept {
    return power_envelope_;
  }
  [[nodiscard]] const std::optional<CoolingEnvelopeRecord>& cooling_envelope() const noexcept {
    return cooling_envelope_;
  }

  [[nodiscard]] std::optional<MemberRecord> find_member(const MemberKey& key) const;
  [[nodiscard]] std::optional<FailureDomainRecord> find_failure_domain(const FailureDomainId& id) const;

  /// SHA-256 over the deterministic serialization of the snapshot content.
  [[nodiscard]] const std::string& digest() const noexcept { return digest_; }
  /// Deterministic text rendering (stable ordering, stable formatting).
  [[nodiscard]] std::string render() const;

  friend bool operator==(const RackSnapshot&, const RackSnapshot&) = default;

 private:
  friend class RackFabric;
  friend struct RackState;
  friend struct SnapshotBuilder;

  RackSnapshot(const SnapshotId& id, const RackId& rack, const RackEpochId& rack_epoch)
      : id_(id), rack_(rack), rack_epoch_(rack_epoch) {}

  SnapshotId id_;
  SnapshotGeneration generation_;
  PublicationGeneration publication_generation_;
  Timestamp created_at_ = Timestamp::unknown();
  RackId rack_;
  RackEpochId rack_epoch_;
  GenerationSet generations_;
  RackLifecycle lifecycle_ = RackLifecycle::Undeclared;
  std::vector<MemberRecord> members_;
  std::vector<RelationshipRecord> relationships_;
  std::vector<FailureDomainRecord> failure_domains_;
  std::optional<PowerEnvelopeRecord> power_envelope_;
  std::optional<CoolingEnvelopeRecord> cooling_envelope_;
  std::string digest_;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_SNAPSHOT_HPP
