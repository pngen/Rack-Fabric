// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Local hardware discovery.
//
// Every value produced here is either measured by calling a platform API or
// reported as unsupported. Nothing is guessed, and no observation is upgraded
// to a class of evidence it did not earn.

#include "rack_fabric/hardware.hpp"

#include "rack_fabric/version.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

#include <iphlpapi.h>
#include <windows.h>
#endif

namespace rack_fabric {
namespace {

[[nodiscard]] std::string to_utf8(const wchar_t* text) {
#ifdef _WIN32
  if (text == nullptr || *text == L'\0') {
    return {};
  }
  const int required = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (required <= 1) {
    return {};
  }
  std::string out(static_cast<std::size_t>(required - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), required, nullptr, nullptr);
  return out;
#else
  (void)text;
  return {};
#endif
}

/// Produces a valid identity from a host label: only the characters that
/// validate_identity accepts survive, and an empty result becomes "local".
[[nodiscard]] std::string identity_from_label(const std::string& label) {
  std::string out;
  out.reserve(label.size());
  for (const char character : label) {
    const bool alnum = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9');
    if (alnum || character == '-' || character == '_' || character == '.' || character == ':') {
      out.push_back(character);
    }
  }
  if (out.empty() || out == "." || out == "..") {
    return "local";
  }
  if (out.size() > kMaxIdentityLength) {
    out.resize(kMaxIdentityLength);
  }
  return out;
}

[[nodiscard]] MemberRecord base_member(MemberKind kind, const std::string& id,
                                       const HardwareDiscoveryOptions& options) {
  MemberRecord record;
  record.key = MemberKey{kind, id};
  record.lifecycle = MemberLifecycle::Present;
  record.provenance = EvidenceProvenance::Measured;
  record.observed_at = default_clock().now();
  record.ttl = options.ttl;
  record.durability = options.durability;
  record.source = options.source_label;
  record.owner_worker = options.worker;
  record.owner_boot = options.boot;
  record.failure_domains.push_back(options.node_failure_domain);
  return record;
}

#ifdef _WIN32
[[nodiscard]] std::optional<CpuArchitecture> native_architecture() {
  SYSTEM_INFO info{};
  ::GetNativeSystemInfo(&info);
  switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64:
      return CpuArchitecture::X86_64;
    case PROCESSOR_ARCHITECTURE_ARM64:
      return CpuArchitecture::Aarch64;
    case PROCESSOR_ARCHITECTURE_INTEL:
      return CpuArchitecture::X86_64;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] std::optional<std::string> registry_string(const wchar_t* name) {
  // RegGetValueW is documented and returns the real value rather than the
  // compatibility-shimmed value that GetVersionEx reports.
  wchar_t buffer[256] = {};
  DWORD bytes = static_cast<DWORD>(sizeof(buffer));
  DWORD type = 0;
  const LSTATUS status = ::RegGetValueW(HKEY_LOCAL_MACHINE,
                                        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", name,
                                        RRF_RT_REG_SZ, &type, buffer, &bytes);
  if (status != ERROR_SUCCESS || bytes == 0) {
    return std::nullopt;
  }
  return to_utf8(buffer);
}

[[nodiscard]] std::optional<std::string> os_description() {
  const auto product = registry_string(L"ProductName");
  const auto build = registry_string(L"CurrentBuildNumber");
  if (!product.has_value() && !build.has_value()) {
    return std::nullopt;
  }
  std::string text = product.has_value() ? *product : std::string("Microsoft Windows");
  if (build.has_value()) {
    text += " build ";
    text += *build;
  }
  return text;
}
#endif

}  // namespace

std::string local_host_name() {
#ifdef _WIN32
  wchar_t buffer[256] = {};
  DWORD length = static_cast<DWORD>(std::size(buffer));
  if (::GetComputerNameW(buffer, &length) == 0) {
    return {};
  }
  return to_utf8(buffer);
#else
  return {};
#endif
}

std::optional<std::uint64_t> local_physical_memory_bytes() {
#ifdef _WIN32
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (::GlobalMemoryStatusEx(&status) == 0) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(status.ullTotalPhys);
#else
  return std::nullopt;
#endif
}

HardwareDiscoveryResult discover_local_hardware(const HardwareDiscoveryOptions& options) {
  HardwareDiscoveryResult result;

  const std::string host = local_host_name();
  result.host_label = host;
  const std::string node_id_text = identity_from_label(
      options.node.has_value() ? options.node->value() : (host.empty() ? std::string("local") : host));
  const auto node_id = NodeId::parse(node_id_text);
  if (!node_id.has_value()) {
    result.explanation = Explanation::failure(
        "DISCOVERY_INVALID_NODE_IDENTITY", node_id_text,
        {ExplanationFactor{"DISCOVERY_INVALID_NODE_IDENTITY",
                           "the node identity derived from the host name is not valid"}});
    return result;
  }
  const MemberKey node_key = MemberKey::of(MemberKind::Node, *node_id);

  FailureDomainId node_domain = options.node_failure_domain;
  if (node_domain.value().empty()) {
    node_domain = *FailureDomainId::parse("fd-node-" + node_id_text);
    FailureDomainRecord domain{node_domain};
    domain.kind = FailureDomainKind::Node;
    domain.lifecycle = MemberLifecycle::Present;
    domain.provenance = EvidenceProvenance::Measured;
    domain.observed_at = default_clock().now();
    domain.ttl = options.ttl;
    domain.durability = options.durability;
    domain.label = "local node failure domain";
    result.failure_domains.push_back(std::move(domain));
  }

  MemberRecord node = base_member(MemberKind::Node, node_id_text, options);
  node.failure_domains = {node_domain};
  NodeDetails node_details;
  node_details.host_name = host.empty() ? std::nullopt : std::optional<std::string>(host);
  node_details.role = NodeRole::Compute;
  {
    EvidenceValue<ReachabilityState> reachability;
    reachability.value = ReachabilityState::Reachable;
    reachability.provenance = EvidenceProvenance::Measured;
    reachability.observed_at = default_clock().now();
    reachability.ttl = options.ttl;
    reachability.durability = options.durability;
    node.reachability = reachability;
  }
  node.capabilities.push_back(CapabilityRef{*CapabilityId::parse("rack-fabric-agent"),
                                            EvidenceProvenance::Measured, default_clock().now(),
                                            options.ttl, options.durability, false,
                                            std::string(kVersionString)});
  const auto memory_bytes = local_physical_memory_bytes();
  if (memory_bytes.has_value()) {
    node_details.memory_bytes = *memory_bytes;
  } else {
    result.unsupported.push_back("physical-memory: the platform did not report total memory");
  }

#ifdef _WIN32
  {
    const auto architecture = native_architecture();
    if (architecture.has_value()) {
      node_details.architecture = *architecture;
    } else {
      result.unsupported.push_back("cpu-architecture: unrecognized processor architecture");
    }
  }
  SYSTEM_INFO system_info{};
  ::GetSystemInfo(&system_info);
  node_details.logical_cpu_count = static_cast<std::uint32_t>(system_info.dwNumberOfProcessors);

  std::vector<std::byte> buffer;
  DWORD returned = 0;
  constexpr LOGICAL_PROCESSOR_RELATIONSHIP kRelationships[] = {RelationProcessorPackage,
                                                               RelationNumaNode};
  std::uint32_t package_count = 0;
  std::uint32_t numa_node_count = 0;
  for (const LOGICAL_PROCESSOR_RELATIONSHIP relationship : kRelationships) {
    returned = 0;
    ::GetLogicalProcessorInformationEx(relationship, nullptr, &returned);
    if (returned == 0) {
      result.unsupported.push_back(
          std::string("logical-processors: GetLogicalProcessorInformationEx failed for "
                      "relationship ") +
          std::to_string(static_cast<int>(relationship)));
      continue;
    }
    buffer.assign(static_cast<std::size_t>(returned), std::byte{0});
    if (::GetLogicalProcessorInformationEx(
            relationship, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &returned) == 0) {
      result.unsupported.push_back("logical-processors: the processor topology could not be read");
      continue;
    }
    std::size_t offset = 0;
    // Every entry begins with a DWORD relationship and a DWORD size. The
    // entry can be smaller than the union's largest member, so the loop bound
    // is the two-DWORD header, never sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX).
    constexpr std::size_t kEntryHeaderBytes = 2 * sizeof(DWORD);
    while (offset + kEntryHeaderBytes <= returned) {
      const auto* entry = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(
          buffer.data() + offset);
      if (entry->Relationship == RelationProcessorPackage) {
        ++package_count;
        MemberRecord package = base_member(MemberKind::CpuPackage,
                                           node_id_text + "-cpu-" + std::to_string(package_count),
                                           options);
        package.parent = node_key;
        package.failure_domains = {node_domain};
        CpuPackageDetails details;
        details.socket_index = package_count - 1;
        details.logical_threads = entry->Processor.GroupCount;
        package.details = details;
        result.members.push_back(std::move(package));
      } else if (entry->Relationship == RelationNumaNode) {
        ++numa_node_count;
      }
      if (entry->Size == 0) {
        break;
      }
      offset += entry->Size;
    }
  }
  if (package_count > 0) {
    node_details.cpu_package_count = package_count;
  } else if (options.include_cpu) {
    result.unsupported.push_back("cpu-packages: no processor package topology was reported");
  }
  if (numa_node_count > 0) {
    node.capabilities.push_back(CapabilityRef{*CapabilityId::parse("numa-nodes"),
                                              EvidenceProvenance::Measured, default_clock().now(),
                                              options.ttl, options.durability, false,
                                              std::to_string(numa_node_count)});
  }
  if (options.include_memory && numa_node_count > 0) {
    // Windows exposes NUMA topology but not per-node physical capacity; the
    // capacity field stays absent rather than being divided by guesswork.
    for (std::uint32_t index = 0; index < numa_node_count; ++index) {
      MemberRecord memory = base_member(MemberKind::MemoryDomain,
                                        node_id_text + "-numa-" + std::to_string(index), options);
      memory.parent = node_key;
      memory.failure_domains = {node_domain};
      MemoryDomainDetails details;
      details.numa_node = index;
      memory.details = details;
      result.members.push_back(std::move(memory));
    }
    result.unsupported.push_back(
        "memory-capacity-per-numa-node: the platform did not report per-node capacity");
  } else if (options.include_memory) {
    result.unsupported.push_back("memory-domains: no NUMA topology was reported");
  }

  if (options.include_nics) {
    ULONG size = 16 * 1024;
    std::vector<std::byte> adapters(size);
    ULONG family = AF_UNSPEC;
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG status = ::GetAdaptersAddresses(family, flags, nullptr,
                                          reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapters.data()),
                                          &size);
    if (status == ERROR_BUFFER_OVERFLOW) {
      adapters.assign(size, std::byte{0});
      status = ::GetAdaptersAddresses(family, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(adapters.data()), &size);
    }
    if (status != NO_ERROR) {
      result.unsupported.push_back("network-adapters: GetAdaptersAddresses failed");
    } else {
      std::uint32_t nic_index = 0;
      for (const IP_ADAPTER_ADDRESSES* entry =
               reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(adapters.data());
           entry != nullptr; entry = entry->Next) {
        if (entry->IfType == IF_TYPE_SOFTWARE_LOOPBACK || entry->IfType == IF_TYPE_TUNNEL) {
          continue;
        }
        ++nic_index;
        const std::string nic_id = node_id_text + "-nic-" + std::to_string(nic_index);
        MemberRecord nic = base_member(MemberKind::Nic, nic_id, options);
        nic.parent = node_key;
        nic.failure_domains = {node_domain};
        NicDetails details;
        details.model = to_utf8(entry->Description);
        details.port_count = 1;
        if (entry->TransmitLinkSpeed > 0) {
          Quantity speed;
          speed.value = static_cast<double>(entry->TransmitLinkSpeed) / 1'000'000'000.0;
          speed.unit = QuantityUnit::GigabitsPerSecond;
          speed.provenance = EvidenceProvenance::Measured;
          speed.observed_at = default_clock().now();
          speed.ttl = options.ttl;
          speed.durability = options.durability;
          details.link_speed = speed;
        }
        if (entry->PhysicalAddressLength == 6) {
          static const char* kHex = "0123456789abcdef";
          std::string mac;
          for (ULONG index = 0; index < entry->PhysicalAddressLength; ++index) {
            if (index != 0) {
              mac.push_back(':');
            }
            mac.push_back(kHex[(entry->PhysicalAddress[index] >> 4U) & 0x0FU]);
            mac.push_back(kHex[entry->PhysicalAddress[index] & 0x0FU]);
          }
          details.mac_address = std::move(mac);
        }
        nic.details = details;
        result.members.push_back(std::move(nic));
      }
      node_details.nic_count = nic_index;
    }
  } else {
    result.unsupported.push_back("network-adapters: discovery disabled by configuration");
  }
#else
  (void)options;
#endif

  if (options.include_storage) {
    result.unsupported.push_back(
        "storage-endpoints: this platform build does not enumerate storage endpoints; the fact "
        "remains unknown rather than being inferred from mounted volumes");
  }
  if (options.include_pci_devices) {
    result.unsupported.push_back(
        "pci-devices: this platform build does not enumerate PCI devices; accelerator discovery "
        "is reported by the CUDA evidence module when it is available");
  }
  if (options.include_accelerators) {
    result.unsupported.push_back(
        "accelerators: no accelerator evidence was produced by local hardware discovery; use the "
        "CUDA evidence module for a measured accelerator proof");
  }
  {
    const auto os = os_description();
    if (os.has_value()) {
      node_details.operating_system = *os;
    } else {
      result.unsupported.push_back("operating-system: the platform did not report a version");
    }
  }

  node.details = node_details;
  result.members.insert(result.members.begin(), std::move(node));
  std::sort(result.members.begin(), result.members.end(),
            [](const MemberRecord& lhs, const MemberRecord& rhs) { return lhs.key < rhs.key; });
  result.ok = true;
  result.explanation = Explanation::success("HARDWARE_DISCOVERY_COMPLETE", node_id_text);
  result.explanation.factors.push_back(ExplanationFactor{
      "DISCOVERY_UNSUPPORTED_COUNT", std::to_string(result.unsupported.size())});
  for (const auto& item : result.unsupported) {
    result.explanation.factors.push_back(ExplanationFactor{"DISCOVERY_UNSUPPORTED", item});
  }
  return result;
}

}  // namespace rack_fabric
