// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The node agent.
//
// An agent is a client of exactly one coordinator. It owns a process
// incarnation identity (worker plus boot) that is independent of every other
// agent, presents that identity on every mutation, and republishes its
// evidence after a restart. It is not a scheduler, a health authority or a
// hardware capability registry: it publishes rack member evidence and nothing
// else.

#ifndef RACK_FABRIC_AGENT_HPP
#define RACK_FABRIC_AGENT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "rack_fabric/hardware.hpp"
#include "rack_fabric/limits.hpp"
#include "rack_fabric/mutation.hpp"

namespace rack_fabric {

struct AgentOptions {
  std::string coordinator_host = "127.0.0.1";
  std::uint16_t coordinator_port = 0;
  /// Worker identity. When absent, a worker identity is derived from the
  /// host name.
  std::optional<WorkerId> worker;
  /// Process incarnation identity. When absent, a fresh boot identity is
  /// generated, so two runs of the same program are never the same
  /// incarnation.
  std::optional<AgentBootId> boot;
  std::optional<RackId> rack;
  std::string label;
  HardwareDiscoveryOptions hardware;
  ResourceLimits limits;
  bool publish_hardware = true;
  bool enable_heartbeat = true;
  std::chrono::milliseconds heartbeat_interval{500};
  std::chrono::milliseconds io_timeout{5000};
  std::chrono::milliseconds connect_timeout{5000};
  std::size_t max_retained_results = 256;
};

struct AgentReport {
  bool registered = false;
  CoordinatorEpoch coordinator_epoch;
  PublicationGeneration publication_generation;
  std::size_t members_published = 0;
  std::size_t failure_domains_published = 0;
  std::size_t heartbeats_sent = 0;
  std::vector<MutationResult> results;
  std::vector<std::string> diagnostics;
  std::string last_error;
};

class Agent {
 public:
  [[nodiscard]] static std::optional<std::unique_ptr<Agent>> start(const AgentOptions& options,
                                                                   std::string& error);

  Agent(const Agent&) = delete;
  Agent& operator=(const Agent&) = delete;
  ~Agent();

  /// Stops the heartbeat thread and closes the connection. Deterministic: no
  /// thread is left running.
  void stop();

  [[nodiscard]] bool connected() const noexcept;
  [[nodiscard]] CoordinatorEpoch coordinator_epoch() const;
  [[nodiscard]] AgentBootId boot_id() const;
  [[nodiscard]] WorkerId worker_id() const;
  [[nodiscard]] AgentReport report() const;

  /// Republishes the agent's local hardware evidence under the current
  /// authority. Used after a coordinator restart and by tests.
  [[nodiscard]] bool publish_now(std::string& error);

  /// Closes the transport without touching the coordinator, simulating an
  /// agent-side transport failure.
  void disconnect() noexcept;

  /// Generates a fresh boot identity that is unique within this process and
  /// across processes.
  [[nodiscard]] static AgentBootId generate_boot_id();

 private:
  explicit Agent(const AgentOptions& options);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_AGENT_HPP
