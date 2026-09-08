// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Version and release constants for Rack Fabric.

#ifndef RACK_FABRIC_VERSION_HPP
#define RACK_FABRIC_VERSION_HPP

#include <cstdint>
#include <string_view>

namespace rack_fabric {

inline constexpr int kVersionMajor = 1;
inline constexpr int kVersionMinor = 0;
inline constexpr int kVersionPatch = 0;
inline constexpr std::string_view kVersionString = "1.0.0";
inline constexpr std::string_view kProductName = "Rack Fabric";
inline constexpr std::string_view kCopyrightNotice = "Copyright 2026 Summon Software Labs.";
inline constexpr std::string_view kLicenseName = "Apache License 2.0";

/// Wire protocol version of the framed rack protocol. Bumped whenever the
/// frame layout or the payload encoding of any message changes incompatibly.
inline constexpr std::uint16_t kProtocolVersion = 1;

/// On-disk persistence format version. Bumped whenever the durable encoding
/// changes. Files carrying any other version are rejected as incompatible.
inline constexpr std::uint32_t kPersistenceFormatVersion = 1;

}  // namespace rack_fabric

#endif  // RACK_FABRIC_VERSION_HPP
