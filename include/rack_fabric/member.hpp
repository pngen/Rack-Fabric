// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The rack component model.
//
// Rack Fabric models heterogeneous racks. No assumption is made about vendor,
// accelerator count per node, NIC count per node, switch layer count,
// topology shape, DPU model, interconnect existence, node homogeneity, power
// domain uniformity, cooling uniformity or failure-domain shape.

#ifndef RACK_FABRIC_MEMBER_HPP
#define RACK_FABRIC_MEMBER_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/lifecycle.hpp"

namespace rack_fabric {

enum class MemberKind : std::uint8_t {
  Node = 0,
  Accelerator = 1,
  CpuPackage = 2,
  MemoryDomain = 3,
  Nic = 4,
  Dpu = 5,
  Switch = 6,
  StorageEndpoint = 7,
  PowerDomain = 8,
  CoolingDomain = 9,
};

[[nodiscard]] std::string_view to_string(MemberKind value) noexcept;
[[nodiscard]] std::optional<MemberKind> member_kind_from_string(std::string_view) noexcept;
[[nodiscard]] constexpr bool is_device_kind(MemberKind value) noexcept {
  return value != MemberKind::Node && value != MemberKind::PowerDomain && value != MemberKind::CoolingDomain;
}

/// Canonical identity of one member: a kind plus a validated identity string.
struct MemberKey {
  MemberKind kind = MemberKind::Node;
  std::string id;

  [[nodiscard]] static std::optional<MemberKey> parse(MemberKind kind, std::string_view id) {
    if (!validate_identity(id).ok()) {
      return std::nullopt;
    }
    return MemberKey{kind, std::string(id)};
  }

  template <class Tag>
  [[nodiscard]] static MemberKey of(MemberKind kind, const StrongId<Tag>& id) {
    return MemberKey{kind, id.value()};
  }

  friend bool operator==(const MemberKey&, const MemberKey&) = default;
  friend auto operator<=>(const MemberKey&, const MemberKey&) = default;

  [[nodiscard]] std::string to_string() const;
};

struct MemberKeyHash {
  [[nodiscard]] std::size_t operator()(const MemberKey& key) const noexcept;
};

// ---------------------------------------------------------------------------
// Per-kind detail records. Every field is optional: an unknown field is
// represented by an absent value, never by a fabricated default.
// ---------------------------------------------------------------------------

enum class NodeRole : std::uint8_t { Unknown = 0, Compute = 1, Storage = 2, Management = 3, Mixed = 4 };
[[nodiscard]] std::string_view to_string(NodeRole value) noexcept;

enum class CpuArchitecture : std::uint8_t {
  Unknown = 0,
  X86_64 = 1,
  Aarch64 = 2,
  Riscv64 = 3,
  Other = 4,
};
[[nodiscard]] std::string_view to_string(CpuArchitecture value) noexcept;

struct NodeDetails {
  std::optional<std::string> host_name;
  std::optional<NodeRole> role;
  std::optional<CpuArchitecture> architecture;
  std::optional<std::string> operating_system;
  std::optional<std::uint64_t> memory_bytes;
  std::optional<std::uint32_t> cpu_package_count;
  std::optional<std::uint32_t> logical_cpu_count;
  std::optional<std::uint32_t> accelerator_count;
  std::optional<std::uint32_t> nic_count;
  std::optional<std::uint32_t> dpu_count;
  std::optional<WorkerId> worker;
  std::optional<AgentBootId> boot;

  friend bool operator==(const NodeDetails&, const NodeDetails&) = default;
};

struct AcceleratorDetails {
  std::optional<std::string> vendor;
  std::optional<std::string> model;
  std::optional<std::string> architecture;
  std::optional<std::string> uuid;
  std::optional<std::string> driver_version;
  std::optional<std::string> pci_address;
  std::optional<std::uint64_t> memory_bytes;
  std::optional<std::uint32_t> compute_capability_major;
  std::optional<std::uint32_t> compute_capability_minor;
  std::optional<std::uint32_t> sm_count;
  std::optional<std::uint32_t> max_threads_per_block;
  /// Interconnect actually observed for this device, when evidence exists.
  /// Absence means unknown, not "none".
  std::optional<std::string> interconnect;

  friend bool operator==(const AcceleratorDetails&, const AcceleratorDetails&) = default;
};

struct CpuPackageDetails {
  std::optional<std::string> vendor;
  std::optional<std::string> model;
  std::optional<std::uint32_t> physical_cores;
  std::optional<std::uint32_t> logical_threads;
  std::optional<std::uint32_t> base_clock_mhz;
  std::optional<std::uint32_t> socket_index;
  std::optional<std::uint32_t> numa_node;

  friend bool operator==(const CpuPackageDetails&, const CpuPackageDetails&) = default;
};

enum class MemoryKind : std::uint8_t { Unknown = 0, Ddr = 1, Hbm = 2, Lpddr = 3, Other = 4 };
[[nodiscard]] std::string_view to_string(MemoryKind value) noexcept;

struct MemoryDomainDetails {
  std::optional<MemoryKind> kind;
  std::optional<std::uint64_t> capacity_bytes;
  std::optional<std::uint32_t> numa_node;
  std::optional<Quantity> bandwidth;

  friend bool operator==(const MemoryDomainDetails&, const MemoryDomainDetails&) = default;
};

struct NicDetails {
  std::optional<std::string> vendor;
  std::optional<std::string> model;
  std::optional<std::string> pci_address;
  std::optional<std::uint32_t> port_count;
  std::optional<Quantity> link_speed;
  std::optional<std::string> mac_address;

  friend bool operator==(const NicDetails&, const NicDetails&) = default;
};

struct DpuDetails {
  std::optional<std::string> vendor;
  std::optional<std::string> model;
  std::optional<std::string> firmware_version;
  std::optional<std::string> pci_address;
  std::optional<std::uint32_t> port_count;

  friend bool operator==(const DpuDetails&, const DpuDetails&) = default;
};

struct SwitchDetails {
  std::optional<std::string> vendor;
  std::optional<std::string> model;
  std::optional<std::uint32_t> port_count;
  std::optional<std::uint32_t> layer_index;
  std::optional<Quantity> link_speed;

  friend bool operator==(const SwitchDetails&, const SwitchDetails&) = default;
};

enum class StorageKind : std::uint8_t { Unknown = 0, Nvme = 1, Sata = 2, Sas = 3, Network = 4, Other = 5 };
[[nodiscard]] std::string_view to_string(StorageKind value) noexcept;

struct StorageEndpointDetails {
  std::optional<StorageKind> kind;
  std::optional<std::string> model;
  std::optional<std::string> interface_name;
  std::optional<std::uint64_t> capacity_bytes;

  friend bool operator==(const StorageEndpointDetails&, const StorageEndpointDetails&) = default;
};

enum class PowerDomainKind : std::uint8_t {
  Unknown = 0,
  Rack = 1,
  Pdu = 2,
  Feed = 3,
  Busway = 4,
  Outlet = 5,
  Other = 6,
};
[[nodiscard]] std::string_view to_string(PowerDomainKind value) noexcept;

struct PowerDomainDetails {
  std::optional<PowerDomainKind> kind;
  std::optional<std::string> label;

  friend bool operator==(const PowerDomainDetails&, const PowerDomainDetails&) = default;
};

enum class CoolingDomainKind : std::uint8_t {
  Unknown = 0,
  Rack = 1,
  Zone = 2,
  Row = 3,
  Inlet = 4,
  Crac = 5,
  Other = 6,
};
[[nodiscard]] std::string_view to_string(CoolingDomainKind value) noexcept;

struct CoolingDomainDetails {
  std::optional<CoolingDomainKind> kind;
  std::optional<std::string> label;

  friend bool operator==(const CoolingDomainDetails&, const CoolingDomainDetails&) = default;
};

using MemberDetails = std::variant<std::monostate, NodeDetails, AcceleratorDetails, CpuPackageDetails,
                                   MemoryDomainDetails, NicDetails, DpuDetails, SwitchDetails,
                                   StorageEndpointDetails, PowerDomainDetails, CoolingDomainDetails>;

/// The detail alternative that corresponds to a member kind, or std::nullopt
/// when the kind and the detail record disagree.
[[nodiscard]] std::optional<MemberKind> member_kind_of_details(const MemberDetails& details) noexcept;
[[nodiscard]] bool details_match_kind(MemberKind kind, const MemberDetails& details) noexcept;

/// A capability reference published as evidence about a member. Rack Fabric
/// references capability identities owned by a capability registry; it does
/// not become that registry.
struct CapabilityRef {
  CapabilityId id;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
  bool revalidation_required = false;
  /// Optional declared value (a version, a mode, a limit) as asserted by the
  /// source. Absence means the capability is asserted to exist but its value
  /// is unknown.
  std::optional<std::string> value;

  [[nodiscard]] Freshness freshness_at(Timestamp now) const noexcept;

  friend bool operator==(const CapabilityRef&, const CapabilityRef&) = default;
};

/// One member of the rack.
struct MemberRecord {
  MemberKey key;
  MemberLifecycle lifecycle = MemberLifecycle::Unknown;
  MemberGeneration generation;
  PublicationGeneration publication_generation;
  EvidenceProvenance provenance = EvidenceProvenance::Unknown;
  Timestamp observed_at = Timestamp::unknown();
  std::chrono::milliseconds ttl{0};
  Durability durability = Durability::Durable;
  bool revalidation_required = false;
  RevalidationReason revalidation_reason = RevalidationReason::None;

  /// Owner of the evidence. Absent for declarations made directly by an
  /// operator through the in-process API.
  std::optional<WorkerId> owner_worker;
  std::optional<AgentBootId> owner_boot;

  /// Containment parent. A device belongs to a node; a node may belong to no
  /// parent. Cycles are rejected.
  std::optional<MemberKey> parent;

  std::vector<FailureDomainId> failure_domains;
  std::optional<PowerDomainId> power_domain;
  std::optional<CoolingDomainId> cooling_domain;
  std::optional<SwitchId> switch_domain;

  EvidenceValue<HealthState> health;
  EvidenceValue<ReadinessState> readiness;
  EvidenceValue<ReachabilityState> reachability;
  std::vector<CapabilityRef> capabilities;

  MemberDetails details;

  /// Human-readable label of the source that produced the current evidence
  /// (for example "agent:node-01" or "hardware:cuda").
  std::optional<std::string> source;

  [[nodiscard]] Freshness freshness_at(Timestamp now) const noexcept;
  [[nodiscard]] bool is_current_at(Timestamp now) const noexcept;
  /// True when the member is present, its evidence is current and no
  /// revalidation is outstanding.
  [[nodiscard]] bool is_authoritatively_current_at(Timestamp now) const noexcept;

  friend bool operator==(const MemberRecord&, const MemberRecord&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_MEMBER_HPP
