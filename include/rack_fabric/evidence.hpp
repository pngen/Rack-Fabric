// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Evidence classification, freshness and quantities.
//
// Rack Fabric never collapses provenance into a single "valid" boolean. Every
// observation records how it was obtained, when, and how long that claim
// remains defensible. Absence of evidence is not positive evidence: the
// default provenance of every field is UNKNOWN.

#ifndef RACK_FABRIC_EVIDENCE_HPP
#define RACK_FABRIC_EVIDENCE_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>

namespace rack_fabric {

/// How an observation was obtained.
///
///   MEASURED      - read directly from the device or platform at the moment
///                   of publication, by the process that published it.
///   REPORTED      - asserted by another runtime or by an operator; Rack
///                   Fabric relays the claim without having observed it.
///   DERIVED       - computed by Rack Fabric from other evidence that is
///                   itself classified (for example power headroom from a
///                   limit and a draw).
///   ESTIMATED     - inferred by a model or interpolation; the source did not
///                   provide the value and no measurement backs it.
///   SYNTHETIC     - produced by the deterministic synthetic rack laboratory.
///                   Never a physical fact.
///   RECONSTRUCTED - recovered from persisted state after a restart. It is a
///                   memory of a past observation, not a current measurement.
///   UNKNOWN       - nothing is known.
enum class EvidenceProvenance : std::uint8_t {
  Unknown = 0,
  Measured = 1,
  Reported = 2,
  Derived = 3,
  Estimated = 4,
  Synthetic = 5,
  Reconstructed = 6,
};

[[nodiscard]] std::string_view to_string(EvidenceProvenance value) noexcept;
[[nodiscard]] std::optional<EvidenceProvenance> evidence_provenance_from_string(std::string_view) noexcept;

/// True only for provenance classes that represent a physical observation or
/// a directly asserted external claim. Used by the readiness contract and by
/// reports that must distinguish physical from synthetic proof.
[[nodiscard]] constexpr bool is_physical_provenance(EvidenceProvenance value) noexcept {
  return value == EvidenceProvenance::Measured || value == EvidenceProvenance::Reported ||
         value == EvidenceProvenance::Derived;
}

/// Freshness of an observation relative to the observing clock.
enum class Freshness : std::uint8_t {
  /// No timestamp, or no evidence at all.
  Unknown = 0,
  /// Within the declared time-to-live.
  Fresh = 1,
  /// Past the declared time-to-live. The observation happened, but it is no
  /// longer a statement about the present.
  Stale = 2,
  /// The authority that produced the observation is gone (process death) or
  /// the coordinator was restarted. The observation must be republished or
  /// revalidated before it can be considered current.
  RevalidationRequired = 3,
};

[[nodiscard]] std::string_view to_string(Freshness value) noexcept;

/// Whether a fact is worth persisting across a coordinator restart.
///
///   DURABLE   - a declaration about the infrastructure that remains true
///               until explicitly superseded (identities, declared model
///               names, declared envelopes, membership topology).
///   EPHEMERAL - a live observation that is only true while the process that
///               made it is alive and within its time-to-live (reachability,
///               temperature, current draw, worker-owned health results).
enum class Durability : std::uint8_t {
  Durable = 0,
  Ephemeral = 1,
};

[[nodiscard]] std::string_view to_string(Durability value) noexcept;

/// Wall-clock instant in milliseconds since the Unix epoch. Zero means
/// unknown; it is never used as a valid observation time.
struct Timestamp {
  std::int64_t unix_millis = 0;

  [[nodiscard]] static constexpr Timestamp unknown() noexcept { return Timestamp{}; }
  [[nodiscard]] static constexpr Timestamp from_unix_millis(std::int64_t millis) noexcept {
    return Timestamp{millis};
  }
  [[nodiscard]] constexpr bool known() const noexcept { return unix_millis > 0; }
  [[nodiscard]] constexpr std::int64_t millis() const noexcept { return unix_millis; }

  friend constexpr bool operator==(Timestamp lhs, Timestamp rhs) noexcept = default;
  friend constexpr auto operator<=>(Timestamp lhs, Timestamp rhs) noexcept = default;
};

/// Source of timestamps. Injectable so that tests are deterministic.
class Clock {
 public:
  Clock() = default;
  Clock(const Clock&) = default;
  Clock(Clock&&) = default;
  Clock& operator=(const Clock&) = default;
  Clock& operator=(Clock&&) = default;
  virtual ~Clock() = default;

  [[nodiscard]] virtual Timestamp now() const = 0;
};

/// The process default clock, backed by the system wall clock.
[[nodiscard]] const Clock& default_clock() noexcept;

/// Test clock with a manually advanced instant.
class ManualClock final : public Clock {
 public:
  explicit ManualClock(std::int64_t start_unix_millis = 1'700'000'000'000LL) noexcept
      : now_(start_unix_millis) {}

  [[nodiscard]] Timestamp now() const override { return Timestamp::from_unix_millis(now_); }

  void advance(std::chrono::milliseconds delta) noexcept { now_ += delta.count(); }
  void set(std::int64_t unix_millis) noexcept { now_ = unix_millis; }

 private:
  std::int64_t now_;
};

/// An observation of type T, with its provenance and lifetime.
///
/// The default value is the T-specific "unknown" sentinel and the default
/// provenance is Unknown, so a default-constructed EvidenceValue never
/// asserts anything.
template <class T>
struct EvidenceValue {
  T value{};
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
  /// Set when the producing authority was lost and the value must be
  /// republished before it can be treated as current.
  bool revalidation_required = false;

  [[nodiscard]] bool has_evidence() const noexcept { return provenance != EvidenceProvenance::Unknown; }

  [[nodiscard]] Freshness freshness_at(Timestamp now) const noexcept {
    if (revalidation_required) {
      return Freshness::RevalidationRequired;
    }
    if (!has_evidence() || !observed_at.known()) {
      return Freshness::Unknown;
    }
    if (durability == Durability::Durable && ttl.count() == 0) {
      // A durable declaration does not expire; it is superseded, not aged.
      return Freshness::Fresh;
    }
    if (ttl.count() <= 0) {
      // An ephemeral observation without a declared lifetime can never be
      // shown to be current.
      return Freshness::Stale;
    }
    if (!now.known()) {
      return Freshness::Unknown;
    }
    const std::int64_t age = now.millis() - observed_at.millis();
    if (age < 0) {
      // An observation from the future is not evidence about the present.
      return Freshness::Stale;
    }
    return age <= ttl.count() ? Freshness::Fresh : Freshness::Stale;
  }

  [[nodiscard]] bool is_current_at(Timestamp now) const noexcept {
    return freshness_at(now) == Freshness::Fresh;
  }

  friend bool operator==(const EvidenceValue&, const EvidenceValue&) = default;
};

enum class QuantityUnit : std::uint8_t {
  None = 0,
  Watts = 1,
  Celsius = 2,
  Bytes = 3,
  Count = 4,
  BytesPerSecond = 5,
  GigabitsPerSecond = 6,
  Megahertz = 7,
  Volts = 8,
  Amperes = 9,
};

[[nodiscard]] std::string_view to_string(QuantityUnit value) noexcept;

/// A scalar measurement with an explicit unit, provenance and lifetime.
///
/// uncertainty is std::nullopt when the source made no precision claim. Rack
/// Fabric never invents precision: a caller that supplies no uncertainty gets
/// none reported back.
struct Quantity {
  double value = 0.0;
  QuantityUnit unit = QuantityUnit::None;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  std::optional<double> uncertainty;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
  bool revalidation_required = false;

  [[nodiscard]] bool has_evidence() const noexcept { return provenance != EvidenceProvenance::Unknown; }

  [[nodiscard]] Freshness freshness_at(Timestamp now) const noexcept {
    EvidenceValue<double> e;
    e.value = value;
    e.provenance = provenance;
    e.observed_at = observed_at;
    e.ttl = ttl;
    e.durability = durability;
    e.revalidation_required = revalidation_required;
    return e.freshness_at(now);
  }

  [[nodiscard]] bool is_current_at(Timestamp now) const noexcept {
    return freshness_at(now) == Freshness::Fresh;
  }

  friend bool operator==(const Quantity&, const Quantity&) = default;
};

/// True for finite, non-negative values that are meaningful as a magnitude.
[[nodiscard]] bool is_valid_magnitude(double value) noexcept;

}  // namespace rack_fabric

#endif  // RACK_FABRIC_EVIDENCE_HPP
