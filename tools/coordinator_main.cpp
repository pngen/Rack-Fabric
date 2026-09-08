// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// rack_fabric_coordinator: runs the authoritative rack runtime as a real
// operating-system process.
//
// Shutdown is deterministic: the process exits when the stop file appears, so
// a test harness never has to kill it on a timer. Killing it outright is also
// supported and is a supported failure mode: the next process incarnation
// advances the coordinator epoch and recovers conservatively.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "rack_fabric/coordinator.hpp"
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
  std::string rack = "rack-demo";
  std::string rack_epoch = "epoch-demo";
  std::string rack_label = "rack_fabric_coordinator";
  std::uint16_t port = 0;
  std::string state_path;
  bool persist = false;
  std::string stop_file;
  std::string lease_millis;
  bool help = false;
};

void print_usage() {
  std::cout << "rack_fabric_coordinator " << rack_fabric::kVersionString << "\n"
            << "Usage: rack_fabric_coordinator [options]\n"
            << "  --rack <id>            rack identity (default rack-demo)\n"
            << "  --rack-epoch <id>      rack incarnation (default epoch-demo)\n"
            << "  --rack-label <text>    human-readable label\n"
            << "  --port <n>             loopback port; 0 selects an ephemeral port\n"
            << "  --state <path>         persist authoritative state to this file\n"
            << "  --persist              write state after every accepted mutation\n"
            << "  --stop-file <path>     exit when this file exists\n"
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
    if (argument == "--rack") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.rack = *value;
    } else if (argument == "--rack-epoch") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.rack_epoch = *value;
    } else if (argument == "--rack-label") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.rack_label = *value;
    } else if (argument == "--port") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.port = static_cast<std::uint16_t>(std::stoi(*value));
    } else if (argument == "--state") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.state_path = *value;
    } else if (argument == "--persist") {
      options.persist = true;
    } else if (argument == "--stop-file") {
      const auto value = next();
      if (!value.has_value()) return false;
      options.stop_file = *value;
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

#ifdef _WIN32
  ::SetConsoleCtrlHandler(console_handler, TRUE);
#endif

  rack_fabric::CoordinatorOptions coordinator_options;
  coordinator_options.port = options.port;
  coordinator_options.rack = rack_fabric::RackId::parse(options.rack);
  coordinator_options.rack_epoch = rack_fabric::RackEpochId::parse(options.rack_epoch);
  coordinator_options.rack_label = options.rack_label;
  coordinator_options.persistence_path = options.state_path;
  coordinator_options.persist_after_mutation = options.persist;
  if (!coordinator_options.rack.has_value() || !coordinator_options.rack_epoch.has_value()) {
    std::cerr << "the rack identity and rack epoch are not valid identities\n";
    return 2;
  }

  std::string error;
  auto server = rack_fabric::CoordinatorServer::start(coordinator_options, error);
  if (!server.has_value()) {
    std::cerr << "the coordinator could not start: " << error << "\n";
    return 1;
  }
  if (options.state_path.empty()) {
    // Nothing to recover.
  } else if (std::filesystem::exists(options.state_path)) {
    const rack_fabric::PersistenceResult loaded = (*server)->fabric().load_state(options.state_path);
    if (loaded.status != rack_fabric::PersistenceStatus::Ok) {
      std::cerr << "state recovery failed: " << loaded.explanation.render() << "\n";
      return 1;
    }
    std::cout << "recovered epoch=" << (*server)->epoch().value() << "\n";
  }

  std::cout << "listening port=" << (*server)->port() << " epoch=" << (*server)->epoch().value()
            << " version=" << rack_fabric::kVersionString << "\n";
  std::cout.flush();

  while (!g_stop.load(std::memory_order_acquire)) {
    if (!options.stop_file.empty() && std::filesystem::exists(options.stop_file)) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }

  if (!options.state_path.empty()) {
    const rack_fabric::PersistenceResult saved =
        (*server)->fabric().save_state(options.state_path);
    if (saved.status != rack_fabric::PersistenceStatus::Ok) {
      std::cerr << "final persistence failed: " << saved.explanation.render() << "\n";
    }
  }
  (*server)->stop();
  std::cout << "stopped accepted=" << (*server)->accepted_mutations()
            << " rejected=" << (*server)->rejected_mutations()
            << " connections=" << (*server)->total_connections() << "\n";
  std::cout.flush();
  return 0;
}
