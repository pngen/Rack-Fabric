// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The distributed coordinator.
//
// The coordinator owns authoritative state and speaks the framed protocol over
// a loopback TCP endpoint. It is a real out-of-process server: node agents are
// separate operating-system processes, their boot identities are independent,
// and losing an agent is observable as a fenced boot identity rather than as
// an in-process callback that never returns.
//
// The server is single-host and loopback-only by construction. That is a
// deliberate limitation, not a claim about a multi-host deployment.

#ifndef RACK_FABRIC_COORDINATOR_HPP
#define RACK_FABRIC_COORDINATOR_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/limits.hpp"
#include "rack_fabric/rack_fabric.hpp"

namespace rack_fabric {

struct CoordinatorOptions {
  /// Loopback bind address. Only loopback addresses are accepted.
  std::string bind_host = "127.0.0.1";
  /// 0 selects an ephemeral port, reported by CoordinatorServer::port().
  std::uint16_t port = 0;
  ResourceLimits limits;
  RackFabricOptions fabric;
  /// Rack identity declared by the coordinator at start. A publisher can only
  /// register against a rack that already exists, so the coordinator owns the
  /// declaration unless the state was loaded from persistence.
  std::optional<RackId> rack;
  std::optional<RackEpochId> rack_epoch;
  std::optional<std::string> rack_label;
  /// When set, authoritative state is persisted after every accepted mutation.
  std::string persistence_path;
  bool persist_after_mutation = false;
  /// Bounded waits so that shutdown is deterministic and no thread blocks
  /// indefinitely on a socket.
  std::chrono::milliseconds accept_wait{20};
  std::chrono::milliseconds session_wait{200};
  std::chrono::milliseconds connect_timeout{5000};
  std::size_t max_connections = 64;
  std::size_t max_retained_events = 512;
};

struct CoordinatorEvent {
  Timestamp at = Timestamp::unknown();
  std::string kind;
  std::string detail;
};

class CoordinatorServer {
 public:
  [[nodiscard]] static std::optional<std::unique_ptr<CoordinatorServer>> start(
      const CoordinatorOptions& options, std::string& error);

  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;
  ~CoordinatorServer();

  /// Deterministic shutdown: stop accepting, close every session socket, then
  /// join every thread. Safe to call more than once.
  void stop();

  [[nodiscard]] std::uint16_t port() const noexcept;
  [[nodiscard]] RackFabric& fabric() noexcept;
  [[nodiscard]] const RackFabric& fabric() const noexcept;
  [[nodiscard]] CoordinatorEpoch epoch() const noexcept;
  [[nodiscard]] std::size_t open_connections() const;
  [[nodiscard]] std::size_t total_connections() const;
  [[nodiscard]] std::size_t accepted_mutations() const;
  [[nodiscard]] std::size_t rejected_mutations() const;
  [[nodiscard]] std::size_t malformed_frames() const;
  [[nodiscard]] std::vector<CoordinatorEvent> events() const;

 private:
  explicit CoordinatorServer(const CoordinatorOptions& options);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_COORDINATOR_HPP
