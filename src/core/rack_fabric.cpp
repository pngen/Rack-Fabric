// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The authoritative rack runtime.
//
// Every mutation follows the same sequence: validate authority, validate the
// request, validate references and invariants against the candidate state,
// then commit atomically and advance the generations that the change
// actually affects. On any failure canonical state is left untouched and no
// generation is advanced.

#include "rack_fabric/rack_fabric.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/canonical.hpp"
#include "core/impl.hpp"
#include "core/state.hpp"
#include "internal/crypto.hpp"
#include "internal/log.hpp"

namespace rack_fabric {
namespace {

constexpr std::int64_t kMaxTtlMillis = 86'400'000LL;  // 24 hours

[[nodiscard]] bool ttl_is_valid(std::chrono::milliseconds ttl) noexcept {
  return ttl.count() >= 0 && ttl.count() <= kMaxTtlMillis;
}

[[nodiscard]] std::optional<std::string> validate_evidence(
    EvidenceProvenance provenance, Timestamp observed_at, std::chrono::milliseconds ttl,
    Durability durability) {
  if (!ttl_is_valid(ttl)) {
    return "TTL_OUT_OF_RANGE";
  }
  if (durability == Durability::Ephemeral && ttl.count() == 0) {
    return "EPHEMERAL_EVIDENCE_REQUIRES_TTL";
  }
  if (provenance != EvidenceProvenance::Unknown && !observed_at.known()) {
    return "EVIDENCE_WITHOUT_OBSERVATION_TIME";
  }
  if (provenance == EvidenceProvenance::Unknown && observed_at.known()) {
    return "OBSERVATION_TIME_WITHOUT_PROVENANCE";
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> validate_quantity(const Quantity& quantity,
                                                           QuantityUnit expected_unit) {
  // A quantity with no unit is not a measurement: it cannot be compared with
  // anything, so it is refused before any comparison is attempted.
  if (quantity.unit == QuantityUnit::None) {
    return "QUANTITY_WITHOUT_UNIT";
  }
  if (!std::isfinite(quantity.value)) {
    return "QUANTITY_NOT_FINITE";
  }
  if (quantity.value < 0.0) {
    return "QUANTITY_OUT_OF_RANGE";
  }
  if (quantity.unit != expected_unit) {
    return "QUANTITY_UNIT_MISMATCH";
  }
  if (quantity.uncertainty.has_value()) {
    if (!std::isfinite(*quantity.uncertainty)) {
      return "QUANTITY_UNCERTAINTY_NOT_FINITE";
    }
    if (*quantity.uncertainty < 0.0) {
      return "QUANTITY_UNCERTAINTY_OUT_OF_RANGE";
    }
  }
  return validate_evidence(quantity.provenance, quantity.observed_at, quantity.ttl, quantity.durability);
}

[[nodiscard]] std::optional<std::string> validate_member_record(const MemberRecord& record,
                                                               const ResourceLimits& limits) {
  if (!validate_identity(record.key.id).ok()) {
    return "INVALID_MEMBER_IDENTITY";
  }
  if (!details_match_kind(record.key.kind, record.details)) {
    return "DETAILS_DO_NOT_MATCH_KIND";
  }
  if (record.source.has_value() && !validate_label(*record.source).ok()) {
    return "INVALID_SOURCE_LABEL";
  }
  if (record.failure_domains.size() > limits.max_failure_domains_per_member) {
    return "FAILURE_DOMAIN_LIMIT";
  }
  if (record.capabilities.size() > limits.max_capabilities_per_member) {
    return "CAPABILITY_LIMIT";
  }
  if (const auto error = validate_evidence(record.provenance, record.observed_at, record.ttl,
                                           record.durability);
      error.has_value()) {
    return error;
  }
  if (record.lifecycle == MemberLifecycle::Present &&
      record.provenance == EvidenceProvenance::Unknown) {
    return "PRESENT_WITHOUT_EVIDENCE";
  }
  if (record.health.has_evidence()) {
    if (const auto error = validate_evidence(record.health.provenance, record.health.observed_at,
                                             record.health.ttl, record.health.durability);
        error.has_value()) {
      return error;
    }
  }
  if (record.readiness.has_evidence()) {
    if (const auto error = validate_evidence(record.readiness.provenance, record.readiness.observed_at,
                                             record.readiness.ttl, record.readiness.durability);
        error.has_value()) {
      return error;
    }
  }
  if (record.reachability.has_evidence()) {
    if (const auto error =
            validate_evidence(record.reachability.provenance, record.reachability.observed_at,
                              record.reachability.ttl, record.reachability.durability);
        error.has_value()) {
      return error;
    }
  }
  for (const auto& capability : record.capabilities) {
    if (!validate_identity(capability.id.value()).ok()) {
      return "INVALID_CAPABILITY_IDENTITY";
    }
    if (capability.value.has_value() && !validate_label(*capability.value).ok()) {
      return "INVALID_CAPABILITY_VALUE";
    }
    if (const auto error = validate_evidence(capability.provenance, capability.observed_at,
                                             capability.ttl, capability.durability);
        error.has_value()) {
      return error;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool same_member_content(const MemberRecord& lhs, const MemberRecord& rhs) {
  MemberRecord left = lhs;
  MemberRecord right = rhs;
  left.generation = MemberGeneration{};
  right.generation = MemberGeneration{};
  left.publication_generation = PublicationGeneration{};
  right.publication_generation = PublicationGeneration{};
  return left == right;
}

[[nodiscard]] bool same_relationship_content(const RelationshipRecord& lhs,
                                             const RelationshipRecord& rhs) {
  RelationshipRecord left = lhs;
  RelationshipRecord right = rhs;
  left.generation = TopologyGeneration{};
  right.generation = TopologyGeneration{};
  return left == right;
}

[[nodiscard]] bool same_failure_domain_content(const FailureDomainRecord& lhs,
                                               const FailureDomainRecord& rhs) {
  FailureDomainRecord left = lhs;
  FailureDomainRecord right = rhs;
  left.generation = FailureDomainGeneration{};
  right.generation = FailureDomainGeneration{};
  return left == right;
}

[[nodiscard]] ExplanationFactor factor(std::string code, std::string detail) {
  ExplanationFactor out;
  out.code = std::move(code);
  out.detail = std::move(detail);
  return out;
}

[[nodiscard]] ExplanationFactor generation_factor(std::string code, std::string detail,
                                                  std::string name, std::uint64_t expected,
                                                  std::uint64_t current) {
  ExplanationFactor out;
  out.code = std::move(code);
  out.detail = std::move(detail);
  out.generation_name = std::move(name);
  out.expected_generation = expected;
  out.current_generation = current;
  return out;
}

[[nodiscard]] MutationResult make_result(const RackState& state, MutationOutcome outcome,
                                         std::string code, std::string subject,
                                         std::vector<ExplanationFactor> factors) {
  MutationResult result;
  result.outcome = outcome;
  result.generations_after = state.generations;
  result.explanation.ok = is_accepted(outcome);
  result.explanation.code = std::move(code);
  result.explanation.subject = std::move(subject);
  result.explanation.factors = std::move(factors);
  return result;
}

[[nodiscard]] MutationResult rejected(const RackState& state, MutationOutcome outcome,
                                      std::string code, std::string subject,
                                      std::vector<ExplanationFactor> factors) {
  return make_result(state, outcome, std::move(code), std::move(subject), std::move(factors));
}

[[nodiscard]] MutationResult accepted(const RackState& state, std::string code, std::string subject) {
  return make_result(state, MutationOutcome::Accepted, std::move(code), std::move(subject), {});
}

[[nodiscard]] MutationResult unchanged(const RackState& state, std::string code, std::string subject) {
  return make_result(state, MutationOutcome::NoChange, std::move(code), std::move(subject), {});
}

struct AuthorityCheck {
  MutationOutcome outcome = MutationOutcome::Accepted;
  std::string code;
  std::string subject;
  std::vector<ExplanationFactor> factors;
  PublisherRecord* publisher = nullptr;

  [[nodiscard]] bool ok() const noexcept { return outcome == MutationOutcome::Accepted; }
};

[[nodiscard]] AuthorityCheck check_authority(RackState& state, const AuthorityToken& token,
                                             Timestamp now, bool rack_scoped) {
  AuthorityCheck check;
  if (token.coordinator_epoch != state.generations.coordinator_epoch) {
    check.outcome = MutationOutcome::RejectStaleCoordinatorEpoch;
    check.code = "STALE_COORDINATOR_EPOCH";
    check.subject = "coordinator_epoch";
    check.factors.push_back(generation_factor("STALE_COORDINATOR_EPOCH",
                                              "the mutation carries an epoch that is not current",
                                              "coordinator_epoch", token.coordinator_epoch.value(),
                                              state.generations.coordinator_epoch.value()));
    return check;
  }
  if (rack_scoped) {
    if (!state.rack.has_value()) {
      check.outcome = MutationOutcome::RejectUnknownRack;
      check.code = "RACK_NOT_DECLARED";
      check.subject = token.rack.has_value() ? token.rack->value() : std::string("<none>");
      check.factors.push_back(factor("RACK_NOT_DECLARED", "no rack identity has been declared"));
      return check;
    }
    if (!token.rack.has_value()) {
      check.outcome = MutationOutcome::RejectInvalidInput;
      check.code = "RACK_IDENTITY_REQUIRED";
      check.subject = state.rack->id.value();
      check.factors.push_back(factor("RACK_IDENTITY_REQUIRED",
                                     "the authority token does not name a rack"));
      return check;
    }
    if (!(*token.rack == state.rack->id)) {
      check.outcome = MutationOutcome::RejectUnknownRack;
      check.code = "RACK_IDENTITY_MISMATCH";
      check.subject = token.rack->value();
      check.factors.push_back(factor("RACK_IDENTITY_MISMATCH",
                                     "the mutation names a different rack than the declared one"));
      return check;
    }
  }
  if (state.rack.has_value() && state.rack->retired) {
    check.outcome = MutationOutcome::RejectRackRetired;
    check.code = "RACK_RETIRED";
    check.subject = state.rack->id.value();
    check.factors.push_back(factor("RACK_RETIRED", "the rack has been retired"));
    return check;
  }
  if (!token.boot.has_value()) {
    // In-process operator authority. The coordinator epoch check above is the
    // only process identity involved.
    return check;
  }
  const auto fenced = state.fenced_boots.find(*token.boot);
  if (fenced != state.fenced_boots.end()) {
    check.outcome = MutationOutcome::RejectStaleWorkerBoot;
    check.code = "BOOT_FENCED";
    check.subject = token.boot->value();
    check.factors.push_back(factor("BOOT_FENCED",
                                   "the boot identity has been fenced and can never reacquire "
                                   "authority"));
    return check;
  }
  const auto publisher = state.publishers.find(*token.boot);
  if (publisher == state.publishers.end()) {
    check.outcome = MutationOutcome::RejectNotRegistered;
    check.code = "PUBLISHER_NOT_REGISTERED";
    check.subject = token.boot->value();
    check.factors.push_back(factor("PUBLISHER_NOT_REGISTERED",
                                   "the boot identity has not been registered with this coordinator"));
    return check;
  }
  if (publisher->second.state == PublisherState::Lost ||
      publisher->second.state == PublisherState::Fenced) {
    check.outcome = MutationOutcome::RejectStaleWorkerBoot;
    check.code = "PUBLISHER_NOT_CURRENT";
    check.subject = token.boot->value();
    check.factors.push_back(factor("PUBLISHER_NOT_CURRENT",
                                   "the publisher is no longer current; its authority was revoked"));
    return check;
  }
  if (publisher->second.coordinator_epoch != state.generations.coordinator_epoch) {
    check.outcome = MutationOutcome::RejectStaleCoordinatorEpoch;
    check.code = "PUBLISHER_STALE_EPOCH";
    check.subject = token.boot->value();
    check.factors.push_back(generation_factor("PUBLISHER_STALE_EPOCH",
                                              "the publisher registered under a previous epoch",
                                              "coordinator_epoch",
                                              publisher->second.coordinator_epoch.value(),
                                              state.generations.coordinator_epoch.value()));
    return check;
  }
  if (token.publication_generation.has_value()) {
    const std::uint64_t expected = token.publication_generation->value();
    const std::uint64_t current = publisher->second.publication_generation.value();
    if (expected < current) {
      check.outcome = MutationOutcome::RejectStaleGeneration;
      check.code = "STALE_PUBLICATION_GENERATION";
      check.subject = token.boot->value();
      check.factors.push_back(generation_factor("STALE_PUBLICATION_GENERATION",
                                                "the publication generation is older than the "
                                                "publisher's current generation",
                                                "publication_generation", expected, current));
      return check;
    }
    if (expected > current) {
      check.outcome = MutationOutcome::RejectConflict;
      check.code = "PUBLICATION_GENERATION_AHEAD";
      check.subject = token.boot->value();
      check.factors.push_back(generation_factor("PUBLICATION_GENERATION_AHEAD",
                                                "the publication generation is ahead of the "
                                                "publisher's current generation",
                                                "publication_generation", expected, current));
      return check;
    }
  }
  (void)now;
  check.publisher = &publisher->second;
  return check;
}

void advance_publication(RackState& state, PublisherRecord* publisher) {
  if (publisher != nullptr) {
    const auto next = publisher->publication_generation.next();
    if (next.has_value()) {
      publisher->publication_generation = *next;
    }
    publisher->state = PublisherState::Active;
    ++publisher->accepted_mutations;
  } else {
    const auto next = state.operator_publication_generation.next();
    if (next.has_value()) {
      state.operator_publication_generation = *next;
    }
  }
}

void note_rejection(PublisherRecord* publisher) {
  if (publisher != nullptr) {
    ++publisher->rejected_mutations;
  }
}

void apply_owner(RackState& state, MemberRecord& record, const AuthorityToken& token) {
  if (token.boot.has_value()) {
    record.owner_boot = token.boot;
    if (state.publishers.find(*token.boot) != state.publishers.end()) {
      record.owner_worker = state.publishers.at(*token.boot).worker;
    }
  }
}

[[nodiscard]] bool owner_conflict(const std::optional<AgentBootId>& existing,
                                  const AuthorityToken& token) {
  if (!token.boot.has_value() || !existing.has_value()) {
    return false;
  }
  return !(*existing == *token.boot);
}

void fence_member_evidence(MemberRecord& record, RevalidationReason reason) {
  // The record was published by a process that is now fenced, so the record
  // must be revalidated before it can be treated as authoritative again,
  // whether its own durability is durable or ephemeral.
  record.revalidation_required = true;
  record.revalidation_reason = reason;
  // Live observations are kept for diagnosis but can never be current again:
  // the authority that produced them is gone.
  if (record.health.has_evidence() && record.health.durability == Durability::Ephemeral) {
    record.health.revalidation_required = true;
  }
  if (record.readiness.has_evidence() && record.readiness.durability == Durability::Ephemeral) {
    record.readiness.revalidation_required = true;
  }
  if (record.reachability.has_evidence() && record.reachability.durability == Durability::Ephemeral) {
    record.reachability.revalidation_required = true;
  }
  for (auto& capability : record.capabilities) {
    if (capability.durability == Durability::Ephemeral) {
      capability.revalidation_required = true;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------

RackFabric::Impl::Impl(RackFabricOptions options_in) : options(std::move(options_in)) {
  clock = options.clock != nullptr ? options.clock.get() : &default_clock();
  state.limits = options.limits;
  state.contract = options.readiness;
  state.generations.coordinator_epoch = CoordinatorEpoch::first();
}

std::string RackFabric::Impl::summary_digest(const RackSummary& summary) const {
  {
    internal::ByteWriter writer(state.limits);
    writer.string(summary.rack.has_value() ? summary.rack->value() : std::string());
    writer.string(summary.rack_epoch.has_value() ? summary.rack_epoch->value() : std::string());
    writer.u8(static_cast<std::uint8_t>(summary.lifecycle));
    internal::encode_generation_set(writer, summary.generations);
    const std::uint64_t counts[] = {
        summary.member_count,          summary.node_count,
        summary.accelerator_count,     summary.cpu_package_count,
        summary.memory_domain_count,   summary.nic_count,
        summary.dpu_count,             summary.switch_count,
        summary.storage_endpoint_count, summary.power_domain_count,
        summary.cooling_domain_count,  summary.present_member_count,
        summary.unavailable_member_count, summary.retired_member_count,
        summary.revalidation_required_member_count, summary.stale_member_count,
        summary.relationship_count,    summary.failure_domain_count,
        summary.publisher_count,       summary.active_publisher_count,
        summary.fenced_boot_count,     summary.snapshot_count};
    for (const std::uint64_t count : counts) {
      writer.u64(count);
    }
    writer.boolean(summary.power_envelope_known);
    writer.boolean(summary.cooling_envelope_known);
    const auto& bytes = writer.bytes();
    return internal::Sha256::hash(bytes.data(), bytes.size());
  }
}

RackFabric::RackFabric(RackFabricOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

RackFabric::~RackFabric() = default;

const RackFabricOptions& RackFabric::options() const { return impl_->options; }

const Clock& RackFabric::clock() const noexcept { return *impl_->clock; }

CoordinatorEpoch RackFabric::coordinator_epoch() const noexcept {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.generations.coordinator_epoch;
}

// ---------------------------------------------------------------------------
// Mutations
// ---------------------------------------------------------------------------

MutationResult RackFabric::register_publisher(const RegisterPublisherRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  if (!request.rack.has_value() || !request.worker.has_value() || !request.boot.has_value()) {
    return rejected(state, MutationOutcome::RejectInvalidInput, "REGISTRATION_INCOMPLETE", "<none>",
                    {factor("REGISTRATION_INCOMPLETE",
                            "a registration requires a rack, a worker and a boot identity")});
  }
  if (request.coordinator_epoch != state.generations.coordinator_epoch) {
    return rejected(state, MutationOutcome::RejectStaleCoordinatorEpoch, "STALE_COORDINATOR_EPOCH",
                    request.boot->value(),
                    {generation_factor("STALE_COORDINATOR_EPOCH",
                                       "registration presents a coordinator epoch that is not current",
                                       "coordinator_epoch", request.coordinator_epoch.value(),
                                       state.generations.coordinator_epoch.value())});
  }
  if (!state.rack.has_value()) {
    return rejected(state, MutationOutcome::RejectUnknownRack, "RACK_NOT_DECLARED",
                    request.rack->value(),
                    {factor("RACK_NOT_DECLARED", "no rack identity has been declared")});
  }
  if (!(*request.rack == state.rack->id)) {
    return rejected(state, MutationOutcome::RejectUnknownRack, "RACK_IDENTITY_MISMATCH",
                    request.rack->value(),
                    {factor("RACK_IDENTITY_MISMATCH", "registration names a different rack")});
  }
  if (state.rack->retired) {
    return rejected(state, MutationOutcome::RejectRackRetired, "RACK_RETIRED", request.rack->value(),
                    {factor("RACK_RETIRED", "the rack has been retired")});
  }
  if (state.fenced_boots.find(*request.boot) != state.fenced_boots.end()) {
    return rejected(state, MutationOutcome::RejectStaleWorkerBoot, "BOOT_FENCED",
                    request.boot->value(),
                    {factor("BOOT_FENCED",
                            "the boot identity has been fenced and can never reacquire authority")});
  }
  if (state.publishers.size() >= impl_->options.limits.max_connections &&
      state.publishers.find(*request.boot) == state.publishers.end()) {
    return rejected(state, MutationOutcome::RejectLimitExceeded, "PUBLISHER_LIMIT_EXCEEDED",
                    request.boot->value(),
                    {factor("PUBLISHER_LIMIT_EXCEEDED", "the publisher limit has been reached")});
  }
  const auto existing = state.publishers.find(*request.boot);
  if (existing != state.publishers.end()) {
    if (existing->second.state == PublisherState::Lost ||
        existing->second.state == PublisherState::Fenced) {
      return rejected(state, MutationOutcome::RejectStaleWorkerBoot, "BOOT_NOT_CURRENT",
                      request.boot->value(),
                      {factor("BOOT_NOT_CURRENT",
                              "a boot identity that has been lost or fenced cannot reacquire "
                              "authority")});
    }
    if (!(existing->second.worker == *request.worker)) {
      return rejected(state, MutationOutcome::RejectNotAuthorized, "WORKER_IDENTITY_CHANGED",
                      request.boot->value(),
                      {factor("WORKER_IDENTITY_CHANGED",
                              "the boot identity is already registered for a different worker")});
    }
    existing->second.last_seen_at = now;
    existing->second.state = PublisherState::Active;
    return unchanged(state, "PUBLISHER_ALREADY_REGISTERED", request.boot->value());
  }
  if (request.label.has_value() && !validate_label(*request.label).ok()) {
    return rejected(state, MutationOutcome::RejectInvalidInput, "INVALID_LABEL",
                    request.boot->value(),
                    {factor("INVALID_LABEL", "the publisher label is not a valid bounded label")});
  }

  PublisherRecord publisher{*request.worker, *request.boot};
  publisher.coordinator_epoch = state.generations.coordinator_epoch;
  publisher.publication_generation = PublicationGeneration::first();
  publisher.state = PublisherState::Active;
  publisher.registered_at = now;
  publisher.last_seen_at = now;
  publisher.lease_millis = impl_->options.limits.publisher_lease_millis;
  publisher.label = request.label;
  state.publishers.emplace(*request.boot, std::move(publisher));
  return accepted(state, "PUBLISHER_REGISTERED", request.boot->value());
}

MutationResult RackFabric::declare_rack(const DeclareRackRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  if (!request.epoch.has_value()) {
    return rejected(state, MutationOutcome::RejectInvalidInput, "RACK_EPOCH_REQUIRED", "<none>",
                    {factor("RACK_EPOCH_REQUIRED",
                            "declaring a rack requires an explicit rack incarnation identity")});
  }
  const RackEpochId epoch = *request.epoch;
  if (!validate_identity(epoch.value()).ok()) {
    return rejected(state, MutationOutcome::RejectInvalidInput, "INVALID_RACK_EPOCH", epoch.value(),
                    {factor("INVALID_RACK_EPOCH", "the rack epoch is not a valid identity")});
  }
  if (request.label.has_value() && !validate_label(*request.label).ok()) {
    return rejected(state, MutationOutcome::RejectInvalidInput, "INVALID_LABEL", epoch.value(),
                    {factor("INVALID_LABEL", "the rack label is not a valid bounded label")});
  }
  if (!request.authority.rack.has_value()) {
    return rejected(state, MutationOutcome::RejectInvalidInput, "RACK_IDENTITY_REQUIRED",
                    epoch.value(),
                    {factor("RACK_IDENTITY_REQUIRED", "the authority token does not name a rack")});
  }
  const AuthorityCheck check = check_authority(state, request.authority, now, false);
  if (!check.ok()) {
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }

  if (state.rack.has_value()) {
    if (!(state.rack->id == *request.authority.rack)) {
      return rejected(state, MutationOutcome::RejectConflict, "RACK_IDENTITY_CONFLICT",
                      request.authority.rack->value(),
                      {factor("RACK_IDENTITY_CONFLICT",
                              "this instance already describes a different rack")});
    }
    if (state.rack->retired) {
      return rejected(state, MutationOutcome::RejectRackRetired, "RACK_RETIRED", state.rack->id.value(),
                      {factor("RACK_RETIRED", "a retired rack cannot be redeclared")});
    }
    if (state.rack->epoch == epoch) {
      if (request.label.has_value() && state.rack->label != request.label) {
        return rejected(state, MutationOutcome::RejectConflict, "RACK_LABEL_CONFLICT",
                        state.rack->id.value(),
                        {factor("RACK_LABEL_CONFLICT",
                                "the rack already exists with a different label")});
      }
      return unchanged(state, "RACK_ALREADY_DECLARED", state.rack->id.value());
    }
    if (!request.redeclare) {
      return rejected(state, MutationOutcome::RejectConflict, "RACK_EPOCH_CONFLICT",
                      state.rack->id.value(),
                      {factor("RACK_EPOCH_CONFLICT",
                              "the rack is declared under a different epoch; redeclare must be "
                              "explicit")});
    }
    // A new rack incarnation: every previous observation is bound to the old
    // incarnation and must be republished.
    const auto next_generation = state.rack->generation.next();
    if (!next_generation.has_value()) {
      return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                      state.rack->id.value(), {factor("GENERATION_EXHAUSTED", "rack generation exhausted")});
    }
    state.rack->epoch = epoch;
    state.rack->generation = *next_generation;
    state.rack->label = request.label;
    state.rack->declared_at = now;
    for (auto& [key, record] : state.members) {
      (void)key;
      record.revalidation_required = true;
      record.revalidation_reason = RevalidationReason::Superseded;
    }
    for (auto& [key, record] : state.relationships) {
      (void)key;
      record.revalidation_required = true;
    }
    for (auto& [key, record] : state.failure_domains) {
      (void)key;
      record.revalidation_required = true;
    }
    state.generations.rack = *next_generation;
    return accepted(state, "RACK_REDECLARED", state.rack->id.value());
  }

  RackRecord record{*request.authority.rack, epoch, RackGeneration::first(), false,
                    request.label, now};
  state.rack = std::move(record);
  state.generations.rack = RackGeneration::first();
  return accepted(state, "RACK_DECLARED", state.rack->id.value());
}

MutationResult RackFabric::publish_member(const PublishMemberRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  MemberRecord record = request.record;
  if (const auto error = validate_member_record(record, impl_->options.limits); error.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, *error, record.key.to_string(),
                    {factor(*error, "the member record is not structurally valid")});
  }
  if (record.owner_boot.has_value() && request.authority.boot.has_value() &&
      !(*record.owner_boot == *request.authority.boot)) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectNotAuthorized, "OWNERSHIP_CHANGE_REFUSED",
                    record.key.to_string(),
                    {factor("OWNERSHIP_CHANGE_REFUSED",
                            "a publisher cannot assign ownership of a member to another process")});
  }

  const auto existing_it = state.members.find(record.key);
  const bool is_new = existing_it == state.members.end();
  if (is_new && state.members.size() >= impl_->options.limits.max_members) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "MEMBER_LIMIT_EXCEEDED",
                    record.key.to_string(),
                    {factor("MEMBER_LIMIT_EXCEEDED", "the member limit has been reached")});
  }
  if (!is_new) {
    const MemberRecord& existing = existing_it->second;
    if (existing.lifecycle == MemberLifecycle::Retired &&
        record.lifecycle != MemberLifecycle::Retired) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectRetired, "MEMBER_RETIRED",
                      record.key.to_string(),
                      {factor("MEMBER_RETIRED", "a retired identity cannot be resurrected")});
    }
    if (owner_conflict(existing.owner_boot, request.authority)) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectNotAuthorized, "MEMBER_OWNED_BY_OTHER_PROCESS",
                      record.key.to_string(),
                      {factor("MEMBER_OWNED_BY_OTHER_PROCESS",
                              "the member is owned by a different process incarnation")});
    }
    if (request.expected_generation.has_value()) {
      if (*request.expected_generation < existing.generation) {
        note_rejection(check.publisher);
        return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_MEMBER_GENERATION",
                        record.key.to_string(),
                        {generation_factor("STALE_MEMBER_GENERATION",
                                           "the expected member generation is older than the current one",
                                           "member_generation", request.expected_generation->value(),
                                           existing.generation.value())});
      }
      if (*request.expected_generation > existing.generation) {
        note_rejection(check.publisher);
        return rejected(state, MutationOutcome::RejectConflict, "MEMBER_GENERATION_AHEAD",
                        record.key.to_string(),
                        {generation_factor("MEMBER_GENERATION_AHEAD",
                                           "the expected member generation is ahead of the current one",
                                           "member_generation", request.expected_generation->value(),
                                           existing.generation.value())});
      }
    } else if (record.generation.is_set()) {
      if (record.generation < existing.generation) {
        note_rejection(check.publisher);
        return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_MEMBER_GENERATION",
                        record.key.to_string(),
                        {generation_factor("STALE_MEMBER_GENERATION",
                                           "the published member generation is older than the current one",
                                           "member_generation", record.generation.value(),
                                           existing.generation.value())});
      }
      if (record.generation == existing.generation) {
        if (same_member_content(existing, record)) {
          return unchanged(state, "MEMBER_UNCHANGED", record.key.to_string());
        }
        note_rejection(check.publisher);
        return rejected(state, MutationOutcome::RejectConflict, "CONFLICTING_MEMBER_PUBLICATION",
                        record.key.to_string(),
                        {generation_factor("CONFLICTING_MEMBER_PUBLICATION",
                                           "a publication for the current generation carries "
                                           "different content",
                                           "member_generation", record.generation.value(),
                                           existing.generation.value())});
      }
      const auto maximum_allowed = existing.generation.next();
      if (!maximum_allowed.has_value() || record.generation > *maximum_allowed) {
        if (!request.supersede) {
          note_rejection(check.publisher);
          return rejected(state, MutationOutcome::RejectConflict, "MEMBER_GENERATION_JUMP",
                          record.key.to_string(),
                          {generation_factor("MEMBER_GENERATION_JUMP",
                                             "the published generation jumps ahead of the current one "
                                             "and explicit supersession was not requested",
                                             "member_generation", record.generation.value(),
                                             existing.generation.value())});
        }
      }
    } else if (same_member_content(existing, record)) {
      return unchanged(state, "MEMBER_UNCHANGED", record.key.to_string());
    }
  }

  if (record.lifecycle == MemberLifecycle::Retired) {
    // A publication that declares a retired identity is a retirement: it
    // cannot also carry a live observation about the present.
    record.observed_at = Timestamp::unknown();
    record.ttl = std::chrono::milliseconds{0};
    record.health = EvidenceValue<HealthState>{};
    record.readiness = EvidenceValue<ReadinessState>{};
    record.reachability = EvidenceValue<ReachabilityState>{};
  }

  // Reference validation against the candidate state.
  if (record.parent.has_value()) {
    if (*record.parent == record.key) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectInvalidRelationship, "MEMBER_SELF_PARENT",
                      record.key.to_string(),
                      {factor("MEMBER_SELF_PARENT", "a member cannot contain itself")});
    }
    if (state.members.find(*record.parent) == state.members.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectUnknownParent, "UNKNOWN_PARENT_MEMBER",
                      record.key.to_string(),
                      {factor("UNKNOWN_PARENT_MEMBER",
                              "the parent member does not exist in the current rack")});
    }
    if (state.contains_cycle(*record.parent, record.key)) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectInvalidRelationship, "CONTAINMENT_CYCLE",
                      record.key.to_string(),
                      {factor("CONTAINMENT_CYCLE", "the containment edge would create a cycle")});
    }
  }
  for (const auto& domain : record.failure_domains) {
    if (state.failure_domains.find(domain) == state.failure_domains.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectUnknownParent, "UNKNOWN_FAILURE_DOMAIN",
                      record.key.to_string(),
                      {factor("UNKNOWN_FAILURE_DOMAIN",
                              "the referenced failure domain " + domain.value() + " does not exist")});
    }
  }
  if (record.power_domain.has_value()) {
    const auto key = MemberKey::of(MemberKind::PowerDomain, *record.power_domain);
    if (state.members.find(key) == state.members.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectUnknownParent, "UNKNOWN_POWER_DOMAIN",
                      record.key.to_string(),
                      {factor("UNKNOWN_POWER_DOMAIN", "the referenced power domain member does not exist")});
    }
  }
  if (record.cooling_domain.has_value()) {
    const auto key = MemberKey::of(MemberKind::CoolingDomain, *record.cooling_domain);
    if (state.members.find(key) == state.members.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectUnknownParent, "UNKNOWN_COOLING_DOMAIN",
                      record.key.to_string(),
                      {factor("UNKNOWN_COOLING_DOMAIN",
                              "the referenced cooling domain member does not exist")});
    }
  }
  if (record.switch_domain.has_value()) {
    const auto key = MemberKey::of(MemberKind::Switch, *record.switch_domain);
    if (state.members.find(key) == state.members.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectUnknownParent, "UNKNOWN_SWITCH_DOMAIN",
                      record.key.to_string(),
                      {factor("UNKNOWN_SWITCH_DOMAIN",
                              "the referenced switch member does not exist")});
    }
  }

  // Commit.
  const MemberGeneration previous = is_new ? MemberGeneration{} : existing_it->second.generation;
  MemberGeneration target;
  if (record.generation.is_set() && record.generation > previous) {
    target = record.generation;
  } else {
    const auto next = previous.next();
    if (!next.has_value()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                      record.key.to_string(),
                      {factor("GENERATION_EXHAUSTED", "member generation exhausted")});
    }
    target = *next;
  }
  record.generation = target;
  apply_owner(state, record, request.authority);
  if (check.publisher != nullptr) {
    record.publication_generation = check.publisher->publication_generation;
  }
  if (!is_new) {
    state.index_remove_member(existing_it->second);
  }
  state.index_add_member(record);
  state.members.insert_or_assign(record.key, std::move(record));

  const auto membership_next = state.generations.membership.is_unset()
                                   ? std::optional<MembershipGeneration>(MembershipGeneration::first())
                                   : state.generations.membership.next();
  if (membership_next.has_value()) {
    state.generations.membership = *membership_next;
  }
  if (!state.generations.health.is_set()) {
    state.generations.health = HealthGeneration::first();
  }
  if (!state.generations.capabilities.is_set()) {
    state.generations.capabilities = CapabilityGeneration::first();
  }
  advance_publication(state, check.publisher);
  return accepted(state, is_new ? "MEMBER_ADDED" : "MEMBER_UPDATED", request.record.key.to_string());
}

MutationResult RackFabric::publish_relationship(const PublishRelationshipRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  RelationshipRecord record = request.record;
  record.key = RelationshipKey::canonicalize(record.key.cls, record.key.from, record.key.to);
  if (record.key.from == record.key.to) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidRelationship, "RELATIONSHIP_SELF_LINK",
                    record.key.to_string(),
                    {factor("RELATIONSHIP_SELF_LINK", "a relationship cannot link a member to itself")});
  }
  if (const auto error = validate_evidence(record.provenance, record.observed_at, record.ttl,
                                           record.durability);
      error.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, *error, record.key.to_string(),
                    {factor(*error, "the relationship evidence is not structurally valid")});
  }
  if (record.capacity.has_value()) {
    if (const auto error = validate_quantity(*record.capacity, record.capacity->unit); error.has_value()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectInvalidInput, *error, record.key.to_string(),
                      {factor(*error, "the relationship capacity is not structurally valid")});
    }
  }
  if (state.members.find(record.key.from) == state.members.end() ||
      state.members.find(record.key.to) == state.members.end()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectUnknownParent, "UNKNOWN_RELATIONSHIP_ENDPOINT",
                    record.key.to_string(),
                    {factor("UNKNOWN_RELATIONSHIP_ENDPOINT",
                            "a relationship endpoint does not exist in the current rack")});
  }
  if (record.key.cls == RelationshipClass::Contains &&
      (state.contains_cycle(record.key.from, record.key.to) ||
       state.relationship_contains_cycle(record.key.from, record.key.to))) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidRelationship, "CONTAINMENT_CYCLE",
                    record.key.to_string(),
                    {factor("CONTAINMENT_CYCLE", "the containment edge would create a cycle")});
  }
  if (state.relationships.size() >= impl_->options.limits.max_relationships) {
    const auto existing = state.relationships.find(record.key);
    if (existing == state.relationships.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectLimitExceeded, "RELATIONSHIP_LIMIT_EXCEEDED",
                      record.key.to_string(),
                      {factor("RELATIONSHIP_LIMIT_EXCEEDED", "the relationship limit has been reached")});
    }
  }

  const auto existing_it = state.relationships.find(record.key);
  if (existing_it != state.relationships.end()) {
    const RelationshipRecord& existing = existing_it->second;
    if (owner_conflict(existing.owner_boot, request.authority)) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectNotAuthorized, "RELATIONSHIP_OWNED_BY_OTHER_PROCESS",
                      record.key.to_string(),
                      {factor("RELATIONSHIP_OWNED_BY_OTHER_PROCESS",
                              "the relationship is owned by a different process incarnation")});
    }
    if (request.expected_generation.has_value() &&
        *request.expected_generation < state.generations.topology) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_TOPOLOGY_GENERATION",
                      record.key.to_string(),
                      {generation_factor("STALE_TOPOLOGY_GENERATION",
                                         "the expected topology generation is older than the current one",
                                         "topology_generation", request.expected_generation->value(),
                                         state.generations.topology.value())});
    }
    if (record.generation.is_set() && record.generation < existing.generation) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_RELATIONSHIP_GENERATION",
                      record.key.to_string(),
                      {generation_factor("STALE_RELATIONSHIP_GENERATION",
                                         "the published relationship generation is older than the "
                                         "current one",
                                         "topology_generation", record.generation.value(),
                                         existing.generation.value())});
    }
    if (same_relationship_content(existing, record)) {
      return unchanged(state, "RELATIONSHIP_UNCHANGED", record.key.to_string());
    }
  }

  const auto next = state.generations.topology.is_unset()
                        ? std::optional<TopologyGeneration>(TopologyGeneration::first())
                        : state.generations.topology.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    record.key.to_string(),
                    {factor("GENERATION_EXHAUSTED", "topology generation exhausted")});
  }
  record.generation = *next;
  if (request.authority.boot.has_value()) {
    record.owner_boot = request.authority.boot;
    if (check.publisher != nullptr) {
      record.owner_worker = check.publisher->worker;
    }
  }
  if (existing_it != state.relationships.end()) {
    state.index_remove_relationship(existing_it->second);
  }
  state.index_add_relationship(record);
  state.relationships.insert_or_assign(record.key, std::move(record));
  state.generations.topology = *next;
  advance_publication(state, check.publisher);
  return accepted(state, "RELATIONSHIP_PUBLISHED", request.record.key.to_string());
}

MutationResult RackFabric::publish_failure_domain(const PublishFailureDomainRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  FailureDomainRecord record = request.record;
  if (!validate_identity(record.id.value()).ok()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, "INVALID_FAILURE_DOMAIN_IDENTITY",
                    record.id.value(),
                    {factor("INVALID_FAILURE_DOMAIN_IDENTITY", "the failure domain identity is invalid")});
  }
  if (record.label.has_value() && !validate_label(*record.label).ok()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, "INVALID_LABEL", record.id.value(),
                    {factor("INVALID_LABEL", "the failure domain label is invalid")});
  }
  if (const auto error = validate_evidence(record.provenance, record.observed_at, record.ttl,
                                           record.durability);
      error.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, *error, record.id.value(),
                    {factor(*error, "the failure domain evidence is not structurally valid")});
  }
  std::set<FailureDomainId> unique_parents(record.parents.begin(), record.parents.end());
  record.parents.assign(unique_parents.begin(), unique_parents.end());
  for (const auto& parent : record.parents) {
    if (parent == record.id) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectInvalidRelationship, "FAILURE_DOMAIN_SELF_PARENT",
                      record.id.value(),
                      {factor("FAILURE_DOMAIN_SELF_PARENT", "a failure domain cannot parent itself")});
    }
    if (state.failure_domains.find(parent) == state.failure_domains.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectUnknownParent, "UNKNOWN_FAILURE_DOMAIN_PARENT",
                      record.id.value(),
                      {factor("UNKNOWN_FAILURE_DOMAIN_PARENT",
                              "the parent failure domain does not exist")});
    }
    if (state.domain_contains_cycle(parent, record.id)) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectInvalidRelationship, "FAILURE_DOMAIN_CYCLE",
                      record.id.value(),
                      {factor("FAILURE_DOMAIN_CYCLE", "the parent edge would create a cycle")});
    }
  }

  const auto existing_it = state.failure_domains.find(record.id);
  const bool is_new = existing_it == state.failure_domains.end();
  if (is_new && state.failure_domains.size() >= impl_->options.limits.max_failure_domains) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "FAILURE_DOMAIN_LIMIT_EXCEEDED",
                    record.id.value(),
                    {factor("FAILURE_DOMAIN_LIMIT_EXCEEDED", "the failure domain limit has been reached")});
  }
  if (!is_new) {
    const FailureDomainRecord& existing = existing_it->second;
    if (existing.lifecycle == MemberLifecycle::Retired &&
        record.lifecycle != MemberLifecycle::Retired) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectRetired, "FAILURE_DOMAIN_RETIRED", record.id.value(),
                      {factor("FAILURE_DOMAIN_RETIRED", "a retired failure domain cannot be resurrected")});
    }
    if (owner_conflict(existing.owner_boot, request.authority)) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectNotAuthorized,
                      "FAILURE_DOMAIN_OWNED_BY_OTHER_PROCESS", record.id.value(),
                      {factor("FAILURE_DOMAIN_OWNED_BY_OTHER_PROCESS",
                              "the failure domain is owned by a different process incarnation")});
    }
    if (request.expected_generation.has_value() &&
        *request.expected_generation < state.generations.failure_domains) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectStaleGeneration,
                      "STALE_FAILURE_DOMAIN_GENERATION", record.id.value(),
                      {generation_factor("STALE_FAILURE_DOMAIN_GENERATION",
                                         "the expected failure-domain generation is older than the "
                                         "current one",
                                         "failure_domain_generation",
                                         request.expected_generation->value(),
                                         state.generations.failure_domains.value())});
    }
    if (record.generation.is_set() && record.generation < existing.generation) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectStaleGeneration,
                      "STALE_FAILURE_DOMAIN_GENERATION", record.id.value(),
                      {generation_factor("STALE_FAILURE_DOMAIN_GENERATION",
                                         "the published failure-domain generation is older than the "
                                         "current one",
                                         "failure_domain_generation", record.generation.value(),
                                         existing.generation.value())});
    }
    if (same_failure_domain_content(existing, record)) {
      return unchanged(state, "FAILURE_DOMAIN_UNCHANGED", record.id.value());
    }
  }

  const auto next = state.generations.failure_domains.is_unset()
                        ? std::optional<FailureDomainGeneration>(FailureDomainGeneration::first())
                        : state.generations.failure_domains.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED", record.id.value(),
                    {factor("GENERATION_EXHAUSTED", "failure domain generation exhausted")});
  }
  record.generation = *next;
  if (request.authority.boot.has_value()) {
    record.owner_boot = request.authority.boot;
    if (check.publisher != nullptr) {
      record.owner_worker = check.publisher->worker;
    }
  }
  if (!is_new) {
    state.index_remove_failure_domain(existing_it->second);
  }
  state.index_add_failure_domain(record);
  state.failure_domains.insert_or_assign(record.id, std::move(record));
  state.generations.failure_domains = *next;
  advance_publication(state, check.publisher);
  return accepted(state, is_new ? "FAILURE_DOMAIN_ADDED" : "FAILURE_DOMAIN_UPDATED",
                  request.record.id.value());
}

MutationResult RackFabric::publish_power_envelope(const PublishPowerEnvelopeRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  PowerEnvelopeRecord record = request.record;
  if (const auto error = validate_evidence(record.provenance, record.observed_at, record.ttl,
                                           record.durability);
      error.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, *error, "power_envelope",
                    {factor(*error, "the power envelope evidence is not structurally valid")});
  }
  if (record.rack_limit.has_value()) {
    if (const auto error = validate_quantity(*record.rack_limit, QuantityUnit::Watts);
        error.has_value()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectInvalidInput, *error, "power_envelope",
                      {factor(*error, "the declared rack power limit is not structurally valid")});
    }
  }
  if (record.rack_observed_draw.has_value()) {
    // The observed draw is stored whatever unit it is in. Headroom is only
    // derived when it is comparable with the limit; a different unit is not an
    // error, it simply produces no derived headroom.
    const auto error = validate_quantity(*record.rack_observed_draw,
                                        record.rack_observed_draw->unit);
    if (error.has_value()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectInvalidInput, *error, "power_envelope",
                      {factor(*error, "the observed rack draw is not structurally valid")});
    }
  }
  if (state.power.has_value() && request.expected_generation.has_value() &&
      *request.expected_generation < state.generations.power) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_POWER_ENVELOPE_GENERATION",
                    "power_envelope",
                    {generation_factor("STALE_POWER_ENVELOPE_GENERATION",
                                       "the expected power envelope generation is older than the "
                                       "current one",
                                       "power_envelope_generation",
                                       request.expected_generation->value(),
                                       state.generations.power.value())});
  }
  if (state.power.has_value() && record.generation.is_set() &&
      record.generation < state.power->generation) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_POWER_ENVELOPE_GENERATION",
                    "power_envelope",
                    {generation_factor("STALE_POWER_ENVELOPE_GENERATION",
                                       "the published power envelope generation is older than the "
                                       "current one",
                                       "power_envelope_generation", record.generation.value(),
                                       state.power->generation.value())});
  }
  const auto next = state.generations.power.is_unset()
                        ? std::optional<PowerEnvelopeGeneration>(PowerEnvelopeGeneration::first())
                        : state.generations.power.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    "power_envelope",
                    {factor("GENERATION_EXHAUSTED", "power envelope generation exhausted")});
  }
  record.generation = *next;
  record.revalidation_required = false;
  if (request.authority.boot.has_value()) {
    record.owner_boot = request.authority.boot;
    if (check.publisher != nullptr) {
      record.owner_worker = check.publisher->worker;
    }
  }
  state.power = std::move(record);
  state.generations.power = *next;
  state.recompute_power_derived(now);
  const auto constraint_next = state.generations.constraints.is_unset()
                                   ? std::optional<ConstraintGeneration>(ConstraintGeneration::first())
                                   : state.generations.constraints.next();
  if (constraint_next.has_value()) {
    state.generations.constraints = *constraint_next;
  }
  advance_publication(state, check.publisher);
  return accepted(state, "POWER_ENVELOPE_PUBLISHED", "power_envelope");
}

MutationResult RackFabric::publish_cooling_envelope(const PublishCoolingEnvelopeRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  CoolingEnvelopeRecord record = request.record;
  if (const auto error = validate_evidence(record.provenance, record.observed_at, record.ttl,
                                           record.durability);
      error.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, *error, "cooling_envelope",
                    {factor(*error, "the cooling envelope evidence is not structurally valid")});
  }
  for (const auto& zone : record.zones) {
    if (state.members.find(MemberKey::of(MemberKind::CoolingDomain, zone.zone)) == state.members.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectUnknownParent, "UNKNOWN_COOLING_DOMAIN",
                      zone.zone.value(),
                      {factor("UNKNOWN_COOLING_DOMAIN",
                              "the cooling zone does not correspond to a cooling domain member")});
    }
    if (zone.design_thermal_limit.has_value()) {
      if (const auto error = validate_quantity(*zone.design_thermal_limit, QuantityUnit::Watts);
          error.has_value()) {
        note_rejection(check.publisher);
        return rejected(state, MutationOutcome::RejectInvalidInput, *error, zone.zone.value(),
                        {factor(*error, "a cooling envelope quantity is not structurally valid")});
      }
    }
    if (zone.cooling_capacity.has_value()) {
      if (const auto error = validate_quantity(*zone.cooling_capacity, QuantityUnit::Watts);
          error.has_value()) {
        note_rejection(check.publisher);
        return rejected(state, MutationOutcome::RejectInvalidInput, *error, zone.zone.value(),
                        {factor(*error, "a cooling envelope quantity is not structurally valid")});
      }
    }
    if (zone.observed_temperature.has_value()) {
      if (const auto error = validate_quantity(*zone.observed_temperature, QuantityUnit::Celsius);
          error.has_value()) {
        note_rejection(check.publisher);
        return rejected(state, MutationOutcome::RejectInvalidInput, *error, zone.zone.value(),
                        {factor(*error, "a cooling envelope quantity is not structurally valid")});
      }
    }
    if (zone.throttling.has_value()) {
      if (const auto error = validate_evidence(zone.throttling->provenance,
                                               zone.throttling->observed_at, zone.throttling->ttl,
                                               zone.throttling->durability);
          error.has_value()) {
        note_rejection(check.publisher);
        return rejected(state, MutationOutcome::RejectInvalidInput, *error, zone.zone.value(),
                        {factor(*error, "the throttling evidence is not structurally valid")});
      }
    }
  }
  std::sort(record.zones.begin(), record.zones.end(),
            [](const CoolingZoneRecord& lhs, const CoolingZoneRecord& rhs) { return lhs.zone < rhs.zone; });
  if (state.cooling.has_value() && request.expected_generation.has_value() &&
      *request.expected_generation < state.generations.cooling) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_COOLING_ENVELOPE_GENERATION",
                    "cooling_envelope",
                    {generation_factor("STALE_COOLING_ENVELOPE_GENERATION",
                                       "the expected cooling envelope generation is older than the "
                                       "current one",
                                       "cooling_envelope_generation",
                                       request.expected_generation->value(),
                                       state.generations.cooling.value())});
  }
  if (state.cooling.has_value() && record.generation.is_set() &&
      record.generation < state.cooling->generation) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_COOLING_ENVELOPE_GENERATION",
                    "cooling_envelope",
                    {generation_factor("STALE_COOLING_ENVELOPE_GENERATION",
                                       "the published cooling envelope generation is older than the "
                                       "current one",
                                       "cooling_envelope_generation", record.generation.value(),
                                       state.cooling->generation.value())});
  }
  const auto next = state.generations.cooling.is_unset()
                        ? std::optional<CoolingEnvelopeGeneration>(CoolingEnvelopeGeneration::first())
                        : state.generations.cooling.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    "cooling_envelope",
                    {factor("GENERATION_EXHAUSTED", "cooling envelope generation exhausted")});
  }
  record.generation = *next;
  record.revalidation_required = false;
  if (request.authority.boot.has_value()) {
    record.owner_boot = request.authority.boot;
    if (check.publisher != nullptr) {
      record.owner_worker = check.publisher->worker;
    }
  }
  state.cooling = std::move(record);
  state.generations.cooling = *next;
  state.recompute_cooling_derived(now);
  const auto constraint_next = state.generations.constraints.is_unset()
                                   ? std::optional<ConstraintGeneration>(ConstraintGeneration::first())
                                   : state.generations.constraints.next();
  if (constraint_next.has_value()) {
    state.generations.constraints = *constraint_next;
  }
  advance_publication(state, check.publisher);
  return accepted(state, "COOLING_ENVELOPE_PUBLISHED", "cooling_envelope");
}

MutationResult RackFabric::publish_health(const PublishHealthRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  const auto existing = state.members.find(request.member);
  if (existing == state.members.end()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectUnknownMember, "UNKNOWN_MEMBER",
                    request.member.to_string(),
                    {factor("UNKNOWN_MEMBER", "the member does not exist in the current rack")});
  }
  if (owner_conflict(existing->second.owner_boot, request.authority)) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectNotAuthorized, "MEMBER_OWNED_BY_OTHER_PROCESS",
                    request.member.to_string(),
                    {factor("MEMBER_OWNED_BY_OTHER_PROCESS",
                            "the member is owned by a different process incarnation")});
  }
  if (request.expected_generation.has_value() &&
      *request.expected_generation < existing->second.generation) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectStaleGeneration, "STALE_MEMBER_GENERATION",
                    request.member.to_string(),
                    {generation_factor("STALE_MEMBER_GENERATION",
                                       "the expected member generation is older than the current one",
                                       "member_generation", request.expected_generation->value(),
                                       existing->second.generation.value())});
  }
  if (const auto error = validate_evidence(request.health.provenance, request.health.observed_at,
                                           request.health.ttl, request.health.durability);
      error.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, *error, request.member.to_string(),
                    {factor(*error, "the health evidence is not structurally valid")});
  }
  if (request.publish_readiness) {
    if (const auto error =
            validate_evidence(request.readiness.provenance, request.readiness.observed_at,
                              request.readiness.ttl, request.readiness.durability);
        error.has_value()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectInvalidInput, *error, request.member.to_string(),
                      {factor(*error, "the readiness evidence is not structurally valid")});
    }
  }
  MemberRecord record = existing->second;
  if (request.health.has_evidence()) {
    record.health = request.health;
  } else {
    record.health = EvidenceValue<HealthState>{};
  }
  if (request.publish_readiness) {
    record.readiness = request.readiness;
  }
  const auto next = record.generation.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    request.member.to_string(),
                    {factor("GENERATION_EXHAUSTED", "member generation exhausted")});
  }
  record.generation = *next;
  if (check.publisher != nullptr) {
    record.publication_generation = check.publisher->publication_generation;
  }
  state.index_remove_member(existing->second);
  state.index_add_member(record);
  state.members.insert_or_assign(record.key, std::move(record));
  const auto health_next = state.generations.health.is_unset()
                               ? std::optional<HealthGeneration>(HealthGeneration::first())
                               : state.generations.health.next();
  if (health_next.has_value()) {
    state.generations.health = *health_next;
  }
  advance_publication(state, check.publisher);
  return accepted(state, "HEALTH_PUBLISHED", request.member.to_string());
}

MutationResult RackFabric::publish_capability(const PublishCapabilityRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  const auto existing = state.members.find(request.member);
  if (existing == state.members.end()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectUnknownMember, "UNKNOWN_MEMBER",
                    request.member.to_string(),
                    {factor("UNKNOWN_MEMBER", "the member does not exist in the current rack")});
  }
  if (owner_conflict(existing->second.owner_boot, request.authority)) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectNotAuthorized, "MEMBER_OWNED_BY_OTHER_PROCESS",
                    request.member.to_string(),
                    {factor("MEMBER_OWNED_BY_OTHER_PROCESS",
                            "the member is owned by a different process incarnation")});
  }
  if (!request.capability.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, "CAPABILITY_REQUIRED",
                    request.member.to_string(),
                    {factor("CAPABILITY_REQUIRED", "the request does not carry a capability")});
  }
  const CapabilityRef& requested_capability = *request.capability;
  if (!validate_identity(requested_capability.id.value()).ok()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, "INVALID_CAPABILITY_IDENTITY",
                    request.member.to_string(),
                    {factor("INVALID_CAPABILITY_IDENTITY", "the capability identity is invalid")});
  }
  if (requested_capability.value.has_value() && !validate_label(*requested_capability.value).ok()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, "INVALID_CAPABILITY_VALUE",
                    request.member.to_string(),
                    {factor("INVALID_CAPABILITY_VALUE", "the capability value is invalid")});
  }
  if (const auto error = validate_evidence(requested_capability.provenance,
                                           requested_capability.observed_at,
                                           requested_capability.ttl,
                                           requested_capability.durability);
      error.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectInvalidInput, *error, request.member.to_string(),
                    {factor(*error, "the capability evidence is not structurally valid")});
  }
  MemberRecord record = existing->second;
  const auto found = std::find_if(record.capabilities.begin(), record.capabilities.end(),
                                  [&requested_capability](const CapabilityRef& capability) {
                                    return capability.id == requested_capability.id;
                                  });
  if (found != record.capabilities.end()) {
    CapabilityRef candidate = requested_capability;
    candidate.revalidation_required = false;
    if (*found == candidate) {
      return unchanged(state, "CAPABILITY_UNCHANGED", request.member.to_string());
    }
    *found = std::move(candidate);
  } else {
    if (record.capabilities.size() >= impl_->options.limits.max_capabilities_per_member) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectLimitExceeded, "CAPABILITY_LIMIT_EXCEEDED",
                      request.member.to_string(),
                      {factor("CAPABILITY_LIMIT_EXCEEDED", "the capability limit has been reached")});
    }
    CapabilityRef candidate = requested_capability;
    candidate.revalidation_required = false;
    record.capabilities.push_back(std::move(candidate));
  }
  const auto next = record.generation.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    request.member.to_string(),
                    {factor("GENERATION_EXHAUSTED", "member generation exhausted")});
  }
  record.generation = *next;
  if (check.publisher != nullptr) {
    record.publication_generation = check.publisher->publication_generation;
  }
  state.index_remove_member(existing->second);
  state.index_add_member(record);
  state.members.insert_or_assign(record.key, std::move(record));
  const auto capability_next = state.generations.capabilities.is_unset()
                                   ? std::optional<CapabilityGeneration>(CapabilityGeneration::first())
                                   : state.generations.capabilities.next();
  if (capability_next.has_value()) {
    state.generations.capabilities = *capability_next;
  }
  advance_publication(state, check.publisher);
  return accepted(state, "CAPABILITY_PUBLISHED", request.member.to_string());
}

MutationResult RackFabric::withdraw_evidence(const WithdrawEvidenceRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  const auto existing = state.members.find(request.member);
  if (existing == state.members.end()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectUnknownMember, "UNKNOWN_MEMBER",
                    request.member.to_string(),
                    {factor("UNKNOWN_MEMBER", "the member does not exist in the current rack")});
  }
  if (owner_conflict(existing->second.owner_boot, request.authority)) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectNotAuthorized, "MEMBER_OWNED_BY_OTHER_PROCESS",
                    request.member.to_string(),
                    {factor("MEMBER_OWNED_BY_OTHER_PROCESS",
                            "the member is owned by a different process incarnation")});
  }
  MemberRecord record = existing->second;
  const auto reset_evidence = [](auto& evidence) {
    evidence = std::remove_reference_t<decltype(evidence)>{};
  };
  switch (request.scope) {
    case WithdrawScope::All:
      record.provenance = EvidenceProvenance::Unknown;
      record.observed_at = Timestamp::unknown();
      record.ttl = std::chrono::milliseconds{0};
      record.revalidation_required = false;
      record.revalidation_reason = RevalidationReason::None;
      record.lifecycle = MemberLifecycle::Declared;
      reset_evidence(record.health);
      reset_evidence(record.readiness);
      reset_evidence(record.reachability);
      record.capabilities.clear();
      break;
    case WithdrawScope::Health:
      reset_evidence(record.health);
      break;
    case WithdrawScope::Readiness:
      reset_evidence(record.readiness);
      break;
    case WithdrawScope::Reachability:
      reset_evidence(record.reachability);
      break;
    case WithdrawScope::Capabilities:
      record.capabilities.clear();
      break;
    case WithdrawScope::Details:
      record.details = std::monostate{};
      break;
  }
  const auto next = record.generation.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    request.member.to_string(),
                    {factor("GENERATION_EXHAUSTED", "member generation exhausted")});
  }
  record.generation = *next;
  if (check.publisher != nullptr) {
    record.publication_generation = check.publisher->publication_generation;
  }
  state.index_remove_member(existing->second);
  state.index_add_member(record);
  state.members.insert_or_assign(record.key, std::move(record));
  const auto membership_next = state.generations.membership.is_unset()
                                   ? std::optional<MembershipGeneration>(MembershipGeneration::first())
                                   : state.generations.membership.next();
  if (membership_next.has_value()) {
    state.generations.membership = *membership_next;
  }
  advance_publication(state, check.publisher);
  return accepted(state, "EVIDENCE_WITHDRAWN", request.member.to_string());
}

MutationResult RackFabric::withdraw_relationship(const WithdrawRelationshipRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  const RelationshipKey key = RelationshipKey::canonicalize(request.key.cls, request.key.from,
                                                            request.key.to);
  const auto existing = state.relationships.find(key);
  if (existing == state.relationships.end()) {
    return unchanged(state, "RELATIONSHIP_ABSENT", key.to_string());
  }
  if (owner_conflict(existing->second.owner_boot, request.authority)) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectNotAuthorized, "RELATIONSHIP_OWNED_BY_OTHER_PROCESS",
                    key.to_string(),
                    {factor("RELATIONSHIP_OWNED_BY_OTHER_PROCESS",
                            "the relationship is owned by a different process incarnation")});
  }
  state.index_remove_relationship(existing->second);
  state.relationships.erase(existing);
  const auto next = state.generations.topology.is_unset()
                        ? std::optional<TopologyGeneration>(TopologyGeneration::first())
                        : state.generations.topology.next();
  if (next.has_value()) {
    state.generations.topology = *next;
  }
  advance_publication(state, check.publisher);
  return accepted(state, "RELATIONSHIP_WITHDRAWN", key.to_string());
}

MutationResult RackFabric::mark_unavailable(const MarkUnavailableRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  const auto existing = state.members.find(request.member);
  if (existing == state.members.end()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectUnknownMember, "UNKNOWN_MEMBER",
                    request.member.to_string(),
                    {factor("UNKNOWN_MEMBER", "the member does not exist in the current rack")});
  }
  if (owner_conflict(existing->second.owner_boot, request.authority)) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectNotAuthorized, "MEMBER_OWNED_BY_OTHER_PROCESS",
                    request.member.to_string(),
                    {factor("MEMBER_OWNED_BY_OTHER_PROCESS",
                            "the member is owned by a different process incarnation")});
  }
  if (existing->second.lifecycle == MemberLifecycle::Retired) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectRetired, "MEMBER_RETIRED", request.member.to_string(),
                    {factor("MEMBER_RETIRED", "a retired member cannot be marked unavailable")});
  }
  MemberRecord record = existing->second;
  if (record.lifecycle == MemberLifecycle::Unavailable) {
    return unchanged(state, "MEMBER_ALREADY_UNAVAILABLE", request.member.to_string());
  }
  record.lifecycle = MemberLifecycle::Unavailable;
  record.revalidation_required = true;
  record.revalidation_reason = request.reason;
  const auto next = record.generation.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    request.member.to_string(),
                    {factor("GENERATION_EXHAUSTED", "member generation exhausted")});
  }
  record.generation = *next;
  state.index_remove_member(existing->second);
  state.index_add_member(record);
  state.members.insert_or_assign(record.key, std::move(record));
  const auto membership_next = state.generations.membership.is_unset()
                                   ? std::optional<MembershipGeneration>(MembershipGeneration::first())
                                   : state.generations.membership.next();
  if (membership_next.has_value()) {
    state.generations.membership = *membership_next;
  }
  advance_publication(state, check.publisher);
  return accepted(state, "MEMBER_UNAVAILABLE", request.member.to_string());
}

MutationResult RackFabric::retire_member(const RetireMemberRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  const auto existing = state.members.find(request.member);
  if (existing == state.members.end()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectUnknownMember, "UNKNOWN_MEMBER",
                    request.member.to_string(),
                    {factor("UNKNOWN_MEMBER", "the member does not exist in the current rack")});
  }
  if (existing->second.lifecycle == MemberLifecycle::Retired) {
    return unchanged(state, "MEMBER_ALREADY_RETIRED", request.member.to_string());
  }
  MemberRecord record = existing->second;
  record.lifecycle = MemberLifecycle::Retired;
  record.revalidation_required = false;
  record.revalidation_reason = RevalidationReason::None;
  // Retirement supersedes the record's live observation. A retired identity is
  // not a statement about the present, so its observation time, lifetime and
  // live evidence values are cleared; the identity and its provenance remain
  // as history.
  record.observed_at = Timestamp::unknown();
  record.ttl = std::chrono::milliseconds{0};
  record.health = EvidenceValue<HealthState>{};
  record.readiness = EvidenceValue<ReadinessState>{};
  record.reachability = EvidenceValue<ReachabilityState>{};
  const auto next = record.generation.next();
  if (!next.has_value()) {
    note_rejection(check.publisher);
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    request.member.to_string(),
                    {factor("GENERATION_EXHAUSTED", "member generation exhausted")});
  }
  record.generation = *next;

  // A retired member is not part of the current composition, so relationships
  // that reference it as a current endpoint are withdrawn with it.
  std::vector<RelationshipKey> removed;
  const auto touching = state.relationships_by_member.find(request.member);
  if (touching != state.relationships_by_member.end()) {
    removed.assign(touching->second.begin(), touching->second.end());
  }
  for (const auto& key : removed) {
    const auto it = state.relationships.find(key);
    if (it != state.relationships.end()) {
      state.index_remove_relationship(it->second);
      state.relationships.erase(it);
    }
  }
  // Nested containment is preserved as a historical fact only if the parent
  // still exists; children are detached so that no current member claims a
  // retired parent.
  for (auto& [key, child] : state.members) {
    (void)key;
    if (child.parent.has_value() && *child.parent == request.member) {
      state.index_remove_member(child);
      child.parent.reset();
      state.index_add_member(child);
    }
  }

  state.index_remove_member(existing->second);
  state.index_add_member(record);
  state.members.insert_or_assign(record.key, std::move(record));
  const auto membership_next = state.generations.membership.is_unset()
                                   ? std::optional<MembershipGeneration>(MembershipGeneration::first())
                                   : state.generations.membership.next();
  if (membership_next.has_value()) {
    state.generations.membership = *membership_next;
  }
  if (!removed.empty()) {
    const auto topology_next = state.generations.topology.is_unset()
                                   ? std::optional<TopologyGeneration>(TopologyGeneration::first())
                                   : state.generations.topology.next();
    if (topology_next.has_value()) {
      state.generations.topology = *topology_next;
    }
  }
  advance_publication(state, check.publisher);
  return accepted(state, "MEMBER_RETIRED", request.member.to_string());
}

MutationResult RackFabric::retire_rack(const RetireRackRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  if (!state.rack.has_value()) {
    return rejected(state, MutationOutcome::RejectUnknownRack, "RACK_NOT_DECLARED", "<none>",
                    {factor("RACK_NOT_DECLARED", "no rack identity has been declared")});
  }
  if (state.rack->retired) {
    return unchanged(state, "RACK_ALREADY_RETIRED", state.rack->id.value());
  }
  if (request.authority.coordinator_epoch != state.generations.coordinator_epoch) {
    return rejected(state, MutationOutcome::RejectStaleCoordinatorEpoch, "STALE_COORDINATOR_EPOCH",
                    state.rack->id.value(),
                    {generation_factor("STALE_COORDINATOR_EPOCH",
                                       "the mutation carries an epoch that is not current",
                                       "coordinator_epoch",
                                       request.authority.coordinator_epoch.value(),
                                       state.generations.coordinator_epoch.value())});
  }
  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  const auto next = state.rack->generation.next();
  if (!next.has_value()) {
    return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                    state.rack->id.value(),
                    {factor("GENERATION_EXHAUSTED", "rack generation exhausted")});
  }
  state.rack->retired = true;
  state.rack->generation = *next;
  state.generations.rack = *next;
  for (auto& [key, record] : state.members) {
    (void)key;
    if (record.lifecycle == MemberLifecycle::Present) {
      record.lifecycle = MemberLifecycle::Unavailable;
      record.revalidation_required = true;
      record.revalidation_reason = RevalidationReason::Superseded;
    }
  }
  for (auto& [boot, publisher] : state.publishers) {
    publisher.state = PublisherState::Lost;
    FencedBootRecord fenced{boot};
    fenced.worker = publisher.worker;
    fenced.fenced_under_epoch = state.generations.coordinator_epoch;
    fenced.fenced_at = now;
    fenced.reason = RevalidationReason::Superseded;
    state.fenced_boots.insert_or_assign(boot, std::move(fenced));
  }
  advance_publication(state, check.publisher);
  return accepted(state, "RACK_RETIRED", state.rack->id.value());
}

MutationResult RackFabric::revalidate_members(const RevalidateMembersRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  std::vector<MemberKey> targets = request.members;
  if (request.all) {
    targets.clear();
    for (const auto& [key, record] : state.members) {
      if (record.revalidation_required) {
        targets.push_back(key);
      }
    }
  }
  if (targets.empty()) {
    return unchanged(state, "NOTHING_TO_REVALIDATE", "<none>");
  }
  std::size_t revalidated = 0;
  for (const auto& key : targets) {
    const auto it = state.members.find(key);
    if (it == state.members.end()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectUnknownMember, "UNKNOWN_MEMBER", key.to_string(),
                      {factor("UNKNOWN_MEMBER", "the member does not exist in the current rack")});
    }
    if (owner_conflict(it->second.owner_boot, request.authority)) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectNotAuthorized, "MEMBER_OWNED_BY_OTHER_PROCESS",
                      key.to_string(),
                      {factor("MEMBER_OWNED_BY_OTHER_PROCESS",
                              "the member is owned by a different process incarnation")});
    }
    if (!it->second.revalidation_required) {
      continue;
    }
    MemberRecord record = it->second;
    record.revalidation_required = false;
    record.revalidation_reason = RevalidationReason::None;
    const auto next = record.generation.next();
    if (!next.has_value()) {
      note_rejection(check.publisher);
      return rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED", key.to_string(),
                      {factor("GENERATION_EXHAUSTED", "member generation exhausted")});
    }
    record.generation = *next;
    state.index_remove_member(it->second);
    state.index_add_member(record);
    state.members.insert_or_assign(key, std::move(record));
    ++revalidated;
  }
  if (revalidated == 0) {
    return unchanged(state, "NOTHING_TO_REVALIDATE", "<none>");
  }
  const auto membership_next = state.generations.membership.is_unset()
                                   ? std::optional<MembershipGeneration>(MembershipGeneration::first())
                                   : state.generations.membership.next();
  if (membership_next.has_value()) {
    state.generations.membership = *membership_next;
  }
  advance_publication(state, check.publisher);
  return accepted(state, "MEMBERS_REVALIDATED", std::to_string(revalidated));
}

MutationResult RackFabric::heartbeat(const HeartbeatRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  const AuthorityCheck check = check_authority(state, request.authority, now, false);
  if (!check.ok()) {
    note_rejection(check.publisher);
    return rejected(state, check.outcome, check.code, check.subject, check.factors);
  }
  if (check.publisher == nullptr) {
    return rejected(state, MutationOutcome::RejectNotRegistered, "HEARTBEAT_REQUIRES_BOOT",
                    request.authority.boot.has_value() ? request.authority.boot->value() : "<none>",
                    {factor("HEARTBEAT_REQUIRES_BOOT",
                            "a heartbeat must present a registered boot identity")});
  }
  check.publisher->last_seen_at = now;
  check.publisher->state = PublisherState::Active;
  return accepted(state, "HEARTBEAT_ACCEPTED", check.publisher->boot.value());
}

MutationResult RackFabric::fence_publisher(const FencePublisherRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  if (!request.boot.has_value()) {
    return rejected(state, MutationOutcome::RejectInvalidInput, "FENCE_REQUIRES_BOOT", "<none>",
                    {factor("FENCE_REQUIRES_BOOT",
                            "a fence request must name the boot identity to fence")});
  }
  const AgentBootId boot = *request.boot;
  if (request.coordinator_epoch != state.generations.coordinator_epoch) {
    return rejected(state, MutationOutcome::RejectStaleCoordinatorEpoch, "STALE_COORDINATOR_EPOCH",
                    boot.value(),
                    {generation_factor("STALE_COORDINATOR_EPOCH",
                                       "the fence request carries an epoch that is not current",
                                       "coordinator_epoch", request.coordinator_epoch.value(),
                                       state.generations.coordinator_epoch.value())});
  }
  const auto publisher_it = state.publishers.find(boot);
  if (publisher_it == state.publishers.end()) {
    if (state.fenced_boots.find(boot) != state.fenced_boots.end()) {
      return unchanged(state, "PUBLISHER_ALREADY_FENCED", boot.value());
    }
    return rejected(state, MutationOutcome::RejectNotRegistered, "PUBLISHER_NOT_REGISTERED",
                    boot.value(),
                    {factor("PUBLISHER_NOT_REGISTERED", "the boot identity is not registered")});
  }

  if (publisher_it->second.state == PublisherState::Fenced) {
    // Fencing is idempotent: a fenced incarnation cannot be fenced again, and
    // repeating the request must not churn member generations.
    return unchanged(state, "PUBLISHER_ALREADY_FENCED", boot.value());
  }
  publisher_it->second.state = PublisherState::Fenced;
  FencedBootRecord fenced{boot};
  fenced.worker = publisher_it->second.worker;
  fenced.fenced_under_epoch = state.generations.coordinator_epoch;
  fenced.fenced_at = now;
  fenced.reason = request.reason;
  state.fenced_boots.insert_or_assign(boot, std::move(fenced));

  const auto owned = state.members_by_owner_boot.find(boot);
  std::vector<MemberKey> owned_keys;
  if (owned != state.members_by_owner_boot.end()) {
    owned_keys.assign(owned->second.begin(), owned->second.end());
  }
  for (const auto& key : owned_keys) {
    const auto it = state.members.find(key);
    if (it == state.members.end()) {
      continue;
    }
    MemberRecord record = it->second;
    fence_member_evidence(record, request.reason);
    const auto next = record.generation.next();
    if (next.has_value()) {
      record.generation = *next;
    }
    state.index_remove_member(it->second);
    state.index_add_member(record);
    state.members.insert_or_assign(key, std::move(record));
  }
  for (auto& [key, record] : state.relationships) {
    (void)key;
    if (record.owner_boot.has_value() && *record.owner_boot == boot &&
        record.durability == Durability::Ephemeral) {
      record.revalidation_required = true;
    }
  }
  for (auto& [key, record] : state.failure_domains) {
    (void)key;
    if (record.owner_boot.has_value() && *record.owner_boot == boot &&
        record.durability == Durability::Ephemeral) {
      record.revalidation_required = true;
    }
  }
  if (state.power.has_value() && state.power->durability == Durability::Ephemeral &&
      state.power->owner_boot.has_value() && *state.power->owner_boot == boot) {
    state.power->revalidation_required = true;
    state.power->rack_observed_draw.reset();
    state.power->rack_headroom.reset();
  }
  if (state.cooling.has_value() && state.cooling->durability == Durability::Ephemeral &&
      state.cooling->owner_boot.has_value() && *state.cooling->owner_boot == boot) {
    state.cooling->revalidation_required = true;
    for (auto& zone : state.cooling->zones) {
      zone.observed_temperature.reset();
      zone.thermal_headroom.reset();
      zone.throttling.reset();
    }
  }
  const auto membership_next = state.generations.membership.is_unset()
                                   ? std::optional<MembershipGeneration>(MembershipGeneration::first())
                                   : state.generations.membership.next();
  if (membership_next.has_value()) {
    state.generations.membership = *membership_next;
  }
  return accepted(state, "PUBLISHER_FENCED", boot.value());
}

std::size_t RackFabric::expire_publishers() {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();
  std::vector<AgentBootId> expired;
  for (const auto& [boot, publisher] : state.publishers) {
    if (publisher.state != PublisherState::Registered && publisher.state != PublisherState::Active) {
      continue;
    }
    if (!publisher.last_seen_at.known() || publisher.lease_millis <= 0) {
      continue;
    }
    if (now.millis() - publisher.last_seen_at.millis() > publisher.lease_millis) {
      expired.push_back(boot);
    }
  }
  lock.unlock();
  std::size_t count = 0;
  for (const auto& boot : expired) {
    FencePublisherRequest request;
    request.coordinator_epoch = coordinator_epoch();
    request.boot = boot;
    request.reason = RevalidationReason::OwnerProcessLost;
    if (fence_publisher(request).accepted()) {
      ++count;
    }
  }
  return count;
}

// ---------------------------------------------------------------------------
// Inspection
// ---------------------------------------------------------------------------

RackLifecycle RackFabric::lifecycle() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.derive_lifecycle(impl_->now());
}

GenerationSet RackFabric::generations() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.generations;
}

std::optional<RackId> RackFabric::rack_id() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  if (!impl_->state.rack.has_value()) {
    return std::nullopt;
  }
  return impl_->state.rack->id;
}

std::optional<RackEpochId> RackFabric::rack_epoch() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  if (!impl_->state.rack.has_value()) {
    return std::nullopt;
  }
  return impl_->state.rack->epoch;
}

RackSummary RackFabric::summary() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const RackState& state = impl_->state;
  const Timestamp now = impl_->now();
  RackSummary summary;
  summary.lifecycle = state.derive_lifecycle(now);
  summary.generations = state.generations;
  if (state.rack.has_value()) {
    summary.rack = state.rack->id;
    summary.rack_epoch = state.rack->epoch;
  }
  summary.member_count = state.members.size();
  summary.relationship_count = state.relationships.size();
  summary.failure_domain_count = state.failure_domains.size();
  summary.publisher_count = state.publishers.size();
  summary.fenced_boot_count = state.fenced_boots.size();
  summary.snapshot_count = state.snapshots.size();
  summary.power_envelope_known = state.power.has_value();
  summary.cooling_envelope_known = state.cooling.has_value();
  for (const auto& [key, record] : state.members) {
    switch (key.kind) {
      case MemberKind::Node:
        ++summary.node_count;
        break;
      case MemberKind::Accelerator:
        ++summary.accelerator_count;
        break;
      case MemberKind::CpuPackage:
        ++summary.cpu_package_count;
        break;
      case MemberKind::MemoryDomain:
        ++summary.memory_domain_count;
        break;
      case MemberKind::Nic:
        ++summary.nic_count;
        break;
      case MemberKind::Dpu:
        ++summary.dpu_count;
        break;
      case MemberKind::Switch:
        ++summary.switch_count;
        break;
      case MemberKind::StorageEndpoint:
        ++summary.storage_endpoint_count;
        break;
      case MemberKind::PowerDomain:
        ++summary.power_domain_count;
        break;
      case MemberKind::CoolingDomain:
        ++summary.cooling_domain_count;
        break;
    }
    if (record.lifecycle == MemberLifecycle::Present) {
      ++summary.present_member_count;
    } else if (record.lifecycle == MemberLifecycle::Unavailable) {
      ++summary.unavailable_member_count;
    } else if (record.lifecycle == MemberLifecycle::Retired) {
      ++summary.retired_member_count;
    }
    if (record.revalidation_required) {
      ++summary.revalidation_required_member_count;
    }
    if (is_current_member(record.lifecycle) && record.freshness_at(now) != Freshness::Fresh) {
      ++summary.stale_member_count;
    }
  }
  for (const auto& [boot, publisher] : state.publishers) {
    (void)boot;
    if (publisher.state == PublisherState::Active) {
      ++summary.active_publisher_count;
    }
  }
  summary.digest = impl_->summary_digest(summary);
  return summary;
}

std::optional<MemberRecord> RackFabric::find_member(const MemberKey& key) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const auto it = impl_->state.members.find(key);
  if (it == impl_->state.members.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<MemberRecord> RackFabric::members(std::optional<MemberKind> kind) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<MemberRecord> out;
  if (kind.has_value()) {
    const auto it = impl_->state.members_by_kind.find(*kind);
    if (it == impl_->state.members_by_kind.end()) {
      return out;
    }
    out.reserve(it->second.size());
    for (const auto& key : it->second) {
      const auto record = impl_->state.members.find(key);
      if (record != impl_->state.members.end()) {
        out.push_back(record->second);
      }
    }
    return out;
  }
  out.reserve(impl_->state.members.size());
  for (const auto& [key, record] : impl_->state.members) {
    (void)key;
    out.push_back(record);
  }
  return out;
}

std::vector<RelationshipRecord> RackFabric::relationships() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<RelationshipRecord> out;
  out.reserve(impl_->state.relationships.size());
  for (const auto& [key, record] : impl_->state.relationships) {
    (void)key;
    out.push_back(record);
  }
  return out;
}

std::vector<RelationshipRecord> RackFabric::relationships_touching(const MemberKey& key) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<RelationshipRecord> out;
  const auto it = impl_->state.relationships_by_member.find(key);
  if (it == impl_->state.relationships_by_member.end()) {
    return out;
  }
  out.reserve(it->second.size());
  for (const auto& relationship_key : it->second) {
    const auto record = impl_->state.relationships.find(relationship_key);
    if (record != impl_->state.relationships.end()) {
      out.push_back(record->second);
    }
  }
  return out;
}

std::optional<RelationshipRecord> RackFabric::find_relationship(const RelationshipKey& key) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const RelationshipKey canonical =
      RelationshipKey::canonicalize(key.cls, key.from, key.to);
  const auto it = impl_->state.relationships.find(canonical);
  if (it == impl_->state.relationships.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<FailureDomainRecord> RackFabric::failure_domains() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<FailureDomainRecord> out;
  out.reserve(impl_->state.failure_domains.size());
  for (const auto& [id, record] : impl_->state.failure_domains) {
    (void)id;
    out.push_back(record);
  }
  return out;
}

std::optional<FailureDomainRecord> RackFabric::find_failure_domain(const FailureDomainId& id) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const auto it = impl_->state.failure_domains.find(id);
  if (it == impl_->state.failure_domains.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<FailureDomainId> RackFabric::failure_domains_of(const MemberKey& key) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.failure_domains_of(key);
}

std::vector<MemberKey> RackFabric::members_in_failure_domain(const FailureDomainId& id,
                                                             bool include_nested) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.members_in_failure_domain(id, include_nested);
}

std::vector<MemberKey> RackFabric::members_affected_by(const FailureDomainId& id) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.members_affected_by(id);
}

bool RackFabric::shares_failure_domain(const MemberKey& lhs, const MemberKey& rhs) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.shares_failure_domain(lhs, rhs);
}

std::optional<PowerEnvelopeRecord> RackFabric::power_envelope() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.power;
}

std::optional<CoolingEnvelopeRecord> RackFabric::cooling_envelope() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.cooling;
}

std::vector<PublisherRecord> RackFabric::publishers() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<PublisherRecord> out;
  out.reserve(impl_->state.publishers.size());
  for (const auto& [boot, publisher] : impl_->state.publishers) {
    (void)boot;
    out.push_back(publisher);
  }
  return out;
}

std::optional<PublisherRecord> RackFabric::find_publisher(const AgentBootId& boot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const auto it = impl_->state.publishers.find(boot);
  if (it == impl_->state.publishers.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<FencedBootRecord> RackFabric::fenced_boots() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<FencedBootRecord> out;
  out.reserve(impl_->state.fenced_boots.size());
  for (const auto& [boot, record] : impl_->state.fenced_boots) {
    (void)boot;
    out.push_back(record);
  }
  return out;
}

bool RackFabric::is_boot_fenced(const AgentBootId& boot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.fenced_boots.find(boot) != impl_->state.fenced_boots.end();
}

ReadinessEvaluation RackFabric::evaluate_readiness() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.evaluate_readiness(impl_->now());
}

// ---------------------------------------------------------------------------
// Explanations
// ---------------------------------------------------------------------------

Explanation RackFabric::explain_readiness() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const RackState& state = impl_->state;
  const Timestamp now = impl_->now();
  const std::string subject = state.rack.has_value() ? state.rack->id.value() : std::string("<none>");
  if (!state.rack.has_value()) {
    return Explanation::failure(
        "RACK_NOT_DECLARED", subject,
        {factor("RACK_NOT_DECLARED", "no rack identity has been declared in this instance")});
  }
  if (state.rack->retired) {
    return Explanation::failure("RACK_RETIRED", subject,
                                {factor("RACK_RETIRED", "the rack has been retired")});
  }
  const ReadinessEvaluation evaluation = state.evaluate_readiness(now);
  if (evaluation.satisfied) {
    Explanation explanation = Explanation::success("RACK_READY", subject);
    explanation.factors.push_back(
        factor("READINESS_CONTRACT_SATISFIED",
               "the configured readiness contract is satisfied under current authority"));
    return explanation;
  }
  std::vector<ExplanationFactor> factors;
  factors.reserve(evaluation.deficits.size());
  for (const auto& deficit : evaluation.deficits) {
    ExplanationFactor entry;
    entry.code = deficit.code;
    entry.detail = deficit.detail;
    entry.expected_generation = deficit.required;
    entry.current_generation = deficit.observed;
    factors.push_back(std::move(entry));
  }
  return Explanation::failure("RACK_NOT_READY", subject, std::move(factors));
}

Explanation RackFabric::explain_member(const MemberKey& key) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const RackState& state = impl_->state;
  const Timestamp now = impl_->now();
  const std::string subject = key.to_string();
  const auto it = state.members.find(key);
  if (it == state.members.end()) {
    return Explanation::failure(
        "MEMBER_UNKNOWN", subject,
        {factor("MEMBER_UNKNOWN", "the member does not exist in the current rack")});
  }
  const MemberRecord& record = it->second;
  const Freshness freshness = record.freshness_at(now);
  if (record.lifecycle == MemberLifecycle::Present && freshness == Freshness::Fresh) {
    Explanation explanation = Explanation::success("MEMBER_CURRENT", subject);
    explanation.factors.push_back(
        factor("MEMBER_PRESENT", "the member is present with current evidence"));
    return explanation;
  }
  std::vector<ExplanationFactor> factors;
  {
    ExplanationFactor entry;
    entry.code = "MEMBER_LIFECYCLE";
    entry.detail = "the member lifecycle is not PRESENT";
    entry.state = std::string(to_string(record.lifecycle));
    factors.push_back(std::move(entry));
  }
  {
    ExplanationFactor entry;
    entry.code = "MEMBER_FRESHNESS";
    entry.detail = "the member evidence is not current";
    entry.state = std::string(to_string(freshness));
    factors.push_back(std::move(entry));
  }
  {
    ExplanationFactor entry;
    entry.code = "MEMBER_PROVENANCE";
    entry.detail = "the provenance class of the member evidence";
    entry.state = std::string(to_string(record.provenance));
    factors.push_back(std::move(entry));
  }
  if (record.revalidation_required) {
    factors.push_back(factor("MEMBER_REVALIDATION_REQUIRED",
                             std::string("the member requires revalidation: ") +
                                 std::string(to_string(record.revalidation_reason))));
  }
  if (record.owner_boot.has_value()) {
    const bool fenced = state.fenced_boots.find(*record.owner_boot) != state.fenced_boots.end();
    factors.push_back(factor("MEMBER_OWNER",
                             std::string("owned by boot ") + record.owner_boot->value() +
                                 (fenced ? " (fenced)" : "")));
  }
  if (record.health.has_evidence()) {
    ExplanationFactor entry;
    entry.code = "MEMBER_HEALTH";
    entry.detail = "the published health state";
    entry.state = std::string(to_string(record.health.value));
    factors.push_back(std::move(entry));
  } else {
    factors.push_back(factor("MEMBER_HEALTH_UNKNOWN", "no health evidence has been published"));
  }
  return Explanation::failure("MEMBER_NOT_CURRENT", subject, std::move(factors));
}

Explanation RackFabric::explain_relationship(const RelationshipKey& key) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const RackState& state = impl_->state;
  const Timestamp now = impl_->now();
  const RelationshipKey canonical = RelationshipKey::canonicalize(key.cls, key.from, key.to);
  const std::string subject = canonical.to_string();
  const auto it = state.relationships.find(canonical);
  if (it == state.relationships.end()) {
    return Explanation::failure(
        "RELATIONSHIP_UNKNOWN", subject,
        {factor("RELATIONSHIP_UNKNOWN", "no such relationship is published")});
  }
  const RelationshipRecord& record = it->second;
  const Freshness freshness = record.freshness_at(now);
  if (freshness == Freshness::Fresh && !record.revalidation_required) {
    Explanation explanation = Explanation::success("RELATIONSHIP_CURRENT", subject);
    explanation.factors.push_back(factor("RELATIONSHIP_CURRENT",
                                         "the relationship carries current evidence"));
    return explanation;
  }
  std::vector<ExplanationFactor> factors;
  {
    ExplanationFactor entry;
    entry.code = "RELATIONSHIP_FRESHNESS";
    entry.detail = "the relationship evidence is not current";
    entry.state = std::string(to_string(freshness));
    factors.push_back(std::move(entry));
  }
  if (record.generation.is_set()) {
    factors.push_back(generation_factor("RELATIONSHIP_GENERATION",
                                        "the topology generation bound to the relationship",
                                        "topology_generation", record.generation.value(),
                                        state.generations.topology.value()));
  }
  return Explanation::failure("RELATIONSHIP_NOT_CURRENT", subject, std::move(factors));
}

Explanation RackFabric::explain_publication(const MutationResult& result) const { return result.explanation; }

// ---------------------------------------------------------------------------
// Snapshots
// ---------------------------------------------------------------------------

SnapshotResult RackFabric::publish_snapshot(const SnapshotRequest& request) {
  std::unique_lock<std::shared_mutex> lock(impl_->mutex);
  RackState& state = impl_->state;
  const Timestamp now = impl_->now();

  SnapshotResult out;
  const AuthorityCheck check = check_authority(state, request.authority, now, true);
  if (!check.ok()) {
    note_rejection(check.publisher);
    out.result = rejected(state, check.outcome, check.code, check.subject, check.factors);
    return out;
  }
  const auto next_generation = state.snapshot_generation.is_unset()
                                   ? std::optional<SnapshotGeneration>(SnapshotGeneration::first())
                                   : state.snapshot_generation.next();
  if (!next_generation.has_value()) {
    out.result = rejected(state, MutationOutcome::RejectLimitExceeded, "GENERATION_EXHAUSTED",
                          "snapshot", {factor("GENERATION_EXHAUSTED", "snapshot generation exhausted")});
    return out;
  }
  PublicationGeneration publication;
  if (check.publisher != nullptr) {
    publication = check.publisher->publication_generation;
  } else {
    publication = state.operator_publication_generation.is_unset()
                      ? PublicationGeneration::first()
                      : state.operator_publication_generation;
  }
  const std::string id_text = "snap-" + state.rack->id.value() + "-" +
                              std::to_string(next_generation->value());
  const auto snapshot_id = SnapshotId::parse(id_text);
  if (!snapshot_id.has_value()) {
    out.result = rejected(state, MutationOutcome::RejectInvalidInput, "SNAPSHOT_IDENTITY_INVALID",
                          id_text,
                          {factor("SNAPSHOT_IDENTITY_INVALID",
                                  "the derived snapshot identity is not a valid identity")});
    return out;
  }
  RackSnapshot snapshot = state.build_snapshot(*snapshot_id, *next_generation, publication, now);
  state.snapshot_generation = *next_generation;
  if (state.snapshots.size() >= impl_->options.limits.max_retained_snapshots) {
    state.snapshots.erase(state.snapshots.begin());
  }
  state.snapshots.insert_or_assign(*next_generation, snapshot);
  advance_publication(state, check.publisher);
  out.result = accepted(state, "SNAPSHOT_PUBLISHED", snapshot_id->value());
  out.snapshot = std::move(snapshot);
  return out;
}

SnapshotValidation RackFabric::validate_snapshot(const RackSnapshot& snapshot) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  const RackState& state = impl_->state;
  SnapshotValidation validation;

  // Recompute the digest so that a tampered snapshot cannot be accepted.
  if (compute_snapshot_digest(snapshot, state.limits) != snapshot.digest()) {
    validation.status = SnapshotValidationStatus::Invalid;
    validation.explanation = Explanation::failure(
        "SNAPSHOT_DIGEST_MISMATCH", snapshot.id().value(),
        {factor("SNAPSHOT_DIGEST_MISMATCH",
                "the snapshot content does not match its recorded digest")});
    return validation;
  }

  if (!state.rack.has_value() || !(state.rack->id == snapshot.rack())) {
    validation.status = SnapshotValidationStatus::UnknownRack;
    validation.explanation = Explanation::failure(
        "SNAPSHOT_UNKNOWN_RACK", snapshot.id().value(),
        {factor("SNAPSHOT_UNKNOWN_RACK",
                "the snapshot describes a rack that this instance does not describe")});
    return validation;
  }

  std::vector<ExplanationFactor> factors;
  const auto compare = [&factors](const char* name, std::uint64_t expected, std::uint64_t current) {
    if (expected != current) {
      factors.push_back(generation_factor("SNAPSHOT_STALE_GENERATION",
                                          "the snapshot binds a generation that is no longer current",
                                          name, expected, current));
    }
  };
  const GenerationSet& bound = snapshot.generations();
  compare("rack_generation", bound.rack.value(), state.generations.rack.value());
  compare("membership_generation", bound.membership.value(), state.generations.membership.value());
  compare("topology_generation", bound.topology.value(), state.generations.topology.value());
  compare("failure_domain_generation", bound.failure_domains.value(),
          state.generations.failure_domains.value());
  compare("constraint_generation", bound.constraints.value(), state.generations.constraints.value());
  compare("power_envelope_generation", bound.power.value(), state.generations.power.value());
  compare("cooling_envelope_generation", bound.cooling.value(), state.generations.cooling.value());
  compare("health_generation", bound.health.value(), state.generations.health.value());
  compare("capability_generation", bound.capabilities.value(),
          state.generations.capabilities.value());
  compare("coordinator_epoch", bound.coordinator_epoch.value(),
          state.generations.coordinator_epoch.value());
  if (!(state.rack->epoch == snapshot.rack_epoch())) {
    factors.push_back(factor("SNAPSHOT_RACK_EPOCH_CHANGED",
                             "the rack incarnation has changed since the snapshot was produced"));
  }
  if (factors.empty()) {
    validation.status = SnapshotValidationStatus::Current;
    validation.explanation = Explanation::success("SNAPSHOT_CURRENT", snapshot.id().value());
    return validation;
  }
  validation.status = SnapshotValidationStatus::Stale;
  validation.explanation =
      Explanation::failure("SNAPSHOT_STALE", snapshot.id().value(), std::move(factors));
  return validation;
}

std::optional<RackSnapshot> RackFabric::find_snapshot(const SnapshotId& id) const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  for (const auto& [generation, snapshot] : impl_->state.snapshots) {
    (void)generation;
    if (snapshot.id() == id) {
      return snapshot;
    }
  }
  return std::nullopt;
}

std::vector<SnapshotId> RackFabric::snapshot_ids() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  std::vector<SnapshotId> out;
  out.reserve(impl_->state.snapshots.size());
  for (const auto& [generation, snapshot] : impl_->state.snapshots) {
    (void)generation;
    out.push_back(snapshot.id());
  }
  return out;
}

std::size_t RackFabric::snapshot_count() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.snapshots.size();
}

InvariantReport RackFabric::check_invariants() const {
  std::shared_lock<std::shared_mutex> lock(impl_->mutex);
  return impl_->state.check_invariants(impl_->now());
}

}  // namespace rack_fabric
