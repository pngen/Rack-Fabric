// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Shared helpers for the test suites. Everything here builds records and
// authority tokens through the public API only.

#ifndef RACK_FABRIC_TEST_SUPPORT_HPP
#define RACK_FABRIC_TEST_SUPPORT_HPP

#include <chrono>
#include <string>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"

namespace rf_test {

using namespace rack_fabric;

inline constexpr std::int64_t kObservationMillis = 1'700'000'000'000LL;

[[nodiscard]] inline Timestamp observed() {
  return Timestamp::from_unix_millis(kObservationMillis);
}

[[nodiscard]] inline MemberRecord member(MemberKind kind, const std::string& id,
                                         EvidenceProvenance provenance = EvidenceProvenance::Measured,
                                         MemberLifecycle lifecycle = MemberLifecycle::Present,
                                         Timestamp at = observed()) {
  MemberRecord record;
  record.key = MemberKey{kind, id};
  record.lifecycle = lifecycle;
  record.provenance = provenance;
  record.observed_at = provenance == EvidenceProvenance::Unknown ? Timestamp::unknown() : at;
  record.ttl = std::chrono::milliseconds{0};
  record.durability = Durability::Durable;
  switch (kind) {
    case MemberKind::Node: {
      NodeDetails details;
      details.host_name = id;
      details.role = NodeRole::Compute;
      details.architecture = CpuArchitecture::X86_64;
      record.details = details;
      break;
    }
    case MemberKind::Accelerator: {
      AcceleratorDetails details;
      details.vendor = "vendor";
      details.model = "model";
      details.memory_bytes = static_cast<std::uint64_t>(64) << 30U;
      record.details = details;
      break;
    }
    case MemberKind::CpuPackage: {
      CpuPackageDetails details;
      details.vendor = "vendor";
      details.physical_cores = 32;
      record.details = details;
      break;
    }
    case MemberKind::MemoryDomain: {
      MemoryDomainDetails details;
      details.kind = MemoryKind::Ddr;
      details.capacity_bytes = static_cast<std::uint64_t>(512) << 30U;
      record.details = details;
      break;
    }
    case MemberKind::Nic: {
      NicDetails details;
      details.vendor = "vendor";
      details.port_count = 2;
      record.details = details;
      break;
    }
    case MemberKind::Dpu: {
      DpuDetails details;
      details.vendor = "vendor";
      details.port_count = 2;
      record.details = details;
      break;
    }
    case MemberKind::Switch: {
      SwitchDetails details;
      details.vendor = "vendor";
      details.port_count = 64;
      record.details = details;
      break;
    }
    case MemberKind::StorageEndpoint: {
      StorageEndpointDetails details;
      details.kind = StorageKind::Nvme;
      details.capacity_bytes = static_cast<std::uint64_t>(4) << 40U;
      record.details = details;
      break;
    }
    case MemberKind::PowerDomain: {
      PowerDomainDetails details;
      details.kind = PowerDomainKind::Pdu;
      details.label = id;
      record.details = details;
      break;
    }
    case MemberKind::CoolingDomain: {
      CoolingDomainDetails details;
      details.kind = CoolingDomainKind::Zone;
      details.label = id;
      record.details = details;
      break;
    }
  }
  return record;
}

[[nodiscard]] inline AuthorityToken operator_token(const RackFabric& fabric,
                                                   const std::string& rack) {
  AuthorityToken token;
  token.coordinator_epoch = fabric.coordinator_epoch();
  token.rack = RackId{rack};
  return token;
}

inline void declare_rack(RackFabric& fabric, const std::string& rack_id, const std::string& epoch,
                         bool redeclare = false) {
  DeclareRackRequest request;
  request.authority = operator_token(fabric, rack_id);
  request.epoch = RackEpochId{epoch};
  request.redeclare = redeclare;
  const MutationResult result = fabric.declare_rack(request);
  RF_REQUIRE(result.accepted());
}

struct Publisher {
  AuthorityToken token;
  WorkerId worker;
  AgentBootId boot;
};

[[nodiscard]] inline Publisher register_publisher(RackFabric& fabric, const std::string& rack,
                                                  const std::string& worker_id,
                                                  const std::string& boot_id) {
  Publisher publisher;
  publisher.worker = WorkerId{worker_id};
  publisher.boot = AgentBootId{boot_id};
  RegisterPublisherRequest request;
  request.rack = RackId{rack};
  request.worker = publisher.worker;
  request.boot = publisher.boot;
  request.coordinator_epoch = fabric.coordinator_epoch();
  const MutationResult result = fabric.register_publisher(request);
  RF_CHECK(result.accepted());
  if (!result.accepted()) {
    rftest::record_failure(__FILE__, __LINE__,
                           "publisher registration failed: " + result.explanation.render());
    return publisher;
  }
  publisher.token.coordinator_epoch = fabric.coordinator_epoch();
  publisher.token.boot = publisher.boot;
  publisher.token.rack = RackId{rack};
  publisher.token.publication_generation = PublicationGeneration::first();
  return publisher;
}

[[nodiscard]] inline FailureDomainRecord failure_domain(const std::string& id,
                                                        FailureDomainKind kind,
                                                        EvidenceProvenance provenance =
                                                            EvidenceProvenance::Measured) {
  FailureDomainRecord record;
  record.id = FailureDomainId{id};
  record.kind = kind;
  record.provenance = provenance;
  record.observed_at = provenance == EvidenceProvenance::Unknown ? Timestamp::unknown() : observed();
  record.ttl = std::chrono::milliseconds{0};
  record.durability = Durability::Durable;
  return record;
}

/// Advances a token exactly as a live publisher must after an accepted
/// publication. The protocol fences stale publication generations, so a test
/// that publishes twice through the same process must advance its token.
inline void advance(AuthorityToken& token) {
  if (!token.publication_generation.has_value()) {
    return;
  }
  const auto next = token.publication_generation->next();
  if (next.has_value()) {
    token.publication_generation = *next;
  }
}

inline void publish_domain(RackFabric& fabric, const AuthorityToken& authority,
                           const FailureDomainRecord& record) {
  PublishFailureDomainRequest request;
  request.authority = authority;
  request.record = record;
  const MutationResult result = fabric.publish_failure_domain(request);
  RF_REQUIRE(result.accepted());
}

inline void publish_domain(RackFabric& fabric, AuthorityToken& authority,
                           const FailureDomainRecord& record) {
  PublishFailureDomainRequest request;
  request.authority = authority;
  request.record = record;
  const MutationResult result = fabric.publish_failure_domain(request);
  RF_REQUIRE(result.accepted());
  advance(authority);
}

inline void publish_member(RackFabric& fabric, const AuthorityToken& authority,
                           const MemberRecord& record) {
  PublishMemberRequest request;
  request.authority = authority;
  request.record = record;
  const MutationResult result = fabric.publish_member(request);
  RF_REQUIRE(result.accepted());
}

inline void publish_member(RackFabric& fabric, AuthorityToken& authority,
                           const MemberRecord& record) {
  PublishMemberRequest request;
  request.authority = authority;
  request.record = record;
  const MutationResult result = fabric.publish_member(request);
  RF_REQUIRE(result.accepted());
  advance(authority);
}

/// Publishes a rack with the requested number of nodes, each with current
/// measured evidence, and returns the authority used.
[[nodiscard]] inline AuthorityToken publish_rack_with_nodes(RackFabric& fabric,
                                                            const std::string& rack_id,
                                                            const std::string& epoch,
                                                            std::size_t nodes) {
  declare_rack(fabric, rack_id, epoch);
  const AuthorityToken authority = operator_token(fabric, rack_id);
  for (std::size_t index = 0; index < nodes; ++index) {
    publish_member(fabric, authority, member(MemberKind::Node, "node-" + std::to_string(index)));
  }
  return authority;
}

}  // namespace rf_test

#endif  // RACK_FABRIC_TEST_SUPPORT_HPP
