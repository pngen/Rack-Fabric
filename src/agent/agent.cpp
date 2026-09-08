// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_fabric/agent.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/log.hpp"
#include "rack_fabric/version.hpp"
#include "transport/tcp.hpp"

namespace rack_fabric {
namespace {

std::atomic<std::uint64_t> g_boot_counter{0};

[[nodiscard]] std::string sanitize(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (const char character : value) {
    const bool alnum = (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9');
    if (alnum || character == '-' || character == '_' || character == '.' || character == ':') {
      out.push_back(character);
    }
  }
  if (out.empty() || out == "." || out == "..") {
    return "local";
  }
  if (out.size() > kMaxIdentityLength) {
    out.resize(kMaxIdentityLength);
  }
  return out;
}

[[nodiscard]] MutationOutcome outcome_from(const ResultMessage& message) { return message.outcome; }

}  // namespace

struct Agent::Impl {
  explicit Impl(const AgentOptions& options_in) : options(options_in) {}

  AgentOptions options;
  transport::TcpConnection connection;
  std::mutex io_mutex;
  std::atomic<bool> stopping{false};
  std::thread heartbeat_thread;
  mutable std::mutex state_mutex;
  AgentReport report;
  PublicationGeneration publication_generation;
  CoordinatorEpoch coordinator_epoch;
  WorkerId worker;
  AgentBootId boot;
  std::uint64_t correlation = 0;

  [[nodiscard]] std::uint64_t next_correlation() { return ++correlation; }

  [[nodiscard]] AuthorityToken token() const {
    AuthorityToken authority;
    authority.coordinator_epoch = coordinator_epoch;
    authority.boot = boot;
    authority.publication_generation = publication_generation;
    authority.rack = options.rack;
    return authority;
  }

  [[nodiscard]] bool exchange(const Frame& request, Frame& response, std::string& error) {
    if (!connection.send(request, options.limits, error)) {
      return false;
    }
    if (!connection.receive(response, options.limits, options.io_timeout, error)) {
      return false;
    }
    if (!response.is_response()) {
      error = "the coordinator sent a request where a response was expected";
      return false;
    }
    if (response.correlation != request.correlation) {
      error = "the coordinator response correlation does not match the request";
      return false;
    }
    return true;
  }

  [[nodiscard]] bool send_and_expect_result(MessageType type,
                                            const std::vector<std::byte>& payload,
                                            MutationResult& result, std::string& error) {
    Frame request;
    request.type = type;
    request.correlation = next_correlation();
    request.payload = payload;
    Frame response;
    if (!exchange(request, response, error)) {
      return false;
    }
    if (response.type == MessageType::Error) {
      ErrorMessage message;
      if (MessageCodec::decode(response.payload.data(), response.payload.size(), message,
                               options.limits) != DecodeStatus::Ok) {
        error = "the coordinator sent a malformed error";
        return false;
      }
      error = std::string("coordinator error: ") + std::string(to_string(message.error)) + " " +
              message.detail;
      return false;
    }
    if (response.type != MessageType::Result) {
      error = "the coordinator sent an unexpected response type";
      return false;
    }
    ResultMessage message;
    if (MessageCodec::decode(response.payload.data(), response.payload.size(), message,
                             options.limits) != DecodeStatus::Ok) {
      error = "the coordinator sent a malformed result";
      return false;
    }
    result.outcome = outcome_from(message);
    result.generations_after = message.generations;
    result.explanation.code = message.code;
    result.explanation.ok = message.accepted;
    result.explanation.subject = message.detail;
    if (message.accepted) {
      const auto next = publication_generation.next();
      if (next.has_value()) {
        publication_generation = *next;
      }
    }
    return true;
  }

  void remember(const MutationResult& result) {
    std::lock_guard<std::mutex> guard(state_mutex);
    report.results.push_back(result);
    while (report.results.size() > options.max_retained_results) {
      report.results.erase(report.results.begin());
    }
  }

  void set_error(const std::string& error) {
    std::lock_guard<std::mutex> guard(state_mutex);
    report.last_error = error;
  }

  [[nodiscard]] bool publish_failure_domains(const std::vector<FailureDomainRecord>& domains,
                                             std::string& error) {
    for (const auto& record : domains) {
      PublishFailureDomainMessage message;
      message.authority = token();
      message.record = record;
      const auto payload = MessageCodec::encode(message, options.limits);
      if (!payload.has_value()) {
        error = "the failure domain message exceeds the configured bound";
        return false;
      }
      MutationResult result;
      if (!send_and_expect_result(MessageType::PublishFailureDomain, *payload, result, error)) {
        return false;
      }
      remember(result);
      if (result.accepted()) {
        std::lock_guard<std::mutex> guard(state_mutex);
        ++report.failure_domains_published;
      } else {
        set_error(result.explanation.code);
      }
    }
    return true;
  }

  [[nodiscard]] bool publish_members(std::vector<MemberRecord> members, std::string& error) {
    // Power and cooling domain members are published first so that a member
    // that references one always finds it.
    std::stable_sort(members.begin(), members.end(), [](const MemberRecord& lhs,
                                                        const MemberRecord& rhs) {
      const auto rank = [](MemberKind kind) {
        return kind == MemberKind::PowerDomain || kind == MemberKind::CoolingDomain ? 0 : 1;
      };
      return rank(lhs.key.kind) < rank(rhs.key.kind);
    });
    for (const auto& record : members) {
      PublishMemberMessage message;
      message.authority = token();
      message.record = record;
      const auto payload = MessageCodec::encode(message, options.limits);
      if (!payload.has_value()) {
        error = "the member message exceeds the configured bound";
        return false;
      }
      MutationResult result;
      if (!send_and_expect_result(MessageType::PublishNode, *payload, result, error)) {
        return false;
      }
      remember(result);
      if (result.accepted()) {
        std::lock_guard<std::mutex> guard(state_mutex);
        ++report.members_published;
      } else {
        set_error(result.explanation.code);
      }
    }
    return true;
  }

  [[nodiscard]] bool do_publish(std::string& error) {
    if (!options.publish_hardware) {
      return true;
    }
    const HardwareDiscoveryResult discovery = discover_local_hardware(options.hardware);
    {
      std::lock_guard<std::mutex> guard(state_mutex);
      report.diagnostics = discovery.diagnostics;
      for (const auto& unsupported : discovery.unsupported) {
        report.diagnostics.push_back("unsupported: " + unsupported);
      }
    }
    if (!discovery.ok) {
      error = discovery.explanation.render();
      return false;
    }
    if (!publish_failure_domains(discovery.failure_domains, error)) {
      return false;
    }
    return publish_members(discovery.members, error);
  }

  void heartbeat_loop() {
    while (!stopping.load(std::memory_order_acquire)) {
      const auto deadline =
          std::chrono::steady_clock::now() + options.heartbeat_interval;
      while (!stopping.load(std::memory_order_acquire) &&
             std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{2});
      }
      if (stopping.load(std::memory_order_acquire)) {
        break;
      }
      HeartbeatMessage message;
      message.authority = token();
      message.authority.publication_generation.reset();
      message.publication_generation = publication_generation;
      const auto payload = MessageCodec::encode(message, options.limits);
      if (!payload.has_value()) {
        continue;
      }
      MutationResult result;
      std::string error;
      const std::lock_guard<std::mutex> guard(io_mutex);
      if (!send_and_expect_result(MessageType::Heartbeat, *payload, result, error)) {
        set_error(error);
        connection.close();
        break;
      }
      if (result.accepted()) {
        std::lock_guard<std::mutex> state_guard(state_mutex);
        ++report.heartbeats_sent;
      } else {
        set_error(result.explanation.code);
        if (result.outcome == MutationOutcome::RejectStaleWorkerBoot ||
            result.outcome == MutationOutcome::RejectStaleCoordinatorEpoch) {
          // Authority has been revoked; stop talking to the coordinator.
          connection.close();
          break;
        }
      }
    }
  }
};

AgentBootId Agent::generate_boot_id() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto millis =
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
  const std::uint64_t counter = g_boot_counter.fetch_add(1, std::memory_order_relaxed);
  const std::string text = "boot-" + std::to_string(millis) + "-" + std::to_string(counter);
  const auto parsed = AgentBootId::parse(text);
  if (parsed.has_value()) {
    return *parsed;
  }
  return *AgentBootId::parse("boot-fallback");
}

std::optional<std::unique_ptr<Agent>> Agent::start(const AgentOptions& options, std::string& error) {
  if (options.coordinator_port == 0) {
    error = "a coordinator port is required";
    return std::nullopt;
  }
  auto agent = std::unique_ptr<Agent>(new Agent(options));
  Impl& impl = *agent->impl_;
  impl.worker = options.worker.value_or(*WorkerId::parse(sanitize(local_host_name())));
  impl.boot = options.boot.value_or(generate_boot_id());

  std::string connect_error;
  std::optional<transport::TcpConnection> connection =
      transport::connect_loopback(options.coordinator_port, options.connect_timeout, connect_error);
  if (!connection.has_value()) {
    error = connect_error;
    return std::nullopt;
  }
  impl.connection = std::move(*connection);

  HelloMessage hello;
  hello.protocol_version = kProtocolVersion;
  hello.coordinator_epoch = CoordinatorEpoch{};
  hello.agent_version = kVersionString;
  const auto hello_payload = MessageCodec::encode(hello, options.limits);
  if (!hello_payload.has_value()) {
    error = "the hello message could not be encoded";
    return std::nullopt;
  }
  Frame hello_frame;
  hello_frame.type = MessageType::Hello;
  hello_frame.correlation = impl.next_correlation();
  hello_frame.payload = *hello_payload;
  Frame hello_response;
  if (!impl.exchange(hello_frame, hello_response, error)) {
    return std::nullopt;
  }
  if (hello_response.type != MessageType::HelloAck) {
    error = "the coordinator did not answer HELLO with HELLO_ACK";
    return std::nullopt;
  }
  HelloAckMessage ack;
  if (MessageCodec::decode(hello_response.payload.data(), hello_response.payload.size(), ack,
                           options.limits) != DecodeStatus::Ok) {
    error = "the coordinator sent a malformed HELLO_ACK";
    return std::nullopt;
  }
  if (ack.protocol_version != kProtocolVersion) {
    error = "the coordinator speaks an unsupported protocol version";
    return std::nullopt;
  }
  impl.coordinator_epoch = ack.coordinator_epoch;
  if (!ack.rack.has_value()) {
    error = "the coordinator has not declared a rack";
    return std::nullopt;
  }

  RegisterMessage register_message;
  register_message.rack = ack.rack;
  register_message.worker = impl.worker;
  register_message.boot = impl.boot;
  register_message.coordinator_epoch = ack.coordinator_epoch;
  register_message.label = options.label.empty() ? std::nullopt
                                                 : std::optional<std::string>(options.label);
  const auto register_payload = MessageCodec::encode(register_message, options.limits);
  if (!register_payload.has_value()) {
    error = "the register message could not be encoded";
    return std::nullopt;
  }
  Frame register_frame;
  register_frame.type = MessageType::Register;
  register_frame.correlation = impl.next_correlation();
  register_frame.payload = *register_payload;
  Frame register_response;
  if (!impl.exchange(register_frame, register_response, error)) {
    return std::nullopt;
  }
  if (register_response.type == MessageType::Error) {
    ErrorMessage message;
    if (MessageCodec::decode(register_response.payload.data(), register_response.payload.size(),
                             message, options.limits) != DecodeStatus::Ok) {
      error = "the coordinator sent a malformed error";
      return std::nullopt;
    }
    error = std::string("registration failed: ") + std::string(to_string(message.error)) + " " +
            message.detail;
    return std::nullopt;
  }
  if (register_response.type != MessageType::RegisterAck) {
    error = "the coordinator did not answer REGISTER with REGISTER_ACK";
    return std::nullopt;
  }
  RegisterAckMessage register_ack;
  if (MessageCodec::decode(register_response.payload.data(), register_response.payload.size(),
                           register_ack, options.limits) != DecodeStatus::Ok) {
    error = "the coordinator sent a malformed REGISTER_ACK";
    return std::nullopt;
  }
  if (!register_ack.accepted) {
    error = "the coordinator refused registration for boot " + impl.boot.value();
    return std::nullopt;
  }
  impl.coordinator_epoch = register_ack.coordinator_epoch;
  impl.publication_generation = register_ack.publication_generation;
  {
    std::lock_guard<std::mutex> guard(impl.state_mutex);
    impl.report.registered = true;
    impl.report.coordinator_epoch = impl.coordinator_epoch;
    impl.report.publication_generation = impl.publication_generation;
  }

  if (!impl.do_publish(error)) {
    return std::nullopt;
  }
  {
    std::lock_guard<std::mutex> guard(impl.state_mutex);
    impl.report.publication_generation = impl.publication_generation;
  }
  if (options.enable_heartbeat) {
    impl.heartbeat_thread = std::thread([&impl]() { impl.heartbeat_loop(); });
  }
  return agent;
}

Agent::Agent(const AgentOptions& options) : impl_(std::make_unique<Impl>(options)) {}

Agent::~Agent() { stop(); }

void Agent::stop() {
  if (impl_ == nullptr) {
    return;
  }
  const bool already_stopping = impl_->stopping.exchange(true, std::memory_order_acq_rel);
  if (already_stopping) {
    return;
  }
  impl_->connection.close();
  if (impl_->heartbeat_thread.joinable()) {
    impl_->heartbeat_thread.join();
  }
}

bool Agent::connected() const noexcept { return impl_->connection.valid(); }

CoordinatorEpoch Agent::coordinator_epoch() const { return impl_->coordinator_epoch; }

AgentBootId Agent::boot_id() const { return impl_->boot; }

WorkerId Agent::worker_id() const { return impl_->worker; }

AgentReport Agent::report() const {
  std::lock_guard<std::mutex> guard(impl_->state_mutex);
  return impl_->report;
}

bool Agent::publish_now(std::string& error) {
  const std::lock_guard<std::mutex> guard(impl_->io_mutex);
  if (!impl_->connection.valid()) {
    error = "the agent is not connected";
    return false;
  }
  if (!impl_->do_publish(error)) {
    return false;
  }
  std::lock_guard<std::mutex> state_guard(impl_->state_mutex);
  impl_->report.publication_generation = impl_->publication_generation;
  return true;
}

void Agent::disconnect() noexcept {
  impl_->stopping.store(true, std::memory_order_release);
  impl_->connection.close();
  if (impl_->heartbeat_thread.joinable()) {
    impl_->heartbeat_thread.join();
  }
}

}  // namespace rack_fabric
