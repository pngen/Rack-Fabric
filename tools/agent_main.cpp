// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// rack_fabric_agent: a node agent running as a real operating-system process.
//
// Every invocation is a distinct process incarnation: unless a boot identity is
// supplied, a fresh one is generated, so a restarted agent is never confused
// with its predecessor.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <thread>

#include "rack_fabric/agent.hpp"
#include "rack_fabric/version.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_stop{false};

#ifdef _WIN32
BOOL WINAPI console_handler(DWORD event) {
  if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
    g_stop.store(true, std::memory_order_release);
    return TRUE;
  }
  return FALSE;
}
#endif

struct Options {
  std::uint16_t port = 0;
  std::string worker;
  std::string boot;
  std::string rack;
  std::string label;
  std::string stop_file;
  std::string status_file;
  bool no_heartbeat = false;
  bool no_hardware = false;
  int heartbeat_millis = 250;
  bool help = false;
};

void print_usage() {
  std::cout << "rack_fabric_agent " << rack_fabric::kVersionString << "\n"
            << "Usage: rack_fabric_agent --port <n> [options]\n"
            << "  --port <n>             coordinator loopback port (required)\n"
            << "  --worker <id>          worker identity\n"
            << "  --boot <id>            process incarnation identity\n"
            << "  --rack <id>            expected rack identity\n"
            << "  --label <text>         human-readable label\n"
            << "  --heartbeat-ms <n>     heartbeat interval (default 250)\n"
            << "  --no-heartbeat         do not send heartbeats\n"
            << "  --no-hardware          register without publishing hardware\n"
            << "  --stop-file <path>     exit when this file exists\n"
            << "  --status-file <path>   write a status line to this file\n"
            << "  --version              print the version and exit\n"
            << "  --help                 print this message\n";
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    const auto next = [&index, argc, argv]() -> std::optional<std::string> {
      if (index + 1 >= argc) {
        return std::nullopt;
      }
      return std::string(argv[++index]);
    };
    if (argument == "--port") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.port = static_cast<std::uint16_t>(std::stoi(*value));
    } else if (argument == "--worker") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.worker = *value;
    } else if (argument == "--boot") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.boot = *value;
    } else if (argument == "--rack") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.rack = *value;
    } else if (argument == "--label") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.label = *value;
    } else if (argument == "--heartbeat-ms") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.heartbeat_millis = std::stoi(*value);
    } else if (argument == "--no-heartbeat") {
      options.no_heartbeat = true;
    } else if (argument == "--no-hardware") {
      options.no_hardware = true;
    } else if (argument == "--stop-file") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.stop_file = *value;
    } else if (argument == "--status-file") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.status_file = *value;
    } else if (argument == "--version") {
      std::cout << rack_fabric::kVersionString << "\n";
      std::exit(0);
    } else if (argument == "--help" || argument == "-h") {
      options.help = true;
    } else {
      std::cerr << "unknown argument: " << argument << "\n";
      return false;
    }
  }
  return true;
}

void write_status(const Options& options, const rack_fabric::Agent& agent) {
  if (options.status_file.empty()) {
    return;
  }
  const rack_fabric::AgentReport report = agent.report();
  std::ofstream out(options.status_file, std::ios::trunc);
  if (!out) {
    return;
  }
  out << "boot=" << agent.boot_id().value() << "\n";
  out << "worker=" << agent.worker_id().value() << "\n";
  out << "epoch=" << agent.coordinator_epoch().value() << "\n";
  out << "registered=" << (report.registered ? 1 : 0) << "\n";
  out << "members=" << report.members_published << "\n";
  out << "failure_domains=" << report.failure_domains_published << "\n";
  out << "heartbeats=" << report.heartbeats_sent << "\n";
  out << "connected=" << (agent.connected() ? 1 : 0) << "\n";
  out << "last_error=" << report.last_error << "\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 2;
  }
  if (options.help) {
    print_usage();
    return 0;
  }
  if (options.port == 0) {
    std::cerr << "a coordinator port is required\n";
    return 2;
  }

#ifdef _WIN32
  ::SetConsoleCtrlHandler(console_handler, TRUE);
#endif

  rack_fabric::AgentOptions agent_options;
  agent_options.coordinator_port = options.port;
  if (!options.worker.empty()) {
    agent_options.worker = rack_fabric::WorkerId::parse(options.worker);
    if (!agent_options.worker.has_value()) {
      std::cerr << "the worker identity is not valid\n";
      return 2;
    }
  }
  if (!options.boot.empty()) {
    agent_options.boot = rack_fabric::AgentBootId::parse(options.boot);
    if (!agent_options.boot.has_value()) {
      std::cerr << "the boot identity is not valid\n";
      return 2;
    }
  }
  if (!options.rack.empty()) {
    agent_options.rack = rack_fabric::RackId::parse(options.rack);
  }
  agent_options.label = options.label;
  agent_options.enable_heartbeat = !options.no_heartbeat;
  agent_options.publish_hardware = !options.no_hardware;
  agent_options.heartbeat_interval = std::chrono::milliseconds{options.heartbeat_millis};

  std::string error;
  auto agent = rack_fabric::Agent::start(agent_options, error);
  if (!agent.has_value()) {
    std::cerr << "the agent could not start: " << error << "\n";
    return 1;
  }

  const rack_fabric::AgentReport report = (*agent)->report();
  std::cout << "registered boot=" << (*agent)->boot_id().value()
            << " worker=" << (*agent)->worker_id().value()
            << " epoch=" << (*agent)->coordinator_epoch().value()
            << " members=" << report.members_published
            << " failure_domains=" << report.failure_domains_published << "\n";
  std::cout.flush();
  write_status(options, **agent);

  while (!g_stop.load(std::memory_order_acquire)) {
    if (!options.stop_file.empty() && std::filesystem::exists(options.stop_file)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }

  (*agent)->stop();
  write_status(options, **agent);
  std::cout << "stopped boot=" << (*agent)->boot_id().value() << "\n";
  std::cout.flush();
  return 0;
}
