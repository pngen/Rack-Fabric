// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Durable state persistence.
//
// The on-disk format is versioned, length-checked, integrity-protected and
// written atomically. Decoding always builds a temporary candidate state,
// validates it completely and only then commits it, so a corrupt, truncated
// or hostile file can never partially mutate authoritative state.
//
// Only facts that deserve durability are written. Live observations are
// recorded as RECONSTRUCTED evidence and are never treated as current after
// a reload.

#ifndef RACK_FABRIC_PERSISTENCE_HPP
#define RACK_FABRIC_PERSISTENCE_HPP

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include "rack_fabric/evidence.hpp"
#include "rack_fabric/explanation.hpp"
#include "rack_fabric/generation.hpp"
#include "rack_fabric/identity.hpp"
#include "rack_fabric/lifecycle.hpp"

namespace rack_fabric {

enum class PersistenceStatus : std::uint8_t {
  Ok = 0,
  NotFound = 1,
  IoError = 2,
  Corrupt = 3,
  Truncated = 4,
  UnsupportedVersion = 5,
  TrailingGarbage = 6,
  IntegrityFailure = 7,
  LimitExceeded = 8,
  InvalidState = 9,
  Refused = 10,
};

[[nodiscard]] std::string_view to_string(PersistenceStatus value) noexcept;

struct PersistenceOptions {
  /// Replace the target file atomically. When false the file is written
  /// directly and an interrupted write can leave a partial file.
  bool atomic_replace = true;
  /// Upper bound on the size of the encoded state.
  std::uint64_t max_bytes = 512ULL * 1024ULL * 1024ULL;
};

struct PersistenceResult {
  PersistenceStatus status = PersistenceStatus::Ok;
  Explanation explanation;
  std::size_t bytes_written = 0;
  std::size_t bytes_read = 0;
  std::string path;

  [[nodiscard]] bool ok() const noexcept { return status == PersistenceStatus::Ok; }

  friend bool operator==(const PersistenceResult&, const PersistenceResult&) = default;
};

/// Header and summary of a persisted state file, without applying it.
struct PersistedStateInfo {
  std::uint32_t format_version = 0;
  std::optional<RackId> rack;
  std::optional<RackEpochId> rack_epoch;
  GenerationSet generations;
  RackLifecycle lifecycle = RackLifecycle::Undeclared;
  std::size_t member_count = 0;
  std::size_t relationship_count = 0;
  std::size_t failure_domain_count = 0;
  std::size_t publisher_count = 0;
  std::size_t fenced_boot_count = 0;
  std::size_t snapshot_count = 0;
  Timestamp written_at = Timestamp::unknown();
  std::string content_digest;
  std::uint64_t encoded_bytes = 0;

  friend bool operator==(const PersistedStateInfo&, const PersistedStateInfo&) = default;
};

/// Reads and validates a persisted state file without mutating any runtime.
[[nodiscard]] PersistenceResult inspect_persisted_state(const std::filesystem::path& path,
                                                        PersistedStateInfo& out);

}  // namespace rack_fabric

#endif  // RACK_FABRIC_PERSISTENCE_HPP
