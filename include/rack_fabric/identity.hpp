// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Strongly typed identities.
//
// Rack Fabric never models distinct identities as interchangeable raw
// integers or strings. Every identity below is a distinct C++ type: a
// RackId cannot be passed where a NodeId is expected, and neither can be
// silently converted to an integer.
//
// Textual identities are bounded and validated at the point of construction.
// The accepted alphabet is deliberately narrow: ASCII letters, digits and
// the separators '-', '_', '.', ':', '@' and '+'. Whitespace, control
// characters, non-ASCII bytes and path separators are rejected, so an
// identity can never be used to traverse a path or to inject framing.

#ifndef RACK_FABRIC_IDENTITY_HPP
#define RACK_FABRIC_IDENTITY_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace rack_fabric {

/// Maximum number of bytes in any textual identity, name or label accepted
/// from a peer, a file or an API caller.
inline constexpr std::size_t kMaxIdentityLength = 128;

enum class IdentityStatus {
  Ok = 0,
  Empty,
  TooLong,
  InvalidCharacter,
  Reserved,
};

struct IdentityValidation {
  IdentityStatus status = IdentityStatus::Ok;
  std::size_t position = 0;

  [[nodiscard]] bool ok() const noexcept { return status == IdentityStatus::Ok; }
};

[[nodiscard]] constexpr bool is_identity_char(char c) noexcept {
  const unsigned char u = static_cast<unsigned char>(c);
  if (u >= '0' && u <= '9') return true;
  if (u >= 'a' && u <= 'z') return true;
  if (u >= 'A' && u <= 'Z') return true;
  switch (c) {
    case '-':
    case '_':
    case '.':
    case ':':
    case '@':
    case '+':
      return true;
    default:
      return false;
  }
}

/// Validates a textual identity. Pure and allocation free so it can be used
/// on untrusted input before any memory is reserved for it.
[[nodiscard]] constexpr IdentityValidation validate_identity(std::string_view value) noexcept {
  if (value.empty()) {
    return IdentityValidation{IdentityStatus::Empty, 0};
  }
  if (value.size() > kMaxIdentityLength) {
    return IdentityValidation{IdentityStatus::TooLong, kMaxIdentityLength};
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (!is_identity_char(value[i])) {
      return IdentityValidation{IdentityStatus::InvalidCharacter, i};
    }
  }
  if (value == "." || value == "..") {
    return IdentityValidation{IdentityStatus::Reserved, 0};
  }
  if (value.find("..") != std::string_view::npos) {
    return IdentityValidation{IdentityStatus::Reserved, value.find("..")};
  }
  return IdentityValidation{};
}

/// Validation for free-form labels that may contain spaces (host names,
/// model names, source labels). Still bounded and still ASCII only.
[[nodiscard]] constexpr IdentityValidation validate_label(std::string_view value) noexcept {
  if (value.empty()) {
    return IdentityValidation{IdentityStatus::Empty, 0};
  }
  if (value.size() > kMaxIdentityLength) {
    return IdentityValidation{IdentityStatus::TooLong, kMaxIdentityLength};
  }
  for (std::size_t i = 0; i < value.size(); ++i) {
    const unsigned char u = static_cast<unsigned char>(value[i]);
    if (u < 0x20 || u > 0x7E) {
      return IdentityValidation{IdentityStatus::InvalidCharacter, i};
    }
  }
  return IdentityValidation{};
}

/// A distinct, validated textual identity type. Tag supplies the type
/// identity; only tags declared in this header are ever used.
template <class Tag>
class StrongId {
 public:
  /// A default-constructed identity is the "unset" identity: its value is
  /// empty, validate_identity rejects it, and is_valid() reports false. It is
  /// never a valid identity and can never be mistaken for one.
  StrongId() = default;
  StrongId(const StrongId&) = default;
  StrongId(StrongId&&) noexcept = default;
  StrongId& operator=(const StrongId&) = default;
  StrongId& operator=(StrongId&&) noexcept = default;
  ~StrongId() = default;

  /// Constructs an identity from text. Text that is not a valid identity
  /// yields the unset identity rather than a live invalid one, so an invalid
  /// value can never travel through the runtime: every consumer of an identity
  /// already treats the unset identity as invalid input. Use parse() when you
  /// need to distinguish "invalid" from "unset".
  explicit StrongId(std::string_view value) {
    if (validate_identity(value).ok()) {
      value_.assign(value);
    }
  }

  /// Returns std::nullopt when the text is not a valid identity.
  [[nodiscard]] static std::optional<StrongId> parse(std::string_view value) {
    if (!validate_identity(value).ok()) {
      return std::nullopt;
    }
    return StrongId(value);
  }

  [[nodiscard]] static bool is_valid(std::string_view value) noexcept {
    return validate_identity(value).ok();
  }

  [[nodiscard]] const std::string& value() const noexcept { return value_; }
  [[nodiscard]] std::string_view view() const noexcept { return value_; }
  /// False for a default-constructed (unset) identity.
  [[nodiscard]] bool is_valid() const noexcept { return !value_.empty(); }

  friend bool operator==(const StrongId& lhs, const StrongId& rhs) = default;
  friend auto operator<=>(const StrongId& lhs, const StrongId& rhs) = default;

  [[nodiscard]] std::size_t hash() const noexcept { return std::hash<std::string>{}(value_); }

 private:
  std::string value_;
};

struct RackIdTag;
struct RackEpochIdTag;
struct NodeIdTag;
struct WorkerIdTag;
struct AgentBootIdTag;
struct DeviceIdTag;
struct AcceleratorIdTag;
struct CpuPackageIdTag;
struct MemoryDomainIdTag;
struct NicIdTag;
struct DpuIdTag;
struct SwitchIdTag;
struct StorageEndpointIdTag;
struct PowerDomainIdTag;
struct CoolingDomainIdTag;
struct FailureDomainIdTag;
struct LinkIdTag;
struct CapabilityIdTag;
struct SnapshotIdTag;

/// Identity of one rack. A rack is a single logical or physical rack; the
/// identity is stable across reincarnations of that rack.
using RackId = StrongId<RackIdTag>;

/// Identity of one incarnation of a rack. A rack that is re-declared as a
/// different physical assembly receives a new epoch, so evidence bound to a
/// previous incarnation cannot be mistaken for evidence about the current one.
using RackEpochId = StrongId<RackEpochIdTag>;

using NodeId = StrongId<NodeIdTag>;
using WorkerId = StrongId<WorkerIdTag>;

/// Identity of one operating-system process incarnation of an agent. A new
/// process must always use a fresh boot identity.
using AgentBootId = StrongId<AgentBootIdTag>;

/// Worker-facing name for the same process-incarnation identity.
using WorkerBootId = AgentBootId;

using DeviceId = StrongId<DeviceIdTag>;
using AcceleratorId = StrongId<AcceleratorIdTag>;
using CpuPackageId = StrongId<CpuPackageIdTag>;
using MemoryDomainId = StrongId<MemoryDomainIdTag>;
using NicId = StrongId<NicIdTag>;
using DpuId = StrongId<DpuIdTag>;
using SwitchId = StrongId<SwitchIdTag>;
using StorageEndpointId = StrongId<StorageEndpointIdTag>;
using PowerDomainId = StrongId<PowerDomainIdTag>;
using CoolingDomainId = StrongId<CoolingDomainIdTag>;
using FailureDomainId = StrongId<FailureDomainIdTag>;
using LinkId = StrongId<LinkIdTag>;

/// Reference to a capability identity owned by another runtime (for example a
/// Hardware Capability Registry). Rack Fabric references capabilities; it is
/// not the canonical database of them.
using CapabilityId = StrongId<CapabilityIdTag>;

using SnapshotId = StrongId<SnapshotIdTag>;

template <class Tag>
struct StrongIdHash {
  [[nodiscard]] std::size_t operator()(const StrongId<Tag>& id) const noexcept { return id.hash(); }
};

}  // namespace rack_fabric

namespace std {
template <class Tag>
struct hash<rack_fabric::StrongId<Tag>> {
  [[nodiscard]] std::size_t operator()(const rack_fabric::StrongId<Tag>& id) const noexcept {
    return id.hash();
  }
};
}  // namespace std

#endif  // RACK_FABRIC_IDENTITY_HPP
