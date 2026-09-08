// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Two real operating-system processes: a coordinator and a node agent, talking
// over loopback TCP. This is a single-host demonstration, not a multi-host
// deployment, and it says so.
//
// The example starts both processes itself and stops them deterministically by
// creating a stop file, so it never depends on a timeout or on killing a
// process.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

struct ChildProcess {
  std::string command;
  std::string stdout_path;
  std::string stop_file;
#ifdef _WIN32
  PROCESS_INFORMATION info{};
#endif
  bool started = false;
};

#ifdef _WIN32
[[nodiscard]] std::string quote(const std::string& value) { return "\"" + value + "\""; }

[[nodiscard]] bool launch(ChildProcess& child, const std::string& arguments) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE output = ::CreateFileA(child.stdout_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                                &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    return false;
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = output;
  startup.hStdError = output;
  startup.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
  std::string command_line = quote(child.command) + " " + arguments;
  const BOOL created =
      ::CreateProcessA(nullptr, command_line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &startup, &child.info);
  ::CloseHandle(output);
  child.started = created != FALSE;
  return child.started;
}

void stop_and_wait(ChildProcess& child) {
  if (!child.started) {
    return;
  }
  std::ofstream stop(child.stop_file, std::ios::trunc);
  stop << "stop";
  stop.close();
  ::WaitForSingleObject(child.info.hProcess, INFINITE);
  ::CloseHandle(child.info.hThread);
  ::CloseHandle(child.info.hProcess);
  child.started = false;
}

[[nodiscard]] std::string read_file(const std::string& path) {
  std::ifstream stream(path);
  std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  return content;
}

[[nodiscard]] std::string wait_for_port(const std::string& path) {
  for (int attempt = 0; attempt < 600; ++attempt) {
    const std::string content = read_file(path);
    const std::size_t position = content.find("port=");
    if (position != std::string::npos) {
      const std::size_t start = position + 5;
      std::size_t end = start;
      while (end < content.size() && content[end] >= '0' && content[end] <= '9') {
        ++end;
      }
      if (end > start) {
        return content.substr(start, end - start);
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  return {};
}
#endif

}  // namespace

int main(int argc, char** argv) {
#ifndef _WIN32
  (void)argc;
  (void)argv;
  std::cout << "the multiprocess example requires Windows process creation\n";
  return 0;
#else
  const std::string coordinator = argc > 1 ? argv[1] : "rack_fabric_coordinator.exe";
  const std::string agent = argc > 2 ? argv[2] : "rack_fabric_agent.exe";
  const std::filesystem::path directory = std::filesystem::temp_directory_path() / "rack_fabric_demo";
  std::filesystem::create_directories(directory);

  ChildProcess server;
  server.command = coordinator;
  server.stdout_path = (directory / "coordinator.out").string();
  server.stop_file = (directory / "coordinator.stop").string();
  if (!launch(server, "--rack rack-demo --rack-epoch epoch-demo --port 0 --stop-file " +
                           quote(server.stop_file))) {
    std::cerr << "the coordinator could not be started\n";
    return 1;
  }
  const std::string port = wait_for_port(server.stdout_path);
  if (port.empty()) {
    std::cerr << "the coordinator did not report a port\n";
    stop_and_wait(server);
    return 1;
  }
  std::cout << "coordinator listening on port " << port << "\n";

  ChildProcess node;
  node.command = agent;
  node.stdout_path = (directory / "agent.out").string();
  node.stop_file = (directory / "agent.stop").string();
  if (!launch(node, "--port " + port + " --worker demo-worker --no-hardware --stop-file " +
                        quote(node.stop_file))) {
    std::cerr << "the agent could not be started\n";
    stop_and_wait(server);
    return 1;
  }

  for (int attempt = 0; attempt < 600; ++attempt) {
    const std::string content = read_file(node.stdout_path);
    if (content.find("registered") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  std::cout << "agent output: " << read_file(node.stdout_path);
  stop_and_wait(node);
  stop_and_wait(server);
  std::cout << "coordinator output: " << read_file(server.stdout_path);
  std::filesystem::remove_all(directory);
  return 0;
#endif
}
