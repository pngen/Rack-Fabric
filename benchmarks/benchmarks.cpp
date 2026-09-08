// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Benchmarks measure completed operations, not scheduled work: every number
// printed here is the count of operations that returned, divided by the wall
// time they took. Nothing is extrapolated and nothing is timed out.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "rack_fabric/rack_fabric.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Row {
  std::string name;
  std::uint64_t components = 0;
  std::uint64_t relationships = 0;
  std::uint64_t operations = 0;
  double milliseconds = 0.0;
};

void print_row(const Row& row) {
  const double seconds = row.milliseconds / 1000.0;
  const double per_second = seconds > 0.0 ? static_cast<double>(row.operations) / seconds : 0.0;
  std::printf("benchmark=%-28s components=%-7llu relationships=%-7llu operations=%-9llu "
              "total_ms=%-10.3f ops_per_second=%.0f\n",
              row.name.c_str(), static_cast<unsigned long long>(row.components),
              static_cast<unsigned long long>(row.relationships),
              static_cast<unsigned long long>(row.operations), row.milliseconds, per_second);
  std::fflush(stdout);
}

[[nodiscard]] rack_fabric::MemberRecord make_member(rack_fabric::MemberKind kind,
                                                    const std::string& id,
                                                    const rack_fabric::MemberKey* parent) {
  rack_fabric::MemberRecord record;
  record.key = rack_fabric::MemberKey{kind, id};
  record.lifecycle = rack_fabric::MemberLifecycle::Present;
  record.provenance = rack_fabric::EvidenceProvenance::Synthetic;
  record.observed_at = rack_fabric::default_clock().now();
  record.ttl = std::chrono::milliseconds{0};
  record.durability = rack_fabric::Durability::Durable;
  if (parent != nullptr) {
    record.parent = *parent;
  }
  switch (kind) {
    case rack_fabric::MemberKind::Node: {
      rack_fabric::NodeDetails details;
      details.host_name = id;
      details.role = rack_fabric::NodeRole::Compute;
      details.architecture = rack_fabric::CpuArchitecture::X86_64;
      record.details = details;
      break;
    }
    case rack_fabric::MemberKind::Accelerator: {
      rack_fabric::AcceleratorDetails details;
      details.vendor = "synthetic";
      details.model = "bench-accelerator";
      details.memory_bytes = static_cast<std::uint64_t>(80) << 30U;
      details.compute_capability_major = 9;
      details.compute_capability_minor = 0;
      record.details = details;
      break;
    }
    case rack_fabric::MemberKind::Nic: {
      rack_fabric::NicDetails details;
      details.vendor = "synthetic";
      details.port_count = 2;
      record.details = details;
      break;
    }
    case rack_fabric::MemberKind::Switch: {
      rack_fabric::SwitchDetails details;
      details.vendor = "synthetic";
      details.port_count = 64;
      record.details = details;
      break;
    }
    default:
      break;
  }
  return record;
}

struct Population {
  std::vector<rack_fabric::MemberKey> nodes;
  std::vector<rack_fabric::MemberKey> accelerators;
};

[[nodiscard]] Population populate(rack_fabric::RackFabric& fabric,
                                  const rack_fabric::AuthorityToken& authority,
                                  std::uint64_t target_components, std::uint64_t& published,
                                  std::uint64_t& rejected) {
  Population population;
  const std::uint64_t node_count = std::max<std::uint64_t>(1, target_components / 10);
  for (std::uint64_t index = 0; index < node_count; ++index) {
    const std::string node_id = "node-" + std::to_string(index);
    const rack_fabric::MemberKey node_key{rack_fabric::MemberKind::Node, node_id};
    rack_fabric::PublishMemberRequest node_request;
    node_request.authority = authority;
    node_request.record = make_member(rack_fabric::MemberKind::Node, node_id, nullptr);
    if (fabric.publish_member(node_request).accepted()) {
      ++published;
    } else {
      ++rejected;
    }
    population.nodes.push_back(node_key);
    for (int device = 0; device < 9; ++device) {
      const rack_fabric::MemberKind kind =
          device < 4   ? rack_fabric::MemberKind::Accelerator
          : device < 6 ? rack_fabric::MemberKind::Nic
                       : rack_fabric::MemberKind::CpuPackage;
      const std::string id = node_id + "-dev-" + std::to_string(device);
      rack_fabric::PublishMemberRequest request;
      request.authority = authority;
      request.record = make_member(kind, id, &node_key);
      if (fabric.publish_member(request).accepted()) {
        ++published;
      } else {
        ++rejected;
      }
      if (kind == rack_fabric::MemberKind::Accelerator) {
        population.accelerators.push_back(rack_fabric::MemberKey{kind, id});
      }
    }
  }
  return population;
}

void benchmark_components(std::uint64_t target_components) {
  rack_fabric::RackFabricOptions options;
  options.readiness.require_physical_provenance = false;
  rack_fabric::RackFabric fabric(options);
  rack_fabric::AuthorityToken authority;
  authority.coordinator_epoch = fabric.coordinator_epoch();
  authority.rack = *rack_fabric::RackId::parse("bench-rack");

  rack_fabric::DeclareRackRequest declare;
  declare.authority = authority;
  declare.epoch = *rack_fabric::RackEpochId::parse("bench-epoch");
  if (!fabric.declare_rack(declare).accepted()) {
    std::cerr << "benchmark setup failed: the rack could not be declared\n";
    return;
  }

  std::uint64_t published = 0;
  std::uint64_t rejected = 0;
  const auto publish_start = Clock::now();
  const Population population = populate(fabric, authority, target_components, published, rejected);
  const auto publish_end = Clock::now();
  const double publish_ms =
      std::chrono::duration<double, std::milli>(publish_end - publish_start).count();
  print_row(Row{"publish_member", published, 0, published, publish_ms});
  if (rejected != 0) {
    std::cerr << "benchmark " << target_components << " rejected " << rejected << " members\n";
  }

  {
    const std::uint64_t operations = 200;
    const auto start = Clock::now();
    std::uint64_t members_seen = 0;
    for (std::uint64_t index = 0; index < operations; ++index) {
      members_seen += fabric.summary().member_count;
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    print_row(Row{"summary", published, 0, operations, ms});
    if (members_seen == 0) {
      std::cerr << "summary returned no members\n";
    }
  }
  {
    const std::uint64_t operations = 100000;
    const auto start = Clock::now();
    std::uint64_t found = 0;
    for (std::uint64_t index = 0; index < operations; ++index) {
      const auto& key = population.nodes[index % population.nodes.size()];
      if (fabric.find_member(key).has_value()) {
        ++found;
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    print_row(Row{"find_member", published, 0, operations, ms});
    if (found != operations) {
      std::cerr << "find_member missed " << (operations - found) << " lookups\n";
    }
  }
  {
    const std::uint64_t operations = 200;
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < operations; ++index) {
      const auto report = fabric.check_invariants();
      if (!report.ok()) {
        std::cerr << "invariant violation during benchmark\n";
        break;
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    print_row(Row{"check_invariants", published, 0, operations, ms});
  }
  {
    const std::uint64_t operations = 100;
    const auto start = Clock::now();
    for (std::uint64_t index = 0; index < operations; ++index) {
      rack_fabric::SnapshotRequest request;
      request.authority = authority;
      const rack_fabric::SnapshotResult snapshot = fabric.publish_snapshot(request);
      if (snapshot.snapshot.has_value()) {
        const auto validation = fabric.validate_snapshot(*snapshot.snapshot);
        if (validation.status != rack_fabric::SnapshotValidationStatus::Current) {
          std::cerr << "snapshot validation was not current\n";
        }
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    print_row(Row{"snapshot_publish_validate", published, 0, operations, ms});
  }
  {
    const std::string path =
        (std::filesystem::temp_directory_path() / "rack_fabric_bench.state").string();
    const std::uint64_t operations = 20;
    const auto start = Clock::now();
    std::uint64_t bytes = 0;
    for (std::uint64_t index = 0; index < operations; ++index) {
      const auto saved = fabric.save_state(path);
      bytes += saved.bytes_written;
      if (!saved.ok()) {
        std::cerr << "save_state failed: " << to_string(saved.status) << " "
                  << saved.explanation.render();
        break;
      }
    }
    const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    print_row(Row{"save_state", published, 0, operations, ms});

    const auto load_start = Clock::now();
    std::uint64_t loaded_members = 0;
    for (std::uint64_t index = 0; index < operations; ++index) {
      rack_fabric::RackFabric recovered(options);
      const auto loaded = recovered.load_state(path);
      if (loaded.status == rack_fabric::PersistenceStatus::Ok) {
        loaded_members += recovered.summary().member_count;
      } else {
        std::cerr << "load_state failed: " << to_string(loaded.status) << " "
                  << loaded.explanation.render();
        break;
      }
    }
    const double load_ms = std::chrono::duration<double, std::milli>(Clock::now() - load_start).count();
    print_row(Row{"load_state", published, 0, operations, load_ms});
    std::error_code error;
    std::filesystem::remove(path, error);
    if (loaded_members == 0 || bytes == 0) {
      std::cerr << "persistence benchmark produced no data\n";
    }
  }
}

void benchmark_relationships(std::uint64_t target_relationships) {
  rack_fabric::RackFabricOptions options;
  options.readiness.require_physical_provenance = false;
  options.limits.max_relationships = 4'000'000;
  rack_fabric::RackFabric fabric(options);
  rack_fabric::AuthorityToken authority;
  authority.coordinator_epoch = fabric.coordinator_epoch();
  authority.rack = *rack_fabric::RackId::parse("bench-rack-rel");

  rack_fabric::DeclareRackRequest declare;
  declare.authority = authority;
  declare.epoch = *rack_fabric::RackEpochId::parse("bench-epoch");
  if (!fabric.declare_rack(declare).accepted()) {
    std::cerr << "relationship benchmark setup failed\n";
    return;
  }

  const std::uint64_t node_count = std::max<std::uint64_t>(1, target_relationships / 20);
  std::vector<rack_fabric::MemberKey> keys;
  for (std::uint64_t index = 0; index < node_count; ++index) {
    const std::string id = "node-" + std::to_string(index);
    rack_fabric::PublishMemberRequest request;
    request.authority = authority;
    request.record = make_member(rack_fabric::MemberKind::Node, id, nullptr);
    if (!fabric.publish_member(request).accepted()) {
      std::cerr << "relationship benchmark could not publish a node\n";
      return;
    }
    keys.push_back(rack_fabric::MemberKey{rack_fabric::MemberKind::Node, id});
  }

  std::uint64_t published = 0;
  std::uint64_t rejected = 0;
  const auto start = Clock::now();
  for (std::uint64_t index = 0; index < target_relationships; ++index) {
    const auto& from = keys[index % keys.size()];
    const auto& to = keys[(index * 7 + 1) % keys.size()];
    if (from == to) {
      continue;
    }
    rack_fabric::PublishRelationshipRequest request;
    request.authority = authority;
    request.record.key = rack_fabric::RelationshipKey::canonicalize(
        rack_fabric::RelationshipClass::ConnectedTo, from, to);
    request.record.provenance = rack_fabric::EvidenceProvenance::Synthetic;
    request.record.observed_at = rack_fabric::default_clock().now();
    request.record.ttl = std::chrono::milliseconds{0};
    request.record.durability = rack_fabric::Durability::Durable;
    if (fabric.publish_relationship(request).accepted()) {
      ++published;
    } else {
      ++rejected;
    }
  }
  const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
  print_row(Row{"publish_relationship", node_count, published, published, ms});
  if (rejected != 0) {
    std::cerr << "relationship benchmark rejected " << rejected << " relationships\n";
  }

  {
    const std::uint64_t operations = 100000;
    const auto traversal_start = Clock::now();
    std::uint64_t seen = 0;
    for (std::uint64_t index = 0; index < operations; ++index) {
      seen += fabric.relationships_touching(keys[index % keys.size()]).size();
    }
    const double traversal_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - traversal_start).count();
    print_row(Row{"relationships_touching", node_count, published, operations, traversal_ms});
    if (seen == 0) {
      std::cerr << "relationship traversal returned nothing\n";
    }
  }
  {
    const std::uint64_t operations = 100;
    const auto invariant_start = Clock::now();
    for (std::uint64_t index = 0; index < operations; ++index) {
      if (!fabric.check_invariants().ok()) {
        std::cerr << "invariant violation in the relationship benchmark\n";
        break;
      }
    }
    const double invariant_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - invariant_start).count();
    print_row(Row{"check_invariants", node_count, published, operations, invariant_ms});
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::uint64_t relationship_target = 100000;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--relationships" && index + 1 < argc) {
      relationship_target = std::stoull(argv[++index]);
    }
  }
  std::printf("rack_fabric benchmarks: completed operations only, no extrapolation\n");
  for (const std::uint64_t components : {100ULL, 1000ULL, 10000ULL}) {
    benchmark_components(components);
  }
  benchmark_relationships(relationship_target);
  return 0;
}
