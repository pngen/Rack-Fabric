// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The narrow CUDA boundary.
//
// Every CUDA runtime call lives in the .cu translation unit. The rest of the
// project sees only plain C++ types, so Rack Fabric does not impose a CUDA
// dependency on any consumer that does not ask for accelerator evidence.

#ifndef RACK_FABRIC_CUDA_BACKEND_HPP
#define RACK_FABRIC_CUDA_BACKEND_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rack_fabric::cuda {

struct BackendDeviceInfo {
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
  // CUDA 13 no longer exposes clock rates through cudaDeviceProp.

  int memory_bus_width_bits = 0;
  int l2_cache_bytes = 0;
};

struct BackendProbeResult {
  bool available = false;
  std::string runtime_version;
  std::string driver_version;
  int device_count = 0;
  std::vector<BackendDeviceInfo> devices;
  std::vector<std::string> diagnostics;
};

struct BackendComputeProof {
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

[[nodiscard]] BackendProbeResult probe_devices();

[[nodiscard]] BackendComputeProof run_compute_proof(int device_index, std::size_t elements);

}  // namespace rack_fabric::cuda

#endif  // RACK_FABRIC_CUDA_BACKEND_HPP
