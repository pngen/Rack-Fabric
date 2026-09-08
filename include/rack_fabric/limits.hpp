// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Explicit resource bounds.
//
// Every bound below is enforced before memory is reserved for a peer-supplied
// value. Peer-provided lengths are never trusted.

#ifndef RACK_FABRIC_LIMITS_HPP
#define RACK_FABRIC_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace rack_fabric {

struct ResourceLimits {
  /// Maximum size of one protocol frame, including header and checksum.
  std::size_t max_frame_bytes = 1U << 20;  // 1 MiB
  /// Maximum size of one protocol payload.
  std::size_t max_payload_bytes = (1U << 20) - 64;
  /// Maximum bytes of any single string carried by a message or a file.
  std::size_t max_string_bytes = 256;
  /// Maximum number of elements in any collection carried by one message.
  std::size_t max_collection_items = 4096;
  /// Maximum number of relationships accepted by one message.
  std::size_t max_relationships_per_message = 4096;
  /// Maximum number of members in one rack.
  std::size_t max_members = 1'000'000;
  /// Maximum number of relationships in one rack.
  std::size_t max_relationships = 4'000'000;
  /// Maximum number of failure domains in one rack.
  std::size_t max_failure_domains = 100'000;
  /// Maximum number of capabilities attached to one member.
  std::size_t max_capabilities_per_member = 256;
  /// Maximum number of failure domains attached to one member.
  std::size_t max_failure_domains_per_member = 64;
  /// Maximum number of concurrent client sessions.
  std::size_t max_connections = 64;
  /// Maximum number of outstanding requests a client may have in flight.
  std::size_t max_outstanding_requests = 64;
  /// Maximum queued outbound frames per session before the session is closed.
  std::size_t max_send_queue_frames = 1024;
  /// Maximum bytes of persisted state.
  std::uint64_t max_persisted_bytes = 512ULL * 1024ULL * 1024ULL;
  /// Maximum number of snapshots retained in one instance.
  std::size_t max_retained_snapshots = 256;
  /// Maximum number of remembered fenced boot identities.
  std::size_t max_fenced_boots = 65'536;
  /// Maximum number of explanation factors returned by one explanation.
  std::size_t max_explanation_factors = 64;
  /// Maximum number of retries performed by internal I/O helpers.
  std::size_t max_io_retries = 4;
  /// Maximum worker threads used by the coordinator for sessions.
  std::size_t max_session_threads = 32;
  /// Duration after which a publisher with no heartbeat is considered lost.
  std::int64_t publisher_lease_millis = 5000;
  /// Socket poll interval used by blocking internal I/O loops.
  std::int64_t socket_poll_millis = 50;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_LIMITS_HPP
