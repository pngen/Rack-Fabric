// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "core/canonical.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>
#include <variant>

namespace rack_fabric::internal {
namespace {

void write_bytes(ByteWriter& writer, const void* data, std::size_t size) {
  writer.raw(static_cast<const std::byte*>(data), size);
}

template <class T>
void write_scalar(ByteWriter& writer, T value) {
  write_bytes(writer, &value, sizeof(T));
}

void encode_quantity(ByteWriter& writer, const Quantity& value) {
  writer.f64(value.value);
  writer.u8(static_cast<std::uint8_t>(value.unit));
  writer.u8(static_cast<std::uint8_t>(value.provenance));
  writer.optional_double(value.uncertainty);
  writer.timestamp(value.observed_at);
  writer.ttl(value.ttl);
  writer.u8(static_cast<std::uint8_t>(value.durability));
  writer.boolean(value.revalidation_required);
}

void decode_quantity(ByteReader& reader, Quantity& out) {
  out.value = reader.f64();
  out.unit = reader.enum8(QuantityUnit::Amperes);
  out.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
  out.uncertainty = reader.optional_double();
  out.observed_at = reader.timestamp();
  out.ttl = reader.ttl();
  out.durability = reader.enum8(Durability::Ephemeral);
  out.revalidation_required = reader.boolean();
}

template <class T>
void encode_evidence(ByteWriter& writer, const EvidenceValue<T>& value) {
  write_scalar(writer, value.value);
  writer.u8(static_cast<std::uint8_t>(value.provenance));
  writer.timestamp(value.observed_at);
  writer.ttl(value.ttl);
  writer.u8(static_cast<std::uint8_t>(value.durability));
  writer.boolean(value.revalidation_required);
}

template <class T, class Enum>
void decode_evidence(ByteReader& reader, EvidenceValue<T>& out, Enum maximum) {
  out.value = static_cast<T>(reader.u8());
  if (static_cast<std::uint8_t>(out.value) > static_cast<std::uint8_t>(maximum)) {
    reader.fail(CodecError::InvalidEnum);
  }
  out.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
  out.observed_at = reader.timestamp();
  out.ttl = reader.ttl();
  out.durability = reader.enum8(Durability::Ephemeral);
  out.revalidation_required = reader.boolean();
}

void encode_details(ByteWriter& writer, const MemberDetails& details) {
  std::visit(
      [&writer](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          writer.u8(0);
        } else if constexpr (std::is_same_v<T, NodeDetails>) {
          writer.u8(1);
          writer.optional_string(value.host_name);
          writer.optional_u32(value.role.has_value() ? std::optional<std::uint32_t>(
                                                           static_cast<std::uint32_t>(*value.role))
                                                     : std::nullopt);
          writer.optional_u32(value.architecture.has_value()
                                  ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*value.architecture))
                                  : std::nullopt);
          writer.optional_string(value.operating_system);
          writer.optional_u64(value.memory_bytes);
          writer.optional_u32(value.cpu_package_count);
          writer.optional_u32(value.logical_cpu_count);
          writer.optional_u32(value.accelerator_count);
          writer.optional_u32(value.nic_count);
          writer.optional_u32(value.dpu_count);
          writer.optional_string(value.worker.has_value() ? std::optional<std::string>(value.worker->value())
                                                         : std::nullopt);
          writer.optional_string(value.boot.has_value() ? std::optional<std::string>(value.boot->value())
                                                       : std::nullopt);
        } else if constexpr (std::is_same_v<T, AcceleratorDetails>) {
          writer.u8(2);
          writer.optional_string(value.vendor);
          writer.optional_string(value.model);
          writer.optional_string(value.architecture);
          writer.optional_string(value.uuid);
          writer.optional_string(value.driver_version);
          writer.optional_string(value.pci_address);
          writer.optional_u64(value.memory_bytes);
          writer.optional_u32(value.compute_capability_major);
          writer.optional_u32(value.compute_capability_minor);
          writer.optional_u32(value.sm_count);
          writer.optional_u32(value.max_threads_per_block);
          writer.optional_string(value.interconnect);
        } else if constexpr (std::is_same_v<T, CpuPackageDetails>) {
          writer.u8(3);
          writer.optional_string(value.vendor);
          writer.optional_string(value.model);
          writer.optional_u32(value.physical_cores);
          writer.optional_u32(value.logical_threads);
          writer.optional_u32(value.base_clock_mhz);
          writer.optional_u32(value.socket_index);
          writer.optional_u32(value.numa_node);
        } else if constexpr (std::is_same_v<T, MemoryDomainDetails>) {
          writer.u8(4);
          writer.optional_u32(value.kind.has_value()
                                  ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*value.kind))
                                  : std::nullopt);
          writer.optional_u64(value.capacity_bytes);
          writer.optional_u32(value.numa_node);
          writer.boolean(value.bandwidth.has_value());
          if (value.bandwidth.has_value()) {
            encode_quantity(writer, *value.bandwidth);
          }
        } else if constexpr (std::is_same_v<T, NicDetails>) {
          writer.u8(5);
          writer.optional_string(value.vendor);
          writer.optional_string(value.model);
          writer.optional_string(value.pci_address);
          writer.optional_u32(value.port_count);
          writer.boolean(value.link_speed.has_value());
          if (value.link_speed.has_value()) {
            encode_quantity(writer, *value.link_speed);
          }
          writer.optional_string(value.mac_address);
        } else if constexpr (std::is_same_v<T, DpuDetails>) {
          writer.u8(6);
          writer.optional_string(value.vendor);
          writer.optional_string(value.model);
          writer.optional_string(value.firmware_version);
          writer.optional_string(value.pci_address);
          writer.optional_u32(value.port_count);
        } else if constexpr (std::is_same_v<T, SwitchDetails>) {
          writer.u8(7);
          writer.optional_string(value.vendor);
          writer.optional_string(value.model);
          writer.optional_u32(value.port_count);
          writer.optional_u32(value.layer_index);
          writer.boolean(value.link_speed.has_value());
          if (value.link_speed.has_value()) {
            encode_quantity(writer, *value.link_speed);
          }
        } else if constexpr (std::is_same_v<T, StorageEndpointDetails>) {
          writer.u8(8);
          writer.optional_u32(value.kind.has_value()
                                  ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*value.kind))
                                  : std::nullopt);
          writer.optional_string(value.model);
          writer.optional_string(value.interface_name);
          writer.optional_u64(value.capacity_bytes);
        } else if constexpr (std::is_same_v<T, PowerDomainDetails>) {
          writer.u8(9);
          writer.optional_u32(value.kind.has_value()
                                  ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*value.kind))
                                  : std::nullopt);
          writer.optional_string(value.label);
        } else if constexpr (std::is_same_v<T, CoolingDomainDetails>) {
          writer.u8(10);
          writer.optional_u32(value.kind.has_value()
                                  ? std::optional<std::uint32_t>(static_cast<std::uint32_t>(*value.kind))
                                  : std::nullopt);
          writer.optional_string(value.label);
        }
      },
      details);
}

std::optional<WorkerId> parse_worker(const std::optional<std::string>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return WorkerId::parse(*value);
}

std::optional<AgentBootId> parse_boot(const std::optional<std::string>& value) {
  if (!value.has_value()) {
    return std::nullopt;
  }
  return AgentBootId::parse(*value);
}

void decode_details(ByteReader& reader, MemberDetails& out) {
  const std::uint8_t tag = reader.u8();
  if (!reader.ok()) {
    return;
  }
  switch (tag) {
    case 0:
      out = std::monostate{};
      break;
    case 1: {
      NodeDetails value;
      value.host_name = reader.optional_string();
      const auto role = reader.optional_u32();
      if (role.has_value()) {
        if (*role > static_cast<std::uint32_t>(NodeRole::Mixed)) {
          reader.fail(CodecError::InvalidEnum);
          return;
        }
        value.role = static_cast<NodeRole>(*role);
      }
      const auto architecture = reader.optional_u32();
      if (architecture.has_value()) {
        if (*architecture > static_cast<std::uint32_t>(CpuArchitecture::Other)) {
          reader.fail(CodecError::InvalidEnum);
          return;
        }
        value.architecture = static_cast<CpuArchitecture>(*architecture);
      }
      value.operating_system = reader.optional_string();
      value.memory_bytes = reader.optional_u64();
      value.cpu_package_count = reader.optional_u32();
      value.logical_cpu_count = reader.optional_u32();
      value.accelerator_count = reader.optional_u32();
      value.nic_count = reader.optional_u32();
      value.dpu_count = reader.optional_u32();
      const auto worker = reader.optional_string();
      if (worker.has_value()) {
        value.worker = WorkerId::parse(*worker);
        if (!value.worker.has_value()) {
          reader.fail(CodecError::InvalidIdentity);
          return;
        }
      }
      const auto boot = reader.optional_string();
      if (boot.has_value()) {
        value.boot = AgentBootId::parse(*boot);
        if (!value.boot.has_value()) {
          reader.fail(CodecError::InvalidIdentity);
          return;
        }
      }
      out = value;
      break;
    }
    case 2: {
      AcceleratorDetails value;
      value.vendor = reader.optional_string();
      value.model = reader.optional_string();
      value.architecture = reader.optional_string();
      value.uuid = reader.optional_string();
      value.driver_version = reader.optional_string();
      value.pci_address = reader.optional_string();
      value.memory_bytes = reader.optional_u64();
      value.compute_capability_major = reader.optional_u32();
      value.compute_capability_minor = reader.optional_u32();
      value.sm_count = reader.optional_u32();
      value.max_threads_per_block = reader.optional_u32();
      value.interconnect = reader.optional_string();
      out = value;
      break;
    }
    case 3: {
      CpuPackageDetails value;
      value.vendor = reader.optional_string();
      value.model = reader.optional_string();
      value.physical_cores = reader.optional_u32();
      value.logical_threads = reader.optional_u32();
      value.base_clock_mhz = reader.optional_u32();
      value.socket_index = reader.optional_u32();
      value.numa_node = reader.optional_u32();
      out = value;
      break;
    }
    case 4: {
      MemoryDomainDetails value;
      const auto kind = reader.optional_u32();
      if (kind.has_value()) {
        if (*kind > static_cast<std::uint32_t>(MemoryKind::Other)) {
          reader.fail(CodecError::InvalidEnum);
          return;
        }
        value.kind = static_cast<MemoryKind>(*kind);
      }
      value.capacity_bytes = reader.optional_u64();
      value.numa_node = reader.optional_u32();
      if (reader.boolean()) {
        Quantity quantity;
        decode_quantity(reader, quantity);
        value.bandwidth = quantity;
      }
      out = value;
      break;
    }
    case 5: {
      NicDetails value;
      value.vendor = reader.optional_string();
      value.model = reader.optional_string();
      value.pci_address = reader.optional_string();
      value.port_count = reader.optional_u32();
      if (reader.boolean()) {
        Quantity quantity;
        decode_quantity(reader, quantity);
        value.link_speed = quantity;
      }
      value.mac_address = reader.optional_string();
      out = value;
      break;
    }
    case 6: {
      DpuDetails value;
      value.vendor = reader.optional_string();
      value.model = reader.optional_string();
      value.firmware_version = reader.optional_string();
      value.pci_address = reader.optional_string();
      value.port_count = reader.optional_u32();
      out = value;
      break;
    }
    case 7: {
      SwitchDetails value;
      value.vendor = reader.optional_string();
      value.model = reader.optional_string();
      value.port_count = reader.optional_u32();
      value.layer_index = reader.optional_u32();
      if (reader.boolean()) {
        Quantity quantity;
        decode_quantity(reader, quantity);
        value.link_speed = quantity;
      }
      out = value;
      break;
    }
    case 8: {
      StorageEndpointDetails value;
      const auto kind = reader.optional_u32();
      if (kind.has_value()) {
        if (*kind > static_cast<std::uint32_t>(StorageKind::Other)) {
          reader.fail(CodecError::InvalidEnum);
          return;
        }
        value.kind = static_cast<StorageKind>(*kind);
      }
      value.model = reader.optional_string();
      value.interface_name = reader.optional_string();
      value.capacity_bytes = reader.optional_u64();
      out = value;
      break;
    }
    case 9: {
      PowerDomainDetails value;
      const auto kind = reader.optional_u32();
      if (kind.has_value()) {
        if (*kind > static_cast<std::uint32_t>(PowerDomainKind::Other)) {
          reader.fail(CodecError::InvalidEnum);
          return;
        }
        value.kind = static_cast<PowerDomainKind>(*kind);
      }
      value.label = reader.optional_string();
      out = value;
      break;
    }
    case 10: {
      CoolingDomainDetails value;
      const auto kind = reader.optional_u32();
      if (kind.has_value()) {
        if (*kind > static_cast<std::uint32_t>(CoolingDomainKind::Other)) {
          reader.fail(CodecError::InvalidEnum);
          return;
        }
        value.kind = static_cast<CoolingDomainKind>(*kind);
      }
      value.label = reader.optional_string();
      out = value;
      break;
    }
    default:
      reader.fail(CodecError::InvalidEnum);
      break;
  }
}

[[nodiscard]] std::optional<FailureDomainId> parse_failure_domain(const std::string& value) {
  return FailureDomainId::parse(value);
}

}  // namespace

std::string_view to_string(CodecError value) noexcept {
  switch (value) {
    case CodecError::None:
      return "NONE";
    case CodecError::Truncated:
      return "TRUNCATED";
    case CodecError::LengthTooLarge:
      return "LENGTH_TOO_LARGE";
    case CodecError::CountTooLarge:
      return "COUNT_TOO_LARGE";
    case CodecError::InvalidEnum:
      return "INVALID_ENUM";
    case CodecError::InvalidIdentity:
      return "INVALID_IDENTITY";
    case CodecError::InvalidNumber:
      return "INVALID_NUMBER";
    case CodecError::TrailingData:
      return "TRAILING_DATA";
    case CodecError::InvalidValue:
      return "INVALID_VALUE";
  }
  return "UNKNOWN";
}

void ByteWriter::u8(std::uint8_t value) { buffer_.push_back(static_cast<std::byte>(value)); }

void ByteWriter::u16(std::uint16_t value) { write_scalar(*this, value); }
void ByteWriter::u32(std::uint32_t value) { write_scalar(*this, value); }
void ByteWriter::u64(std::uint64_t value) { write_scalar(*this, value); }
void ByteWriter::i64(std::int64_t value) { write_scalar(*this, value); }
void ByteWriter::f64(double value) { write_scalar(*this, value); }

void ByteWriter::boolean(bool value) { u8(value ? 1U : 0U); }

void ByteWriter::string(std::string_view value) {
  if (value.size() > limits_.max_string_bytes) {
    fail(CodecError::LengthTooLarge);
    return;
  }
  const auto size = static_cast<std::uint32_t>(value.size());
  u32(size);
  raw(reinterpret_cast<const std::byte*>(value.data()), value.size());
}

void ByteWriter::optional_string(const std::optional<std::string>& value) {
  if (!value.has_value()) {
    u8(0);
    return;
  }
  u8(1);
  string(*value);
}

void ByteWriter::optional_double(const std::optional<double>& value) {
  if (!value.has_value()) {
    u8(0);
    return;
  }
  u8(1);
  f64(*value);
}

void ByteWriter::optional_u32(const std::optional<std::uint32_t>& value) {
  if (!value.has_value()) {
    u8(0);
    return;
  }
  u8(1);
  u32(*value);
}

void ByteWriter::optional_u64(const std::optional<std::uint64_t>& value) {
  if (!value.has_value()) {
    u8(0);
    return;
  }
  u8(1);
  u64(*value);
}

void ByteWriter::optional_i64(const std::optional<std::int64_t>& value) {
  if (!value.has_value()) {
    u8(0);
    return;
  }
  u8(1);
  i64(*value);
}

void ByteWriter::count(std::size_t value, std::size_t maximum) {
  if (value > maximum || value > 0xFFFFFFFFULL) {
    fail(CodecError::CountTooLarge);
    return;
  }
  u32(static_cast<std::uint32_t>(value));
}

void ByteWriter::timestamp(Timestamp value) { i64(value.unix_millis); }
void ByteWriter::ttl(std::chrono::milliseconds value) { i64(static_cast<std::int64_t>(value.count())); }

void ByteWriter::raw(const std::byte* data, std::size_t size) {
  // The payload bound is enforced here so that an encoder can never produce a
  // payload that the decoder would refuse, and so that no encoder can grow an
  // unbounded buffer. The bound is the writer's own capacity: protocol frames
  // use the frame payload limit, persisted state uses the persistence limit.
  if (size > max_bytes_ || buffer_.size() > max_bytes_ - size) {
    fail(CodecError::LengthTooLarge);
    return;
  }
  buffer_.insert(buffer_.end(), data, data + size);
}

bool ByteReader::need(std::size_t count_bytes) noexcept {
  if (!ok_) {
    return false;
  }
  if (count_bytes > size_ - offset_) {
    fail(CodecError::Truncated);
    return false;
  }
  return true;
}

std::uint8_t ByteReader::u8() {
  if (!need(1)) {
    return 0;
  }
  return static_cast<std::uint8_t>(data_[offset_++]);
}

std::uint16_t ByteReader::u16() {
  if (!need(2)) {
    return 0;
  }
  std::uint16_t value = 0;
  std::memcpy(&value, data_ + offset_, 2);
  offset_ += 2;
  return value;
}

std::uint32_t ByteReader::u32() {
  if (!need(4)) {
    return 0;
  }
  std::uint32_t value = 0;
  std::memcpy(&value, data_ + offset_, 4);
  offset_ += 4;
  return value;
}

std::uint64_t ByteReader::u64() {
  if (!need(8)) {
    return 0;
  }
  std::uint64_t value = 0;
  std::memcpy(&value, data_ + offset_, 8);
  offset_ += 8;
  return value;
}

std::int64_t ByteReader::i64() {
  if (!need(8)) {
    return 0;
  }
  std::int64_t value = 0;
  std::memcpy(&value, data_ + offset_, 8);
  offset_ += 8;
  return value;
}

double ByteReader::f64() {
  if (!need(8)) {
    return 0.0;
  }
  double value = 0.0;
  std::memcpy(&value, data_ + offset_, 8);
  offset_ += 8;
  return value;
}

bool ByteReader::boolean() {
  const std::uint8_t value = u8();
  if (!ok_) {
    return false;
  }
  if (value > 1U) {
    fail(CodecError::InvalidValue);
    return false;
  }
  return value == 1U;
}

std::string ByteReader::string() {
  const std::uint32_t length = u32();
  if (!ok_) {
    return {};
  }
  if (length > limits_.max_string_bytes) {
    fail(CodecError::LengthTooLarge);
    return {};
  }
  if (!need(length)) {
    return {};
  }
  std::string value(reinterpret_cast<const char*>(data_ + offset_), length);
  offset_ += length;
  return value;
}

std::optional<std::string> ByteReader::optional_string() {
  const std::uint8_t flag = u8();
  if (!ok_) {
    return std::nullopt;
  }
  if (flag == 0) {
    return std::nullopt;
  }
  if (flag != 1) {
    fail(CodecError::InvalidValue);
    return std::nullopt;
  }
  return string();
}

std::optional<double> ByteReader::optional_double() {
  const std::uint8_t flag = u8();
  if (!ok_ || flag == 0) {
    return std::nullopt;
  }
  if (flag != 1) {
    fail(CodecError::InvalidValue);
    return std::nullopt;
  }
  return f64();
}

std::optional<std::uint32_t> ByteReader::optional_u32() {
  const std::uint8_t flag = u8();
  if (!ok_ || flag == 0) {
    return std::nullopt;
  }
  if (flag != 1) {
    fail(CodecError::InvalidValue);
    return std::nullopt;
  }
  return u32();
}

std::optional<std::uint64_t> ByteReader::optional_u64() {
  const std::uint8_t flag = u8();
  if (!ok_ || flag == 0) {
    return std::nullopt;
  }
  if (flag != 1) {
    fail(CodecError::InvalidValue);
    return std::nullopt;
  }
  return u64();
}

std::optional<std::int64_t> ByteReader::optional_i64() {
  const std::uint8_t flag = u8();
  if (!ok_ || flag == 0) {
    return std::nullopt;
  }
  if (flag != 1) {
    fail(CodecError::InvalidValue);
    return std::nullopt;
  }
  return i64();
}

std::size_t ByteReader::count(std::size_t maximum) {
  const std::uint32_t value = u32();
  if (!ok_) {
    return 0;
  }
  if (value > maximum) {
    fail(CodecError::CountTooLarge);
    return 0;
  }
  // A count can never exceed the number of remaining bytes, since every
  // element occupies at least one byte.
  if (value > remaining()) {
    fail(CodecError::CountTooLarge);
    return 0;
  }
  return value;
}

Timestamp ByteReader::timestamp() { return Timestamp::from_unix_millis(i64()); }

std::chrono::milliseconds ByteReader::ttl() {
  const std::int64_t value = i64();
  if (value < 0 || value > 86'400'000LL) {
    fail(CodecError::InvalidValue);
    return std::chrono::milliseconds{0};
  }
  return std::chrono::milliseconds{value};
}

bool ByteReader::raw(std::byte* out, std::size_t size) {
  if (!need(size)) {
    return false;
  }
  std::memcpy(out, data_ + offset_, size);
  offset_ += size;
  return true;
}

void encode_member_details(ByteWriter& writer, const MemberDetails& details) {
  encode_details(writer, details);
}

void decode_member_details(ByteReader& reader, MemberDetails& out) { decode_details(reader, out); }

void encode_member_key(ByteWriter& writer, const MemberKey& key) {
  writer.u8(static_cast<std::uint8_t>(key.kind));
  writer.string(key.id);
}

void decode_member_key(ByteReader& reader, MemberKey& out) {
  out.kind = reader.enum8(MemberKind::CoolingDomain);
  out.id = reader.string();
  if (!reader.ok()) {
    return;
  }
  if (!validate_identity(out.id).ok()) {
    reader.fail(CodecError::InvalidIdentity);
  }
}

void encode_generation_set(ByteWriter& writer, const GenerationSet& value) {
  writer.u64(value.rack.value());
  writer.u64(value.membership.value());
  writer.u64(value.topology.value());
  writer.u64(value.failure_domains.value());
  writer.u64(value.constraints.value());
  writer.u64(value.power.value());
  writer.u64(value.cooling.value());
  writer.u64(value.health.value());
  writer.u64(value.capabilities.value());
  writer.u64(value.coordinator_epoch.value());
}

void decode_generation_set(ByteReader& reader, GenerationSet& out) {
  out.rack = RackGeneration::from_value(reader.u64());
  out.membership = MembershipGeneration::from_value(reader.u64());
  out.topology = TopologyGeneration::from_value(reader.u64());
  out.failure_domains = FailureDomainGeneration::from_value(reader.u64());
  out.constraints = ConstraintGeneration::from_value(reader.u64());
  out.power = PowerEnvelopeGeneration::from_value(reader.u64());
  out.cooling = CoolingEnvelopeGeneration::from_value(reader.u64());
  out.health = HealthGeneration::from_value(reader.u64());
  out.capabilities = CapabilityGeneration::from_value(reader.u64());
  out.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
}

void encode_authority(ByteWriter& writer, const AuthorityToken& token) {
  writer.u64(token.coordinator_epoch.value());
  writer.optional_string(token.boot.has_value() ? std::optional<std::string>(token.boot->value())
                                                : std::nullopt);
  writer.optional_u64(token.publication_generation.has_value()
                          ? std::optional<std::uint64_t>(token.publication_generation->value())
                          : std::nullopt);
  writer.optional_string(token.rack.has_value() ? std::optional<std::string>(token.rack->value())
                                                : std::nullopt);
}

void decode_authority(ByteReader& reader, AuthorityToken& out) {
  out.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
  const auto boot = reader.optional_string();
  if (boot.has_value()) {
    out.boot = AgentBootId::parse(*boot);
    if (!out.boot.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto publication = reader.optional_u64();
  if (publication.has_value()) {
    out.publication_generation = PublicationGeneration::from_value(*publication);
  }
  if (!reader.ok()) {
    return;
  }
  const auto rack = reader.optional_string();
  if (rack.has_value()) {
    out.rack = RackId::parse(*rack);
    if (!out.rack.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
    }
  }
}

void encode_member(ByteWriter& writer, const MemberRecord& record) {
  encode_member_key(writer, record.key);
  writer.u8(static_cast<std::uint8_t>(record.lifecycle));
  writer.u64(record.generation.value());
  writer.u64(record.publication_generation.value());
  writer.u8(static_cast<std::uint8_t>(record.provenance));
  writer.timestamp(record.observed_at);
  writer.ttl(record.ttl);
  writer.u8(static_cast<std::uint8_t>(record.durability));
  writer.boolean(record.revalidation_required);
  writer.u8(static_cast<std::uint8_t>(record.revalidation_reason));
  writer.optional_string(record.owner_worker.has_value()
                             ? std::optional<std::string>(record.owner_worker->value())
                             : std::nullopt);
  writer.optional_string(record.owner_boot.has_value()
                             ? std::optional<std::string>(record.owner_boot->value())
                             : std::nullopt);
  writer.boolean(record.parent.has_value());
  if (record.parent.has_value()) {
    encode_member_key(writer, *record.parent);
  }
  writer.count(record.failure_domains.size());
  for (const auto& domain : record.failure_domains) {
    writer.string(domain.value());
  }
  writer.optional_string(record.power_domain.has_value()
                             ? std::optional<std::string>(record.power_domain->value())
                             : std::nullopt);
  writer.optional_string(record.cooling_domain.has_value()
                             ? std::optional<std::string>(record.cooling_domain->value())
                             : std::nullopt);
  writer.optional_string(record.switch_domain.has_value()
                             ? std::optional<std::string>(record.switch_domain->value())
                             : std::nullopt);
  encode_evidence(writer, record.health);
  encode_evidence(writer, record.readiness);
  encode_evidence(writer, record.reachability);
  writer.count(record.capabilities.size());
  for (const auto& capability : record.capabilities) {
    writer.string(capability.id.value());
    writer.u8(static_cast<std::uint8_t>(capability.provenance));
    writer.timestamp(capability.observed_at);
    writer.ttl(capability.ttl);
    writer.u8(static_cast<std::uint8_t>(capability.durability));
    writer.boolean(capability.revalidation_required);
    writer.optional_string(capability.value);
  }
  encode_details(writer, record.details);
  writer.optional_string(record.source);
}

void decode_member(ByteReader& reader, MemberRecord& out) {
  decode_member_key(reader, out.key);
  out.lifecycle = reader.enum8(MemberLifecycle::Retired);
  out.generation = MemberGeneration::from_value(reader.u64());
  out.publication_generation = PublicationGeneration::from_value(reader.u64());
  out.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
  out.observed_at = reader.timestamp();
  out.ttl = reader.ttl();
  out.durability = reader.enum8(Durability::Ephemeral);
  out.revalidation_required = reader.boolean();
  out.revalidation_reason = reader.enum8(RevalidationReason::Superseded);
  const auto worker = reader.optional_string();
  if (worker.has_value()) {
    out.owner_worker = WorkerId::parse(*worker);
    if (!out.owner_worker.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto boot = reader.optional_string();
  if (boot.has_value()) {
    out.owner_boot = AgentBootId::parse(*boot);
    if (!out.owner_boot.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  if (reader.boolean()) {
    MemberKey parent;
    decode_member_key(reader, parent);
    if (!reader.ok()) {
      return;
    }
    out.parent = parent;
  }
  const std::size_t domain_count = reader.count();
  if (!reader.ok()) {
    return;
  }
  out.failure_domains.reserve(domain_count);
  for (std::size_t i = 0; i < domain_count; ++i) {
    const std::string id = reader.string();
    if (!reader.ok()) {
      return;
    }
    const auto domain = FailureDomainId::parse(id);
    if (!domain.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
    out.failure_domains.push_back(*domain);
  }
  const auto power = reader.optional_string();
  if (power.has_value()) {
    out.power_domain = PowerDomainId::parse(*power);
    if (!out.power_domain.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto cooling = reader.optional_string();
  if (cooling.has_value()) {
    out.cooling_domain = CoolingDomainId::parse(*cooling);
    if (!out.cooling_domain.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto switch_domain = reader.optional_string();
  if (switch_domain.has_value()) {
    out.switch_domain = SwitchId::parse(*switch_domain);
    if (!out.switch_domain.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  decode_evidence(reader, out.health, HealthState::NotApplicable);
  decode_evidence(reader, out.readiness, ReadinessState::NotReady);
  decode_evidence(reader, out.reachability, ReachabilityState::Unreachable);
  const std::size_t capability_count = reader.count();
  if (!reader.ok()) {
    return;
  }
  out.capabilities.reserve(capability_count);
  for (std::size_t i = 0; i < capability_count; ++i) {
    const std::string id = reader.string();
    if (!reader.ok()) {
      return;
    }
    const auto parsed = CapabilityId::parse(id);
    if (!parsed.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
    CapabilityRef capability{*parsed};
    capability.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
    capability.observed_at = reader.timestamp();
    capability.ttl = reader.ttl();
    capability.durability = reader.enum8(Durability::Ephemeral);
    capability.revalidation_required = reader.boolean();
    capability.value = reader.optional_string();
    if (!reader.ok()) {
      return;
    }
    out.capabilities.push_back(std::move(capability));
  }
  decode_details(reader, out.details);
  out.source = reader.optional_string();
}

void encode_relationship(ByteWriter& writer, const RelationshipRecord& record) {
  writer.u8(static_cast<std::uint8_t>(record.key.cls));
  encode_member_key(writer, record.key.from);
  encode_member_key(writer, record.key.to);
  writer.u8(static_cast<std::uint8_t>(record.provenance));
  writer.timestamp(record.observed_at);
  writer.ttl(record.ttl);
  writer.u8(static_cast<std::uint8_t>(record.durability));
  writer.boolean(record.revalidation_required);
  writer.u64(record.generation.value());
  writer.optional_string(record.owner_worker.has_value()
                             ? std::optional<std::string>(record.owner_worker->value())
                             : std::nullopt);
  writer.optional_string(record.owner_boot.has_value()
                             ? std::optional<std::string>(record.owner_boot->value())
                             : std::nullopt);
  writer.optional_string(record.link.has_value() ? std::optional<std::string>(record.link->value())
                                                 : std::nullopt);
  writer.boolean(record.capacity.has_value());
  if (record.capacity.has_value()) {
    encode_quantity(writer, *record.capacity);
  }
  writer.optional_string(record.source);
}

void decode_relationship(ByteReader& reader, RelationshipRecord& out) {
  out.key.cls = reader.enum8(RelationshipClass::AcceleratorPeer);
  decode_member_key(reader, out.key.from);
  decode_member_key(reader, out.key.to);
  out.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
  out.observed_at = reader.timestamp();
  out.ttl = reader.ttl();
  out.durability = reader.enum8(Durability::Ephemeral);
  out.revalidation_required = reader.boolean();
  out.generation = TopologyGeneration::from_value(reader.u64());
  const auto owner_worker = reader.optional_string();
  if (owner_worker.has_value()) {
    out.owner_worker = WorkerId::parse(*owner_worker);
    if (!out.owner_worker.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto owner_boot = reader.optional_string();
  if (owner_boot.has_value()) {
    out.owner_boot = AgentBootId::parse(*owner_boot);
    if (!out.owner_boot.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto link = reader.optional_string();
  if (link.has_value()) {
    out.link = LinkId::parse(*link);
    if (!out.link.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  if (reader.boolean()) {
    Quantity quantity;
    decode_quantity(reader, quantity);
    out.capacity = quantity;
  }
  out.source = reader.optional_string();
}

void encode_failure_domain(ByteWriter& writer, const FailureDomainRecord& record) {
  writer.string(record.id.value());
  writer.u8(static_cast<std::uint8_t>(record.kind));
  writer.u8(static_cast<std::uint8_t>(record.lifecycle));
  writer.count(record.parents.size());
  for (const auto& parent : record.parents) {
    writer.string(parent.value());
  }
  writer.u64(record.generation.value());
  writer.u8(static_cast<std::uint8_t>(record.provenance));
  writer.timestamp(record.observed_at);
  writer.ttl(record.ttl);
  writer.u8(static_cast<std::uint8_t>(record.durability));
  writer.boolean(record.revalidation_required);
  writer.optional_string(record.owner_worker.has_value()
                             ? std::optional<std::string>(record.owner_worker->value())
                             : std::nullopt);
  writer.optional_string(record.owner_boot.has_value()
                             ? std::optional<std::string>(record.owner_boot->value())
                             : std::nullopt);
  writer.optional_string(record.label);
  writer.optional_string(record.source);
}

void decode_failure_domain(ByteReader& reader, FailureDomainRecord& out) {
  const std::string id = reader.string();
  if (!reader.ok()) {
    return;
  }
  const auto parsed = FailureDomainId::parse(id);
  if (!parsed.has_value()) {
    reader.fail(CodecError::InvalidIdentity);
    return;
  }
  out.id = *parsed;
  out.kind = reader.enum8(FailureDomainKind::Other);
  out.lifecycle = reader.enum8(MemberLifecycle::Retired);
  const std::size_t parent_count = reader.count();
  if (!reader.ok()) {
    return;
  }
  out.parents.reserve(parent_count);
  for (std::size_t i = 0; i < parent_count; ++i) {
    const std::string parent = reader.string();
    if (!reader.ok()) {
      return;
    }
    const auto parsed_parent = FailureDomainId::parse(parent);
    if (!parsed_parent.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
    out.parents.push_back(*parsed_parent);
  }
  out.generation = FailureDomainGeneration::from_value(reader.u64());
  out.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
  out.observed_at = reader.timestamp();
  out.ttl = reader.ttl();
  out.durability = reader.enum8(Durability::Ephemeral);
  out.revalidation_required = reader.boolean();
  const auto owner_worker = reader.optional_string();
  if (owner_worker.has_value()) {
    out.owner_worker = WorkerId::parse(*owner_worker);
    if (!out.owner_worker.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto owner_boot = reader.optional_string();
  if (owner_boot.has_value()) {
    out.owner_boot = AgentBootId::parse(*owner_boot);
    if (!out.owner_boot.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  out.label = reader.optional_string();
  out.source = reader.optional_string();
}

void encode_power_envelope(ByteWriter& writer, const PowerEnvelopeRecord& record) {
  writer.u64(record.generation.value());
  writer.boolean(record.rack_limit.has_value());
  if (record.rack_limit.has_value()) {
    encode_quantity(writer, *record.rack_limit);
  }
  writer.boolean(record.rack_observed_draw.has_value());
  if (record.rack_observed_draw.has_value()) {
    encode_quantity(writer, *record.rack_observed_draw);
  }
  writer.boolean(record.rack_headroom.has_value());
  if (record.rack_headroom.has_value()) {
    encode_quantity(writer, *record.rack_headroom);
  }
  writer.count(record.domain_budgets.size());
  for (const auto& budget : record.domain_budgets) {
    writer.string(budget.domain.value());
    writer.boolean(budget.limit.has_value());
    if (budget.limit.has_value()) {
      encode_quantity(writer, *budget.limit);
    }
    writer.boolean(budget.observed_draw.has_value());
    if (budget.observed_draw.has_value()) {
      encode_quantity(writer, *budget.observed_draw);
    }
    writer.boolean(budget.headroom.has_value());
    if (budget.headroom.has_value()) {
      encode_quantity(writer, *budget.headroom);
    }
  }
  writer.u8(static_cast<std::uint8_t>(record.provenance));
  writer.timestamp(record.observed_at);
  writer.ttl(record.ttl);
  writer.u8(static_cast<std::uint8_t>(record.durability));
  writer.boolean(record.revalidation_required);
  writer.optional_string(record.owner_worker.has_value()
                             ? std::optional<std::string>(record.owner_worker->value())
                             : std::nullopt);
  writer.optional_string(record.owner_boot.has_value()
                             ? std::optional<std::string>(record.owner_boot->value())
                             : std::nullopt);
  writer.optional_string(record.source);
}

void decode_power_envelope(ByteReader& reader, PowerEnvelopeRecord& out) {
  out.generation = PowerEnvelopeGeneration::from_value(reader.u64());
  if (reader.boolean()) {
    Quantity quantity;
    decode_quantity(reader, quantity);
    out.rack_limit = quantity;
  }
  if (reader.boolean()) {
    Quantity quantity;
    decode_quantity(reader, quantity);
    out.rack_observed_draw = quantity;
  }
  if (reader.boolean()) {
    Quantity quantity;
    decode_quantity(reader, quantity);
    out.rack_headroom = quantity;
  }
  const std::size_t budget_count = reader.count();
  if (!reader.ok()) {
    return;
  }
  out.domain_budgets.reserve(budget_count);
  for (std::size_t i = 0; i < budget_count; ++i) {
    const std::string id = reader.string();
    if (!reader.ok()) {
      return;
    }
    const auto parsed = PowerDomainId::parse(id);
    if (!parsed.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
    PowerDomainBudget budget{*parsed};
    if (reader.boolean()) {
      Quantity quantity;
      decode_quantity(reader, quantity);
      budget.limit = quantity;
    }
    if (reader.boolean()) {
      Quantity quantity;
      decode_quantity(reader, quantity);
      budget.observed_draw = quantity;
    }
    if (reader.boolean()) {
      Quantity quantity;
      decode_quantity(reader, quantity);
      budget.headroom = quantity;
    }
    out.domain_budgets.push_back(std::move(budget));
  }
  out.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
  out.observed_at = reader.timestamp();
  out.ttl = reader.ttl();
  out.durability = reader.enum8(Durability::Ephemeral);
  out.revalidation_required = reader.boolean();
  const auto power_owner_worker = reader.optional_string();
  if (power_owner_worker.has_value()) {
    out.owner_worker = WorkerId::parse(*power_owner_worker);
    if (!out.owner_worker.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto power_owner_boot = reader.optional_string();
  if (power_owner_boot.has_value()) {
    out.owner_boot = AgentBootId::parse(*power_owner_boot);
    if (!out.owner_boot.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  out.source = reader.optional_string();
}

void encode_cooling_envelope(ByteWriter& writer, const CoolingEnvelopeRecord& record) {
  writer.u64(record.generation.value());
  writer.count(record.zones.size());
  for (const auto& zone : record.zones) {
    writer.string(zone.zone.value());
    writer.boolean(zone.design_thermal_limit.has_value());
    if (zone.design_thermal_limit.has_value()) {
      encode_quantity(writer, *zone.design_thermal_limit);
    }
    writer.boolean(zone.observed_temperature.has_value());
    if (zone.observed_temperature.has_value()) {
      encode_quantity(writer, *zone.observed_temperature);
    }
    writer.boolean(zone.cooling_capacity.has_value());
    if (zone.cooling_capacity.has_value()) {
      encode_quantity(writer, *zone.cooling_capacity);
    }
    writer.boolean(zone.thermal_headroom.has_value());
    if (zone.thermal_headroom.has_value()) {
      encode_quantity(writer, *zone.thermal_headroom);
    }
    writer.boolean(zone.throttling.has_value());
    if (zone.throttling.has_value()) {
      encode_evidence(writer, *zone.throttling);
    }
  }
  writer.u8(static_cast<std::uint8_t>(record.provenance));
  writer.timestamp(record.observed_at);
  writer.ttl(record.ttl);
  writer.u8(static_cast<std::uint8_t>(record.durability));
  writer.boolean(record.revalidation_required);
  writer.optional_string(record.owner_worker.has_value()
                             ? std::optional<std::string>(record.owner_worker->value())
                             : std::nullopt);
  writer.optional_string(record.owner_boot.has_value()
                             ? std::optional<std::string>(record.owner_boot->value())
                             : std::nullopt);
  writer.optional_string(record.source);
}

void decode_cooling_envelope(ByteReader& reader, CoolingEnvelopeRecord& out) {
  out.generation = CoolingEnvelopeGeneration::from_value(reader.u64());
  const std::size_t zone_count = reader.count();
  if (!reader.ok()) {
    return;
  }
  out.zones.reserve(zone_count);
  for (std::size_t i = 0; i < zone_count; ++i) {
    const std::string id = reader.string();
    if (!reader.ok()) {
      return;
    }
    const auto parsed = CoolingDomainId::parse(id);
    if (!parsed.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
    CoolingZoneRecord zone{*parsed};
    if (reader.boolean()) {
      Quantity quantity;
      decode_quantity(reader, quantity);
      zone.design_thermal_limit = quantity;
    }
    if (reader.boolean()) {
      Quantity quantity;
      decode_quantity(reader, quantity);
      zone.observed_temperature = quantity;
    }
    if (reader.boolean()) {
      Quantity quantity;
      decode_quantity(reader, quantity);
      zone.cooling_capacity = quantity;
    }
    if (reader.boolean()) {
      Quantity quantity;
      decode_quantity(reader, quantity);
      zone.thermal_headroom = quantity;
    }
    if (reader.boolean()) {
      EvidenceValue<ThrottleState> throttling;
      decode_evidence(reader, throttling, ThrottleState::SeverelyThrottled);
      zone.throttling = throttling;
    }
    out.zones.push_back(std::move(zone));
  }
  out.provenance = reader.enum8(EvidenceProvenance::Reconstructed);
  out.observed_at = reader.timestamp();
  out.ttl = reader.ttl();
  out.durability = reader.enum8(Durability::Ephemeral);
  out.revalidation_required = reader.boolean();
  const auto cooling_owner_worker = reader.optional_string();
  if (cooling_owner_worker.has_value()) {
    out.owner_worker = WorkerId::parse(*cooling_owner_worker);
    if (!out.owner_worker.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  const auto cooling_owner_boot = reader.optional_string();
  if (cooling_owner_boot.has_value()) {
    out.owner_boot = AgentBootId::parse(*cooling_owner_boot);
    if (!out.owner_boot.has_value()) {
      reader.fail(CodecError::InvalidIdentity);
      return;
    }
  }
  out.source = reader.optional_string();
}

}  // namespace rack_fabric::internal
