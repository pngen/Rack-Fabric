// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_fabric/synthetic.hpp"

#include <algorithm>
#include <cstdint>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace rack_fabric {
namespace {

constexpr std::int64_t kSyntheticObservation = 1'700'000'000'000LL;

[[nodiscard]] Timestamp synthetic_time() {
  return Timestamp::from_unix_millis(kSyntheticObservation);
}

[[nodiscard]] Quantity synthetic_quantity(double value, QuantityUnit unit) {
  Quantity quantity;
  quantity.value = value;
  quantity.unit = unit;
  quantity.provenance = EvidenceProvenance::Synthetic;
  quantity.observed_at = synthetic_time();
  quantity.ttl = std::chrono::milliseconds{0};
  quantity.durability = Durability::Durable;
  return quantity;
}

template <class T>
[[nodiscard]] EvidenceValue<T> synthetic_evidence(T value) {
  EvidenceValue<T> evidence;
  evidence.value = value;
  evidence.provenance = EvidenceProvenance::Synthetic;
  evidence.observed_at = synthetic_time();
  evidence.ttl = std::chrono::milliseconds{0};
  evidence.durability = Durability::Durable;
  return evidence;
}

[[nodiscard]] MemberRecord base_member(MemberKind kind, std::string id, std::string source) {
  MemberRecord record;
  record.key = MemberKey{kind, std::move(id)};
  record.lifecycle = MemberLifecycle::Present;
  record.provenance = EvidenceProvenance::Synthetic;
  record.observed_at = synthetic_time();
  record.ttl = std::chrono::milliseconds{0};
  record.durability = Durability::Durable;
  record.health = synthetic_evidence(HealthState::Healthy);
  record.readiness = synthetic_evidence(ReadinessState::Ready);
  record.reachability = synthetic_evidence(ReachabilityState::Reachable);
  record.source = std::move(source);
  return record;
}

[[nodiscard]] RelationshipRecord relationship(RelationshipClass cls, const MemberKey& from,
                                              const MemberKey& to, std::string source) {
  RelationshipRecord record;
  record.key = RelationshipKey::canonicalize(cls, from, to);
  record.provenance = EvidenceProvenance::Synthetic;
  record.observed_at = synthetic_time();
  record.ttl = std::chrono::milliseconds{0};
  record.durability = Durability::Durable;
  record.source = std::move(source);
  return record;
}

[[nodiscard]] FailureDomainRecord domain(std::string id, FailureDomainKind kind,
                                         std::vector<FailureDomainId> parents, std::string label) {
  const auto parsed = FailureDomainId::parse(id);
  FailureDomainRecord record{*parsed};
  record.kind = kind;
  record.lifecycle = MemberLifecycle::Present;
  record.parents = std::move(parents);
  record.provenance = EvidenceProvenance::Synthetic;
  record.observed_at = synthetic_time();
  record.ttl = std::chrono::milliseconds{0};
  record.durability = Durability::Durable;
  record.label = std::move(label);
  return record;
}

[[nodiscard]] std::string index_name(std::string_view prefix, std::size_t index) {
  std::string out(prefix);
  out += '-';
  out += std::to_string(index);
  return out;
}

[[nodiscard]] std::string nested_name(std::string_view prefix, std::size_t outer, std::size_t inner) {
  std::string out(prefix);
  out += '-';
  out += std::to_string(outer);
  out += '-';
  out += std::to_string(inner);
  return out;
}

}  // namespace

std::string synthetic_reproduction_parameters(const SyntheticRackConfig& config) {
  std::ostringstream out;
  out << "seed=" << config.seed << " nodes=" << config.node_count
      << " accelerators=[" << config.min_accelerators_per_node << ','
      << config.max_accelerators_per_node << "] nics=[" << config.min_nics_per_node << ','
      << config.max_nics_per_node << "] dpu=" << config.dpu_count
      << " switches=" << config.switch_count << " storage=" << config.storage_endpoint_count
      << " power_domains=" << config.power_domain_count
      << " cooling_domains=" << config.cooling_domain_count
      << " cpu_per_node=" << config.cpu_packages_per_node
      << " mem_per_node=" << config.memory_domains_per_node
      << " heterogeneity=" << config.heterogeneity << " missing_evidence=" << config.missing_evidence
      << " degraded_fraction=" << config.degraded_fraction << " missing_links=" << config.missing_links
      << " peer_links=" << (config.synthetic_accelerator_peer_links ? 1 : 0)
      << " power_envelope=" << (config.include_power_envelope ? 1 : 0)
      << " cooling_envelope=" << (config.include_cooling_envelope ? 1 : 0);
  return out.str();
}

SyntheticRack generate_synthetic_rack(const SyntheticRackConfig& config) {
  SyntheticRack rack{
      config.rack.value_or(*RackId::parse("synth-rack")),
      config.rack_epoch.value_or(*RackEpochId::parse("synth-epoch")),
      {},
      {},
      {},
      std::nullopt,
      std::nullopt,
      config.seed,
      synthetic_reproduction_parameters(config)};

  std::mt19937_64 engine(config.seed);
  const auto unit = [&engine]() {
    return std::uniform_real_distribution<double>(0.0, 1.0)(engine);
  };
  const auto pick = [&engine](std::size_t low, std::size_t high) {
    if (high <= low) {
      return low;
    }
    return std::uniform_int_distribution<std::size_t>(low, high)(engine);
  };

  const std::string rack_domain_id = "fd-rack";
  rack.failure_domains.push_back(
      domain(rack_domain_id, FailureDomainKind::Rack, {}, "synthetic rack failure domain"));

  std::vector<FailureDomainId> power_domain_fd;
  for (std::size_t p = 0; p < config.power_domain_count; ++p) {
    const std::string id = index_name("fd-power", p);
    rack.failure_domains.push_back(domain(id, FailureDomainKind::Pdu,
                                          {*FailureDomainId::parse(rack_domain_id)},
                                          "synthetic power feed domain"));
    power_domain_fd.push_back(*FailureDomainId::parse(id));
  }
  std::vector<FailureDomainId> cooling_domain_fd;
  for (std::size_t c = 0; c < config.cooling_domain_count; ++c) {
    const std::string id = index_name("fd-cooling", c);
    rack.failure_domains.push_back(domain(id, FailureDomainKind::CoolingZone,
                                          {*FailureDomainId::parse(rack_domain_id)},
                                          "synthetic cooling zone"));
    cooling_domain_fd.push_back(*FailureDomainId::parse(id));
  }
  for (std::size_t s = 0; s < config.switch_count; ++s) {
    rack.failure_domains.push_back(domain(index_name("fd-switch", s), FailureDomainKind::Switch,
                                          {*FailureDomainId::parse(rack_domain_id)},
                                          "synthetic switch domain"));
  }
  for (std::size_t n = 0; n < config.node_count; ++n) {
    rack.failure_domains.push_back(domain(index_name("fd-node", n), FailureDomainKind::Node,
                                          {*FailureDomainId::parse(rack_domain_id)},
                                          "synthetic node domain"));
  }
  for (std::size_t n = 0; n + 1 < config.node_count; n += 2) {
    rack.failure_domains.push_back(domain(index_name("fd-chassis", n / 2), FailureDomainKind::Chassis,
                                          {*FailureDomainId::parse(rack_domain_id)},
                                          "synthetic chassis domain"));
  }

  // Power and cooling domain members.
  std::vector<PowerDomainId> power_domains;
  for (std::size_t p = 0; p < config.power_domain_count; ++p) {
    const std::string id = index_name("pdu", p);
    MemberRecord record = base_member(MemberKind::PowerDomain, id, "synthetic:power");
    PowerDomainDetails details;
    details.kind = PowerDomainKind::Pdu;
    details.label = "synthetic PDU " + std::to_string(p);
    record.details = details;
    record.failure_domains = {power_domain_fd[p]};
    power_domains.push_back(*PowerDomainId::parse(id));
    rack.members.push_back(std::move(record));
  }
  std::vector<CoolingDomainId> cooling_domains;
  for (std::size_t c = 0; c < config.cooling_domain_count; ++c) {
    const std::string id = index_name("cooling", c);
    MemberRecord record = base_member(MemberKind::CoolingDomain, id, "synthetic:cooling");
    CoolingDomainDetails details;
    details.kind = CoolingDomainKind::Zone;
    details.label = "synthetic cooling zone " + std::to_string(c);
    record.details = details;
    record.failure_domains = {cooling_domain_fd[c]};
    cooling_domains.push_back(*CoolingDomainId::parse(id));
    rack.members.push_back(std::move(record));
  }

  std::vector<MemberKey> node_keys;
  std::vector<std::vector<MemberKey>> accelerators_by_node(config.node_count);
  std::vector<MemberKey> switch_keys;

  const std::vector<CpuArchitecture> architectures{CpuArchitecture::X86_64, CpuArchitecture::Aarch64,
                                                   CpuArchitecture::Riscv64, CpuArchitecture::Other};
  const std::vector<std::string> accelerator_vendors{"nvidia", "amd", "intel", "other"};

  for (std::size_t n = 0; n < config.node_count; ++n) {
    const bool heterogeneous = unit() < config.heterogeneity;
    const bool missing = unit() < config.missing_evidence;
    const bool degraded = unit() < config.degraded_fraction;
    const std::string node_id = index_name("node", n);
    const MemberKey node_key = *MemberKey::parse(MemberKind::Node, node_id);
    node_keys.push_back(node_key);

    MemberRecord node = base_member(MemberKind::Node, node_id, "synthetic:node");
    NodeDetails node_details;
    node_details.host_name = node_id + ".synthetic.invalid";
    node_details.role = heterogeneous ? NodeRole::Mixed : NodeRole::Compute;
    node_details.architecture = architectures[n % architectures.size()];
    node_details.operating_system = heterogeneous ? "synthetic-os-b" : "synthetic-os-a";
    node_details.memory_bytes = static_cast<std::uint64_t>(512) << 30U;
    node_details.cpu_package_count = static_cast<std::uint32_t>(config.cpu_packages_per_node);
    node_details.logical_cpu_count = static_cast<std::uint32_t>(config.cpu_packages_per_node * 64);
    node.details = node_details;
    node.failure_domains = {*FailureDomainId::parse(rack_domain_id),
                            *FailureDomainId::parse(index_name("fd-node", n))};
    if (n + 1 < config.node_count) {
      node.failure_domains.push_back(*FailureDomainId::parse(index_name("fd-chassis", n / 2)));
    }
    node.power_domain = power_domains[n % power_domains.size()];
    node.cooling_domain = cooling_domains[n % cooling_domains.size()];
    if (missing) {
      // Deliberate partial evidence: the identity is declared, but the
      // runtime has no observation about it yet.
      node.lifecycle = MemberLifecycle::Declared;
      node.provenance = EvidenceProvenance::Unknown;
      node.observed_at = Timestamp::unknown();
      node.health = EvidenceValue<HealthState>{};
      node.readiness = EvidenceValue<ReadinessState>{};
      node.reachability = EvidenceValue<ReachabilityState>{};
    } else if (degraded) {
      node.health = synthetic_evidence(HealthState::Degraded);
      node.readiness = synthetic_evidence(ReadinessState::NotReady);
    }
    rack.members.push_back(std::move(node));

    for (std::size_t k = 0; k < config.cpu_packages_per_node; ++k) {
      const std::string id = nested_name("cpu", n, k);
      MemberRecord cpu = base_member(MemberKind::CpuPackage, id, "synthetic:cpu");
      CpuPackageDetails details;
      details.vendor = heterogeneous ? "vendor-b" : "vendor-a";
      details.model = "synthetic-cpu";
      const std::uint32_t physical_cores = 32 + static_cast<std::uint32_t>(k) * 8;
      details.physical_cores = physical_cores;
      details.logical_threads = physical_cores * 2;
      details.base_clock_mhz = 2400 + static_cast<std::uint32_t>(k) * 100;
      details.socket_index = static_cast<std::uint32_t>(k);
      details.numa_node = static_cast<std::uint32_t>(k);
      cpu.details = details;
      cpu.parent = node_key;
      cpu.failure_domains = {*FailureDomainId::parse(index_name("fd-node", n))};
      cpu.power_domain = power_domains[n % power_domains.size()];
      cpu.cooling_domain = cooling_domains[n % cooling_domains.size()];
      rack.members.push_back(std::move(cpu));
    }
    for (std::size_t m = 0; m < config.memory_domains_per_node; ++m) {
      const std::string id = nested_name("mem", n, m);
      MemberRecord memory = base_member(MemberKind::MemoryDomain, id, "synthetic:memory");
      MemoryDomainDetails details;
      details.kind = heterogeneous && m == 1 ? MemoryKind::Hbm : MemoryKind::Ddr;
      details.capacity_bytes = static_cast<std::uint64_t>(256) << 30U;
      details.numa_node = static_cast<std::uint32_t>(m);
      details.bandwidth = synthetic_quantity(204.8, QuantityUnit::GigabitsPerSecond);
      memory.details = details;
      memory.parent = node_key;
      memory.failure_domains = {*FailureDomainId::parse(index_name("fd-node", n))};
      rack.members.push_back(std::move(memory));
    }

    const std::size_t accelerator_count =
        pick(config.min_accelerators_per_node, config.max_accelerators_per_node);
    for (std::size_t a = 0; a < accelerator_count; ++a) {
      const std::string id = nested_name("acc", n, a);
      MemberRecord accelerator = base_member(MemberKind::Accelerator, id, "synthetic:accelerator");
      AcceleratorDetails details;
      const std::string& vendor = accelerator_vendors[(n + a) % accelerator_vendors.size()];
      details.vendor = vendor;
      details.model = "synthetic-accelerator-" + std::to_string(a);
      details.architecture = "synthetic-arch";
      details.memory_bytes = static_cast<std::uint64_t>(80) << 30U;
      details.compute_capability_major = 9;
      details.compute_capability_minor = 0;
      details.sm_count = 128;
      details.max_threads_per_block = 1024;
      if (unit() >= config.missing_evidence) {
        details.pci_address = "0000:0" + std::to_string(a) + ":00.0";
      }
      // interconnect is deliberately absent: absence means unknown, and the
      // generator never invents an interconnect.
      accelerator.details = details;
      accelerator.parent = node_key;
      accelerator.failure_domains = {*FailureDomainId::parse(index_name("fd-node", n))};
      accelerator.power_domain = power_domains[n % power_domains.size()];
      accelerator.cooling_domain = cooling_domains[n % cooling_domains.size()];
      accelerator.capabilities.push_back(
          CapabilityRef{*CapabilityId::parse("compute-capability-9.0"), EvidenceProvenance::Synthetic,
                        synthetic_time(), std::chrono::milliseconds{0}, Durability::Durable, false,
                        std::string("9.0")});
      accelerators_by_node[n].push_back(accelerator.key);
      rack.members.push_back(std::move(accelerator));
    }
    const std::size_t nic_count = pick(config.min_nics_per_node, config.max_nics_per_node);
    for (std::size_t i = 0; i < nic_count; ++i) {
      const std::string id = nested_name("nic", n, i);
      MemberRecord nic = base_member(MemberKind::Nic, id, "synthetic:nic");
      NicDetails details;
      details.vendor = heterogeneous ? "vendor-b" : "vendor-a";
      details.model = "synthetic-nic";
      details.port_count = 2;
      details.link_speed = synthetic_quantity(
          static_cast<double>(100U * (1U + static_cast<unsigned>(n % 4U))),
          QuantityUnit::GigabitsPerSecond);
      nic.details = details;
      nic.parent = node_key;
      nic.failure_domains = {*FailureDomainId::parse(index_name("fd-node", n))};
      nic.power_domain = power_domains[n % power_domains.size()];
      nic.cooling_domain = cooling_domains[n % cooling_domains.size()];
      rack.members.push_back(std::move(nic));
    }
    if (n < config.dpu_count) {
      const std::string id = nested_name("dpu", n, 0);
      MemberRecord dpu = base_member(MemberKind::Dpu, id, "synthetic:dpu");
      DpuDetails details;
      details.vendor = "vendor-d";
      details.model = "synthetic-dpu";
      details.port_count = 2;
      dpu.details = details;
      dpu.parent = node_key;
      dpu.failure_domains = {*FailureDomainId::parse(index_name("fd-node", n))};
      rack.members.push_back(std::move(dpu));
    }
    if (n < config.storage_endpoint_count) {
      const std::string id = index_name("storage", n);
      MemberRecord storage = base_member(MemberKind::StorageEndpoint, id, "synthetic:storage");
      StorageEndpointDetails details;
      details.kind = heterogeneous ? StorageKind::Nvme : StorageKind::Sata;
      details.model = "synthetic-storage";
      details.capacity_bytes = static_cast<std::uint64_t>(4) << 40U;
      storage.details = details;
      storage.parent = node_key;
      storage.failure_domains = {*FailureDomainId::parse(index_name("fd-node", n))};
      rack.members.push_back(std::move(storage));
    }
  }

  for (std::size_t s = 0; s < config.switch_count; ++s) {
    const std::string id = index_name("switch", s);
    MemberRecord sw = base_member(MemberKind::Switch, id, "synthetic:switch");
    SwitchDetails details;
    details.vendor = s % 2 == 0 ? "vendor-a" : "vendor-c";
    details.model = "synthetic-switch";
    details.port_count = 64;
    details.layer_index = static_cast<std::uint32_t>(s % 2);
    details.link_speed = synthetic_quantity(400.0, QuantityUnit::GigabitsPerSecond);
    sw.details = details;
    sw.failure_domains = {*FailureDomainId::parse(index_name("fd-switch", s))};
    switch_keys.push_back(sw.key);
    rack.members.push_back(std::move(sw));
  }

  // Topology: containment is carried by the member parent field, so the
  // relationship set describes connectivity and locality.
  for (std::size_t n = 0; n < node_keys.size() && !switch_keys.empty(); ++n) {
    if (unit() < config.missing_links) {
      continue;
    }
    const MemberKey& sw = switch_keys[n % switch_keys.size()];
    rack.relationships.push_back(relationship(RelationshipClass::ConnectedTo, node_keys[n], sw,
                                              "synthetic:topology"));
    rack.relationships.push_back(
        relationship(RelationshipClass::ReachableThrough, node_keys[n], sw, "synthetic:topology"));
  }
  for (std::size_t n = 0; n < node_keys.size(); ++n) {
    for (const auto& accelerator : accelerators_by_node[n]) {
      rack.relationships.push_back(relationship(RelationshipClass::StorageLocal, node_keys[n],
                                                accelerator, "synthetic:topology"));
    }
  }
  if (config.synthetic_accelerator_peer_links) {
    for (std::size_t n = 0; n < accelerators_by_node.size(); ++n) {
      const auto& devices = accelerators_by_node[n];
      for (std::size_t a = 0; a + 1 < devices.size(); ++a) {
        rack.relationships.push_back(relationship(RelationshipClass::AcceleratorPeer, devices[a],
                                                  devices[a + 1], "synthetic:peer-link"));
      }
    }
  }

  if (config.include_power_envelope) {
    PowerEnvelopeRecord power;
    power.rack_limit = synthetic_quantity(40000.0, QuantityUnit::Watts);
    power.rack_observed_draw = synthetic_quantity(22000.0, QuantityUnit::Watts);
    power.provenance = EvidenceProvenance::Synthetic;
    power.observed_at = synthetic_time();
    power.ttl = std::chrono::milliseconds{0};
    power.durability = Durability::Durable;
    power.source = "synthetic:power-envelope";
    for (std::size_t p = 0; p < power_domains.size(); ++p) {
      PowerDomainBudget budget{power_domains[p]};
      budget.limit = synthetic_quantity(20000.0, QuantityUnit::Watts);
      budget.observed_draw = synthetic_quantity(11000.0, QuantityUnit::Watts);
      power.domain_budgets.push_back(std::move(budget));
    }
    rack.power = std::move(power);
  }
  if (config.include_cooling_envelope) {
    CoolingEnvelopeRecord cooling;
    cooling.provenance = EvidenceProvenance::Synthetic;
    cooling.observed_at = synthetic_time();
    cooling.ttl = std::chrono::milliseconds{0};
    cooling.durability = Durability::Durable;
    cooling.source = "synthetic:cooling-envelope";
    for (const auto& zone : cooling_domains) {
      CoolingZoneRecord record{zone};
      record.design_thermal_limit = synthetic_quantity(20000.0, QuantityUnit::Watts);
      record.observed_temperature = synthetic_quantity(27.5, QuantityUnit::Celsius);
      record.cooling_capacity = synthetic_quantity(24000.0, QuantityUnit::Watts);
      record.throttling = synthetic_evidence(ThrottleState::None);
      cooling.zones.push_back(std::move(record));
    }
    rack.cooling = std::move(cooling);
  }

  std::sort(rack.members.begin(), rack.members.end(),
            [](const MemberRecord& lhs, const MemberRecord& rhs) { return lhs.key < rhs.key; });
  std::sort(rack.relationships.begin(), rack.relationships.end(),
            [](const RelationshipRecord& lhs, const RelationshipRecord& rhs) {
              return lhs.key < rhs.key;
            });
  std::sort(rack.failure_domains.begin(), rack.failure_domains.end(),
            [](const FailureDomainRecord& lhs, const FailureDomainRecord& rhs) {
              return lhs.id < rhs.id;
            });
  return rack;
}

SyntheticPublishSummary publish_synthetic_rack(RackFabric& fabric, const SyntheticRack& rack,
                                               const AuthorityToken& authority) {
  SyntheticPublishSummary summary;
  AuthorityToken token = authority;
  token.rack = rack.rack;

  DeclareRackRequest declare;
  declare.authority = token;
  declare.epoch = rack.rack_epoch;
  declare.label = "synthetic rack";
  declare.redeclare = true;
  const MutationResult declared = fabric.declare_rack(declare);
  if (!declared.accepted()) {
    summary.rejections.push_back(declared);
    return summary;
  }

  // Failure domains are published parents first: a domain that names a parent
  // domain is rejected until that parent exists. A domain whose parent never
  // arrives (a cycle, or a parent that belongs to another publisher) is still
  // published last, so that the runtime produces the explanation.
  {
    std::vector<const FailureDomainRecord*> pending;
    pending.reserve(rack.failure_domains.size());
    for (const auto& record : rack.failure_domains) {
      pending.push_back(&record);
    }
    std::set<FailureDomainId> published_ids;
    while (!pending.empty()) {
      bool progressed = false;
      for (auto it = pending.begin(); it != pending.end();) {
        const FailureDomainRecord& record = **it;
        bool ready = true;
        for (const auto& parent : record.parents) {
          if (published_ids.find(parent) == published_ids.end()) {
            ready = false;
            break;
          }
        }
        if (!ready) {
          ++it;
          continue;
        }
        PublishFailureDomainRequest request;
        request.authority = token;
        request.record = record;
        const MutationResult result = fabric.publish_failure_domain(request);
        if (result.accepted()) {
          ++summary.failure_domains_accepted;
          published_ids.insert(record.id);
        } else {
          summary.rejections.push_back(result);
        }
        it = pending.erase(it);
        progressed = true;
      }
      if (!progressed) {
        for (const FailureDomainRecord* record : pending) {
          PublishFailureDomainRequest request;
          request.authority = token;
          request.record = *record;
          const MutationResult result = fabric.publish_failure_domain(request);
          if (result.accepted()) {
            ++summary.failure_domains_accepted;
          } else {
            summary.rejections.push_back(result);
          }
        }
        break;
      }
    }
  }
  // Power and cooling domain members come first: other members reference them
  // by domain identity, so a member published before its domain is rejected.
  for (const auto& record : rack.members) {
    if (record.key.kind != MemberKind::PowerDomain && record.key.kind != MemberKind::CoolingDomain) {
      continue;
    }
    PublishMemberRequest request;
    request.authority = token;
    request.record = record;
    const MutationResult result = fabric.publish_member(request);
    if (result.accepted()) {
      ++summary.members_accepted;
    } else {
      summary.rejections.push_back(result);
    }
  }
  // The remaining members are published parents first.
  {
    std::vector<const MemberRecord*> pending;
    pending.reserve(rack.members.size());
    for (const auto& record : rack.members) {
      if (record.key.kind == MemberKind::PowerDomain ||
          record.key.kind == MemberKind::CoolingDomain) {
        continue;
      }
      pending.push_back(&record);
    }
    std::set<MemberKey> published_keys;
    while (!pending.empty()) {
      bool progressed = false;
      for (auto it = pending.begin(); it != pending.end();) {
        const MemberRecord& record = **it;
        if (record.parent.has_value() && published_keys.find(*record.parent) == published_keys.end()) {
          ++it;
          continue;
        }
        PublishMemberRequest request;
        request.authority = token;
        request.record = record;
        const MutationResult result = fabric.publish_member(request);
        if (result.accepted()) {
          ++summary.members_accepted;
          published_keys.insert(record.key);
        } else {
          summary.rejections.push_back(result);
        }
        it = pending.erase(it);
        progressed = true;
      }
      if (!progressed) {
        for (const MemberRecord* record : pending) {
          PublishMemberRequest request;
          request.authority = token;
          request.record = *record;
          const MutationResult result = fabric.publish_member(request);
          if (result.accepted()) {
            ++summary.members_accepted;
          } else {
            summary.rejections.push_back(result);
          }
        }
        break;
      }
    }
  }
  for (const auto& record : rack.relationships) {
    PublishRelationshipRequest request;
    request.authority = token;
    request.record = record;
    const MutationResult result = fabric.publish_relationship(request);
    if (result.accepted()) {
      ++summary.relationships_accepted;
    } else {
      summary.rejections.push_back(result);
    }
  }
  if (rack.power.has_value()) {
    PublishPowerEnvelopeRequest request;
    request.authority = token;
    request.record = *rack.power;
    const MutationResult result = fabric.publish_power_envelope(request);
    if (!result.accepted()) {
      summary.rejections.push_back(result);
    }
  }
  if (rack.cooling.has_value()) {
    PublishCoolingEnvelopeRequest request;
    request.authority = token;
    request.record = *rack.cooling;
    const MutationResult result = fabric.publish_cooling_envelope(request);
    if (!result.accepted()) {
      summary.rejections.push_back(result);
    }
  }
  return summary;
}

}  // namespace rack_fabric
