// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Stable textual names for every enumeration, plus the small value-type
// helpers that do not belong in a header.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/explanation.hpp"
#include "rack_fabric/invariant.hpp"
#include "rack_fabric/lifecycle.hpp"
#include "rack_fabric/member.hpp"
#include "rack_fabric/mutation.hpp"
#include "rack_fabric/persistence.hpp"
#include "rack_fabric/protocol.hpp"
#include "rack_fabric/topology.hpp"

namespace rack_fabric {

// ---------------------------------------------------------------------------
// Evidence
// ---------------------------------------------------------------------------

std::string_view to_string(EvidenceProvenance value) noexcept {
  switch (value) {
    case EvidenceProvenance::Unknown:
      return "UNKNOWN";
    case EvidenceProvenance::Measured:
      return "MEASURED";
    case EvidenceProvenance::Reported:
      return "REPORTED";
    case EvidenceProvenance::Derived:
      return "DERIVED";
    case EvidenceProvenance::Estimated:
      return "ESTIMATED";
    case EvidenceProvenance::Synthetic:
      return "SYNTHETIC";
    case EvidenceProvenance::Reconstructed:
      return "RECONSTRUCTED";
  }
  return "UNKNOWN";
}

std::optional<EvidenceProvenance> evidence_provenance_from_string(std::string_view value) noexcept {
  if (value == "UNKNOWN") return EvidenceProvenance::Unknown;
  if (value == "MEASURED") return EvidenceProvenance::Measured;
  if (value == "REPORTED") return EvidenceProvenance::Reported;
  if (value == "DERIVED") return EvidenceProvenance::Derived;
  if (value == "ESTIMATED") return EvidenceProvenance::Estimated;
  if (value == "SYNTHETIC") return EvidenceProvenance::Synthetic;
  if (value == "RECONSTRUCTED") return EvidenceProvenance::Reconstructed;
  return std::nullopt;
}

std::string_view to_string(Freshness value) noexcept {
  switch (value) {
    case Freshness::Unknown:
      return "UNKNOWN";
    case Freshness::Fresh:
      return "FRESH";
    case Freshness::Stale:
      return "STALE";
    case Freshness::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
  }
  return "UNKNOWN";
}

std::string_view to_string(Durability value) noexcept {
  switch (value) {
    case Durability::Durable:
      return "DURABLE";
    case Durability::Ephemeral:
      return "EPHEMERAL";
  }
  return "UNKNOWN";
}

std::string_view to_string(QuantityUnit value) noexcept {
  switch (value) {
    case QuantityUnit::None:
      return "none";
    case QuantityUnit::Watts:
      return "W";
    case QuantityUnit::Celsius:
      return "C";
    case QuantityUnit::Bytes:
      return "B";
    case QuantityUnit::Count:
      return "count";
    case QuantityUnit::BytesPerSecond:
      return "B/s";
    case QuantityUnit::GigabitsPerSecond:
      return "Gb/s";
    case QuantityUnit::Megahertz:
      return "MHz";
    case QuantityUnit::Volts:
      return "V";
    case QuantityUnit::Amperes:
      return "A";
  }
  return "none";
}

bool is_valid_magnitude(double value) noexcept {
  return std::isfinite(value) && value >= 0.0;
}

namespace {
class SystemClock final : public Clock {
 public:
  [[nodiscard]] Timestamp now() const override {
    const auto duration = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    return Timestamp::from_unix_millis(static_cast<std::int64_t>(millis));
  }
};

const SystemClock kSystemClock{};
}  // namespace

const Clock& default_clock() noexcept { return kSystemClock; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

std::string_view to_string(RackLifecycle value) noexcept {
  switch (value) {
    case RackLifecycle::Undeclared:
      return "UNDECLARED";
    case RackLifecycle::Declared:
      return "DECLARED";
    case RackLifecycle::Discovering:
      return "DISCOVERING";
    case RackLifecycle::Partial:
      return "PARTIAL";
    case RackLifecycle::Ready:
      return "READY";
    case RackLifecycle::Degraded:
      return "DEGRADED";
    case RackLifecycle::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case RackLifecycle::Retired:
      return "RETIRED";
  }
  return "UNDECLARED";
}

std::optional<RackLifecycle> rack_lifecycle_from_string(std::string_view value) noexcept {
  if (value == "UNDECLARED") return RackLifecycle::Undeclared;
  if (value == "DECLARED") return RackLifecycle::Declared;
  if (value == "DISCOVERING") return RackLifecycle::Discovering;
  if (value == "PARTIAL") return RackLifecycle::Partial;
  if (value == "READY") return RackLifecycle::Ready;
  if (value == "DEGRADED") return RackLifecycle::Degraded;
  if (value == "REVALIDATION_REQUIRED") return RackLifecycle::RevalidationRequired;
  if (value == "RETIRED") return RackLifecycle::Retired;
  return std::nullopt;
}

std::string_view to_string(MemberLifecycle value) noexcept {
  switch (value) {
    case MemberLifecycle::Unknown:
      return "UNKNOWN";
    case MemberLifecycle::Declared:
      return "DECLARED";
    case MemberLifecycle::Present:
      return "PRESENT";
    case MemberLifecycle::Unavailable:
      return "UNAVAILABLE";
    case MemberLifecycle::Superseded:
      return "SUPERSEDED";
    case MemberLifecycle::Removed:
      return "REMOVED";
    case MemberLifecycle::Retired:
      return "RETIRED";
  }
  return "UNKNOWN";
}

std::optional<MemberLifecycle> member_lifecycle_from_string(std::string_view value) noexcept {
  if (value == "UNKNOWN") return MemberLifecycle::Unknown;
  if (value == "DECLARED") return MemberLifecycle::Declared;
  if (value == "PRESENT") return MemberLifecycle::Present;
  if (value == "UNAVAILABLE") return MemberLifecycle::Unavailable;
  if (value == "SUPERSEDED") return MemberLifecycle::Superseded;
  if (value == "REMOVED") return MemberLifecycle::Removed;
  if (value == "RETIRED") return MemberLifecycle::Retired;
  return std::nullopt;
}

std::string_view to_string(PublisherState value) noexcept {
  switch (value) {
    case PublisherState::Unknown:
      return "UNKNOWN";
    case PublisherState::Registered:
      return "REGISTERED";
    case PublisherState::Active:
      return "ACTIVE";
    case PublisherState::Lost:
      return "LOST";
    case PublisherState::Fenced:
      return "FENCED";
  }
  return "UNKNOWN";
}

std::string_view to_string(HealthState value) noexcept {
  switch (value) {
    case HealthState::Unknown:
      return "UNKNOWN";
    case HealthState::Healthy:
      return "HEALTHY";
    case HealthState::Degraded:
      return "DEGRADED";
    case HealthState::Unhealthy:
      return "UNHEALTHY";
    case HealthState::NotApplicable:
      return "NOT_APPLICABLE";
  }
  return "UNKNOWN";
}

std::string_view to_string(ReadinessState value) noexcept {
  switch (value) {
    case ReadinessState::Unknown:
      return "UNKNOWN";
    case ReadinessState::Ready:
      return "READY";
    case ReadinessState::NotReady:
      return "NOT_READY";
  }
  return "UNKNOWN";
}

std::string_view to_string(ReachabilityState value) noexcept {
  switch (value) {
    case ReachabilityState::Unknown:
      return "UNKNOWN";
    case ReachabilityState::Reachable:
      return "REACHABLE";
    case ReachabilityState::Unreachable:
      return "UNREACHABLE";
  }
  return "UNKNOWN";
}

std::string_view to_string(ThrottleState value) noexcept {
  switch (value) {
    case ThrottleState::Unknown:
      return "UNKNOWN";
    case ThrottleState::None:
      return "NONE";
    case ThrottleState::Throttled:
      return "THROTTLED";
    case ThrottleState::SeverelyThrottled:
      return "SEVERELY_THROTTLED";
  }
  return "UNKNOWN";
}

std::string_view to_string(RevalidationReason value) noexcept {
  switch (value) {
    case RevalidationReason::None:
      return "NONE";
    case RevalidationReason::OwnerProcessLost:
      return "OWNER_PROCESS_LOST";
    case RevalidationReason::CoordinatorRestarted:
      return "COORDINATOR_RESTARTED";
    case RevalidationReason::EvidenceExpired:
      return "EVIDENCE_EXPIRED";
    case RevalidationReason::Superseded:
      return "SUPERSEDED";
  }
  return "NONE";
}

// ---------------------------------------------------------------------------
// Member
// ---------------------------------------------------------------------------

std::string_view to_string(MemberKind value) noexcept {
  switch (value) {
    case MemberKind::Node:
      return "node";
    case MemberKind::Accelerator:
      return "accelerator";
    case MemberKind::CpuPackage:
      return "cpu_package";
    case MemberKind::MemoryDomain:
      return "memory_domain";
    case MemberKind::Nic:
      return "nic";
    case MemberKind::Dpu:
      return "dpu";
    case MemberKind::Switch:
      return "switch";
    case MemberKind::StorageEndpoint:
      return "storage_endpoint";
    case MemberKind::PowerDomain:
      return "power_domain";
    case MemberKind::CoolingDomain:
      return "cooling_domain";
  }
  return "unknown";
}

std::optional<MemberKind> member_kind_from_string(std::string_view value) noexcept {
  if (value == "node") return MemberKind::Node;
  if (value == "accelerator") return MemberKind::Accelerator;
  if (value == "cpu_package") return MemberKind::CpuPackage;
  if (value == "memory_domain") return MemberKind::MemoryDomain;
  if (value == "nic") return MemberKind::Nic;
  if (value == "dpu") return MemberKind::Dpu;
  if (value == "switch") return MemberKind::Switch;
  if (value == "storage_endpoint") return MemberKind::StorageEndpoint;
  if (value == "power_domain") return MemberKind::PowerDomain;
  if (value == "cooling_domain") return MemberKind::CoolingDomain;
  return std::nullopt;
}

std::string MemberKey::to_string() const {
  std::string out;
  out.reserve(kind == MemberKind::Node ? 8 : 16);
  out.append(rack_fabric::to_string(kind));
  out.push_back(':');
  out.append(id);
  return out;
}

std::size_t MemberKeyHash::operator()(const MemberKey& key) const noexcept {
  const std::size_t kind_hash = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(key.kind));
  const std::size_t id_hash = std::hash<std::string>{}(key.id);
  return kind_hash ^ (id_hash + 0x9E3779B97F4A7C15ULL + (kind_hash << 6U) + (kind_hash >> 2U));
}

std::string_view to_string(NodeRole value) noexcept {
  switch (value) {
    case NodeRole::Unknown:
      return "UNKNOWN";
    case NodeRole::Compute:
      return "COMPUTE";
    case NodeRole::Storage:
      return "STORAGE";
    case NodeRole::Management:
      return "MANAGEMENT";
    case NodeRole::Mixed:
      return "MIXED";
  }
  return "UNKNOWN";
}

std::string_view to_string(CpuArchitecture value) noexcept {
  switch (value) {
    case CpuArchitecture::Unknown:
      return "UNKNOWN";
    case CpuArchitecture::X86_64:
      return "X86_64";
    case CpuArchitecture::Aarch64:
      return "AARCH64";
    case CpuArchitecture::Riscv64:
      return "RISCV64";
    case CpuArchitecture::Other:
      return "OTHER";
  }
  return "UNKNOWN";
}

std::string_view to_string(MemoryKind value) noexcept {
  switch (value) {
    case MemoryKind::Unknown:
      return "UNKNOWN";
    case MemoryKind::Ddr:
      return "DDR";
    case MemoryKind::Hbm:
      return "HBM";
    case MemoryKind::Lpddr:
      return "LPDDR";
    case MemoryKind::Other:
      return "OTHER";
  }
  return "UNKNOWN";
}

std::string_view to_string(StorageKind value) noexcept {
  switch (value) {
    case StorageKind::Unknown:
      return "UNKNOWN";
    case StorageKind::Nvme:
      return "NVME";
    case StorageKind::Sata:
      return "SATA";
    case StorageKind::Sas:
      return "SAS";
    case StorageKind::Network:
      return "NETWORK";
    case StorageKind::Other:
      return "OTHER";
  }
  return "UNKNOWN";
}

std::string_view to_string(PowerDomainKind value) noexcept {
  switch (value) {
    case PowerDomainKind::Unknown:
      return "UNKNOWN";
    case PowerDomainKind::Rack:
      return "RACK";
    case PowerDomainKind::Pdu:
      return "PDU";
    case PowerDomainKind::Feed:
      return "FEED";
    case PowerDomainKind::Busway:
      return "BUSWAY";
    case PowerDomainKind::Outlet:
      return "OUTLET";
    case PowerDomainKind::Other:
      return "OTHER";
  }
  return "UNKNOWN";
}

std::string_view to_string(CoolingDomainKind value) noexcept {
  switch (value) {
    case CoolingDomainKind::Unknown:
      return "UNKNOWN";
    case CoolingDomainKind::Rack:
      return "RACK";
    case CoolingDomainKind::Zone:
      return "ZONE";
    case CoolingDomainKind::Row:
      return "ROW";
    case CoolingDomainKind::Inlet:
      return "INLET";
    case CoolingDomainKind::Crac:
      return "CRAC";
    case CoolingDomainKind::Other:
      return "OTHER";
  }
  return "UNKNOWN";
}

std::optional<MemberKind> member_kind_of_details(const MemberDetails& details) noexcept {
  switch (details.index()) {
    case 0:
      return std::nullopt;
    case 1:
      return MemberKind::Node;
    case 2:
      return MemberKind::Accelerator;
    case 3:
      return MemberKind::CpuPackage;
    case 4:
      return MemberKind::MemoryDomain;
    case 5:
      return MemberKind::Nic;
    case 6:
      return MemberKind::Dpu;
    case 7:
      return MemberKind::Switch;
    case 8:
      return MemberKind::StorageEndpoint;
    case 9:
      return MemberKind::PowerDomain;
    case 10:
      return MemberKind::CoolingDomain;
    default:
      return std::nullopt;
  }
}

bool details_match_kind(MemberKind kind, const MemberDetails& details) noexcept {
  const auto matched = member_kind_of_details(details);
  if (!matched.has_value()) {
    // An empty detail record is accepted: a member may be known to exist
    // without any further description.
    return std::holds_alternative<std::monostate>(details);
  }
  return *matched == kind;
}

Freshness CapabilityRef::freshness_at(Timestamp now) const noexcept {
  EvidenceValue<int> evidence;
  evidence.provenance = provenance;
  evidence.observed_at = observed_at;
  evidence.ttl = ttl;
  evidence.durability = durability;
  evidence.revalidation_required = revalidation_required;
  return evidence.freshness_at(now);
}

Freshness MemberRecord::freshness_at(Timestamp now) const noexcept {
  EvidenceValue<int> evidence;
  evidence.provenance = provenance;
  evidence.observed_at = observed_at;
  evidence.ttl = ttl;
  evidence.durability = durability;
  evidence.revalidation_required = revalidation_required;
  return evidence.freshness_at(now);
}

bool MemberRecord::is_current_at(Timestamp now) const noexcept {
  return freshness_at(now) == Freshness::Fresh;
}

bool MemberRecord::is_authoritatively_current_at(Timestamp now) const noexcept {
  return lifecycle == MemberLifecycle::Present && is_current_at(now);
}

// ---------------------------------------------------------------------------
// Topology
// ---------------------------------------------------------------------------

std::string_view to_string(RelationshipClass value) noexcept {
  switch (value) {
    case RelationshipClass::Contains:
      return "contains";
    case RelationshipClass::AttachedTo:
      return "attached_to";
    case RelationshipClass::ConnectedTo:
      return "connected_to";
    case RelationshipClass::ReachableThrough:
      return "reachable_through";
    case RelationshipClass::SameNode:
      return "same_node";
    case RelationshipClass::SameNumaDomain:
      return "same_numa_domain";
    case RelationshipClass::SameSwitchDomain:
      return "same_switch_domain";
    case RelationshipClass::SamePowerDomain:
      return "same_power_domain";
    case RelationshipClass::SameCoolingDomain:
      return "same_cooling_domain";
    case RelationshipClass::SameFailureDomain:
      return "same_failure_domain";
    case RelationshipClass::StorageLocal:
      return "storage_local";
    case RelationshipClass::NicLocal:
      return "nic_local";
    case RelationshipClass::AcceleratorPeer:
      return "accelerator_peer";
  }
  return "unknown";
}

std::optional<RelationshipClass> relationship_class_from_string(std::string_view value) noexcept {
  if (value == "contains") return RelationshipClass::Contains;
  if (value == "attached_to") return RelationshipClass::AttachedTo;
  if (value == "connected_to") return RelationshipClass::ConnectedTo;
  if (value == "reachable_through") return RelationshipClass::ReachableThrough;
  if (value == "same_node") return RelationshipClass::SameNode;
  if (value == "same_numa_domain") return RelationshipClass::SameNumaDomain;
  if (value == "same_switch_domain") return RelationshipClass::SameSwitchDomain;
  if (value == "same_power_domain") return RelationshipClass::SamePowerDomain;
  if (value == "same_cooling_domain") return RelationshipClass::SameCoolingDomain;
  if (value == "same_failure_domain") return RelationshipClass::SameFailureDomain;
  if (value == "storage_local") return RelationshipClass::StorageLocal;
  if (value == "nic_local") return RelationshipClass::NicLocal;
  if (value == "accelerator_peer") return RelationshipClass::AcceleratorPeer;
  return std::nullopt;
}

RelationshipKey RelationshipKey::canonicalize(RelationshipClass cls, MemberKey from, MemberKey to) {
  if (is_symmetric(cls) && to < from) {
    std::swap(from, to);
  }
  return RelationshipKey{cls, std::move(from), std::move(to)};
}

std::string RelationshipKey::to_string() const {
  std::string out;
  out.append(rack_fabric::to_string(cls));
  out.push_back('(');
  out.append(from.to_string());
  out.append(" -> ");
  out.append(to.to_string());
  out.push_back(')');
  return out;
}

std::size_t RelationshipKeyHash::operator()(const RelationshipKey& key) const noexcept {
  const std::size_t cls_hash = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(key.cls));
  const std::size_t from_hash = MemberKeyHash{}(key.from);
  const std::size_t to_hash = MemberKeyHash{}(key.to);
  std::size_t seed = cls_hash;
  seed ^= from_hash + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U);
  seed ^= to_hash + 0x9E3779B97F4A7C15ULL + (seed << 6U) + (seed >> 2U);
  return seed;
}

Freshness RelationshipRecord::freshness_at(Timestamp now) const noexcept {
  EvidenceValue<int> evidence;
  evidence.provenance = provenance;
  evidence.observed_at = observed_at;
  evidence.ttl = ttl;
  evidence.durability = durability;
  evidence.revalidation_required = revalidation_required;
  return evidence.freshness_at(now);
}

bool RelationshipRecord::is_current_at(Timestamp now) const noexcept {
  return freshness_at(now) == Freshness::Fresh;
}

// ---------------------------------------------------------------------------
// Failure domains and envelopes
// ---------------------------------------------------------------------------

std::string_view to_string(FailureDomainKind value) noexcept {
  switch (value) {
    case FailureDomainKind::Unknown:
      return "UNKNOWN";
    case FailureDomainKind::Node:
      return "NODE";
    case FailureDomainKind::Chassis:
      return "CHASSIS";
    case FailureDomainKind::PowerFeed:
      return "POWER_FEED";
    case FailureDomainKind::Pdu:
      return "PDU";
    case FailureDomainKind::Switch:
      return "SWITCH";
    case FailureDomainKind::NetworkPath:
      return "NETWORK_PATH";
    case FailureDomainKind::CoolingZone:
      return "COOLING_ZONE";
    case FailureDomainKind::StorageEnclosure:
      return "STORAGE_ENCLOSURE";
    case FailureDomainKind::Rack:
      return "RACK";
    case FailureDomainKind::Other:
      return "OTHER";
  }
  return "UNKNOWN";
}

std::optional<FailureDomainKind> failure_domain_kind_from_string(std::string_view value) noexcept {
  if (value == "UNKNOWN") return FailureDomainKind::Unknown;
  if (value == "NODE") return FailureDomainKind::Node;
  if (value == "CHASSIS") return FailureDomainKind::Chassis;
  if (value == "POWER_FEED") return FailureDomainKind::PowerFeed;
  if (value == "PDU") return FailureDomainKind::Pdu;
  if (value == "SWITCH") return FailureDomainKind::Switch;
  if (value == "NETWORK_PATH") return FailureDomainKind::NetworkPath;
  if (value == "COOLING_ZONE") return FailureDomainKind::CoolingZone;
  if (value == "STORAGE_ENCLOSURE") return FailureDomainKind::StorageEnclosure;
  if (value == "RACK") return FailureDomainKind::Rack;
  if (value == "OTHER") return FailureDomainKind::Other;
  return std::nullopt;
}

Freshness FailureDomainRecord::freshness_at(Timestamp now) const noexcept {
  EvidenceValue<int> evidence;
  evidence.provenance = provenance;
  evidence.observed_at = observed_at;
  evidence.ttl = ttl;
  evidence.durability = durability;
  evidence.revalidation_required = revalidation_required;
  return evidence.freshness_at(now);
}

bool FailureDomainRecord::is_current_at(Timestamp now) const noexcept {
  return freshness_at(now) == Freshness::Fresh;
}

Freshness PowerEnvelopeRecord::freshness_at(Timestamp now) const noexcept {
  EvidenceValue<int> evidence;
  evidence.provenance = provenance;
  evidence.observed_at = observed_at;
  evidence.ttl = ttl;
  evidence.durability = durability;
  evidence.revalidation_required = revalidation_required;
  return evidence.freshness_at(now);
}

Freshness CoolingEnvelopeRecord::freshness_at(Timestamp now) const noexcept {
  EvidenceValue<int> evidence;
  evidence.provenance = provenance;
  evidence.observed_at = observed_at;
  evidence.ttl = ttl;
  evidence.durability = durability;
  evidence.revalidation_required = revalidation_required;
  return evidence.freshness_at(now);
}

// ---------------------------------------------------------------------------
// Explanation
// ---------------------------------------------------------------------------

Explanation Explanation::success(std::string code, std::string subject) {
  Explanation explanation;
  explanation.ok = true;
  explanation.code = std::move(code);
  explanation.subject = std::move(subject);
  return explanation;
}

Explanation Explanation::failure(std::string code, std::string subject,
                                 std::vector<ExplanationFactor> factors) {
  Explanation explanation;
  explanation.ok = false;
  explanation.code = std::move(code);
  explanation.subject = std::move(subject);
  explanation.factors = std::move(factors);
  return explanation;
}

std::string Explanation::render() const {
  std::ostringstream out;
  out << (ok ? "OK " : "DENIED ") << code;
  if (!subject.empty()) {
    out << " subject=" << subject;
  }
  for (const auto& factor : factors) {
    out << "\n  - " << factor.code << ": " << factor.detail;
    if (factor.member_kind.has_value() || factor.member_id.has_value()) {
      out << " [member=" << (factor.member_kind.has_value() ? *factor.member_kind : "?") << ':'
          << (factor.member_id.has_value() ? *factor.member_id : "?") << ']';
    }
    if (factor.generation_name.has_value()) {
      out << " [" << *factor.generation_name;
      if (factor.expected_generation.has_value()) {
        out << " expected=" << *factor.expected_generation;
      }
      if (factor.current_generation.has_value()) {
        out << " current=" << *factor.current_generation;
      }
      out << ']';
    }
    if (factor.state.has_value()) {
      out << " [state=" << *factor.state << ']';
    }
  }
  return out.str();
}

std::string InvariantReport::render() const {
  std::ostringstream out;
  out << (violations.empty() ? "INVARIANTS_OK" : "INVARIANTS_VIOLATED") << " checks=" << checks_run
      << " violations=" << violations.size();
  for (const auto& violation : violations) {
    out << "\n  - " << violation.code << ": " << violation.detail;
  }
  return out.str();
}

// ---------------------------------------------------------------------------
// Mutation
// ---------------------------------------------------------------------------

std::string_view to_string(MutationOutcome value) noexcept {
  switch (value) {
    case MutationOutcome::Accepted:
      return "ACCEPTED";
    case MutationOutcome::NoChange:
      return "NO_CHANGE";
    case MutationOutcome::RejectStaleCoordinatorEpoch:
      return "REJECT_STALE_COORDINATOR_EPOCH";
    case MutationOutcome::RejectStaleWorkerBoot:
      return "REJECT_STALE_WORKER_BOOT";
    case MutationOutcome::RejectStaleGeneration:
      return "REJECT_STALE_GENERATION";
    case MutationOutcome::RejectNotAuthorized:
      return "REJECT_NOT_AUTHORIZED";
    case MutationOutcome::RejectConflict:
      return "REJECT_CONFLICT";
    case MutationOutcome::RejectInvalidRelationship:
      return "REJECT_INVALID_RELATIONSHIP";
    case MutationOutcome::RejectUnknownParent:
      return "REJECT_UNKNOWN_PARENT";
    case MutationOutcome::RejectRetired:
      return "REJECT_RETIRED";
    case MutationOutcome::RevalidationRequired:
      return "REVALIDATION_REQUIRED";
    case MutationOutcome::RejectInvalidInput:
      return "REJECT_INVALID_INPUT";
    case MutationOutcome::RejectLimitExceeded:
      return "REJECT_LIMIT_EXCEEDED";
    case MutationOutcome::RejectUnknownRack:
      return "REJECT_UNKNOWN_RACK";
    case MutationOutcome::RejectNotRegistered:
      return "REJECT_NOT_REGISTERED";
    case MutationOutcome::RejectUnknownMember:
      return "REJECT_UNKNOWN_MEMBER";
    case MutationOutcome::RejectRackRetired:
      return "REJECT_RACK_RETIRED";
    case MutationOutcome::RejectUnsupported:
      return "REJECT_UNSUPPORTED";
  }
  return "REJECT_INVALID_INPUT";
}

std::string_view to_string(WithdrawScope value) noexcept {
  switch (value) {
    case WithdrawScope::All:
      return "ALL";
    case WithdrawScope::Health:
      return "HEALTH";
    case WithdrawScope::Readiness:
      return "READINESS";
    case WithdrawScope::Reachability:
      return "REACHABILITY";
    case WithdrawScope::Capabilities:
      return "CAPABILITIES";
    case WithdrawScope::Details:
      return "DETAILS";
  }
  return "ALL";
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

std::string_view to_string(SnapshotValidationStatus value) noexcept {
  switch (value) {
    case SnapshotValidationStatus::Current:
      return "CURRENT";
    case SnapshotValidationStatus::Stale:
      return "STALE";
    case SnapshotValidationStatus::UnknownRack:
      return "UNKNOWN_RACK";
    case SnapshotValidationStatus::Invalid:
      return "INVALID";
  }
  return "INVALID";
}

std::optional<MemberRecord> RackSnapshot::find_member(const MemberKey& key) const {
  const auto it = std::lower_bound(members_.begin(), members_.end(), key,
                                   [](const MemberRecord& record, const MemberKey& value) {
                                     return record.key < value;
                                   });
  if (it == members_.end() || !(it->key == key)) {
    return std::nullopt;
  }
  return *it;
}

std::optional<FailureDomainRecord> RackSnapshot::find_failure_domain(const FailureDomainId& id) const {
  const auto it = std::lower_bound(failure_domains_.begin(), failure_domains_.end(), id,
                                   [](const FailureDomainRecord& record, const FailureDomainId& value) {
                                     return record.id < value;
                                   });
  if (it == failure_domains_.end() || !(it->id == id)) {
    return std::nullopt;
  }
  return *it;
}

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

std::string_view to_string(PersistenceStatus value) noexcept {
  switch (value) {
    case PersistenceStatus::Ok:
      return "OK";
    case PersistenceStatus::NotFound:
      return "NOT_FOUND";
    case PersistenceStatus::IoError:
      return "IO_ERROR";
    case PersistenceStatus::Corrupt:
      return "CORRUPT";
    case PersistenceStatus::Truncated:
      return "TRUNCATED";
    case PersistenceStatus::UnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case PersistenceStatus::TrailingGarbage:
      return "TRAILING_GARBAGE";
    case PersistenceStatus::IntegrityFailure:
      return "INTEGRITY_FAILURE";
    case PersistenceStatus::LimitExceeded:
      return "LIMIT_EXCEEDED";
    case PersistenceStatus::InvalidState:
      return "INVALID_STATE";
    case PersistenceStatus::Refused:
      return "REFUSED";
  }
  return "IO_ERROR";
}

// ---------------------------------------------------------------------------
// Protocol
// ---------------------------------------------------------------------------

std::string_view to_string(MessageType value) noexcept {
  switch (value) {
    case MessageType::Invalid:
      return "INVALID";
    case MessageType::Hello:
      return "HELLO";
    case MessageType::HelloAck:
      return "HELLO_ACK";
    case MessageType::Register:
      return "REGISTER";
    case MessageType::RegisterAck:
      return "REGISTER_ACK";
    case MessageType::DeclareRack:
      return "DECLARE_RACK";
    case MessageType::PublishNode:
      return "PUBLISH_NODE";
    case MessageType::PublishDevice:
      return "PUBLISH_DEVICE";
    case MessageType::PublishLink:
      return "PUBLISH_LINK";
    case MessageType::PublishFailureDomain:
      return "PUBLISH_FAILURE_DOMAIN";
    case MessageType::PublishPower:
      return "PUBLISH_POWER";
    case MessageType::PublishCooling:
      return "PUBLISH_COOLING";
    case MessageType::PublishHealth:
      return "PUBLISH_HEALTH";
    case MessageType::PublishCapability:
      return "PUBLISH_CAPABILITY";
    case MessageType::Withdraw:
      return "WITHDRAW";
    case MessageType::RetireMember:
      return "RETIRE_MEMBER";
    case MessageType::Revalidate:
      return "REVALIDATE";
    case MessageType::Heartbeat:
      return "HEARTBEAT";
    case MessageType::Query:
      return "QUERY";
    case MessageType::SnapshotRequest:
      return "SNAPSHOT_REQUEST";
    case MessageType::SnapshotResponse:
      return "SNAPSHOT_RESPONSE";
    case MessageType::Result:
      return "RESULT";
    case MessageType::Error:
      return "ERROR";
  }
  return "INVALID";
}

std::optional<MessageType> message_type_from_value(std::uint16_t value) noexcept {
  if (value >= 1 && value <= 22) {
    return static_cast<MessageType>(value);
  }
  return std::nullopt;
}

std::string_view to_string(ProtocolError value) noexcept {
  switch (value) {
    case ProtocolError::None:
      return "NONE";
    case ProtocolError::BadMagic:
      return "BAD_MAGIC";
    case ProtocolError::UnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case ProtocolError::UnknownMessageType:
      return "UNKNOWN_MESSAGE_TYPE";
    case ProtocolError::OversizedFrame:
      return "OVERSIZED_FRAME";
    case ProtocolError::TruncatedFrame:
      return "TRUNCATED_FRAME";
    case ProtocolError::ChecksumMismatch:
      return "CHECKSUM_MISMATCH";
    case ProtocolError::MalformedPayload:
      return "MALFORMED_PAYLOAD";
    case ProtocolError::TrailingGarbage:
      return "TRAILING_GARBAGE";
    case ProtocolError::InvalidIdentity:
      return "INVALID_IDENTITY";
    case ProtocolError::CollectionTooLarge:
      return "COLLECTION_TOO_LARGE";
    case ProtocolError::StringTooLong:
      return "STRING_TOO_LONG";
    case ProtocolError::NotRegistered:
      return "NOT_REGISTERED";
    case ProtocolError::StaleCoordinatorEpoch:
      return "STALE_COORDINATOR_EPOCH";
    case ProtocolError::StaleWorkerBoot:
      return "STALE_WORKER_BOOT";
    case ProtocolError::StaleGeneration:
      return "STALE_GENERATION";
    case ProtocolError::NotAuthorized:
      return "NOT_AUTHORIZED";
    case ProtocolError::Conflict:
      return "CONFLICT";
    case ProtocolError::LimitExceeded:
      return "LIMIT_EXCEEDED";
    case ProtocolError::Unsupported:
      return "UNSUPPORTED";
    case ProtocolError::Internal:
      return "INTERNAL";
  }
  return "INTERNAL";
}

std::string_view to_string(DecodeStatus value) noexcept {
  switch (value) {
    case DecodeStatus::Ok:
      return "OK";
    case DecodeStatus::BadMagic:
      return "BAD_MAGIC";
    case DecodeStatus::UnsupportedVersion:
      return "UNSUPPORTED_VERSION";
    case DecodeStatus::UnknownMessageType:
      return "UNKNOWN_MESSAGE_TYPE";
    case DecodeStatus::OversizedFrame:
      return "OVERSIZED_FRAME";
    case DecodeStatus::TruncatedFrame:
      return "TRUNCATED_FRAME";
    case DecodeStatus::ChecksumMismatch:
      return "CHECKSUM_MISMATCH";
    case DecodeStatus::ReservedBitsSet:
      return "RESERVED_BITS_SET";
    case DecodeStatus::TrailingGarbage:
      return "TRAILING_GARBAGE";
  }
  return "TRUNCATED_FRAME";
}

}  // namespace rack_fabric
