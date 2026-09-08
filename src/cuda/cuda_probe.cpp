// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_fabric/cuda_probe.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "cuda/cuda_backend.hpp"

namespace rack_fabric {
namespace {

[[nodiscard]] std::string accelerator_identity(const std::string& uuid, std::size_t index) {
  if (!uuid.empty()) {
    std::string text = "acc-" + uuid;
    if (text.size() > kMaxIdentityLength) {
      text.resize(kMaxIdentityLength);
    }
    return text;
  }
  return "acc-gpu-" + std::to_string(index);
}

}  // namespace

CudaProbeResult probe_cuda_devices() {
  const cuda::BackendProbeResult backend = cuda::probe_devices();
  CudaProbeResult result;
  result.available = backend.available;
  result.runtime_version = backend.runtime_version;
  result.driver_version = backend.driver_version;
  result.device_count = backend.device_count;
  result.diagnostics = backend.diagnostics;
  result.devices.reserve(backend.devices.size());
  for (const auto& device : backend.devices) {
    CudaDeviceInfo info;
    info.index = device.index;
    info.name = device.name;
    info.uuid = device.uuid;
    info.pci_bus_id = device.pci_bus_id;
    info.memory_bytes = device.memory_bytes;
    info.compute_capability_major = device.compute_capability_major;
    info.compute_capability_minor = device.compute_capability_minor;
    info.multiprocessor_count = device.multiprocessor_count;
    info.max_threads_per_block = device.max_threads_per_block;
    info.warp_size = device.warp_size;
    info.memory_bus_width_bits = device.memory_bus_width_bits;
    info.l2_cache_bytes = device.l2_cache_bytes;
    info.architecture_label = "sm_" + std::to_string(device.compute_capability_major) +
                              std::to_string(device.compute_capability_minor);
    result.devices.push_back(std::move(info));
  }
  return result;
}

CudaComputeProof run_cuda_compute_proof(int device_index, std::size_t elements) {
  const cuda::BackendComputeProof backend = cuda::run_compute_proof(device_index, elements);
  CudaComputeProof proof;
  proof.ok = backend.ok;
  proof.device_index = backend.device_index;
  proof.device_name = backend.device_name;
  proof.elements = backend.elements;
  proof.device_memory_free_before = backend.device_memory_free_before;
  proof.device_memory_free_after = backend.device_memory_free_after;
  proof.max_absolute_error = backend.max_absolute_error;
  proof.steps = backend.steps;
  proof.detail = backend.detail;
  return proof;
}

CudaEvidenceResult discover_cuda_accelerator_members(const CudaEvidenceOptions& options) {
  CudaEvidenceResult result;
  result.probe = probe_cuda_devices();
  if (!result.probe.available) {
    result.explanation = Explanation::failure(
        "CUDA_RUNTIME_UNAVAILABLE", "cuda",
        {ExplanationFactor{"CUDA_RUNTIME_UNAVAILABLE",
                           "the CUDA runtime is not available on this host"}});
    return result;
  }
  if (result.probe.device_count == 0) {
    result.explanation = Explanation::failure(
        "CUDA_NO_DEVICE", "cuda",
        {ExplanationFactor{"CUDA_NO_DEVICE", "the CUDA runtime reported no devices"}});
    return result;
  }
  const Timestamp observed_at = default_clock().now();
  for (const auto& device : result.probe.devices) {
    const std::string id = accelerator_identity(device.uuid, device.index);
    const auto parsed = AcceleratorId::parse(id);
    if (!parsed.has_value()) {
      result.probe.diagnostics.push_back("device " + std::to_string(device.index) +
                                         " produced an invalid accelerator identity");
      continue;
    }
    MemberRecord record;
    record.key = MemberKey::of(MemberKind::Accelerator, *parsed);
    record.lifecycle = MemberLifecycle::Present;
    record.provenance = EvidenceProvenance::Measured;
    record.observed_at = observed_at;
    record.ttl = options.ttl;
    record.durability = options.durability;
    record.source = options.source_label;
    record.owner_worker = options.worker;
    record.owner_boot = options.boot;
    if (options.node.has_value()) {
      record.parent = MemberKey::of(MemberKind::Node, *options.node);
    }
    AcceleratorDetails details;
    details.vendor = "nvidia";
    details.model = device.name;
    details.architecture = device.architecture_label;
    if (!device.uuid.empty()) {
      details.uuid = device.uuid;
    }
    if (!device.pci_bus_id.empty()) {
      details.pci_address = device.pci_bus_id;
    }
    details.memory_bytes = device.memory_bytes;
    details.compute_capability_major = static_cast<std::uint32_t>(device.compute_capability_major);
    details.compute_capability_minor = static_cast<std::uint32_t>(device.compute_capability_minor);
    details.sm_count = static_cast<std::uint32_t>(device.multiprocessor_count);
    details.max_threads_per_block = static_cast<std::uint32_t>(device.max_threads_per_block);
    if (!result.probe.driver_version.empty()) {
      details.driver_version = result.probe.driver_version;
    }
    // interconnect is left absent: Rack Fabric does not assume NVLink, and
    // the CUDA runtime does not report a negotiated interconnect here.
    record.details = details;

    CapabilityRef capability{*CapabilityId::parse("cuda-compute-capability"),
                             EvidenceProvenance::Measured, observed_at, options.ttl,
                             options.durability, false,
                             std::to_string(device.compute_capability_major) + "." +
                                 std::to_string(device.compute_capability_minor)};
    record.capabilities.push_back(std::move(capability));
    if (!result.probe.runtime_version.empty()) {
      record.capabilities.push_back(CapabilityRef{*CapabilityId::parse("cuda-runtime-version"),
                                                  EvidenceProvenance::Measured, observed_at,
                                                  options.ttl, options.durability, false,
                                                  result.probe.runtime_version});
    }
    result.members.push_back(std::move(record));
  }
  std::sort(result.members.begin(), result.members.end(),
            [](const MemberRecord& lhs, const MemberRecord& rhs) { return lhs.key < rhs.key; });
  result.ok = !result.members.empty();
  if (result.ok) {
    result.explanation = Explanation::success(
        "CUDA_DEVICES_DISCOVERED", std::to_string(result.members.size()) + " accelerator members");
  } else {
    result.explanation = Explanation::failure(
        "CUDA_NO_MEMBER", "cuda",
        {ExplanationFactor{"CUDA_NO_MEMBER",
                           "no accelerator member could be produced from the discovered devices"}});
  }
  return result;
}

}  // namespace rack_fabric
