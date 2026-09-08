// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Multiprocess proofs. Every process here is a real operating-system process
// and every connection is a real loopback TCP connection. The harness runs on
// a single host, so this is single-host multiprocess evidence, not evidence of
// multi-host distributed behavior.
//
// There is no timeout of any kind: the harness polls child output and child
// liveness without an artificial deadline, so a hang is reported as a hang
// instead of being silently converted into a pass or a kill.

#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "rack_fabric/protocol.hpp"
#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"

namespace {

using namespace rack_fabric;

constexpr const char* kRack = "rack-mp";
constexpr const char* kRackEpoch = "epoch-1";

std::string make_directory(const char* tag) {
  static std::atomic<unsigned> counter{0};
  const std::filesystem::path base =
      std::filesystem::temp_directory_path() /
      ("rack_fabric_mp_" + std::string(tag) + "_" + std::to_string(GetCurrentProcessId()) + "_" +
       std::to_string(counter.fetch_add(1)));
  std::filesystem::create_directories(base);
  return base.string();
}

void remove_directory(const std::string& directory) {
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

std::string executable_path(const char* environment, const char* name) {
  const char* from_environment = std::getenv(environment);
  if (from_environment != nullptr && *from_environment != '\0') {
    return std::string(from_environment);
  }
  char module[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameA(nullptr, module, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return std::string(name);
  }
  // CTest points the environment variables at the built tools. When the suite
  // is run directly, the tools sit next to or one level above the test binary.
  const std::filesystem::path start = std::filesystem::path(module).parent_path();
  std::filesystem::path directory = start;
  for (int level = 0; level < 3; ++level) {
    const std::filesystem::path candidate = directory / name;
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
    directory = directory.parent_path();
  }
  return (start / name).string();
}

std::string read_text(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return std::string();
  }
  return std::string((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
}

struct Child {
  PROCESS_INFORMATION info{};
  std::string output_path;
  std::string stop_path;
  bool launched = false;

  void prepare(const std::string& directory, const std::string& tag) {
    output_path = directory + "\\" + tag + "-output.txt";
    stop_path = directory + "\\" + tag + "-stop.txt";
  }
  [[nodiscard]] bool alive() const {
    return launched && WaitForSingleObject(info.hProcess, 0) == WAIT_TIMEOUT;
  }
  [[nodiscard]] std::string output() const { return read_text(output_path); }
  void request_stop() const {
    std::ofstream stream(stop_path, std::ios::binary | std::ios::trunc);
    stream << "stop";
  }
  [[nodiscard]] DWORD wait_for_exit() const {
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    return code;
  }
  void terminate() const { TerminateProcess(info.hProcess, 3); }
  void release() {
    if (launched) {
      CloseHandle(info.hThread);
      CloseHandle(info.hProcess);
      launched = false;
    }
  }
};

bool launch(Child& child, const std::string& executable, const std::string& arguments,
            const std::string& directory) {
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  const HANDLE output = CreateFileA(child.output_path.c_str(), GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (output == INVALID_HANDLE_VALUE) {
    return false;
  }
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  startup.hStdOutput = output;
  startup.hStdError = output;

  std::string command_line = "\"" + executable + "\" " + arguments;
  std::vector<char> mutable_line(command_line.begin(), command_line.end());
  mutable_line.push_back('\0');
  const BOOL created = CreateProcessA(nullptr, mutable_line.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup,
                                      &child.info);
  CloseHandle(output);
  if (!created) {
    return false;
  }
  child.launched = true;
  return true;
}

/// Polls the redirected output until it contains the needle. The loop ends when
/// the child exits, so it is bounded by real progress, not by a timeout.
bool wait_for_output(Child& child, const std::string& needle) {
  // Bounded polling, never a process kill: if the child does not produce the
  // expected line the check fails with a diagnostic and the suite continues.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
  while (std::chrono::steady_clock::now() < deadline) {
    if (child.output().find(needle) != std::string::npos) {
      return true;
    }
    if (!child.alive()) {
      return child.output().find(needle) != std::string::npos;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  rftest::record_failure(__FILE__, __LINE__,
                         "the child never produced: " + needle + " output so far: " +
                             child.output());
  return false;
}

[[nodiscard]] bool exited_cleanly(Child& child) { return child.wait_for_exit() == 0; }

std::uint16_t parse_port(const std::string& text) {
  const std::size_t marker = text.find("port=");
  if (marker == std::string::npos) {
    return 0;
  }
  return static_cast<std::uint16_t>(std::stoi(text.substr(marker + 5)));
}

std::uint64_t parse_epoch(const std::string& text) {
  const std::size_t marker = text.find("epoch=");
  if (marker == std::string::npos) {
    return 0;
  }
  return std::stoull(text.substr(marker + 6));
}

std::size_t parse_number(const std::string& text, const std::string& key) {
  const std::size_t marker = text.find(key);
  if (marker == std::string::npos) {
    return 0;
  }
  return static_cast<std::size_t>(std::stoull(text.substr(marker + key.size())));
}

/// A minimal protocol client built only from the public wire API. The socket
/// is owned by the value: every session, successful or not, is closed, so the
/// client can never exhaust the coordinator's connection limit.
struct Wire {
  SOCKET socket = INVALID_SOCKET;
  std::uint64_t correlation = 0;

  Wire() = default;
  Wire(const Wire&) = delete;
  Wire& operator=(const Wire&) = delete;
  ~Wire() { close(); }

  bool open(std::uint16_t port, std::string& error) {
    socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
      error = "socket() failed";
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
      error = "inet_pton failed";
      return false;
    }
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) ==
        SOCKET_ERROR) {
      error = "connect failed: " + std::to_string(::WSAGetLastError());
      return false;
    }
    // A transport receive timeout: a peer that stops answering fails the
    // check with a diagnostic instead of blocking the suite forever. This is
    // a socket option, never a test timeout that terminates anything.
    const DWORD receive_timeout_ms = 500;
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&receive_timeout_ms), sizeof(receive_timeout_ms));
    return true;
  }
  void close() {
    if (socket != INVALID_SOCKET) {
      ::shutdown(socket, SD_BOTH);
      ::closesocket(socket);
      socket = INVALID_SOCKET;
    }
  }
  bool send_bytes(const std::byte* data, std::size_t size, std::string& error) {
    std::size_t sent = 0;
    while (sent < size) {
      const int written = ::send(socket, reinterpret_cast<const char*>(data + sent),
                                 static_cast<int>(size - sent), 0);
      if (written <= 0) {
        error = "send failed: " + std::to_string(::WSAGetLastError());
        return false;
      }
      sent += static_cast<std::size_t>(written);
    }
    return true;
  }
  bool receive_exact(std::byte* data, std::size_t size, std::string& error) {
    std::size_t received = 0;
    while (received < size) {
      const int read = ::recv(socket, reinterpret_cast<char*>(data + received),
                              static_cast<int>(size - received), 0);
      if (read <= 0) {
        error = "receive failed: " + std::to_string(::WSAGetLastError());
        return false;
      }
      received += static_cast<std::size_t>(read);
    }
    return true;
  }
  bool send_frame(MessageType type, const std::vector<std::byte>& payload, std::string& error) {
    Frame frame;
    frame.type = type;
    frame.correlation = ++correlation;
    frame.payload = payload;
    const auto bytes = encode_frame(frame, ResourceLimits{});
    if (!bytes.has_value()) {
      error = "the frame could not be encoded";
      return false;
    }
    return send_bytes(bytes->data(), bytes->size(), error);
  }
  bool receive_frame(Frame& frame, std::string& error) {
    // Exactly the 24-byte header first, then exactly the declared payload and
    // trailer. Reading the overhead up front would consume payload bytes and
    // then block waiting for four bytes that never arrive.
    std::vector<std::byte> buffer(kFrameHeaderBytes);
    if (!receive_exact(buffer.data(), buffer.size(), error)) {
      return false;
    }
    std::uint32_t payload_length = 0;
    std::memcpy(&payload_length, buffer.data() + 20, sizeof(payload_length));
    if (payload_length > ResourceLimits{}.max_payload_bytes) {
      error = "the coordinator declared an oversized payload";
      return false;
    }
    buffer.resize(kFrameHeaderBytes + payload_length + kFrameTrailerBytes);
    if (!receive_exact(buffer.data() + kFrameHeaderBytes, payload_length + kFrameTrailerBytes,
                       error)) {
      return false;
    }
    const DecodeResult decoded = decode_frame(buffer.data(), buffer.size(), ResourceLimits{});
    if (!decoded.ok()) {
      error = "decode failed: " + decoded.detail;
      return false;
    }
    frame = decoded.frame;
    return true;
  }
};

struct QueryOutcome {
  bool ok = false;
  std::string detail;
  std::string error;
};

/// Opens a fresh session, introduces itself and runs one query.
QueryOutcome ask(std::uint16_t port, QueryKind kind) {
  QueryOutcome outcome;
  Wire wire;
  if (!wire.open(port, outcome.error)) {
    return outcome;
  }
  HelloMessage hello;
  hello.coordinator_epoch = CoordinatorEpoch::first();
  hello.agent_version = kVersionString;
  const auto hello_payload = MessageCodec::encode(hello, ResourceLimits{});
  if (!hello_payload.has_value()) {
    outcome.error = "HELLO could not be encoded";
    return outcome;
  }
  if (!wire.send_frame(MessageType::Hello, *hello_payload, outcome.error)) {
    return outcome;
  }
  Frame ack;
  if (!wire.receive_frame(ack, outcome.error)) {
    return outcome;
  }
  if (ack.type != MessageType::HelloAck) {
    outcome.error = "expected HELLO_ACK";
    return outcome;
  }
  QueryMessage query;
  query.query_kind = static_cast<std::uint16_t>(kind);
  const auto query_payload = MessageCodec::encode(query, ResourceLimits{});
  if (!query_payload.has_value()) {
    outcome.error = "QUERY could not be encoded";
    return outcome;
  }
  if (!wire.send_frame(MessageType::Query, *query_payload, outcome.error)) {
    return outcome;
  }
  Frame reply;
  if (!wire.receive_frame(reply, outcome.error)) {
    return outcome;
  }
  ResultMessage result;
  if (MessageCodec::decode(reply.payload.data(), reply.payload.size(), result, ResourceLimits{}) !=
      DecodeStatus::Ok) {
    outcome.error = "the reply is not a RESULT message";
    return outcome;
  }
  outcome.ok = true;
  outcome.detail = result.detail;
  wire.close();
  return outcome;
}

/// Polls the coordinator until the predicate accepts a reply. The loop ends
/// when the child exits, so it is bounded by real progress.
template <class Predicate>
bool poll_query(Child& child, std::uint16_t port, QueryKind kind, Predicate predicate,
                std::string& last) {
  // Bounded by elapsed time, never by killing anything: the coordinator is
  // given a generous window to reach the expected state, then the check fails
  // with the last reply it produced.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
  while (std::chrono::steady_clock::now() < deadline) {
    const QueryOutcome outcome = ask(port, kind);
    if (outcome.ok) {
      last = outcome.detail;
      if (predicate(outcome.detail)) {
        return true;
      }
    } else {
      last = outcome.error;
    }
    if (!child.alive()) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
  }
  rftest::record_failure(__FILE__, __LINE__,
                         "the coordinator never reached the expected state; last reply: " + last);
  return false;
}

struct Winsock {
  Winsock() {
    WSADATA data{};
    RF_REQUIRE(WSAStartup(MAKEWORD(2, 2), &data) == 0);
  }
  ~Winsock() { WSACleanup(); }
  Winsock(const Winsock&) = delete;
  Winsock& operator=(const Winsock&) = delete;
};

/// Starts a coordinator in its own process and reports its loopback port.
void start_coordinator(Child& child, const std::string& directory, const std::string& tag,
                       const std::string& extra, std::uint16_t& port) {
  child.prepare(directory, tag);
  const std::string arguments = "--rack " + std::string(kRack) + " --rack-epoch " +
                                std::string(kRackEpoch) + " --port 0 --stop-file \"" +
                                child.stop_path + "\"" + (extra.empty() ? "" : " " + extra);
  RF_REQUIRE(launch(child, executable_path("RACK_FABRIC_COORDINATOR", "rack_fabric_coordinator.exe"),
                    arguments, directory));
  RF_REQUIRE(wait_for_output(child, "listening port="));
  port = parse_port(child.output());
  RF_CHECK(port != 0);
}

void start_agent(Child& child, const std::string& directory, const std::string& tag,
                 std::uint16_t port, const std::string& worker, const std::string& boot,
                 const std::string& extra) {
  child.prepare(directory, tag);
  const std::string arguments = "--port " + std::to_string(port) + " --worker " + worker +
                                " --rack " + std::string(kRack) +
                                (boot.empty() ? "" : " --boot " + boot) + " --stop-file \"" +
                                child.stop_path + "\"" + (extra.empty() ? "" : " " + extra);
  RF_REQUIRE(launch(child, executable_path("RACK_FABRIC_AGENT", "rack_fabric_agent.exe"), arguments,
                    directory));
}

}  // namespace

RF_TEST(multiprocess, coordinator_and_agent_are_separate_operating_system_processes) {
  const Winsock winsock;
  const std::string directory = make_directory("separate");
  Child coordinator;
  std::uint16_t port = 0;
  start_coordinator(coordinator, directory, "coordinator", "", port);

  const QueryOutcome before = ask(port, QueryKind::Summary);
  RF_REQUIRE(before.ok);
  RF_CHECK_EQ(parse_number(before.detail, "members="), std::size_t{0});

  Child agent;
  start_agent(agent, directory, "agent", port, "worker-1", "", "");
  RF_REQUIRE(wait_for_output(agent, "registered boot="));
  RF_CHECK(agent.output().find("members=") != std::string::npos);

  const QueryOutcome after = ask(port, QueryKind::Summary);
  RF_REQUIRE(after.ok);
  RF_CHECK(parse_number(after.detail, "members=") > 0);
  RF_CHECK_EQ(parse_number(after.detail, "publishers="), std::size_t{1});
  RF_CHECK(agent.alive());

  agent.request_stop();
  RF_REQUIRE(exited_cleanly(agent));
  agent.release();

  // Process death is detected by the runtime: the lease expires, the boot is
  // fenced and the evidence it published stops counting as current.
  std::string observed;
  RF_REQUIRE(poll_query(coordinator, port, QueryKind::Publishers,
                        [](const std::string& detail) {
                          return parse_number(detail, "fenced=") >= 1;
                        },
                        observed));
  const QueryOutcome fenced = ask(port, QueryKind::Publishers);
  RF_REQUIRE(fenced.ok);
  RF_CHECK_EQ(parse_number(fenced.detail, "publishers="), std::size_t{1});

  const QueryOutcome lifecycle = ask(port, QueryKind::Summary);
  RF_REQUIRE(lifecycle.ok);
  RF_CHECK(lifecycle.detail.find("lifecycle=READY") == std::string::npos);

  coordinator.request_stop();
  RF_REQUIRE(exited_cleanly(coordinator));
  RF_CHECK(coordinator.output().find("stopped accepted=") != std::string::npos);
  coordinator.release();
  remove_directory(directory);
}

RF_TEST(multiprocess, a_killed_agent_is_fenced_and_reincarnates_with_a_fresh_boot_identity) {
  const Winsock winsock;
  const std::string directory = make_directory("kill");
  Child coordinator;
  std::uint16_t port = 0;
  start_coordinator(coordinator, directory, "coordinator", "", port);

  Child first;
  start_agent(first, directory, "first", port, "worker-1", "boot-alpha", "");
  RF_REQUIRE(wait_for_output(first, "registered boot=boot-alpha"));
  RF_CHECK(first.alive());

  // Real process death: the process is terminated without any cooperation.
  first.terminate();
  RF_CHECK_NE(first.wait_for_exit(), static_cast<DWORD>(0));
  first.release();

  std::string observed;
  RF_REQUIRE(poll_query(coordinator, port, QueryKind::Publishers,
                        [](const std::string& detail) {
                          return parse_number(detail, "fenced=") >= 1;
                        },
                        observed));

  // A new incarnation of the same worker registers with a fresh boot identity.
  Child second;
  start_agent(second, directory, "second", port, "worker-1", "boot-beta", "");
  RF_REQUIRE(wait_for_output(second, "registered boot=boot-beta"));
  const QueryOutcome publishers = ask(port, QueryKind::Publishers);
  RF_REQUIRE(publishers.ok);
  RF_CHECK_EQ(parse_number(publishers.detail, "publishers="), std::size_t{2});

  // The dead incarnation can never regain authority: replaying its boot
  // identity is refused.
  Child replay;
  start_agent(replay, directory, "replay", port, "worker-1", "boot-alpha", "");
  RF_CHECK_NE(replay.wait_for_exit(), static_cast<DWORD>(0));
  RF_CHECK(replay.output().find("could not start") != std::string::npos);
  RF_CHECK(replay.output().find("refused registration for boot boot-alpha") != std::string::npos);
  replay.release();

  // The live incarnation is unaffected.
  RF_CHECK(second.alive());
  const QueryOutcome summary = ask(port, QueryKind::Summary);
  RF_REQUIRE(summary.ok);
  RF_CHECK(parse_number(summary.detail, "members=") > 0);

  second.request_stop();
  RF_REQUIRE(exited_cleanly(second));
  second.release();
  coordinator.request_stop();
  RF_REQUIRE(exited_cleanly(coordinator));
  coordinator.release();
  remove_directory(directory);
}

RF_TEST(multiprocess, coordinator_restart_advances_the_epoch_and_recovers_conservatively) {
  const Winsock winsock;
  const std::string directory = make_directory("restart");
  const std::string state_path = directory + "\\state.rkf";

  Child first;
  std::uint16_t first_port = 0;
  start_coordinator(first, directory, "first", "--state \"" + state_path + "\" --persist",
                    first_port);
  const std::uint64_t first_epoch = parse_epoch(first.output());
  RF_CHECK_EQ(first_epoch, std::uint64_t{1});

  Child agent;
  start_agent(agent, directory, "agent", first_port, "worker-1", "boot-one", "");
  RF_REQUIRE(wait_for_output(agent, "registered boot=boot-one"));
  const QueryOutcome published = ask(first_port, QueryKind::Summary);
  RF_REQUIRE(published.ok);
  const std::size_t members_before = parse_number(published.detail, "members=");
  RF_CHECK(members_before > 0);

  agent.request_stop();
  RF_REQUIRE(exited_cleanly(agent));
  agent.release();
  first.request_stop();
  RF_REQUIRE(exited_cleanly(first));
  first.release();
  RF_REQUIRE(std::filesystem::exists(state_path));

  // A brand new coordinator process recovers the same rack.
  Child second;
  std::uint16_t second_port = 0;
  start_coordinator(second, directory, "second", "--state \"" + state_path + "\" --persist",
                    second_port);
  RF_CHECK_EQ(parse_epoch(second.output()), first_epoch + 1);

  const QueryOutcome generations = ask(second_port, QueryKind::Generations);
  RF_REQUIRE(generations.ok);
  RF_CHECK_EQ(parse_number(generations.detail, "coordinator_epoch="), std::size_t{2});

  const QueryOutcome recovered = ask(second_port, QueryKind::Summary);
  RF_REQUIRE(recovered.ok);
  RF_CHECK_EQ(parse_number(recovered.detail, "members="), members_before);
  RF_CHECK(recovered.detail.find("lifecycle=REVALIDATION_REQUIRED") != std::string::npos);

  const QueryOutcome publishers = ask(second_port, QueryKind::Publishers);
  RF_REQUIRE(publishers.ok);
  RF_CHECK_EQ(parse_number(publishers.detail, "publishers="), std::size_t{1});
  RF_CHECK_EQ(parse_number(publishers.detail, "fenced="), std::size_t{0});

  // A reincarnated agent must publish under the new authority before the rack
  // is current again.
  Child reincarnated;
  start_agent(reincarnated, directory, "reincarnated", second_port, "worker-1", "boot-two", "");
  RF_REQUIRE(wait_for_output(reincarnated, "registered boot=boot-two"));
  const QueryOutcome after = ask(second_port, QueryKind::Summary);
  RF_REQUIRE(after.ok);
  RF_CHECK(after.detail.find("lifecycle=REVALIDATION_REQUIRED") == std::string::npos);

  reincarnated.request_stop();
  RF_REQUIRE(exited_cleanly(reincarnated));
  reincarnated.release();
  second.request_stop();
  RF_REQUIRE(exited_cleanly(second));
  second.release();
  remove_directory(directory);
}

RF_TEST(multiprocess, malformed_wire_input_is_refused_without_terminating_the_coordinator) {
  const Winsock winsock;
  const std::string directory = make_directory("malformed");
  Child coordinator;
  std::uint16_t port = 0;
  start_coordinator(coordinator, directory, "coordinator", "", port);

  {
    // Arbitrary bytes from an unknown peer.
    Wire wire;
    std::string error;
    RF_REQUIRE(wire.open(port, error));
    std::vector<std::byte> garbage(256);
    for (std::size_t index = 0; index < garbage.size(); ++index) {
      garbage[index] = static_cast<std::byte>((index * 37U) & 0xFFU);
    }
    RF_CHECK(wire.send_bytes(garbage.data(), garbage.size(), error));
    wire.close();
  }
  {
    // A structurally valid frame with a broken checksum.
    Wire wire;
    std::string error;
    RF_REQUIRE(wire.open(port, error));
    Frame frame;
    frame.type = MessageType::Hello;
    frame.correlation = 1;
    HelloMessage hello;
    hello.coordinator_epoch = CoordinatorEpoch::first();
    hello.agent_version = kVersionString;
    const auto payload = MessageCodec::encode(hello, ResourceLimits{});
    RF_REQUIRE(payload.has_value());
    frame.payload = *payload;
    const auto bytes = encode_frame(frame, ResourceLimits{});
    RF_REQUIRE(bytes.has_value());
    std::vector<std::byte> corrupted = *bytes;
    corrupted[corrupted.size() - 1] =
        static_cast<std::byte>(corrupted[corrupted.size() - 1] ^ std::byte{0x01});
    RF_CHECK(wire.send_bytes(corrupted.data(), corrupted.size(), error));
    wire.close();
  }
  {
    // A declared payload length far beyond the bound must not allocate.
    Wire wire;
    std::string error;
    RF_REQUIRE(wire.open(port, error));
    std::vector<std::byte> header(kFrameHeaderBytes, std::byte{0});
    std::memcpy(header.data(), kFrameMagic, sizeof(kFrameMagic));
    header[4] = std::byte{1};
    header[6] = std::byte{1};
    const std::uint32_t declared = 0x7FFFFFFFu;
    std::memcpy(header.data() + 20, &declared, sizeof(declared));
    RF_CHECK(wire.send_bytes(header.data(), header.size(), error));
    wire.close();
  }
  {
    // A first message that is not HELLO is refused.
    Wire wire;
    std::string error;
    RF_REQUIRE(wire.open(port, error));
    QueryMessage query;
    query.query_kind = static_cast<std::uint16_t>(QueryKind::Summary);
    const auto payload = MessageCodec::encode(query, ResourceLimits{});
    RF_REQUIRE(payload.has_value());
    RF_CHECK(wire.send_frame(MessageType::Query, *payload, error));
    Frame reply;
    if (wire.receive_frame(reply, error)) {
      RF_CHECK(reply.type == MessageType::Error);
      ErrorMessage message;
      if (MessageCodec::decode(reply.payload.data(), reply.payload.size(), message,
                               ResourceLimits{}) == DecodeStatus::Ok) {
        RF_CHECK(message.error == ProtocolError::MalformedPayload);
      } else {
        RF_CHECK(false);
      }
    }
    wire.close();
  }

  // The coordinator survived every hostile input and still serves real
  // sessions.
  RF_CHECK(coordinator.alive());
  const QueryOutcome summary = ask(port, QueryKind::Summary);
  RF_REQUIRE(summary.ok);
  RF_CHECK(summary.detail.find("lifecycle=") != std::string::npos);

  coordinator.request_stop();
  RF_REQUIRE(exited_cleanly(coordinator));
  coordinator.release();
  remove_directory(directory);
}

RF_TEST(multiprocess, many_concurrent_agent_sessions_are_served) {
  const Winsock winsock;
  const std::string directory = make_directory("many");
  Child coordinator;
  std::uint16_t port = 0;
  start_coordinator(coordinator, directory, "coordinator", "", port);

  constexpr int kAgents = 4;
  std::vector<Child> agents(kAgents);
  for (int index = 0; index < kAgents; ++index) {
    const std::string worker = "worker-" + std::to_string(index);
    const std::string boot = "boot-" + std::to_string(index);
    start_agent(agents[index], directory, "agent-" + std::to_string(index), port, worker, boot,
                "--no-hardware");
    RF_REQUIRE(wait_for_output(agents[index], "registered boot=" + boot));
  }

  const QueryOutcome publishers = ask(port, QueryKind::Publishers);
  RF_REQUIRE(publishers.ok);
  RF_CHECK_EQ(parse_number(publishers.detail, "publishers="), static_cast<std::size_t>(kAgents));
  RF_CHECK_EQ(parse_number(publishers.detail, "fenced="), std::size_t{0});

  for (Child& agent : agents) {
    agent.request_stop();
    RF_REQUIRE(exited_cleanly(agent));
    agent.release();
  }
  coordinator.request_stop();
  RF_REQUIRE(exited_cleanly(coordinator));
  coordinator.release();
  remove_directory(directory);
}
