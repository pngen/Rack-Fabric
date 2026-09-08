// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// CUDA accelerator evidence.
//
// This is not a CUDA compute framework. It exists to bind real local
// accelerator evidence to authoritative rack members, and to prove that a
// real device can be discovered, exercised and fenced. All API calls are the
// CUDA runtime API; no CUDA-specific abstraction is imposed on Rack Fabric.

#ifndef RACK_FABRIC_CUDA_PROBE_HPP
#define RACK_FABRIC_CUDA_PROBE_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/explanation.hpp"
#include "rack_fabric/member.hpp"

namespace rack_fabric {

struct CudaDeviceInfo {
  int index = -1;
  std::string name;
  std::string uuid;
  std::string pci_bus_id;
  std::uint64_t memory_bytes = 0;
  int compute_capability_major = 0;
  int compute_capability_minor = 0;
  int multiprocessor_count = 0;
  int max_threads_per_block = 0;
  int warp_size = 0;
  /// CUDA 13 removed the clock-rate properties from cudaDeviceProp; they are
  /// deliberately absent rather than reported as zero.

  int memory_bus_width_bits = 0;
  int l2_cache_bytes = 0;
  std::string architecture_label;
};

struct CudaProbeResult {
  bool available = false;
  std::string runtime_version;
  std::string driver_version;
  int device_count = 0;
  std::vector<CudaDeviceInfo> devices;
  std::vector<std::string> diagnostics;
};

/// Probes the CUDA runtime. Never throws; a missing runtime is reported as
/// unavailable with diagnostics.
[[nodiscard]] CudaProbeResult probe_cuda_devices();

/// Executes a real CUDA kernel: allocate device memory, copy host data to the
/// device, launch a kernel, synchronise, copy the result back, compare against
/// a CPU reference and free all device memory. Reports the device memory
/// usage before and after so that cleanup can be verified.
struct CudaComputeProof {
  bool ok = false;
  int device_index = -1;
  std::string device_name;
  std::size_t elements = 0;
  std::uint64_t device_memory_free_before = 0;
  std::uint64_t device_memory_free_after = 0;
  double max_absolute_error = 0.0;
  std::vector<std::string> steps;
  std::string detail;
};

[[nodiscard]] CudaComputeProof run_cuda_compute_proof(int device_index, std::size_t elements = 1U << 20);

struct CudaEvidenceOptions {
  std::optional<NodeId> node;
  std::optional<WorkerId> worker;
  std::optional<AgentBootId> boot;
  std::chrono::milliseconds ttl{30'000};
  Durability durability = Durability::Ephemeral;
  std::string source_label = "hardware:cuda";
};

struct CudaEvidenceResult {
  bool ok = false;
  CudaProbeResult probe;
  std::vector<MemberRecord> members;
  Explanation explanation;
};

/// Converts real CUDA device discovery into rack accelerator members. A
/// device that cannot be probed produces no member and a diagnostic; it never
/// produces a synthetic member.
[[nodiscard]] CudaEvidenceResult discover_cuda_accelerator_members(const CudaEvidenceOptions& options);

}  // namespace rack_fabric

#endif  // RACK_FABRIC_CUDA_PROBE_HPP
