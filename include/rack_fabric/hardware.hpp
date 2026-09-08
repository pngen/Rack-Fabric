// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Local hardware discovery.
//
// Discovery is deliberately narrow: it produces the rack member evidence that
// Rack Fabric is responsible for. It is not a universal inventory suite, and
// it never fabricates a fact the platform did not expose. Every unsupported
// observation is reported as unsupported rather than guessed.

#ifndef RACK_FABRIC_HARDWARE_HPP
#define RACK_FABRIC_HARDWARE_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/explanation.hpp"
#include "rack_fabric/failure_domain.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/member.hpp"

namespace rack_fabric {

struct HardwareDiscoveryOptions {
  std::optional<NodeId> node;
  std::optional<WorkerId> worker;
  std::optional<AgentBootId> boot;
  bool include_cpu = true;
  bool include_memory = true;
  bool include_nics = true;
  bool include_storage = true;
  bool include_accelerators = true;
  bool include_pci_devices = true;
  /// Lifetime of the discovered evidence. Discovery results are live
  /// observations of the process that ran them, so the default is ephemeral.
  std::chrono::milliseconds ttl{30'000};
  Durability durability = Durability::Ephemeral;
  std::string source_label = "hardware:local";
  FailureDomainId node_failure_domain;
};

struct HardwareDiscoveryResult {
  bool ok = false;
  std::vector<MemberRecord> members;
  std::vector<FailureDomainRecord> failure_domains;
  /// Facts the platform did not expose. Never silently replaced by a guess.
  std::vector<std::string> unsupported;
  std::vector<std::string> diagnostics;
  std::string host_label;
  Explanation explanation;
};

/// Discovers local hardware using platform APIs. On Windows this uses
/// GetSystemInfo, GetLogicalProcessorInformationEx, GlobalMemoryStatusEx,
/// GetAdaptersAddresses and the CUDA runtime when it is available.
[[nodiscard]] HardwareDiscoveryResult discover_local_hardware(const HardwareDiscoveryOptions& options);

/// Human-readable local host label, or an empty string when unavailable.
[[nodiscard]] std::string local_host_name();

/// Total physical memory in bytes, or std::nullopt when unavailable.
[[nodiscard]] std::optional<std::uint64_t> local_physical_memory_bytes();

}  // namespace rack_fabric

#endif  // RACK_FABRIC_HARDWARE_HPP
