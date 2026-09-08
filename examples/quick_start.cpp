// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A minimal end-to-end example: declare a rack, publish heterogeneous member
// evidence, evaluate the readiness contract, produce and validate a snapshot,
// persist the state and recover it in a new instance.

#include <cstdint>
#include <iostream>
#include <string>

#include "rack_fabric/rack_fabric.hpp"

namespace {

rack_fabric::MemberRecord make_node(const std::string& id) {
  rack_fabric::MemberRecord record;
  record.key = *rack_fabric::MemberKey::parse(rack_fabric::MemberKind::Node, id);
  record.lifecycle = rack_fabric::MemberLifecycle::Present;
  record.provenance = rack_fabric::EvidenceProvenance::Measured;
  record.observed_at = rack_fabric::default_clock().now();
  record.ttl = std::chrono::milliseconds{0};
  record.durability = rack_fabric::Durability::Durable;
  rack_fabric::NodeDetails details;
  details.host_name = id + ".example.invalid";
  details.role = rack_fabric::NodeRole::Compute;
  details.architecture = rack_fabric::CpuArchitecture::X86_64;
  details.memory_bytes = static_cast<std::uint64_t>(1024) << 30U;
  record.details = details;
  record.failure_domains.push_back(*rack_fabric::FailureDomainId::parse("fd-node-" + id));
  return record;
}

}  // namespace

int main() {
  rack_fabric::RackFabric fabric;

  rack_fabric::AuthorityToken authority;
  authority.coordinator_epoch = fabric.coordinator_epoch();

  rack_fabric::DeclareRackRequest declare;
  declare.authority = authority;
  declare.epoch = *rack_fabric::RackEpochId::parse("epoch-1");
  authority.rack = *rack_fabric::RackId::parse("rack-example");
  declare.authority = authority;
  declare.label = "example rack";
  const rack_fabric::MutationResult declared = fabric.declare_rack(declare);
  std::cout << "declare: " << rack_fabric::to_string(declared.outcome) << "\n";
  if (!declared.accepted()) {
    return 1;
  }

  // Failure domains are published before the members that reference them: a
  // member that names an unknown failure domain is rejected, not accepted.
  for (int index = 0; index < 4; ++index) {
    rack_fabric::PublishFailureDomainRequest request;
    request.authority = authority;
    request.record.id = *rack_fabric::FailureDomainId::parse("fd-node-node-" + std::to_string(index));
    request.record.kind = rack_fabric::FailureDomainKind::Node;
    request.record.provenance = rack_fabric::EvidenceProvenance::Reported;
    request.record.observed_at = rack_fabric::default_clock().now();
    request.record.durability = rack_fabric::Durability::Durable;
    const rack_fabric::MutationResult result = fabric.publish_failure_domain(request);
    if (!result.accepted()) {
      std::cout << "failure domain " << request.record.id.value() << ": "
                << rack_fabric::to_string(result.outcome) << " " << result.explanation.code << "\n";
    }
  }

  for (int index = 0; index < 4; ++index) {
    rack_fabric::PublishMemberRequest request;
    request.authority = authority;
    request.record = make_node("node-" + std::to_string(index));
    const rack_fabric::MutationResult result = fabric.publish_member(request);
    std::cout << "publish " << request.record.key.to_string() << ": "
              << rack_fabric::to_string(result.outcome) << "\n";
  }

  const rack_fabric::RackSummary summary = fabric.summary();
  std::cout << "lifecycle: " << rack_fabric::to_string(summary.lifecycle) << " members "
            << summary.member_count << " digest " << summary.digest << "\n";
  std::cout << "readiness: " << fabric.explain_readiness().render() << "\n";

  rack_fabric::SnapshotRequest snapshot_request;
  snapshot_request.authority = authority;
  const rack_fabric::SnapshotResult snapshot = fabric.publish_snapshot(snapshot_request);
  if (snapshot.snapshot.has_value()) {
    const rack_fabric::SnapshotValidation validation = fabric.validate_snapshot(*snapshot.snapshot);
    std::cout << "snapshot " << snapshot.snapshot->id().value() << ": "
              << rack_fabric::to_string(validation.status) << "\n";
  }

  const std::string path = "rack_fabric_quick_start.state";
  const rack_fabric::PersistenceResult saved = fabric.save_state(path);
  std::cout << "save: " << rack_fabric::to_string(saved.status) << " bytes "
            << saved.bytes_written << "\n";

  rack_fabric::RackFabric recovered;
  const rack_fabric::PersistenceResult loaded = recovered.load_state(path);
  std::cout << "load: " << rack_fabric::to_string(loaded.status) << "\n";
  std::cout << "recovered lifecycle: "
            << rack_fabric::to_string(recovered.lifecycle()) << " epoch "
            << recovered.coordinator_epoch().value() << "\n";
  std::cout << "recovered readiness: " << recovered.explain_readiness().render() << "\n";

  std::error_code error;
  std::filesystem::remove(path, error);
  return 0;
}
