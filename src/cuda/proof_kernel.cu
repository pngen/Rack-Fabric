// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The real accelerator proof: allocate, copy, launch, synchronise, copy back,
// compare against a CPU reference, free, and verify that the device memory was
// returned.

#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "cuda/cuda_backend.hpp"

namespace rack_fabric::cuda {
namespace {

__global__ void scale_and_offset_kernel(const float* input, float* output,
                                        unsigned long long count, float factor) {
  const unsigned long long index =
      static_cast<unsigned long long>(blockIdx.x) * blockDim.x + threadIdx.x;
  if (index < count) {
    output[index] = input[index] * factor + 1.0f;
  }
}

[[nodiscard]] std::string error_text(cudaError_t status, const char* operation) {
  std::string out(operation);
  out += ": ";
  out += cudaGetErrorName(status);
  out += " (";
  out += cudaGetErrorString(status);
  out += ")";
  return out;
}

void version_string(int version, std::string& out) {
  char buffer[32] = {};
  std::snprintf(buffer, sizeof(buffer), "%d.%d", version / 1000, (version % 1000) / 10);
  out = buffer;
}

}  // namespace

BackendProbeResult probe_devices() {
  BackendProbeResult result;
  const cudaError_t init = cudaFree(nullptr);
  if (init != cudaSuccess) {
    result.diagnostics.push_back(error_text(init, "cudaFree(nullptr)"));
    return result;
  }
  result.available = true;
  int runtime_version = 0;
  if (cudaRuntimeGetVersion(&runtime_version) == cudaSuccess) {
    version_string(runtime_version, result.runtime_version);
  }
  int driver_version = 0;
  if (cudaDriverGetVersion(&driver_version) == cudaSuccess) {
    version_string(driver_version, result.driver_version);
  }
  int count = 0;
  const cudaError_t counted = cudaGetDeviceCount(&count);
  if (counted != cudaSuccess) {
    result.diagnostics.push_back(error_text(counted, "cudaGetDeviceCount"));
    return result;
  }
  result.device_count = count;
  for (int index = 0; index < count; ++index) {
    cudaDeviceProp properties{};
    const cudaError_t status = cudaGetDeviceProperties(&properties, index);
    if (status != cudaSuccess) {
      result.diagnostics.push_back(error_text(status, "cudaGetDeviceProperties"));
      continue;
    }
    BackendDeviceInfo device;
    device.index = index;
    device.name = properties.name;
    device.memory_bytes = static_cast<std::uint64_t>(properties.totalGlobalMem);
    device.compute_capability_major = properties.major;
    device.compute_capability_minor = properties.minor;
    device.multiprocessor_count = properties.multiProcessorCount;
    device.max_threads_per_block = properties.maxThreadsPerBlock;
    device.warp_size = properties.warpSize;
    device.memory_bus_width_bits = properties.memoryBusWidth;
    device.l2_cache_bytes = properties.l2CacheSize;
    char uuid[64] = {};
    if (properties.uuid.bytes[0] != 0 || properties.uuid.bytes[15] != 0) {
      std::snprintf(uuid, sizeof(uuid),
                    "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                    static_cast<unsigned char>(properties.uuid.bytes[0]),
                    static_cast<unsigned char>(properties.uuid.bytes[1]),
                    static_cast<unsigned char>(properties.uuid.bytes[2]),
                    static_cast<unsigned char>(properties.uuid.bytes[3]),
                    static_cast<unsigned char>(properties.uuid.bytes[4]),
                    static_cast<unsigned char>(properties.uuid.bytes[5]),
                    static_cast<unsigned char>(properties.uuid.bytes[6]),
                    static_cast<unsigned char>(properties.uuid.bytes[7]),
                    static_cast<unsigned char>(properties.uuid.bytes[8]),
                    static_cast<unsigned char>(properties.uuid.bytes[9]),
                    static_cast<unsigned char>(properties.uuid.bytes[10]),
                    static_cast<unsigned char>(properties.uuid.bytes[11]),
                    static_cast<unsigned char>(properties.uuid.bytes[12]),
                    static_cast<unsigned char>(properties.uuid.bytes[13]),
                    static_cast<unsigned char>(properties.uuid.bytes[14]),
                    static_cast<unsigned char>(properties.uuid.bytes[15]));
      device.uuid = uuid;
    }
    char pci[32] = {};
    std::snprintf(pci, sizeof(pci), "%04x:%02x:%02x.0", properties.pciDomainID,
                  properties.pciBusID, properties.pciDeviceID);
    device.pci_bus_id = pci;
    result.devices.push_back(std::move(device));
  }
  return result;
}

BackendComputeProof run_compute_proof(int device_index, std::size_t elements) {
  BackendComputeProof proof;
  proof.device_index = device_index;
  proof.elements = elements;
  if (elements == 0) {
    proof.detail = "the element count must be positive";
    return proof;
  }
  cudaError_t status = cudaFree(nullptr);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaFree(nullptr)");
    return proof;
  }
  int count = 0;
  status = cudaGetDeviceCount(&count);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaGetDeviceCount");
    return proof;
  }
  if (device_index < 0 || device_index >= count) {
    proof.detail = "the requested device index does not exist";
    return proof;
  }
  status = cudaSetDevice(device_index);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaSetDevice");
    return proof;
  }
  cudaDeviceProp properties{};
  status = cudaGetDeviceProperties(&properties, device_index);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaGetDeviceProperties");
    return proof;
  }
  proof.device_name = properties.name;
  proof.steps.push_back("device selected: " + proof.device_name);

  std::size_t free_before = 0;
  std::size_t total_before = 0;
  status = cudaMemGetInfo(&free_before, &total_before);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaMemGetInfo");
    return proof;
  }
  proof.device_memory_free_before = static_cast<std::uint64_t>(free_before);

  const std::size_t bytes = elements * sizeof(float);
  std::vector<float> host_input(elements);
  std::vector<float> host_output(elements, 0.0f);
  constexpr float kFactor = 3.0f;
  for (std::size_t index = 0; index < elements; ++index) {
    host_input[index] = static_cast<float>(index % 1024U) * 0.5f;
  }
  proof.steps.push_back("host reference prepared: " + std::to_string(elements) + " elements");

  float* device_input = nullptr;
  float* device_output = nullptr;
  status = cudaMalloc(reinterpret_cast<void**>(&device_input), bytes);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaMalloc(input)");
    return proof;
  }
  status = cudaMalloc(reinterpret_cast<void**>(&device_output), bytes);
  if (status != cudaSuccess) {
    cudaFree(device_input);
    proof.detail = error_text(status, "cudaMalloc(output)");
    return proof;
  }
  proof.steps.push_back("device memory allocated: " + std::to_string(bytes) + " bytes per buffer");

  status = cudaMemcpy(device_input, host_input.data(), bytes, cudaMemcpyHostToDevice);
  if (status != cudaSuccess) {
    cudaFree(device_input);
    cudaFree(device_output);
    proof.detail = error_text(status, "cudaMemcpy(host to device)");
    return proof;
  }
  proof.steps.push_back("host to device copy complete");

  const int threads_per_block = 256;
  const unsigned long long count_ull = static_cast<unsigned long long>(elements);
  const unsigned long long blocks_ull = (count_ull + threads_per_block - 1) / threads_per_block;
  if (blocks_ull > 0x7FFFFFFFULL) {
    cudaFree(device_input);
    cudaFree(device_output);
    proof.detail = "the launch geometry exceeds the supported grid size";
    return proof;
  }
  const unsigned int blocks = static_cast<unsigned int>(blocks_ull);
  scale_and_offset_kernel<<<blocks, threads_per_block>>>(device_input, device_output, count_ull,
                                                         kFactor);
  status = cudaGetLastError();
  if (status != cudaSuccess) {
    cudaFree(device_input);
    cudaFree(device_output);
    proof.detail = error_text(status, "kernel launch");
    return proof;
  }
  proof.steps.push_back("kernel launched: " + std::to_string(blocks) + " blocks of " +
                        std::to_string(threads_per_block) + " threads");

  status = cudaDeviceSynchronize();
  if (status != cudaSuccess) {
    cudaFree(device_input);
    cudaFree(device_output);
    proof.detail = error_text(status, "cudaDeviceSynchronize");
    return proof;
  }
  proof.steps.push_back("device synchronised");

  status = cudaMemcpy(host_output.data(), device_output, bytes, cudaMemcpyDeviceToHost);
  if (status != cudaSuccess) {
    cudaFree(device_input);
    cudaFree(device_output);
    proof.detail = error_text(status, "cudaMemcpy(device to host)");
    return proof;
  }
  proof.steps.push_back("device to host copy complete");

  double max_error = 0.0;
  for (std::size_t index = 0; index < elements; ++index) {
    const float expected = host_input[index] * kFactor + 1.0f;
    const double difference = std::fabs(static_cast<double>(host_output[index] - expected));
    if (difference > max_error) {
      max_error = difference;
    }
  }
  proof.max_absolute_error = max_error;
  proof.steps.push_back("CPU comparison complete: max absolute error " + std::to_string(max_error));

  status = cudaFree(device_input);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaFree(input)");
    return proof;
  }
  status = cudaFree(device_output);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaFree(output)");
    return proof;
  }
  proof.steps.push_back("device memory freed");

  std::size_t free_after = 0;
  std::size_t total_after = 0;
  status = cudaMemGetInfo(&free_after, &total_after);
  if (status != cudaSuccess) {
    proof.detail = error_text(status, "cudaMemGetInfo");
    return proof;
  }
  proof.device_memory_free_after = static_cast<std::uint64_t>(free_after);
  proof.steps.push_back("device memory free after cleanup: " + std::to_string(free_after));

  if (max_error > 1e-3) {
    proof.detail = "the device result does not match the CPU reference";
    return proof;
  }
  proof.ok = true;
  proof.detail = "device execution matched the CPU reference";
  return proof;
}

}  // namespace rack_fabric::cuda
