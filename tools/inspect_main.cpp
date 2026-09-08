// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// rack_fabric_inspect: deterministic inspection and explanation.
//
// Every command is read-only with respect to authoritative state unless it
// explicitly publishes into a private in-process instance. Output is stable:
// records are ordered by canonical identity, never by container iteration
// order.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "rack_fabric/hardware.hpp"
#include "rack_fabric/persistence.hpp"
#include "rack_fabric/rack_fabric.hpp"
#include "rack_fabric/synthetic.hpp"
#include "rack_fabric/version.hpp"

#ifdef RACK_FABRIC_HAS_CUDA
#include "rack_fabric/cuda_probe.hpp"
#endif

namespace {

struct Options {
  std::string state_path;
  std::string validate_path;
  bool inspect_state = false;
  bool host = false;
  bool cuda = false;
  bool synthetic = false;
  bool help = false;
  bool json = false;
  std::uint64_t seed = 1;
  std::size_t nodes = 8;
  std::size_t accelerators = 2;
  std::size_t switches = 2;
};

void print_usage() {
  std::cout
      << "rack_fabric_inspect " << rack_fabric::kVersionString << "\n"
      << "Usage: rack_fabric_inspect <command> [options]\n"
      << "Commands:\n"
      << "  --state <path>          inspect a persisted state file\n"
      << "  --validate <path>       load a persisted state file and report the recovered rack\n"
      << "  --host                  discover local hardware and print the member evidence\n"
      << "  --cuda                  probe CUDA devices and run the accelerator compute proof\n"
      << "  --synthetic             generate a synthetic rack and print its summary\n"
      << "Options:\n"
      << "  --seed <n>              synthetic seed (default 1)\n"
      << "  --nodes <n>             synthetic node count (default 8)\n"
      << "  --accelerators <n>      synthetic accelerators per node (default 2)\n"
      << "  --switches <n>          synthetic switch count (default 2)\n"
      << "  --json                  machine-readable output\n"
      << "  --version               print the version and exit\n"
      << "  --help                  print this message\n";
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&index, argc, argv]() -> std::optional<std::string> {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      return std::string(argv[++index]);
    };
    if (argument == "--state") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.state_path = *value;
      options.inspect_state = true;
    } else if (argument == "--validate") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.validate_path = *value;
    } else if (argument == "--host") {
      options.host = true;
    } else if (argument == "--cuda") {
      options.cuda = true;
    } else if (argument == "--synthetic") {
      options.synthetic = true;
    } else if (argument == "--seed") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.seed = std::stoull(*value);
    } else if (argument == "--nodes") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.nodes = static_cast<std::size_t>(std::stoull(*value));
    } else if (argument == "--accelerators") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.accelerators = static_cast<std::size_t>(std::stoull(*value));
    } else if (argument == "--switches") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.switches = static_cast<std::size_t>(std::stoull(*value));
    } else if (argument == "--json") {
      options.json = true;
    } else if (argument == "--version") {
      std::cout << rack_fabric::kVersionString << "\n";
      std::exit(0);
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else {
      std::cerr << "unknown argument: " << argument << "\n";
      return false;
    }
  }
  return true;
}

void print_summary(const rack_fabric::RackSummary& summary, bool json) {
  if (json) {
    std::cout << "{";
    std::cout << "\"lifecycle\":\"" << rack_fabric::to_string(summary.lifecycle) << "\",";
    std::cout << "\"rack\":\"" << (summary.rack.has_value() ? summary.rack->value() : "") << "\",";
    std::cout << "\"rack_epoch\":\""
              << (summary.rack_epoch.has_value() ? summary.rack_epoch->value() : "") << "\",";
    std::cout << "\"members\":" << summary.member_count << ",";
    std::cout << "\"nodes\":" << summary.node_count << ",";
    std::cout << "\"accelerators\":" << summary.accelerator_count << ",";
    std::cout << "\"relationships\":" << summary.relationship_count << ",";
    std::cout << "\"failure_domains\":" << summary.failure_domain_count << ",";
    std::cout << "\"publishers\":" << summary.publisher_count << ",";
    std::cout << "\"fenced_boots\":" << summary.fenced_boot_count << ",";
    std::cout << "\"snapshots\":" << summary.snapshot_count << ",";
    std::cout << "\"revalidation_required\":" << summary.revalidation_required_member_count << ",";
    std::cout << "\"stale_members\":" << summary.stale_member_count << ",";
    std::cout << "\"digest\":\"" << summary.digest << "\"";
    std::cout << "}\n";
    return;
  }
  std::cout << "lifecycle: " << rack_fabric::to_string(summary.lifecycle) << "\n";
  std::cout << "rack: " << (summary.rack.has_value() ? summary.rack->value() : "<none>") << "\n";
  std::cout << "rack_epoch: "
            << (summary.rack_epoch.has_value() ? summary.rack_epoch->value() : "<none>") << "\n";
  std::cout << "members: " << summary.member_count << " (nodes " << summary.node_count
            << ", accelerators " << summary.accelerator_count << ", cpu packages "
            << summary.cpu_package_count << ", memory domains " << summary.memory_domain_count
            << ", nics " << summary.nic_count << ", dpus " << summary.dpu_count << ", switches "
            << summary.switch_count << ", storage " << summary.storage_endpoint_count << ")\n";
  std::cout << "present: " << summary.present_member_count << " unavailable "
            << summary.unavailable_member_count << " retired " << summary.retired_member_count
            << " revalidation_required " << summary.revalidation_required_member_count
            << " stale " << summary.stale_member_count << "\n";
  std::cout << "relationships: " << summary.relationship_count << "\n";
  std::cout << "failure_domains: " << summary.failure_domain_count << "\n";
  std::cout << "publishers: " << summary.publisher_count << " active "
            << summary.active_publisher_count << " fenced_boots " << summary.fenced_boot_count
            << "\n";
  std::cout << "power_envelope: " << (summary.power_envelope_known ? "known" : "unknown")
            << " cooling_envelope: " << (summary.cooling_envelope_known ? "known" : "unknown")
            << "\n";
  std::cout << "snapshots: " << summary.snapshot_count << "\n";
  std::cout << "digest: " << summary.digest << "\n";
}

int command_inspect_state(const Options& options) {
  rack_fabric::PersistedStateInfo info;
  const rack_fabric::PersistenceResult result =
      rack_fabric::inspect_persisted_state(options.state_path, info);
  if (result.status != rack_fabric::PersistenceStatus::Ok) {
    std::cerr << "state inspection failed: " << result.explanation.render() << "\n";
    return 1;
  }
  if (options.json) {
    std::cout << "{";
    std::cout << "\"format_version\":" << info.format_version << ",";
    std::cout << "\"rack\":\"" << (info.rack.has_value() ? info.rack->value() : "") << "\",";
    std::cout << "\"rack_epoch\":\""
              << (info.rack_epoch.has_value() ? info.rack_epoch->value() : "") << "\",";
    std::cout << "\"lifecycle\":\"" << rack_fabric::to_string(info.lifecycle) << "\",";
    std::cout << "\"members\":" << info.member_count << ",";
    std::cout << "\"relationships\":" << info.relationship_count << ",";
    std::cout << "\"failure_domains\":" << info.failure_domain_count << ",";
    std::cout << "\"publishers\":" << info.publisher_count << ",";
    std::cout << "\"fenced_boots\":" << info.fenced_boot_count << ",";
    std::cout << "\"encoded_bytes\":" << info.encoded_bytes << ",";
    std::cout << "\"content_digest\":\"" << info.content_digest << "\"";
    std::cout << "}\n";
    return 0;
  }
  std::cout << "path: " << result.path << "\n";
  std::cout << "format_version: " << info.format_version << "\n";
  std::cout << "rack: " << (info.rack.has_value() ? info.rack->value() : "<none>") << "\n";
  std::cout << "rack_epoch: "
            << (info.rack_epoch.has_value() ? info.rack_epoch->value() : "<none>") << "\n";
  std::cout << "lifecycle: " << rack_fabric::to_string(info.lifecycle) << "\n";
  std::cout << "members: " << info.member_count << " relationships: " << info.relationship_count
            << " failure_domains: " << info.failure_domain_count << "\n";
  std::cout << "publishers: " << info.publisher_count << " fenced_boots: "
            << info.fenced_boot_count << "\n";
  std::cout << "coordinator_epoch: " << info.generations.coordinator_epoch.value() << "\n";
  std::cout << "encoded_bytes: " << info.encoded_bytes << "\n";
  std::cout << "content_digest: " << info.content_digest << "\n";
  return 0;
}

int command_validate(const Options& options) {
  rack_fabric::RackFabricOptions fabric_options;
  rack_fabric::RackFabric fabric(fabric_options);
  const rack_fabric::PersistenceResult loaded = fabric.load_state(options.validate_path);
  if (loaded.status != rack_fabric::PersistenceStatus::Ok) {
    std::cerr << "state recovery failed: " << loaded.explanation.render() << "\n";
    return 1;
  }
  print_summary(fabric.summary(), options.json);
  std::cout << "readiness: " << fabric.explain_readiness().render() << "\n";
  const rack_fabric::InvariantReport report = fabric.check_invariants();
  std::cout << "invariants: " << (report.ok() ? "ok" : "violated") << " checks="
            << report.checks_run << "\n";
  if (!report.ok()) {
    std::cout << report.render();
    return 1;
  }
  return 0;
}

int command_host(const Options& options) {
  const rack_fabric::HardwareDiscoveryResult discovery =
      rack_fabric::discover_local_hardware(rack_fabric::HardwareDiscoveryOptions{});
  if (options.json) {
    std::cout << "{\"host\":\"" << discovery.host_label << "\",\"members\":[";
    bool first = true;
    for (const auto& member : discovery.members) {
      if (!first) {
        std::cout << ",";
      }
      first = false;
      std::cout << "{\"kind\":\"" << rack_fabric::to_string(member.key.kind) << "\",\"id\":\""
                << member.key.id << "\",\"provenance\":\""
                << rack_fabric::to_string(member.provenance) << "\",\"lifecycle\":\""
                << rack_fabric::to_string(member.lifecycle) << "\"}";
    }
    std::cout << "],\"unsupported\":[";
    first = true;
    for (const auto& item : discovery.unsupported) {
      if (!first) {
        std::cout << ",";
      }
      first = false;
      std::cout << "\"" << item << "\"";
    }
    std::cout << "]}\n";
  } else {
    std::cout << "host: " << discovery.host_label << "\n";
    for (const auto& member : discovery.members) {
      std::cout << "member " << rack_fabric::to_string(member.key.kind) << " " << member.key.id
                << " provenance " << rack_fabric::to_string(member.provenance) << " lifecycle "
                << rack_fabric::to_string(member.lifecycle) << "\n";
    }
    for (const auto& item : discovery.unsupported) {
      std::cout << "unsupported: " << item << "\n";
    }
  }
  return discovery.ok ? 0 : 1;
}

int command_cuda(const Options& options) {
#ifdef RACK_FABRIC_HAS_CUDA
  const rack_fabric::CudaProbeResult probe = rack_fabric::probe_cuda_devices();
  std::cout << "cuda_available: " << (probe.available ? "yes" : "no") << "\n";
  std::cout << "runtime: " << probe.runtime_version << " driver: " << probe.driver_version << "\n";
  std::cout << "device_count: " << probe.device_count << "\n";
  for (const auto& device : probe.devices) {
    std::cout << "device " << device.index << ": " << device.name << " uuid " << device.uuid
              << " pci " << device.pci_bus_id << " memory " << device.memory_bytes
              << " capability " << device.compute_capability_major << "."
              << device.compute_capability_minor << " sm " << device.multiprocessor_count << "\n";
  }
  for (const auto& diagnostic : probe.diagnostics) {
    std::cout << "diagnostic: " << diagnostic << "\n";
  }
  if (probe.device_count == 0) {
    return 1;
  }
  const rack_fabric::CudaComputeProof proof = rack_fabric::run_cuda_compute_proof(0);
  std::cout << "compute_proof: " << (proof.ok ? "ok" : "failed") << "\n";
  std::cout << "free_before: " << proof.device_memory_free_before
            << " free_after: " << proof.device_memory_free_after << "\n";
  std::cout << "max_absolute_error: " << std::setprecision(9) << proof.max_absolute_error << "\n";
  for (const auto& step : proof.steps) {
    std::cout << "step: " << step << "\n";
  }
  if (!proof.detail.empty()) {
    std::cout << "detail: " << proof.detail << "\n";
  }
  (void)options;
  return proof.ok ? 0 : 1;
#else
  (void)options;
  std::cout << "cuda_available: no (this build has no CUDA module)\n";
  return 1;
#endif
}

int command_synthetic(const Options& options) {
  rack_fabric::SyntheticRackConfig config;
  config.seed = options.seed;
  config.node_count = options.nodes;
  config.min_accelerators_per_node = options.accelerators;
  config.max_accelerators_per_node = options.accelerators;
  config.switch_count = options.switches;
  config.synthetic_accelerator_peer_links = true;
  const rack_fabric::SyntheticRack rack = rack_fabric::generate_synthetic_rack(config);

  rack_fabric::RackFabricOptions fabric_options;
  fabric_options.readiness.require_physical_provenance = false;
  rack_fabric::RackFabric fabric(fabric_options);
  rack_fabric::AuthorityToken authority;
  authority.coordinator_epoch = fabric.coordinator_epoch();
  authority.rack = rack.rack;
  const rack_fabric::SyntheticPublishSummary published =
      rack_fabric::publish_synthetic_rack(fabric, rack, authority);
  std::cout << "reproduction: " << rack.reproduction_parameters << "\n";
  std::cout << "published members " << published.members_accepted << " relationships "
            << published.relationships_accepted << " failure_domains "
            << published.failure_domains_accepted << " rejections " << published.rejections.size()
            << "\n";
  for (const auto& rejection : published.rejections) {
    std::cout << "rejection: " << rack_fabric::to_string(rejection.outcome) << " "
              << rejection.explanation.render() << "\n";
  }
  print_summary(fabric.summary(), options.json);
  std::cout << "readiness: " << fabric.explain_readiness().render() << "\n";
  return published.rejections.empty() ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 2;
  }
  if (options.help || argc == 1) {
    print_usage();
    return argc == 1 ? 2 : 0;
  }
  int exit_code = 0;
  bool ran = false;
  if (options.inspect_state) {
    exit_code = command_inspect_state(options);
    ran = true;
  }
  if (!options.validate_path.empty()) {
    exit_code = std::max(exit_code, command_validate(options));
    ran = true;
  }
  if (options.host) {
    exit_code = std::max(exit_code, command_host(options));
    ran = true;
  }
  if (options.cuda) {
    exit_code = std::max(exit_code, command_cuda(options));
    ran = true;
  }
  if (options.synthetic) {
    exit_code = std::max(exit_code, command_synthetic(options));
    ran = true;
  }
  if (!ran) {
    print_usage();
    return 2;
  }
  return exit_code;
}
