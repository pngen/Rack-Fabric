// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Versioned, integrity-checked, atomically written durable state.
//
// Layout:
//
//   offset  size  field
//   0       8     magic "RKFSTAT1"
//   8       4     format version
//   12      4     flags (must be zero)
//   16      8     payload length
//   24      8     CRC-32C of the payload
//   32      8     reserved (must be zero)
//   40      32    SHA-256 of the payload
//   72      N     payload
//   72+N    4     trailer magic "RKFE"
//   76+N    4     CRC-32C of the header and the payload
//
// The declared payload length is validated against the configured bound and
// against the actual file size before a single byte of payload is read. A
// file that is longer than its declaration is trailing garbage; a file that
// is shorter is truncated. Neither can mutate authoritative state: decoding
// builds a candidate, validates it completely, and only then commits.

#include "rack_fabric/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/impl.hpp"
#include "core/state.hpp"
#include "internal/crypto.hpp"
#include "internal/files.hpp"
#include "rack_fabric/rack_fabric.hpp"

namespace rack_fabric {
namespace {

constexpr char kMagic[8] = {'R', 'K', 'F', 'S', 'T', 'A', 'T', '1'};
constexpr char kTrailerMagic[4] = {'R', 'K', 'F', 'E'};
constexpr std::size_t kHeaderBytes = 72;
constexpr std::size_t kTrailerBytes = 8;

void store_u32(std::byte* out, std::uint32_t value) {
  std::memcpy(out, &value, sizeof(value));
}

void store_u64(std::byte* out, std::uint64_t value) {
  std::memcpy(out, &value, sizeof(value));
}

[[nodiscard]] std::uint32_t load_u32(const std::byte* in) {
  std::uint32_t value = 0;
  std::memcpy(&value, in, sizeof(value));
  return value;
}

[[nodiscard]] std::uint64_t load_u64(const std::byte* in) {
  std::uint64_t value = 0;
  std::memcpy(&value, in, sizeof(value));
  return value;
}

[[nodiscard]] PersistenceResult persistence_failure(PersistenceStatus status, std::string code,
                                                    std::string detail, std::string path) {
  PersistenceResult result;
  result.status = status;
  result.path = std::move(path);
  ExplanationFactor factor;
  factor.code = std::move(code);
  factor.detail = std::move(detail);
  result.explanation = Explanation::failure("PERSISTENCE_" + std::string(to_string(status)),
                                            result.path, {std::move(factor)});
  return result;
}

/// Applies the conservative recovery policy to a freshly decoded candidate.
///
/// Recovered evidence is not a current measurement. Every ephemeral
/// observation is demoted, and every durable fact that was asserted by a
/// process incarnation must be re-earned before it is current again.
void apply_recovery_policy(RackState& state, CoordinatorEpoch epoch, Timestamp now) {
  (void)now;
  for (auto& [boot, publisher] : state.publishers) {
    (void)boot;
    publisher.state = PublisherState::Lost;
  }
  // Ownership is a fact about a live process incarnation and cannot survive
  // the death of the coordinator process. Recovered records therefore lose
  // their owner: the durable identity is retained, but the record must be
  // re-asserted by a live incarnation before it is current again. Keeping a
  // dead owner would make the rack permanently unrepublishable.
  const auto clear_owner = [](auto& record) {
    record.owner_boot.reset();
    record.owner_worker.reset();
  };
  // A recovered observation is not a current observation. Every record that
  // was produced by a process incarnation, and every record whose own
  // evidence is ephemeral, must be republished before it is current again.
  for (auto& [key, record] : state.members) {
    (void)key;
    if (record.durability == Durability::Ephemeral || record.owner_boot.has_value()) {
      record.revalidation_required = true;
      record.revalidation_reason = RevalidationReason::CoordinatorRestarted;
    }
    clear_owner(record);
    if (record.health.durability == Durability::Ephemeral) {
      record.health = EvidenceValue<HealthState>{};
    }
    if (record.readiness.durability == Durability::Ephemeral) {
      record.readiness = EvidenceValue<ReadinessState>{};
    }
    if (record.reachability.durability == Durability::Ephemeral) {
      record.reachability = EvidenceValue<ReachabilityState>{};
    }
    for (auto& capability : record.capabilities) {
      if (capability.durability == Durability::Ephemeral) {
        capability.revalidation_required = true;
      }
    }
  }
  for (auto& [key, record] : state.relationships) {
    (void)key;
    if (record.durability == Durability::Ephemeral || record.owner_boot.has_value()) {
      record.revalidation_required = true;
    }
    clear_owner(record);
  }
  for (auto& [key, record] : state.failure_domains) {
    (void)key;
    if (record.durability == Durability::Ephemeral || record.owner_boot.has_value()) {
      record.revalidation_required = true;
    }
    clear_owner(record);
  }
  if (state.power.has_value()) {
    if (state.power->durability == Durability::Ephemeral || state.power->owner_boot.has_value()) {
      state.power->revalidation_required = true;
    }
    clear_owner(*state.power);
    if (state.power->durability == Durability::Ephemeral) {
      state.power->rack_observed_draw.reset();
      state.power->rack_headroom.reset();
      for (auto& budget : state.power->domain_budgets) {
        budget.observed_draw.reset();
        budget.headroom.reset();
      }
    }
  }
  if (state.cooling.has_value()) {
    if (state.cooling->durability == Durability::Ephemeral ||
        state.cooling->owner_boot.has_value()) {
      state.cooling->revalidation_required = true;
    }
    clear_owner(*state.cooling);
    if (state.cooling->durability == Durability::Ephemeral) {
      for (auto& zone : state.cooling->zones) {
        zone.observed_temperature.reset();
        zone.thermal_headroom.reset();
        zone.throttling.reset();
      }
    }
  }
  state.generations.coordinator_epoch = epoch;
  state.rebuild_indexes();
}

[[nodiscard]] PersistenceStatus decode_payload(const std::byte* data, std::size_t size,
                                               const ResourceLimits& limits, RackState& candidate,
                                               std::string& error) {
  candidate.limits = limits;
  if (!decode_state_payload(data, size, candidate, error)) {
    if (error == "TRAILING_DATA") {
      return PersistenceStatus::TrailingGarbage;
    }
    if (error == "TRUNCATED") {
      return PersistenceStatus::Truncated;
    }
    return PersistenceStatus::Corrupt;
  }
  return PersistenceStatus::Ok;
}

struct ParsedFile {
  PersistenceStatus status = PersistenceStatus::Ok;
  std::vector<std::byte> payload;
  std::uint64_t encoded_bytes = 0;
  std::string detail;
};

[[nodiscard]] ParsedFile parse_file(const std::filesystem::path& path, const ResourceLimits& limits) {
  ParsedFile parsed;
  std::vector<std::byte> file_bytes;
  std::string detail;
  const internal::FileStatus read_status =
      internal::read_file(path, file_bytes, limits.max_persisted_bytes, detail);
  switch (read_status) {
    case internal::FileStatus::Ok:
      break;
    case internal::FileStatus::NotFound:
      parsed.status = PersistenceStatus::NotFound;
      parsed.detail = detail;
      return parsed;
    case internal::FileStatus::TooLarge:
      parsed.status = PersistenceStatus::LimitExceeded;
      parsed.detail = detail;
      return parsed;
    default:
      parsed.status = PersistenceStatus::IoError;
      parsed.detail = detail;
      return parsed;
  }
  if (file_bytes.size() < kHeaderBytes + kTrailerBytes) {
    parsed.status = PersistenceStatus::Truncated;
    parsed.detail = "file is shorter than the minimum encoded length";
    return parsed;
  }
  const std::byte* data = file_bytes.data();
  if (std::memcmp(data, kMagic, sizeof(kMagic)) != 0) {
    parsed.status = PersistenceStatus::Corrupt;
    parsed.detail = "bad file magic";
    return parsed;
  }
  const std::uint32_t version = load_u32(data + 8);
  if (version != kPersistenceFormatVersion) {
    parsed.status = PersistenceStatus::UnsupportedVersion;
    parsed.detail = "unsupported persistence format version";
    return parsed;
  }
  if (load_u32(data + 12) != 0U) {
    parsed.status = PersistenceStatus::Corrupt;
    parsed.detail = "reserved header flags are set";
    return parsed;
  }
  const std::uint64_t payload_length = load_u64(data + 16);
  const std::uint64_t payload_crc = load_u64(data + 24);
  if (load_u64(data + 32) != 0U) {
    parsed.status = PersistenceStatus::Corrupt;
    parsed.detail = "reserved header field is set";
    return parsed;
  }
  if (payload_length > limits.max_persisted_bytes) {
    parsed.status = PersistenceStatus::LimitExceeded;
    parsed.detail = "declared payload length exceeds the configured bound";
    return parsed;
  }
  // Checked arithmetic: the expected size can never overflow because the
  // declared length is bounded by max_persisted_bytes.
  const std::uint64_t expected = kHeaderBytes + payload_length + kTrailerBytes;
  if (file_bytes.size() > expected) {
    parsed.status = PersistenceStatus::TrailingGarbage;
    parsed.detail = "file is longer than its declared payload";
    return parsed;
  }
  if (file_bytes.size() < expected) {
    parsed.status = PersistenceStatus::Truncated;
    parsed.detail = "file is shorter than its declared payload";
    return parsed;
  }
  const std::byte* payload = data + kHeaderBytes;
  const std::byte* trailer = payload + payload_length;
  if (std::memcmp(trailer, kTrailerMagic, sizeof(kTrailerMagic)) != 0) {
    parsed.status = PersistenceStatus::Corrupt;
    parsed.detail = "bad trailer magic";
    return parsed;
  }
  const std::uint32_t trailer_crc = load_u32(trailer + 4);
  const std::uint32_t computed_trailer_crc =
      internal::crc32c(data, kHeaderBytes + static_cast<std::size_t>(payload_length));
  if (trailer_crc != computed_trailer_crc) {
    parsed.status = PersistenceStatus::IntegrityFailure;
    parsed.detail = "trailer checksum does not match";
    return parsed;
  }
  if (internal::crc32c(payload, static_cast<std::size_t>(payload_length)) != payload_crc) {
    parsed.status = PersistenceStatus::IntegrityFailure;
    parsed.detail = "payload checksum does not match";
    return parsed;
  }
  const std::string digest =
      internal::Sha256::hash(payload, static_cast<std::size_t>(payload_length));
  std::byte stored_digest[32];
  std::memcpy(stored_digest, data + 40, 32);
  if (digest != internal::to_hex(stored_digest, 32)) {
    parsed.status = PersistenceStatus::IntegrityFailure;
    parsed.detail = "payload digest does not match";
    return parsed;
  }
  parsed.payload.assign(payload, payload + payload_length);
  parsed.encoded_bytes = file_bytes.size();
  return parsed;
}

}  // namespace

PersistenceResult RackFabric::save_state(const std::filesystem::path& path,
                                         const PersistenceOptions& options) const {
  PersistenceResult result;
  result.path = path.string();

  RackState snapshot_state;
  {
    std::shared_lock<std::shared_mutex> lock(impl_->mutex);
    snapshot_state = impl_->state;
  }

  std::string encode_error;
  const std::optional<std::vector<std::byte>> encoded =
      encode_state_payload(snapshot_state, &encode_error);
  if (!encoded.has_value()) {
    return persistence_failure(PersistenceStatus::LimitExceeded, "PAYLOAD_TOO_LARGE",
                               std::string("the state cannot be encoded within the configured "
                                           "bounds: ") +
                                   encode_error,
                               result.path);
  }
  const std::vector<std::byte>& payload = *encoded;
  const std::uint64_t max_bytes =
      std::min<std::uint64_t>(options.max_bytes, impl_->options.limits.max_persisted_bytes);
  if (payload.size() > max_bytes) {
    return persistence_failure(PersistenceStatus::LimitExceeded, "PAYLOAD_TOO_LARGE",
                               "the encoded state exceeds the configured bound", result.path);
  }

  std::vector<std::byte> file_bytes(kHeaderBytes + payload.size() + kTrailerBytes);
  std::memcpy(file_bytes.data(), kMagic, sizeof(kMagic));
  store_u32(file_bytes.data() + 8, kPersistenceFormatVersion);
  store_u32(file_bytes.data() + 12, 0);
  store_u64(file_bytes.data() + 16, payload.size());
  store_u64(file_bytes.data() + 24,
            internal::crc32c(payload.data(), payload.size()));
  store_u64(file_bytes.data() + 32, 0);
  const std::string digest = internal::Sha256::hash(payload.data(), payload.size());
  for (std::size_t i = 0; i < 32; ++i) {
    const auto high = static_cast<unsigned char>(digest[i * 2]);
    const auto low = static_cast<unsigned char>(digest[i * 2 + 1]);
    const auto nibble = [](unsigned char c) -> unsigned {
      return c <= '9' ? (c - '0') : (c - 'a' + 10);
    };
    file_bytes[40 + i] = static_cast<std::byte>((nibble(high) << 4U) | nibble(low));
  }
  if (!payload.empty()) {
    std::memcpy(file_bytes.data() + kHeaderBytes, payload.data(), payload.size());
  }
  std::byte* trailer = file_bytes.data() + kHeaderBytes + payload.size();
  std::memcpy(trailer, kTrailerMagic, sizeof(kTrailerMagic));
  store_u32(trailer + 4,
            internal::crc32c(file_bytes.data(), kHeaderBytes + payload.size()));

  std::string detail;
  const internal::FileStatus write_status =
      internal::write_file_atomic(path, file_bytes.data(), file_bytes.size(), options.atomic_replace,
                                  detail);
  if (write_status != internal::FileStatus::Ok) {
    return persistence_failure(PersistenceStatus::IoError, "WRITE_FAILED", detail, result.path);
  }
  result.status = PersistenceStatus::Ok;
  result.bytes_written = file_bytes.size();
  result.explanation = Explanation::success("STATE_SAVED", result.path);
  return result;
}

PersistenceResult RackFabric::load_state(const std::filesystem::path& path) {
  PersistenceResult result;
  result.path = path.string();

  const ParsedFile parsed = parse_file(path, impl_->options.limits);
  if (parsed.status != PersistenceStatus::Ok) {
    return persistence_failure(parsed.status, "LOAD_FAILED", parsed.detail, result.path);
  }

  RackState candidate;
  std::string error;
  const PersistenceStatus decode_status =
      decode_payload(parsed.payload.data(), parsed.payload.size(), impl_->options.limits, candidate,
                     error);
  if (decode_status != PersistenceStatus::Ok) {
    return persistence_failure(decode_status, "DECODE_FAILED", error, result.path);
  }
  candidate.limits = impl_->options.limits;
  candidate.contract = impl_->options.readiness;
  candidate.rebuild_indexes();

  const Timestamp now = impl_->now();
  const InvariantReport report = candidate.check_invariants(now);
  if (!report.ok()) {
    return persistence_failure(PersistenceStatus::InvalidState, "CANDIDATE_INVARIANT_VIOLATION",
                               report.render(), result.path);
  }

  const auto next_epoch = candidate.generations.coordinator_epoch.is_unset()
                              ? std::optional<CoordinatorEpoch>(CoordinatorEpoch::first())
                              : candidate.generations.coordinator_epoch.next();
  if (!next_epoch.has_value()) {
    return persistence_failure(PersistenceStatus::InvalidState, "EPOCH_EXHAUSTED",
                               "the persisted coordinator epoch cannot be advanced", result.path);
  }
  apply_recovery_policy(candidate, *next_epoch, now);

  {
    std::unique_lock<std::shared_mutex> lock(impl_->mutex);
    impl_->state = std::move(candidate);
  }
  result.status = PersistenceStatus::Ok;
  result.bytes_read = parsed.encoded_bytes;
  result.explanation = Explanation::success("STATE_LOADED", result.path);
  return result;
}

PersistenceResult inspect_persisted_state(const std::filesystem::path& path,
                                          PersistedStateInfo& out) {
  PersistenceResult result;
  result.path = path.string();
  const ResourceLimits limits;
  const ParsedFile parsed = parse_file(path, limits);
  if (parsed.status != PersistenceStatus::Ok) {
    return persistence_failure(parsed.status, "LOAD_FAILED", parsed.detail, result.path);
  }
  RackState candidate;
  std::string error;
  const PersistenceStatus decode_status =
      decode_payload(parsed.payload.data(), parsed.payload.size(), limits, candidate, error);
  if (decode_status != PersistenceStatus::Ok) {
    return persistence_failure(decode_status, "DECODE_FAILED", error, result.path);
  }
  out.format_version = kPersistenceFormatVersion;
  if (candidate.rack.has_value()) {
    out.rack = candidate.rack->id;
    out.rack_epoch = candidate.rack->epoch;
    out.lifecycle = candidate.derive_lifecycle(default_clock().now());
  }
  out.generations = candidate.generations;
  out.member_count = candidate.members.size();
  out.relationship_count = candidate.relationships.size();
  out.failure_domain_count = candidate.failure_domains.size();
  out.publisher_count = candidate.publishers.size();
  out.fenced_boot_count = candidate.fenced_boots.size();
  out.snapshot_count = candidate.snapshots.size();
  out.written_at = Timestamp::unknown();
  out.content_digest = internal::Sha256::hash(parsed.payload.data(), parsed.payload.size());
  out.encoded_bytes = parsed.encoded_bytes;
  result.status = PersistenceStatus::Ok;
  result.bytes_read = parsed.encoded_bytes;
  result.explanation = Explanation::success("STATE_INSPECTED", result.path);
  return result;
}

}  // namespace rack_fabric
