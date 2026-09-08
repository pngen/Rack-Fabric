// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_fabric/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <deque>
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

[[nodiscard]] ResultMessage to_result(const MutationResult& result) {
  ResultMessage message;
  message.accepted = result.accepted();
  message.outcome = result.outcome;
  message.generations = result.generations_after;
  message.code = result.explanation.code;
  message.detail = result.explanation.render();
  if (message.detail.size() > 4096) {
    message.detail.resize(4096);
  }
  return message;
}

[[nodiscard]] std::string render_query(const RackFabric& fabric, const QueryMessage& query) {
  const auto kind = static_cast<QueryKind>(query.query_kind);
  switch (kind) {
    case QueryKind::Summary: {
      const RackSummary summary = fabric.summary();
      std::string out = "lifecycle=";
      out += to_string(summary.lifecycle);
      out += " members=" + std::to_string(summary.member_count);
      out += " relationships=" + std::to_string(summary.relationship_count);
      out += " failure_domains=" + std::to_string(summary.failure_domain_count);
      out += " publishers=" + std::to_string(summary.publisher_count);
      out += " snapshots=" + std::to_string(summary.snapshot_count);
      out += " digest=" + summary.digest;
      return out;
    }
    case QueryKind::Generations: {
      const GenerationSet generations = fabric.generations();
      std::string out = "rack=" + std::to_string(generations.rack.value());
      out += " membership=" + std::to_string(generations.membership.value());
      out += " topology=" + std::to_string(generations.topology.value());
      out += " failure_domains=" + std::to_string(generations.failure_domains.value());
      out += " constraints=" + std::to_string(generations.constraints.value());
      out += " power=" + std::to_string(generations.power.value());
      out += " cooling=" + std::to_string(generations.cooling.value());
      out += " health=" + std::to_string(generations.health.value());
      out += " capabilities=" + std::to_string(generations.capabilities.value());
      out += " coordinator_epoch=" + std::to_string(generations.coordinator_epoch.value());
      return out;
    }
    case QueryKind::Readiness:
      return fabric.explain_readiness().render();
    case QueryKind::Publishers: {
      std::string out = "publishers=" + std::to_string(fabric.publishers().size());
      out += " fenced=" + std::to_string(fabric.fenced_boots().size());
      return out;
    }
    case QueryKind::Member:
      if (query.member.has_value()) {
        return fabric.explain_member(*query.member).render();
      }
      return "member query requires a member key";
    case QueryKind::Members:
      return "members=" + std::to_string(fabric.members().size());
    case QueryKind::Relationships:
      return "relationships=" + std::to_string(fabric.relationships().size());
    case QueryKind::FailureDomains:
      return "failure_domains=" + std::to_string(fabric.failure_domains().size());
    case QueryKind::PowerEnvelope: {
      const auto envelope = fabric.power_envelope();
      if (!envelope.has_value()) {
        return "power envelope unknown";
      }
      return "power envelope generation=" + std::to_string(envelope->generation.value());
    }
    case QueryKind::CoolingEnvelope: {
      const auto envelope = fabric.cooling_envelope();
      if (!envelope.has_value()) {
        return "cooling envelope unknown";
      }
      return "cooling envelope generation=" + std::to_string(envelope->generation.value()) +
             " zones=" + std::to_string(envelope->zones.size());
    }
  }
  return "unsupported query";
}

}  // namespace

struct CoordinatorServer::Impl {
  explicit Impl(const CoordinatorOptions& options_in)
      : options(options_in), fabric(options_in.fabric) {}

  CoordinatorOptions options;
  RackFabric fabric;
  transport::TcpListener listener;
  std::atomic<bool> stopping{false};
  std::atomic<std::size_t> open_connections{0};
  std::atomic<std::size_t> total_connections{0};
  std::atomic<std::size_t> accepted_mutations{0};
  std::atomic<std::size_t> rejected_mutations{0};
  std::atomic<std::size_t> malformed_frames{0};
  std::thread accept_thread;
  mutable std::mutex mutex;
  std::vector<std::thread> session_threads;
  std::vector<std::shared_ptr<transport::TcpConnection>> session_connections;
  std::deque<CoordinatorEvent> events;

  void record(const std::string& kind, const std::string& detail) {
    std::lock_guard<std::mutex> guard(mutex);
    CoordinatorEvent event;
    event.at = fabric.clock().now();
    event.kind = kind;
    event.detail = detail;
    events.push_back(std::move(event));
    while (events.size() > options.max_retained_events) {
      events.pop_front();
    }
  }

  void persist_if_configured() {
    if (!options.persist_after_mutation || options.persistence_path.empty()) {
      return;
    }
    PersistenceOptions persistence_options;
    const PersistenceResult result = fabric.save_state(options.persistence_path, persistence_options);
    if (result.status != PersistenceStatus::Ok) {
      record("persist_failed", result.path);
    }
  }

  // A reply that cannot be sent means the peer is gone. The failure is
  // recorded as a coordinator event, the session stops serving this request,
  // and the next read on the socket fails, which ends the session. No reply
  // failure is ever silently treated as a delivered response.
  void reply(transport::TcpConnection& connection, MessageType type, std::uint64_t correlation,
             const std::vector<std::byte>& payload) {
    Frame frame;
    frame.type = type;
    frame.correlation = correlation;
    frame.flags = Frame::response_flag();
    frame.payload = payload;
    std::string error;
    if (!connection.send(frame, options.limits, error)) {
      record("send_failed", error);
    }
  }

  void reply_result(transport::TcpConnection& connection, MessageType type,
                    std::uint64_t correlation, const MutationResult& result) {
    const ResultMessage message = to_result(result);
    const auto payload = MessageCodec::encode(message, options.limits);
    if (!payload.has_value()) {
      record("encode_failed", "result message could not be encoded");
      return;
    }
    reply(connection, type, correlation, *payload);
  }

  void reply_error(transport::TcpConnection& connection, std::uint64_t correlation,
                   ProtocolError error, const std::string& detail) {
    ErrorMessage message;
    message.error = error;
    message.detail = detail;
    const auto payload = MessageCodec::encode(message, options.limits);
    if (!payload.has_value()) {
      record("encode_failed", "error message could not be encoded");
      return;
    }
    reply(connection, MessageType::Error, correlation, *payload);
  }

  [[nodiscard]] AuthorityToken token_for(const AgentBootId& boot,
                                         const AuthorityToken& presented) const {
    AuthorityToken token;
    token.coordinator_epoch = fabric.coordinator_epoch();
    token.boot = boot;
    token.publication_generation = presented.publication_generation;
    token.rack = presented.rack.has_value() ? presented.rack : fabric.rack_id();
    return token;
  }

  [[nodiscard]] bool check_session_identity(const AgentBootId& boot, const AuthorityToken& presented,
                                            std::string& detail) const {
    if (!presented.boot.has_value()) {
      detail = "the mutation does not present a boot identity";
      return false;
    }
    if (!(*presented.boot == boot)) {
      detail = "the mutation presents a boot identity that does not belong to this connection";
      return false;
    }
    return true;
  }

  void handle_hello(transport::TcpConnection& connection, const Frame& frame) {
    HelloMessage hello;
    if (MessageCodec::decode(frame.payload.data(), frame.payload.size(), hello, options.limits) !=
        DecodeStatus::Ok) {
      malformed_frames.fetch_add(1, std::memory_order_relaxed);
      reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                  "HELLO payload is malformed");
      return;
    }
    if (hello.protocol_version != kProtocolVersion) {
      reply_error(connection, frame.correlation, ProtocolError::UnsupportedVersion,
                  "the agent speaks an unsupported protocol version");
      return;
    }
    HelloAckMessage ack;
    ack.protocol_version = kProtocolVersion;
    ack.coordinator_epoch = fabric.coordinator_epoch();
    ack.coordinator_version = kVersionString;
    ack.rack = fabric.rack_id();
    ack.accepts_mutations = true;
    const auto payload = MessageCodec::encode(ack, options.limits);
    if (payload.has_value()) {
      reply(connection, MessageType::HelloAck, frame.correlation, *payload);
    }
    record("hello", hello.agent_version);
  }

  void handle_register(transport::TcpConnection& connection, const Frame& frame,
                       std::optional<AgentBootId>& session_boot, std::optional<WorkerId>& session_worker) {
    RegisterMessage message;
    if (MessageCodec::decode(frame.payload.data(), frame.payload.size(), message, options.limits) !=
        DecodeStatus::Ok) {
      malformed_frames.fetch_add(1, std::memory_order_relaxed);
      reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                  "REGISTER payload is malformed");
      return;
    }
    if (!message.boot.has_value() || !message.worker.has_value()) {
      reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                  "REGISTER requires a worker and a boot identity");
      return;
    }
    if (session_boot.has_value() && !(*session_boot == *message.boot)) {
      reply_error(connection, frame.correlation, ProtocolError::NotAuthorized,
                  "this connection is already bound to a different boot identity");
      return;
    }
    RegisterPublisherRequest request;
    request.rack = message.rack.has_value() ? message.rack : fabric.rack_id();
    request.worker = *message.worker;
    request.boot = *message.boot;
    request.coordinator_epoch = fabric.coordinator_epoch();
    request.label = message.label;
    if (!request.rack.has_value()) {
      reply_error(connection, frame.correlation, ProtocolError::NotAuthorized,
                  "no rack has been declared");
      return;
    }
    const MutationResult result = fabric.register_publisher(request);
    RegisterAckMessage ack;
    ack.accepted = result.accepted();
    ack.coordinator_epoch = fabric.coordinator_epoch();
    ack.generations = result.generations_after;
    if (result.accepted()) {
      session_boot = *message.boot;
      session_worker = *message.worker;
      const auto publisher = fabric.find_publisher(*message.boot);
      ack.publication_generation = publisher.has_value() ? publisher->publication_generation
                                                         : PublicationGeneration::first();
    }
    const auto payload = MessageCodec::encode(ack, options.limits);
    if (payload.has_value()) {
      reply(connection, MessageType::RegisterAck, frame.correlation, *payload);
    }
    record(result.accepted() ? "register" : "register_rejected",
           message.boot->value() + " " + std::string(to_string(result.outcome)));
  }

  void handle_mutation(transport::TcpConnection& connection, const Frame& frame,
                       const AgentBootId& boot) {
    const auto* data = frame.payload.data();
    const std::size_t size = frame.payload.size();
    switch (frame.type) {
      case MessageType::DeclareRack: {
        DeclareRackMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "DECLARE_RACK payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        DeclareRackRequest request;
        request.authority = token_for(boot, message.authority);
        request.epoch = message.epoch;
        request.label = message.label;
        request.redeclare = message.redeclare;
        const MutationResult result = fabric.declare_rack(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::PublishNode:
      case MessageType::PublishDevice: {
        PublishMemberMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "PUBLISH_MEMBER payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        PublishMemberRequest request;
        request.authority = token_for(boot, message.authority);
        request.record = std::move(message.record);
        request.expected_generation = message.expected_generation;
        request.supersede = message.supersede;
        const MutationResult result = fabric.publish_member(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::PublishLink: {
        PublishLinkMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "PUBLISH_LINK payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        PublishRelationshipRequest request;
        request.authority = token_for(boot, message.authority);
        request.record = std::move(message.record);
        request.expected_generation = message.expected_generation;
        request.supersede = message.supersede;
        const MutationResult result = fabric.publish_relationship(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::PublishFailureDomain: {
        PublishFailureDomainMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "PUBLISH_FAILURE_DOMAIN payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        PublishFailureDomainRequest request;
        request.authority = token_for(boot, message.authority);
        request.record = std::move(message.record);
        request.expected_generation = message.expected_generation;
        request.supersede = message.supersede;
        const MutationResult result = fabric.publish_failure_domain(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::PublishPower: {
        PublishPowerMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "PUBLISH_POWER payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        PublishPowerEnvelopeRequest request;
        request.authority = token_for(boot, message.authority);
        request.record = std::move(message.record);
        request.expected_generation = message.expected_generation;
        request.supersede = message.supersede;
        const MutationResult result = fabric.publish_power_envelope(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::PublishCooling: {
        PublishCoolingMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "PUBLISH_COOLING payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        PublishCoolingEnvelopeRequest request;
        request.authority = token_for(boot, message.authority);
        request.record = std::move(message.record);
        request.expected_generation = message.expected_generation;
        request.supersede = message.supersede;
        const MutationResult result = fabric.publish_cooling_envelope(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::PublishHealth: {
        PublishHealthMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "PUBLISH_HEALTH payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        PublishHealthRequest request;
        request.authority = token_for(boot, message.authority);
        request.member = std::move(message.member);
        request.health = message.health;
        request.readiness = message.readiness;
        request.publish_readiness = message.publish_readiness;
        request.expected_generation = message.expected_generation;
        const MutationResult result = fabric.publish_health(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::PublishCapability: {
        PublishCapabilityMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "PUBLISH_CAPABILITY payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        PublishCapabilityRequest request;
        request.authority = token_for(boot, message.authority);
        request.member = std::move(message.member);
        request.capability = std::move(message.capability);
        request.expected_generation = message.expected_generation;
        const MutationResult result = fabric.publish_capability(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::Withdraw: {
        WithdrawMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "WITHDRAW payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        WithdrawEvidenceRequest request;
        request.authority = token_for(boot, message.authority);
        request.member = std::move(message.member);
        request.scope = message.scope;
        request.expected_generation = message.expected_generation;
        const MutationResult result = fabric.withdraw_evidence(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::RetireMember: {
        RetireMemberMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "RETIRE_MEMBER payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        RetireMemberRequest request;
        request.authority = token_for(boot, message.authority);
        request.member = std::move(message.member);
        request.expected_generation = message.expected_generation;
        const MutationResult result = fabric.retire_member(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::Revalidate: {
        RevalidateMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "REVALIDATE payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        RevalidateMembersRequest request;
        request.authority = token_for(boot, message.authority);
        request.members = std::move(message.members);
        request.all = message.all;
        const MutationResult result = fabric.revalidate_members(request);
        finish_mutation(connection, frame, result);
        return;
      }
      case MessageType::Heartbeat: {
        HeartbeatMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "HEARTBEAT payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        HeartbeatRequest request;
        request.authority = token_for(boot, message.authority);
        request.authority.publication_generation.reset();
        request.publication_generation = message.publication_generation;
        const MutationResult result = fabric.heartbeat(request);
        // Every request is answered, so that a session is strictly
        // request/response and can never desynchronize its framing.
        reply_result(connection, MessageType::Result, frame.correlation, result);
        return;
      }
      case MessageType::SnapshotRequest: {
        SnapshotRequestMessage message;
        if (MessageCodec::decode(data, size, message, options.limits) != DecodeStatus::Ok) {
          malformed_frames.fetch_add(1, std::memory_order_relaxed);
          reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                      "SNAPSHOT_REQUEST payload is malformed");
          return;
        }
        std::string detail;
        if (!check_session_identity(boot, message.authority, detail)) {
          reply_error(connection, frame.correlation, ProtocolError::NotAuthorized, detail);
          return;
        }
        SnapshotRequest request;
        request.authority = token_for(boot, message.authority);
        const SnapshotResult published = fabric.publish_snapshot(request);
        if (!published.result.accepted() || !published.snapshot.has_value()) {
          reply_result(connection, MessageType::Result, frame.correlation, published.result);
          return;
        }
        const auto payload = MessageCodec::encode_snapshot_payload(*published.snapshot, options.limits);
        if (payload.has_value()) {
          reply(connection, MessageType::SnapshotResponse, frame.correlation, *payload);
        }
        finish_mutation(connection, frame, published.result);
        return;
      }
      default:
        reply_error(connection, frame.correlation, ProtocolError::Unsupported,
                    "the message type is not a mutation");
        return;
    }
  }

  void finish_mutation(transport::TcpConnection& connection, const Frame& frame,
                       const MutationResult& result) {
    if (result.accepted()) {
      accepted_mutations.fetch_add(1, std::memory_order_relaxed);
      persist_if_configured();
    } else {
      rejected_mutations.fetch_add(1, std::memory_order_relaxed);
    }
    reply_result(connection, MessageType::Result, frame.correlation, result);
  }

  void handle_query(transport::TcpConnection& connection, const Frame& frame) {
    QueryMessage message;
    if (MessageCodec::decode(frame.payload.data(), frame.payload.size(), message, options.limits) !=
        DecodeStatus::Ok) {
      malformed_frames.fetch_add(1, std::memory_order_relaxed);
      reply_error(connection, frame.correlation, ProtocolError::MalformedPayload,
                  "QUERY payload is malformed");
      return;
    }
    ResultMessage result;
    result.accepted = true;
    result.outcome = MutationOutcome::Accepted;
    result.generations = fabric.generations();
    result.code = "QUERY_RESULT";
    result.detail = render_query(fabric, message);
    if (result.detail.size() > options.limits.max_string_bytes) {
      result.detail.resize(options.limits.max_string_bytes);
    }
    const auto payload = MessageCodec::encode(result, options.limits);
    if (payload.has_value()) {
      reply(connection, MessageType::Result, frame.correlation, *payload);
    }
  }

  void serve(std::shared_ptr<transport::TcpConnection> connection) {
    std::optional<AgentBootId> session_boot;
    std::optional<WorkerId> session_worker;
    transport::TcpConnection& local = *connection;
    const std::uintptr_t handle = local.handle();

    // A session must introduce itself first.
    Frame frame;
    std::string error;
    if (!local.receive(frame, options.limits, options.connect_timeout, error)) {
      record("session_rejected", error);
      end_session(handle);
      return;
    }
    if (frame.type != MessageType::Hello) {
      reply_error(local, frame.correlation, ProtocolError::MalformedPayload,
                  "the first message on a session must be HELLO");
      end_session(handle);
      return;
    }
    handle_hello(local, frame);

    while (!stopping.load(std::memory_order_acquire)) {
      if (!local.receive(frame, options.limits, options.session_wait, error)) {
        if (stopping.load(std::memory_order_acquire)) {
          break;
        }
        if (error == "receive timed out") {
          continue;
        }
        record("session_ended", error);
        break;
      }
      if (frame.is_response()) {
        reply_error(local, frame.correlation, ProtocolError::MalformedPayload,
                    "a request message must not carry the response flag");
        continue;
      }
      switch (frame.type) {
        case MessageType::Hello:
          handle_hello(local, frame);
          break;
        case MessageType::Register:
          handle_register(local, frame, session_boot, session_worker);
          break;
        case MessageType::Query:
          handle_query(local, frame);
          break;
        case MessageType::DeclareRack:
        case MessageType::PublishNode:
        case MessageType::PublishDevice:
        case MessageType::PublishLink:
        case MessageType::PublishFailureDomain:
        case MessageType::PublishPower:
        case MessageType::PublishCooling:
        case MessageType::PublishHealth:
        case MessageType::PublishCapability:
        case MessageType::Withdraw:
        case MessageType::RetireMember:
        case MessageType::Revalidate:
        case MessageType::Heartbeat:
        case MessageType::SnapshotRequest:
          if (!session_boot.has_value()) {
            reply_error(local, frame.correlation, ProtocolError::NotRegistered,
                        "the session has not registered a boot identity");
            break;
          }
          handle_mutation(local, frame, *session_boot);
          break;
        default:
          reply_error(local, frame.correlation, ProtocolError::Unsupported,
                      "the message type is not accepted by the coordinator");
          break;
      }
    }
    local.close();
    end_session(handle);
  }

  void end_session(std::uintptr_t handle) {
    std::lock_guard<std::mutex> guard(mutex);
    const auto found = std::find_if(
        session_connections.begin(), session_connections.end(),
        [handle](const std::shared_ptr<transport::TcpConnection>& connection) {
          return connection->handle() == handle;
        });
    if (found != session_connections.end()) {
      session_connections.erase(found);
    }
    if (open_connections.load(std::memory_order_relaxed) > 0) {
      open_connections.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  void accept_loop() {
    while (!stopping.load(std::memory_order_acquire)) {
      std::string error;
      std::optional<transport::TcpConnection> connection =
          listener.accept(options.accept_wait, error);
      if (!connection.has_value()) {
        // A publisher whose lease has expired is fenced here. Process death is
        // therefore detected by the runtime instead of being assumed benign:
        // the dead incarnation can never publish again, and the evidence it
        // supplied stops counting as current.
        if (fabric.expire_publishers() > 0) {
          record("publishers_expired", "one or more publisher leases expired");
        }
        continue;
      }
      if (open_connections.load(std::memory_order_relaxed) >= options.max_connections) {
        record("connection_refused", "connection limit reached");
        connection->close();
        continue;
      }
      open_connections.fetch_add(1, std::memory_order_relaxed);
      total_connections.fetch_add(1, std::memory_order_relaxed);
      auto shared = std::make_shared<transport::TcpConnection>(std::move(*connection));
      std::lock_guard<std::mutex> guard(mutex);
      session_connections.push_back(shared);
      session_threads.emplace_back([this, shared]() { serve(shared); });
    }
  }
};

std::optional<std::unique_ptr<CoordinatorServer>> CoordinatorServer::start(
    const CoordinatorOptions& options, std::string& error) {
  if (options.bind_host != "127.0.0.1" && options.bind_host != "localhost" &&
      options.bind_host != "::1") {
    error = "the coordinator binds loopback addresses only";
    return std::nullopt;
  }
  auto server = std::unique_ptr<CoordinatorServer>(new CoordinatorServer(options));
  std::string bind_error;
  std::optional<transport::TcpListener> listener =
      transport::TcpListener::bind_loopback(options.port, 64, bind_error);
  if (!listener.has_value()) {
    error = bind_error;
    return std::nullopt;
  }
  if (options.rack.has_value() || options.rack_epoch.has_value()) {
    if (!options.rack.has_value() || !options.rack_epoch.has_value()) {
      error = "rack and rack_epoch must be provided together";
      return std::nullopt;
    }
    // Recovered state owns the rack identity. When a rack was recovered, the
    // operator arguments must agree with it; the recovered identity is never
    // silently replaced and a matching declaration is not an error.
    if (server->impl_->fabric.rack_id().has_value()) {
      if (!(server->impl_->fabric.rack_id()->value() == options.rack->value())) {
        error = "the recovered state describes rack " +
                server->impl_->fabric.rack_id()->value() + ", not " + options.rack->value();
        return std::nullopt;
      }
      if (server->impl_->fabric.rack_epoch().has_value() &&
          !(server->impl_->fabric.rack_epoch()->value() == options.rack_epoch->value())) {
        error = "the recovered state describes rack epoch " +
                server->impl_->fabric.rack_epoch()->value() + ", not " +
                options.rack_epoch->value();
        return std::nullopt;
      }
    } else {
      DeclareRackRequest declare;
      declare.authority.coordinator_epoch = server->impl_->fabric.coordinator_epoch();
      declare.authority.rack = *options.rack;
      declare.epoch = *options.rack_epoch;
      declare.label = options.rack_label;
      declare.redeclare = false;
      const MutationResult result = server->impl_->fabric.declare_rack(declare);
      if (!result.accepted()) {
        error = "the coordinator could not declare the rack: " + result.explanation.render();
        return std::nullopt;
      }
    }
  }
  server->impl_->listener = std::move(*listener);
  server->impl_->accept_thread = std::thread([impl = server->impl_.get()]() { impl->accept_loop(); });
  server->impl_->record("listening", std::to_string(server->impl_->listener.port()));
  return server;
}

CoordinatorServer::CoordinatorServer(const CoordinatorOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}

CoordinatorServer::~CoordinatorServer() { stop(); }

void CoordinatorServer::stop() {
  if (impl_ == nullptr) {
    return;
  }
  const bool already_stopping = impl_->stopping.exchange(true, std::memory_order_acq_rel);
  impl_->listener.close();
  std::vector<std::thread> threads;
  std::vector<std::shared_ptr<transport::TcpConnection>> connections;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    connections.swap(impl_->session_connections);
    threads.swap(impl_->session_threads);
  }
  // Closing a socket unblocks a session thread that is waiting on it. close()
  // is idempotent and safe to call concurrently with a read or write.
  for (auto& connection : connections) {
    connection->close();
  }
  if (impl_->accept_thread.joinable()) {
    impl_->accept_thread.join();
  }
  for (auto& thread : threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }
  if (!already_stopping) {
    impl_->record("stopped", "coordinator stopped");
  }
}

std::uint16_t CoordinatorServer::port() const noexcept { return impl_->listener.port(); }

RackFabric& CoordinatorServer::fabric() noexcept { return impl_->fabric; }

const RackFabric& CoordinatorServer::fabric() const noexcept { return impl_->fabric; }

CoordinatorEpoch CoordinatorServer::epoch() const noexcept { return impl_->fabric.coordinator_epoch(); }

std::size_t CoordinatorServer::open_connections() const {
  return impl_->open_connections.load(std::memory_order_relaxed);
}

std::size_t CoordinatorServer::total_connections() const {
  return impl_->total_connections.load(std::memory_order_relaxed);
}

std::size_t CoordinatorServer::accepted_mutations() const {
  return impl_->accepted_mutations.load(std::memory_order_relaxed);
}

std::size_t CoordinatorServer::rejected_mutations() const {
  return impl_->rejected_mutations.load(std::memory_order_relaxed);
}

std::size_t CoordinatorServer::malformed_frames() const {
  return impl_->malformed_frames.load(std::memory_order_relaxed);
}

std::vector<CoordinatorEvent> CoordinatorServer::events() const {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  return {impl_->events.begin(), impl_->events.end()};
}

}  // namespace rack_fabric
