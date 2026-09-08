// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Generation counters.
//
// Every externally significant piece of rack state is generation bound. A
// generation is a strictly monotonic unsigned counter of a distinct type.
// Zero means "unset"; the first published generation of any kind is 1.
//
// All generation arithmetic is checked: next() returns std::nullopt instead
// of wrapping, so a caller can never observe a generation moving backwards.

#ifndef RACK_FABRIC_GENERATION_HPP
#define RACK_FABRIC_GENERATION_HPP

#include <compare>
#include <cstdint>
#include <functional>
#include <optional>

namespace rack_fabric {

template <class Tag>
class Generation {
 public:
  using value_type = std::uint64_t;

  constexpr Generation() noexcept = default;
  constexpr Generation(const Generation&) noexcept = default;
  constexpr Generation(Generation&&) noexcept = default;
  constexpr Generation& operator=(const Generation&) noexcept = default;
  constexpr Generation& operator=(Generation&&) noexcept = default;
  constexpr ~Generation() = default;

  [[nodiscard]] static constexpr Generation from_value(std::uint64_t value) noexcept {
    Generation g;
    g.value_ = value;
    return g;
  }

  /// The first generation of any kind.
  [[nodiscard]] static constexpr Generation first() noexcept { return from_value(1); }

  [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return value_ == 0; }
  [[nodiscard]] constexpr bool is_set() const noexcept { return value_ != 0; }

  /// Checked increment. Returns std::nullopt at the representable maximum.
  [[nodiscard]] constexpr std::optional<Generation> next() const noexcept {
    if (value_ == kMax) {
      return std::nullopt;
    }
    return from_value(value_ + 1);
  }

  [[nodiscard]] static constexpr Generation max() noexcept { return from_value(kMax); }

  friend constexpr bool operator==(Generation lhs, Generation rhs) noexcept = default;
  friend constexpr auto operator<=>(Generation lhs, Generation rhs) noexcept = default;

  [[nodiscard]] constexpr std::size_t hash() const noexcept {
    return std::hash<std::uint64_t>{}(value_);
  }

 private:
  static constexpr std::uint64_t kMax = 0xFFFF'FFFF'FFFF'FFFFULL;
  std::uint64_t value_ = 0;
};

struct RackGenerationTag;
struct RackEpochGenerationTag;
struct MemberGenerationTag;
struct MembershipGenerationTag;
struct TopologyGenerationTag;
struct FailureDomainGenerationTag;
struct ConstraintGenerationTag;
struct PowerEnvelopeGenerationTag;
struct CoolingEnvelopeGenerationTag;
struct HealthGenerationTag;
struct CapabilityGenerationTag;
struct PublicationGenerationTag;
struct SnapshotGenerationTag;
struct CoordinatorEpochTag;

/// Generation of the rack record itself (identity, lifecycle, declaration).
using RackGeneration = Generation<RackGenerationTag>;

/// Per-member monotonic generation. Distinct from the membership generation.
using MemberGeneration = Generation<MemberGenerationTag>;

/// Generation of the rack membership set as a whole.
using MembershipGeneration = Generation<MembershipGenerationTag>;

/// Generation of the topology relationship set.
using TopologyGeneration = Generation<TopologyGenerationTag>;

/// Generation of the failure-domain model.
using FailureDomainGeneration = Generation<FailureDomainGenerationTag>;

/// Generation of rack-level constraints (including envelope constraints).
using ConstraintGeneration = Generation<ConstraintGenerationTag>;

using PowerEnvelopeGeneration = Generation<PowerEnvelopeGenerationTag>;
using CoolingEnvelopeGeneration = Generation<CoolingEnvelopeGenerationTag>;
using HealthGeneration = Generation<HealthGenerationTag>;
using CapabilityGeneration = Generation<CapabilityGenerationTag>;

/// Per-publisher monotonic publication generation.
using PublicationGeneration = Generation<PublicationGenerationTag>;

using SnapshotGeneration = Generation<SnapshotGenerationTag>;

/// Epoch of one coordinator process incarnation. A fresh coordinator process
/// must use a strictly greater epoch than any it has previously issued, so
/// traffic produced under a dead coordinator epoch can be rejected.
using CoordinatorEpoch = Generation<CoordinatorEpochTag>;

/// A bundle of the generations that a snapshot binds.
struct GenerationSet {
  RackGeneration rack;
  MembershipGeneration membership;
  TopologyGeneration topology;
  FailureDomainGeneration failure_domains;
  ConstraintGeneration constraints;
  PowerEnvelopeGeneration power;
  CoolingEnvelopeGeneration cooling;
  HealthGeneration health;
  CapabilityGeneration capabilities;
  CoordinatorEpoch coordinator_epoch;

  friend bool operator==(const GenerationSet&, const GenerationSet&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_GENERATION_HPP
