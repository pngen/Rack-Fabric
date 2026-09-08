// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "core/state.hpp"

#include <algorithm>
#include <deque>
#include <set>
#include <string>
#include <utility>

#include "core/canonical.hpp"
#include "internal/crypto.hpp"

namespace rack_fabric {
namespace {

void push_deficit(ReadinessEvaluation& evaluation, std::string code, std::string detail,
                  std::size_t required, std::size_t observed) {
  if (evaluation.deficits.size() >= 64) {
    return;
  }
  evaluation.deficits.push_back(ReadinessDeficit{std::move(code), std::move(detail), required, observed});
}

[[nodiscard]] bool has_current_evidence(const MemberRecord& record, Timestamp now,
                                        const ReadinessContract& contract) {
  if (record.lifecycle != MemberLifecycle::Present) {
    return false;
  }
  if (record.freshness_at(now) != Freshness::Fresh) {
    return false;
  }
  if (contract.require_physical_provenance && !is_physical_provenance(record.provenance)) {
    return false;
  }
  return true;
}

void encode_snapshot(rack_fabric::internal::ByteWriter& writer, const RackSnapshot& snapshot) {
  using rack_fabric::internal::ByteWriter;
  writer.string(snapshot.id().value());
  writer.u64(snapshot.generation().value());
  writer.u64(snapshot.publication_generation().value());
  writer.timestamp(snapshot.created_at());
  writer.string(snapshot.rack().value());
  writer.string(snapshot.rack_epoch().value());
  rack_fabric::internal::encode_generation_set(writer, snapshot.generations());
  writer.u8(static_cast<std::uint8_t>(snapshot.lifecycle()));
  writer.count(snapshot.members().size(), writer.limits().max_members);
  for (const auto& member : snapshot.members()) {
    rack_fabric::internal::encode_member(writer, member);
  }
  writer.count(snapshot.relationships().size(), writer.limits().max_relationships);
  for (const auto& relationship : snapshot.relationships()) {
    rack_fabric::internal::encode_relationship(writer, relationship);
  }
  writer.count(snapshot.failure_domains().size(), writer.limits().max_failure_domains);
  for (const auto& domain : snapshot.failure_domains()) {
    rack_fabric::internal::encode_failure_domain(writer, domain);
  }
  writer.boolean(snapshot.power_envelope().has_value());
  if (snapshot.power_envelope().has_value()) {
    rack_fabric::internal::encode_power_envelope(writer, *snapshot.power_envelope());
  }
  writer.boolean(snapshot.cooling_envelope().has_value());
  if (snapshot.cooling_envelope().has_value()) {
    rack_fabric::internal::encode_cooling_envelope(writer, *snapshot.cooling_envelope());
  }
}

}  // namespace

struct SnapshotBuilder {
  [[nodiscard]] static bool decode(rack_fabric::internal::ByteReader& reader,
                                   std::optional<RackSnapshot>& out);

 private:
  [[nodiscard]] static bool decode_into(rack_fabric::internal::ByteReader& reader, RackSnapshot& out);
};

bool SnapshotBuilder::decode(rack_fabric::internal::ByteReader& reader,
                             std::optional<RackSnapshot>& out) {
  const std::string id = reader.string();
  const auto parsed_id = SnapshotId::parse(id);
  const SnapshotGeneration generation = SnapshotGeneration::from_value(reader.u64());
  const PublicationGeneration publication = PublicationGeneration::from_value(reader.u64());
  const Timestamp created_at = reader.timestamp();
  const std::string rack = reader.string();
  const auto parsed_rack = RackId::parse(rack);
  const std::string epoch = reader.string();
  const auto parsed_epoch = RackEpochId::parse(epoch);
  if (!reader.ok() || !parsed_id.has_value() || !parsed_rack.has_value() || !parsed_epoch.has_value()) {
    if (reader.ok()) {
      reader.fail(rack_fabric::internal::CodecError::InvalidIdentity);
    }
    return false;
  }
  RackSnapshot built(*parsed_id, *parsed_rack, *parsed_epoch);
  built.generation_ = generation;
  built.publication_generation_ = publication;
  built.created_at_ = created_at;
  if (!decode_into(reader, built)) {
    return false;
  }
  // The digest is derived, never trusted from the file: a recovered snapshot
  // must prove its own content.
  built.digest_ = compute_snapshot_digest(built, reader.limits());
  out = std::move(built);
  return true;
}

bool SnapshotBuilder::decode_into(rack_fabric::internal::ByteReader& reader, RackSnapshot& out) {
  rack_fabric::internal::decode_generation_set(reader, out.generations_);
  out.lifecycle_ = reader.enum8(RackLifecycle::Retired);
  const std::size_t member_count = reader.count(reader.limits().max_members);
  if (!reader.ok()) return false;
  out.members_.reserve(member_count);
  for (std::size_t i = 0; i < member_count; ++i) {
    MemberRecord record;
    rack_fabric::internal::decode_member(reader, record);
    if (!reader.ok()) return false;
    out.members_.push_back(std::move(record));
  }
  const std::size_t relationship_count = reader.count(reader.limits().max_relationships);
  if (!reader.ok()) return false;
  out.relationships_.reserve(relationship_count);
  for (std::size_t i = 0; i < relationship_count; ++i) {
    RelationshipRecord record;
    rack_fabric::internal::decode_relationship(reader, record);
    if (!reader.ok()) return false;
    out.relationships_.push_back(std::move(record));
  }
  const std::size_t domain_count = reader.count(reader.limits().max_failure_domains);
  if (!reader.ok()) return false;
  out.failure_domains_.reserve(domain_count);
  for (std::size_t i = 0; i < domain_count; ++i) {
    FailureDomainRecord record;
    rack_fabric::internal::decode_failure_domain(reader, record);
    if (!reader.ok()) return false;
    out.failure_domains_.push_back(std::move(record));
  }
  if (reader.boolean()) {
    PowerEnvelopeRecord envelope;
    rack_fabric::internal::decode_power_envelope(reader, envelope);
    if (!reader.ok()) return false;
    out.power_envelope_ = std::move(envelope);
  }
  if (reader.boolean()) {
    CoolingEnvelopeRecord envelope;
    rack_fabric::internal::decode_cooling_envelope(reader, envelope);
    if (!reader.ok()) return false;
    out.cooling_envelope_ = std::move(envelope);
  }
  return reader.ok();
}

bool member_has_ephemeral_evidence(const MemberRecord& record) noexcept {
  if (record.durability == Durability::Ephemeral) {
    return true;
  }
  if (record.health.durability == Durability::Ephemeral && record.health.has_evidence()) {
    return true;
  }
  if (record.readiness.durability == Durability::Ephemeral && record.readiness.has_evidence()) {
    return true;
  }
  if (record.reachability.durability == Durability::Ephemeral && record.reachability.has_evidence()) {
    return true;
  }
  for (const auto& capability : record.capabilities) {
    if (capability.durability == Durability::Ephemeral) {
      return true;
    }
  }
  return false;
}

void RackState::rebuild_indexes() {
  relationships_by_member.clear();
  members_by_kind.clear();
  members_by_failure_domain.clear();
  members_by_power_domain.clear();
  members_by_cooling_domain.clear();
  members_by_owner_boot.clear();
  children_by_parent.clear();
  children_by_domain.clear();

  for (const auto& [key, record] : members) {
    index_add_member(record);
    (void)key;
  }
  for (const auto& [key, record] : relationships) {
    index_add_relationship(record);
    (void)key;
  }
  for (const auto& [id, record] : failure_domains) {
    index_add_failure_domain(record);
    (void)id;
  }
}

void RackState::index_add_member(const MemberRecord& record) {
  members_by_kind[record.key.kind].insert(record.key);
  for (const auto& domain : record.failure_domains) {
    members_by_failure_domain[domain].insert(record.key);
  }
  if (record.power_domain.has_value()) {
    members_by_power_domain[*record.power_domain].insert(record.key);
  }
  if (record.cooling_domain.has_value()) {
    members_by_cooling_domain[*record.cooling_domain].insert(record.key);
  }
  if (record.owner_boot.has_value()) {
    members_by_owner_boot[*record.owner_boot].insert(record.key);
  }
  if (record.parent.has_value()) {
    children_by_parent[*record.parent].insert(record.key);
  }
}

void RackState::index_remove_member(const MemberRecord& record) {
  const auto erase_from = [&record](auto& index, const auto& key) {
    const auto it = index.find(key);
    if (it == index.end()) {
      return;
    }
    it->second.erase(record.key);
    if (it->second.empty()) {
      index.erase(it);
    }
  };
  erase_from(members_by_kind, record.key.kind);
  for (const auto& domain : record.failure_domains) {
    erase_from(members_by_failure_domain, domain);
  }
  if (record.power_domain.has_value()) {
    erase_from(members_by_power_domain, *record.power_domain);
  }
  if (record.cooling_domain.has_value()) {
    erase_from(members_by_cooling_domain, *record.cooling_domain);
  }
  if (record.owner_boot.has_value()) {
    erase_from(members_by_owner_boot, *record.owner_boot);
  }
  if (record.parent.has_value()) {
    erase_from(children_by_parent, *record.parent);
  }
}

void RackState::index_add_relationship(const RelationshipRecord& record) {
  relationships_by_member[record.key.from].insert(record.key);
  relationships_by_member[record.key.to].insert(record.key);
}

void RackState::index_remove_relationship(const RelationshipRecord& record) {
  const auto erase_from = [this, &record](const MemberKey& key) {
    const auto it = relationships_by_member.find(key);
    if (it == relationships_by_member.end()) {
      return;
    }
    it->second.erase(record.key);
    if (it->second.empty()) {
      relationships_by_member.erase(it);
    }
  };
  erase_from(record.key.from);
  if (!(record.key.to == record.key.from)) {
    erase_from(record.key.to);
  }
}

void RackState::index_add_failure_domain(const FailureDomainRecord& record) {
  for (const auto& parent : record.parents) {
    children_by_domain[parent].insert(record.id);
  }
  members_by_failure_domain.try_emplace(record.id);
}

void RackState::index_remove_failure_domain(const FailureDomainRecord& record) {
  for (const auto& parent : record.parents) {
    const auto it = children_by_domain.find(parent);
    if (it == children_by_domain.end()) {
      continue;
    }
    it->second.erase(record.id);
    if (it->second.empty()) {
      children_by_domain.erase(it);
    }
  }
  members_by_failure_domain.erase(record.id);
}

std::vector<FailureDomainId> RackState::failure_domains_of(const MemberKey& key) const {
  std::set<FailureDomainId> result;
  const auto member_it = members.find(key);
  if (member_it == members.end()) {
    return {};
  }
  std::deque<FailureDomainId> pending(member_it->second.failure_domains.begin(),
                                      member_it->second.failure_domains.end());
  while (!pending.empty()) {
    const FailureDomainId current = pending.front();
    pending.pop_front();
    if (!result.insert(current).second) {
      continue;
    }
    const auto domain_it = failure_domains.find(current);
    if (domain_it == failure_domains.end()) {
      continue;
    }
    for (const auto& parent : domain_it->second.parents) {
      pending.push_back(parent);
    }
  }
  return {result.begin(), result.end()};
}

std::vector<MemberKey> RackState::members_in_failure_domain(const FailureDomainId& id,
                                                            bool include_nested) const {
  std::set<MemberKey> result;
  const auto direct = members_by_failure_domain.find(id);
  if (direct != members_by_failure_domain.end()) {
    result.insert(direct->second.begin(), direct->second.end());
  }
  if (include_nested) {
    std::deque<FailureDomainId> pending{id};
    std::set<FailureDomainId> visited{id};
    while (!pending.empty()) {
      const FailureDomainId current = pending.front();
      pending.pop_front();
      const auto children = children_by_domain.find(current);
      if (children == children_by_domain.end()) {
        continue;
      }
      for (const auto& child : children->second) {
        if (!visited.insert(child).second) {
          continue;
        }
        pending.push_back(child);
        const auto members_it = members_by_failure_domain.find(child);
        if (members_it != members_by_failure_domain.end()) {
          result.insert(members_it->second.begin(), members_it->second.end());
        }
      }
    }
  }
  return {result.begin(), result.end()};
}

std::vector<MemberKey> RackState::members_affected_by(const FailureDomainId& id) const {
  return members_in_failure_domain(id, true);
}

bool RackState::shares_failure_domain(const MemberKey& lhs, const MemberKey& rhs) const {
  const auto left = failure_domains_of(lhs);
  if (left.empty()) {
    return false;
  }
  const auto right = failure_domains_of(rhs);
  if (right.empty()) {
    return false;
  }
  for (const auto& domain : left) {
    if (std::binary_search(right.begin(), right.end(), domain)) {
      return true;
    }
  }
  return false;
}

bool RackState::contains_cycle(const MemberKey& parent, const MemberKey& child) const {
  if (parent == child) {
    return true;
  }
  std::set<MemberKey> visited;
  MemberKey current = parent;
  while (true) {
    if (!visited.insert(current).second) {
      // Pre-existing cycle in the parent chain; refuse the new edge.
      return true;
    }
    if (current == child) {
      return true;
    }
    const auto it = members.find(current);
    if (it == members.end() || !it->second.parent.has_value()) {
      return false;
    }
    current = *it->second.parent;
  }
}

bool RackState::relationship_contains_cycle(const MemberKey& parent,
                                            const MemberKey& child) const {
  if (parent == child) {
    return true;
  }
  std::set<MemberKey> visited;
  std::deque<MemberKey> pending{parent};
  while (!pending.empty()) {
    const MemberKey current = pending.front();
    pending.pop_front();
    if (current == child) {
      return true;
    }
    if (!visited.insert(current).second) {
      continue;
    }
    const auto touching = relationships_by_member.find(current);
    if (touching == relationships_by_member.end()) {
      continue;
    }
    for (const RelationshipKey& key : touching->second) {
      if (key.cls == RelationshipClass::Contains && key.to == current) {
        pending.push_back(key.from);
      }
    }
  }
  return false;
}

bool RackState::domain_contains_cycle(const FailureDomainId& parent, const FailureDomainId& child) const {
  if (parent == child) {
    return true;
  }
  std::set<FailureDomainId> visited;
  std::deque<FailureDomainId> pending{parent};
  while (!pending.empty()) {
    const FailureDomainId current = pending.front();
    pending.pop_front();
    if (!visited.insert(current).second) {
      continue;
    }
    if (current == child) {
      return true;
    }
    const auto it = failure_domains.find(current);
    if (it == failure_domains.end()) {
      continue;
    }
    for (const auto& next : it->second.parents) {
      pending.push_back(next);
    }
  }
  return false;
}

std::vector<MemberKey> RackState::descendants(const MemberKey& parent) const {
  std::vector<MemberKey> result;
  std::deque<MemberKey> pending{parent};
  std::set<MemberKey> visited{parent};
  while (!pending.empty()) {
    const MemberKey current = pending.front();
    pending.pop_front();
    const auto it = children_by_parent.find(current);
    if (it == children_by_parent.end()) {
      continue;
    }
    for (const auto& child : it->second) {
      if (!visited.insert(child).second) {
        continue;
      }
      result.push_back(child);
      pending.push_back(child);
    }
  }
  std::sort(result.begin(), result.end());
  return result;
}

ReadinessEvaluation RackState::evaluate_readiness(Timestamp now) const {
  ReadinessEvaluation evaluation;
  if (!rack.has_value()) {
    push_deficit(evaluation, "RACK_NOT_DECLARED", "no rack identity has been declared", 1, 0);
    return evaluation;
  }
  if (rack->retired) {
    push_deficit(evaluation, "RACK_RETIRED", "the rack has been retired", 0, 0);
    return evaluation;
  }

  std::set<FailureDomainId> covered_domains;
  for (const auto& [key, record] : members) {
    if (!has_current_evidence(record, now, contract)) {
      continue;
    }
    if (key.kind == MemberKind::Node) {
      ++evaluation.qualifying_nodes;
      for (const auto& domain : failure_domains_of(key)) {
        covered_domains.insert(domain);
      }
    } else if (key.kind == MemberKind::Accelerator) {
      ++evaluation.qualifying_accelerators;
    }
  }
  evaluation.qualifying_failure_domains = covered_domains.size();

  if (evaluation.qualifying_nodes < contract.min_nodes) {
    push_deficit(evaluation, "INSUFFICIENT_NODES",
                 "fewer nodes are PRESENT with current evidence than the contract requires",
                 contract.min_nodes, evaluation.qualifying_nodes);
  }
  if (evaluation.qualifying_accelerators < contract.min_accelerators) {
    push_deficit(evaluation, "INSUFFICIENT_ACCELERATORS",
                 "fewer accelerators are PRESENT with current evidence than the contract requires",
                 contract.min_accelerators, evaluation.qualifying_accelerators);
  }
  if (evaluation.qualifying_failure_domains < contract.min_distinct_failure_domains) {
    push_deficit(evaluation, "INSUFFICIENT_FAILURE_DOMAINS",
                 "fewer distinct failure domains are covered than the contract requires",
                 contract.min_distinct_failure_domains, evaluation.qualifying_failure_domains);
  }

  if (contract.require_all_members_current) {
    std::size_t not_current = 0;
    for (const auto& [key, record] : members) {
      (void)key;
      if (is_current_member(record.lifecycle) && record.freshness_at(now) != Freshness::Fresh) {
        ++not_current;
      }
    }
    if (not_current > 0) {
      push_deficit(evaluation, "MEMBERS_NOT_CURRENT",
                   "members of the current composition do not carry current evidence", 0, not_current);
    }
  }

  if (contract.require_member_health) {
    std::size_t missing = 0;
    for (const auto& [key, record] : members) {
      (void)key;
      if (!is_current_member(record.lifecycle)) {
        continue;
      }
      if (!record.health.has_evidence() ||
          record.health.freshness_at(now) != Freshness::Fresh) {
        ++missing;
      }
    }
    if (missing > 0) {
      push_deficit(evaluation, "MEMBER_HEALTH_UNKNOWN",
                   "members of the current composition lack current health evidence", 0, missing);
    }
  }

  if (contract.require_topology && relationships.empty()) {
    push_deficit(evaluation, "TOPOLOGY_REQUIRED", "no topology relationship has been published", 1, 0);
  }
  if (contract.require_failure_domains && failure_domains.empty()) {
    push_deficit(evaluation, "FAILURE_DOMAINS_REQUIRED", "no failure domain has been published", 1, 0);
  }
  if (contract.require_power_envelope) {
    if (!power.has_value()) {
      push_deficit(evaluation, "POWER_ENVELOPE_REQUIRED", "no power envelope has been published", 1, 0);
    } else if (power->freshness_at(now) != Freshness::Fresh) {
      push_deficit(evaluation, "POWER_ENVELOPE_NOT_CURRENT",
                   "the power envelope is not current", 1, 0);
    }
  }
  if (contract.require_cooling_envelope) {
    if (!cooling.has_value()) {
      push_deficit(evaluation, "COOLING_ENVELOPE_REQUIRED",
                   "no cooling envelope has been published", 1, 0);
    } else if (cooling->freshness_at(now) != Freshness::Fresh) {
      push_deficit(evaluation, "COOLING_ENVELOPE_NOT_CURRENT",
                   "the cooling envelope is not current", 1, 0);
    }
  }
  if (contract.require_active_publisher) {
    const bool any_active = std::any_of(publishers.begin(), publishers.end(), [](const auto& entry) {
      return entry.second.state == PublisherState::Active;
    });
    if (!any_active) {
      push_deficit(evaluation, "NO_ACTIVE_PUBLISHER", "no publisher is currently active", 1, 0);
    }
  }

  evaluation.satisfied = evaluation.deficits.empty();
  return evaluation;
}

RackLifecycle RackState::derive_lifecycle(Timestamp now) const {
  if (!rack.has_value()) {
    return RackLifecycle::Undeclared;
  }
  if (rack->retired) {
    return RackLifecycle::Retired;
  }
  const bool needs_revalidation =
      std::any_of(members.begin(), members.end(),
                  [](const auto& entry) { return entry.second.revalidation_required; }) ||
      std::any_of(relationships.begin(), relationships.end(),
                  [](const auto& entry) { return entry.second.revalidation_required; }) ||
      std::any_of(failure_domains.begin(), failure_domains.end(),
                  [](const auto& entry) { return entry.second.revalidation_required; });
  if (needs_revalidation) {
    return RackLifecycle::RevalidationRequired;
  }

  const ReadinessEvaluation evaluation = evaluate_readiness(now);
  if (evaluation.satisfied) {
    const bool degraded = std::any_of(members.begin(), members.end(), [now](const auto& entry) {
      const MemberRecord& record = entry.second;
      if (!is_current_member(record.lifecycle)) {
        return false;
      }
      if (record.lifecycle == MemberLifecycle::Unavailable) {
        return true;
      }
      if (record.health.has_evidence() && record.health.is_current_at(now) &&
          !is_positive_health(record.health.value)) {
        return true;
      }
      if (record.readiness.has_evidence() && record.readiness.is_current_at(now) &&
          record.readiness.value == ReadinessState::NotReady) {
        return true;
      }
      return false;
    });
    return degraded ? RackLifecycle::Degraded : RackLifecycle::Ready;
  }

  if (members.empty()) {
    return RackLifecycle::Declared;
  }
  const bool any_present = std::any_of(members.begin(), members.end(), [](const auto& entry) {
    return is_current_member(entry.second.lifecycle);
  });
  return any_present ? RackLifecycle::Partial : RackLifecycle::Discovering;
}

void RackState::recompute_power_derived(Timestamp now) {
  if (!power.has_value()) {
    return;
  }
  PowerEnvelopeRecord& envelope = *power;
  envelope.rack_headroom.reset();
  if (envelope.rack_limit.has_value() && envelope.rack_observed_draw.has_value() &&
      envelope.rack_limit->unit == envelope.rack_observed_draw->unit &&
      envelope.rack_limit->is_current_at(now) && envelope.rack_observed_draw->is_current_at(now)) {
    Quantity headroom;
    headroom.value = envelope.rack_limit->value - envelope.rack_observed_draw->value;
    headroom.unit = envelope.rack_limit->unit;
    headroom.provenance = EvidenceProvenance::Derived;
    headroom.observed_at = std::max(envelope.rack_limit->observed_at, envelope.rack_observed_draw->observed_at);
    headroom.ttl = std::min(envelope.rack_limit->ttl, envelope.rack_observed_draw->ttl);
    headroom.durability = Durability::Ephemeral;
    if (headroom.value >= 0.0) {
      envelope.rack_headroom = headroom;
    }
  }
  for (auto& budget : envelope.domain_budgets) {
    budget.headroom.reset();
    if (budget.limit.has_value() && budget.observed_draw.has_value() &&
        budget.limit->unit == budget.observed_draw->unit && budget.limit->is_current_at(now) &&
        budget.observed_draw->is_current_at(now)) {
      Quantity headroom;
      headroom.value = budget.limit->value - budget.observed_draw->value;
      headroom.unit = budget.limit->unit;
      headroom.provenance = EvidenceProvenance::Derived;
      headroom.observed_at = std::max(budget.limit->observed_at, budget.observed_draw->observed_at);
      headroom.ttl = std::min(budget.limit->ttl, budget.observed_draw->ttl);
      headroom.durability = Durability::Ephemeral;
      if (headroom.value >= 0.0) {
        budget.headroom = headroom;
      }
    }
  }
}

void RackState::recompute_cooling_derived(Timestamp now) {
  if (!cooling.has_value()) {
    return;
  }
  for (auto& zone : cooling->zones) {
    zone.thermal_headroom.reset();
    if (zone.cooling_capacity.has_value() && zone.design_thermal_limit.has_value() &&
        zone.cooling_capacity->unit == zone.design_thermal_limit->unit &&
        zone.cooling_capacity->is_current_at(now) && zone.design_thermal_limit->is_current_at(now)) {
      Quantity headroom;
      headroom.value = zone.cooling_capacity->value - zone.design_thermal_limit->value;
      headroom.unit = zone.cooling_capacity->unit;
      headroom.provenance = EvidenceProvenance::Derived;
      headroom.observed_at =
          std::max(zone.cooling_capacity->observed_at, zone.design_thermal_limit->observed_at);
      headroom.ttl = std::min(zone.cooling_capacity->ttl, zone.design_thermal_limit->ttl);
      headroom.durability = Durability::Ephemeral;
      if (headroom.value >= 0.0) {
        zone.thermal_headroom = headroom;
      }
    }
  }
}

RackSnapshot RackState::build_snapshot(const SnapshotId& id, SnapshotGeneration generation,
                                       PublicationGeneration publication, Timestamp now) const {
  RackSnapshot snapshot(id, rack->id, rack->epoch);
  snapshot.generation_ = generation;
  snapshot.publication_generation_ = publication;
  snapshot.created_at_ = now;
  snapshot.generations_ = generations;
  snapshot.generations_.coordinator_epoch = generations.coordinator_epoch;
  snapshot.lifecycle_ = derive_lifecycle(now);
  snapshot.members_.reserve(members.size());
  for (const auto& [key, record] : members) {
    (void)key;
    snapshot.members_.push_back(record);
  }
  snapshot.relationships_.reserve(relationships.size());
  for (const auto& [key, record] : relationships) {
    (void)key;
    snapshot.relationships_.push_back(record);
  }
  snapshot.failure_domains_.reserve(failure_domains.size());
  for (const auto& [key, record] : failure_domains) {
    (void)key;
    snapshot.failure_domains_.push_back(record);
  }
  snapshot.power_envelope_ = power;
  snapshot.cooling_envelope_ = cooling;
  snapshot.digest_ = compute_snapshot_digest(snapshot, limits);
  return snapshot;
}

std::string compute_snapshot_digest(const RackSnapshot& snapshot, const ResourceLimits& limits) {
  internal::ByteWriter writer(limits);
  encode_snapshot(writer, snapshot);
  const auto& bytes = writer.bytes();
  return internal::Sha256::hash(bytes.data(), bytes.size());
}

std::string RackSnapshot::render() const {
  std::string out;
  out.append("snapshot ");
  out.append(id_.value());
  out.append("\n  generation: ");
  out.append(std::to_string(generation_.value()));
  out.append("\n  publication_generation: ");
  out.append(std::to_string(publication_generation_.value()));
  out.append("\n  created_at_unix_millis: ");
  out.append(std::to_string(created_at_.millis()));
  out.append("\n  rack: ");
  out.append(rack_.value());
  out.append("\n  rack_epoch: ");
  out.append(rack_epoch_.value());
  out.append("\n  lifecycle: ");
  out.append(to_string(lifecycle_));
  out.append("\n  coordinator_epoch: ");
  out.append(std::to_string(generations_.coordinator_epoch.value()));
  out.append("\n  rack_generation: ");
  out.append(std::to_string(generations_.rack.value()));
  out.append("\n  membership_generation: ");
  out.append(std::to_string(generations_.membership.value()));
  out.append("\n  topology_generation: ");
  out.append(std::to_string(generations_.topology.value()));
  out.append("\n  failure_domain_generation: ");
  out.append(std::to_string(generations_.failure_domains.value()));
  out.append("\n  power_envelope_generation: ");
  out.append(std::to_string(generations_.power.value()));
  out.append("\n  cooling_envelope_generation: ");
  out.append(std::to_string(generations_.cooling.value()));
  out.append("\n  members: ");
  out.append(std::to_string(members_.size()));
  for (const MemberRecord& record : members_) {
    out.append("\n    ");
    out.append(record.key.to_string());
    out.append(" lifecycle=");
    out.append(to_string(record.lifecycle));
    out.append(" provenance=");
    out.append(to_string(record.provenance));
    out.append(" generation=");
    out.append(std::to_string(record.generation.value()));
  }
  out.append("\n  relationships: ");
  out.append(std::to_string(relationships_.size()));
  for (const RelationshipRecord& record : relationships_) {
    out.append("\n    ");
    out.append(record.key.to_string());
    out.append(" provenance=");
    out.append(to_string(record.provenance));
  }
  out.append("\n  failure_domains: ");
  out.append(std::to_string(failure_domains_.size()));
  for (const FailureDomainRecord& record : failure_domains_) {
    out.append("\n    ");
    out.append(record.id.value());
    out.append(" kind=");
    out.append(to_string(record.kind));
  }
  out.append("\n  digest: ");
  out.append(digest_);
  return out;
}

InvariantReport RackState::check_invariants(Timestamp now) const {
  InvariantReport report;
  const auto fail = [&report](std::string code, std::string detail) {
    report.violations.push_back(InvariantViolation{std::move(code), std::move(detail)});
  };

  ++report.checks_run;  // rack record uniqueness (structural)
  if (rack.has_value() && rack->generation.is_unset()) {
    fail("RACK_GENERATION_UNSET", "the rack record carries no rack generation");
  }
  ++report.checks_run;  // member identity and generation
  for (const auto& [key, record] : members) {
    if (!(key == record.key)) {
      fail("MEMBER_KEY_MISMATCH", "member map key does not match the record key");
    }
    if (record.generation.is_unset()) {
      fail("MEMBER_GENERATION_UNSET", "member " + key.to_string() + " has no generation");
    }
    if (record.lifecycle == MemberLifecycle::Retired && record.revalidation_required == false &&
        record.is_current_at(now)) {
      fail("RETIRED_MEMBER_CURRENT", "retired member " + key.to_string() + " is still current");
    }
    if (members.size() > limits.max_members) {
      fail("MEMBER_LIMIT_EXCEEDED", "member count exceeds the configured limit");
      break;
    }
    if (record.parent.has_value()) {
      if (*record.parent == key) {
        fail("MEMBER_SELF_PARENT", "member " + key.to_string() + " is its own parent");
      }
      if (members.find(*record.parent) == members.end()) {
        fail("MEMBER_PARENT_MISSING",
             "member " + key.to_string() + " references a parent that does not exist");
      }
    }
    if (record.failure_domains.size() > limits.max_failure_domains_per_member) {
      fail("MEMBER_FAILURE_DOMAIN_LIMIT", "member " + key.to_string() + " exceeds the failure-domain limit");
    }
    if (record.capabilities.size() > limits.max_capabilities_per_member) {
      fail("MEMBER_CAPABILITY_LIMIT", "member " + key.to_string() + " exceeds the capability limit");
    }
    for (const auto& domain : record.failure_domains) {
      if (failure_domains.find(domain) == failure_domains.end()) {
        fail("MEMBER_FAILURE_DOMAIN_MISSING",
             "member " + key.to_string() + " references failure domain " + domain.value() +
                 " which does not exist");
      }
    }
  }

  ++report.checks_run;  // acyclic containment
  for (const auto& [key, record] : members) {
    if (!record.parent.has_value()) {
      continue;
    }
    std::set<MemberKey> seen{key};
    MemberKey current = *record.parent;
    while (true) {
      if (!seen.insert(current).second) {
        fail("CONTAINMENT_CYCLE", "containment cycle detected at " + key.to_string());
        break;
      }
      const auto it = members.find(current);
      if (it == members.end() || !it->second.parent.has_value()) {
        break;
      }
      current = *it->second.parent;
    }
  }

  ++report.checks_run;  // relationship endpoints
  for (const auto& [key, record] : relationships) {
    if (key.from == key.to) {
      fail("RELATIONSHIP_SELF_LINK", "relationship " + key.to_string() + " is a self link");
    }
    if (members.find(key.from) == members.end()) {
      fail("RELATIONSHIP_ENDPOINT_MISSING",
           "relationship " + key.to_string() + " references a missing source member");
    }
    if (members.find(key.to) == members.end()) {
      fail("RELATIONSHIP_ENDPOINT_MISSING",
           "relationship " + key.to_string() + " references a missing destination member");
    }
    if (record.generation.is_unset()) {
      fail("RELATIONSHIP_GENERATION_UNSET",
           "relationship " + key.to_string() + " has no topology generation");
    }
    if (record.generation > generations.topology) {
      fail("RELATIONSHIP_GENERATION_AHEAD",
           "relationship " + key.to_string() + " carries a generation ahead of the topology generation");
    }
  }

  ++report.checks_run;  // failure domain graph
  for (const auto& [id, record] : failure_domains) {
    if (!(id == record.id)) {
      fail("FAILURE_DOMAIN_KEY_MISMATCH", "failure domain map key does not match the record");
    }
    for (const auto& parent : record.parents) {
      if (failure_domains.find(parent) == failure_domains.end()) {
        fail("FAILURE_DOMAIN_PARENT_MISSING",
             "failure domain " + id.value() + " references a missing parent");
      }
      if (parent == id) {
        fail("FAILURE_DOMAIN_SELF_PARENT", "failure domain " + id.value() + " is its own parent");
      }
    }
    if (record.generation > generations.failure_domains) {
      fail("FAILURE_DOMAIN_GENERATION_AHEAD",
           "failure domain " + id.value() + " carries a generation ahead of the model generation");
    }
  }
  for (const auto& [id, record] : failure_domains) {
    std::set<FailureDomainId> seen{id};
    std::deque<FailureDomainId> pending(record.parents.begin(), record.parents.end());
    while (!pending.empty()) {
      const FailureDomainId current = pending.front();
      pending.pop_front();
      if (!seen.insert(current).second) {
        fail("FAILURE_DOMAIN_CYCLE", "failure domain cycle detected at " + id.value());
        break;
      }
      const auto it = failure_domains.find(current);
      if (it == failure_domains.end()) {
        continue;
      }
      for (const auto& parent : it->second.parents) {
        pending.push_back(parent);
      }
    }
  }

  ++report.checks_run;  // publisher fencing
  for (const auto& [boot, publisher] : publishers) {
    if (fenced_boots.find(boot) != fenced_boots.end() &&
        (publisher.state == PublisherState::Active || publisher.state == PublisherState::Registered)) {
      fail("FENCED_PUBLISHER_ACTIVE",
           "publisher " + boot.value() + " is fenced but still holds authority");
    }
    if (publisher.state == PublisherState::Active && publisher.coordinator_epoch != generations.coordinator_epoch) {
      fail("PUBLISHER_STALE_EPOCH",
           "active publisher " + boot.value() + " carries a stale coordinator epoch");
    }
  }

  ++report.checks_run;  // rack retirement
  if (rack.has_value() && rack->retired) {
    for (const auto& [key, record] : members) {
      if (record.lifecycle == MemberLifecycle::Present) {
        fail("RETIRED_RACK_HAS_PRESENT_MEMBER",
             "retired rack still contains present member " + key.to_string());
        break;
      }
    }
  }

  ++report.checks_run;  // envelope derivation
  if (power.has_value()) {
    const PowerEnvelopeRecord& envelope = *power;
    if (envelope.rack_headroom.has_value() &&
        !(envelope.rack_limit.has_value() && envelope.rack_observed_draw.has_value())) {
      fail("POWER_HEADROOM_UNSUPPORTED",
           "power headroom is present without a limit and a draw");
    }
    if (envelope.generation.is_unset()) {
      fail("POWER_GENERATION_UNSET", "the power envelope has no generation");
    }
    if (envelope.generation > generations.power) {
      fail("POWER_GENERATION_AHEAD", "the power envelope generation is ahead of the rack generation");
    }
  }
  if (cooling.has_value()) {
    if (cooling->generation.is_unset()) {
      fail("COOLING_GENERATION_UNSET", "the cooling envelope has no generation");
    }
    if (cooling->generation > generations.cooling) {
      fail("COOLING_GENERATION_AHEAD", "the cooling envelope generation is ahead of the rack generation");
    }
    for (const auto& zone : cooling->zones) {
      if (zone.thermal_headroom.has_value() &&
          !(zone.cooling_capacity.has_value() && zone.design_thermal_limit.has_value())) {
        fail("COOLING_HEADROOM_UNSUPPORTED",
             "thermal headroom is present for zone " + zone.zone.value() + " without inputs");
      }
    }
  }

  ++report.checks_run;  // snapshot binding
  for (const auto& [generation, snapshot] : snapshots) {
    if (!(generation == snapshot.generation())) {
      fail("SNAPSHOT_GENERATION_MISMATCH", "snapshot map key does not match the snapshot generation");
    }
    if (snapshot.generations().rack > generations.rack) {
      fail("SNAPSHOT_RACK_GENERATION_AHEAD", "snapshot binds a rack generation ahead of the current one");
    }
    if (snapshot.generations().membership > generations.membership) {
      fail("SNAPSHOT_MEMBERSHIP_AHEAD",
           "snapshot binds a membership generation ahead of the current one");
    }
  }

  ++report.checks_run;  // index consistency
  RackState copy;
  copy.limits = limits;
  copy.members = members;
  copy.relationships = relationships;
  copy.failure_domains = failure_domains;
  copy.rebuild_indexes();
  const auto compare = [&fail](const auto& lhs, const auto& rhs, const char* name) {
    if (lhs != rhs) {
      fail("INDEX_MISMATCH", std::string("derived index does not match canonical records: ") + name);
    }
  };
  compare(relationships_by_member, copy.relationships_by_member, "relationships_by_member");
  compare(members_by_kind, copy.members_by_kind, "members_by_kind");
  compare(members_by_failure_domain, copy.members_by_failure_domain, "members_by_failure_domain");
  compare(members_by_power_domain, copy.members_by_power_domain, "members_by_power_domain");
  compare(members_by_cooling_domain, copy.members_by_cooling_domain, "members_by_cooling_domain");
  compare(members_by_owner_boot, copy.members_by_owner_boot, "members_by_owner_boot");
  compare(children_by_parent, copy.children_by_parent, "children_by_parent");
  compare(children_by_domain, copy.children_by_domain, "children_by_domain");

  return report;
}

std::optional<std::vector<std::byte>> encode_state_payload(const RackState& state,
                                                           std::string* failure) {
  // Persisted state is bounded by the persistence limit, never by the protocol
  // frame payload limit, which exists to bound a single wire frame.
  const std::uint64_t persistence_bound = state.limits.max_persisted_bytes;
  const std::size_t capacity =
      persistence_bound > std::numeric_limits<std::size_t>::max()
          ? std::numeric_limits<std::size_t>::max()
          : static_cast<std::size_t>(persistence_bound);
  internal::ByteWriter writer(state.limits, capacity);
  writer.u8(state.rack.has_value() ? 1U : 0U);
  if (state.rack.has_value()) {
    writer.string(state.rack->id.value());
    writer.string(state.rack->epoch.value());
    writer.u64(state.rack->generation.value());
    writer.boolean(state.rack->retired);
    writer.optional_string(state.rack->label);
    writer.timestamp(state.rack->declared_at);
  }
  internal::encode_generation_set(writer, state.generations);
  writer.u64(state.snapshot_generation.value());
  writer.u64(state.operator_publication_generation.value());

  writer.count(state.members.size(), state.limits.max_members);
  for (const auto& [key, record] : state.members) {
    (void)key;
    internal::encode_member(writer, record);
  }
  writer.count(state.relationships.size(), state.limits.max_relationships);
  for (const auto& [key, record] : state.relationships) {
    (void)key;
    internal::encode_relationship(writer, record);
  }
  writer.count(state.failure_domains.size(), state.limits.max_failure_domains);
  for (const auto& [key, record] : state.failure_domains) {
    (void)key;
    internal::encode_failure_domain(writer, record);
  }
  writer.boolean(state.power.has_value());
  if (state.power.has_value()) {
    internal::encode_power_envelope(writer, *state.power);
  }
  writer.boolean(state.cooling.has_value());
  if (state.cooling.has_value()) {
    internal::encode_cooling_envelope(writer, *state.cooling);
  }
  writer.count(state.publishers.size(), state.limits.max_members);
  for (const auto& [boot, publisher] : state.publishers) {
    (void)boot;
    writer.string(publisher.worker.value());
    writer.string(publisher.boot.value());
    writer.u64(publisher.coordinator_epoch.value());
    writer.u64(publisher.publication_generation.value());
    writer.u8(static_cast<std::uint8_t>(publisher.state));
    writer.timestamp(publisher.registered_at);
    writer.timestamp(publisher.last_seen_at);
    writer.i64(publisher.lease_millis);
    writer.optional_string(publisher.label);
    writer.u64(publisher.accepted_mutations);
    writer.u64(publisher.rejected_mutations);
  }
  writer.count(state.fenced_boots.size(), state.limits.max_fenced_boots);
  for (const auto& [boot, fenced] : state.fenced_boots) {
    (void)boot;
    writer.string(fenced.boot.value());
    writer.optional_string(fenced.worker.has_value() ? std::optional<std::string>(fenced.worker->value())
                                                     : std::nullopt);
    writer.u64(fenced.fenced_under_epoch.value());
    writer.timestamp(fenced.fenced_at);
    writer.u8(static_cast<std::uint8_t>(fenced.reason));
  }
  writer.count(state.snapshots.size(), state.limits.max_retained_snapshots);
  for (const auto& [generation, snapshot] : state.snapshots) {
    (void)generation;
    encode_snapshot(writer, snapshot);
  }
  if (!writer.ok()) {
    if (failure != nullptr) {
      *failure = std::string(internal::to_string(writer.error()));
    }
    return std::nullopt;
  }
  return std::move(writer).take();
}

bool decode_state_payload(const std::byte* data, std::size_t size, RackState& out, std::string& error) {
  internal::ByteReader reader(data, size, out.limits);
  if (reader.u8() == 1U) {
    RackRecord record{*RackId::parse("unset"), *RackEpochId::parse("unset")};
    const std::string id = reader.string();
    const std::string epoch = reader.string();
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    const auto parsed_id = RackId::parse(id);
    const auto parsed_epoch = RackEpochId::parse(epoch);
    if (!parsed_id.has_value() || !parsed_epoch.has_value()) {
      error = "INVALID_IDENTITY";
      return false;
    }
    record.id = *parsed_id;
    record.epoch = *parsed_epoch;
    record.generation = RackGeneration::from_value(reader.u64());
    record.retired = reader.boolean();
    record.label = reader.optional_string();
    record.declared_at = reader.timestamp();
    out.rack = std::move(record);
  }
  internal::decode_generation_set(reader, out.generations);
  out.snapshot_generation = SnapshotGeneration::from_value(reader.u64());
  out.operator_publication_generation = PublicationGeneration::from_value(reader.u64());

  const std::size_t member_count = reader.count(out.limits.max_members);
  if (!reader.ok()) {
    error = std::string(internal::to_string(reader.error()));
    return false;
  }
  if (member_count > out.limits.max_members) {
    error = "MEMBER_LIMIT_EXCEEDED";
    return false;
  }
  for (std::size_t i = 0; i < member_count; ++i) {
    MemberRecord record;
    internal::decode_member(reader, record);
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    const auto [it, inserted] = out.members.emplace(record.key, std::move(record));
    (void)it;
    if (!inserted) {
      error = "DUPLICATE_MEMBER_IDENTITY";
      return false;
    }
  }

  const std::size_t relationship_count = reader.count(out.limits.max_relationships);
  if (!reader.ok()) {
    error = std::string(internal::to_string(reader.error()));
    return false;
  }
  if (relationship_count > out.limits.max_relationships) {
    error = "RELATIONSHIP_LIMIT_EXCEEDED";
    return false;
  }
  for (std::size_t i = 0; i < relationship_count; ++i) {
    RelationshipRecord record;
    internal::decode_relationship(reader, record);
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    const auto [it, inserted] = out.relationships.emplace(record.key, std::move(record));
    (void)it;
    if (!inserted) {
      error = "DUPLICATE_RELATIONSHIP_IDENTITY";
      return false;
    }
  }

  const std::size_t domain_count = reader.count(out.limits.max_failure_domains);
  if (!reader.ok()) {
    error = std::string(internal::to_string(reader.error()));
    return false;
  }
  if (domain_count > out.limits.max_failure_domains) {
    error = "FAILURE_DOMAIN_LIMIT_EXCEEDED";
    return false;
  }
  for (std::size_t i = 0; i < domain_count; ++i) {
    FailureDomainRecord record;
    internal::decode_failure_domain(reader, record);
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    const auto [it, inserted] = out.failure_domains.emplace(record.id, std::move(record));
    (void)it;
    if (!inserted) {
      error = "DUPLICATE_FAILURE_DOMAIN_IDENTITY";
      return false;
    }
  }

  if (reader.boolean()) {
    PowerEnvelopeRecord envelope;
    internal::decode_power_envelope(reader, envelope);
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    out.power = std::move(envelope);
  }
  if (reader.boolean()) {
    CoolingEnvelopeRecord envelope;
    internal::decode_cooling_envelope(reader, envelope);
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    out.cooling = std::move(envelope);
  }

  const std::size_t publisher_count = reader.count(out.limits.max_members);
  if (!reader.ok()) {
    error = std::string(internal::to_string(reader.error()));
    return false;
  }
  if (publisher_count > out.limits.max_connections * 16U) {
    error = "PUBLISHER_LIMIT_EXCEEDED";
    return false;
  }
  for (std::size_t i = 0; i < publisher_count; ++i) {
    const std::string worker = reader.string();
    const std::string boot = reader.string();
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    const auto parsed_worker = WorkerId::parse(worker);
    const auto parsed_boot = AgentBootId::parse(boot);
    if (!parsed_worker.has_value() || !parsed_boot.has_value()) {
      error = "INVALID_IDENTITY";
      return false;
    }
    PublisherRecord publisher{*parsed_worker, *parsed_boot};
    publisher.coordinator_epoch = CoordinatorEpoch::from_value(reader.u64());
    publisher.publication_generation = PublicationGeneration::from_value(reader.u64());
    publisher.state = reader.enum8(PublisherState::Fenced);
    publisher.registered_at = reader.timestamp();
    publisher.last_seen_at = reader.timestamp();
    publisher.lease_millis = reader.i64();
    publisher.label = reader.optional_string();
    publisher.accepted_mutations = static_cast<std::size_t>(reader.u64());
    publisher.rejected_mutations = static_cast<std::size_t>(reader.u64());
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    out.publishers.emplace(publisher.boot, std::move(publisher));
  }

  const std::size_t fenced_count = reader.count(out.limits.max_fenced_boots);
  if (!reader.ok()) {
    error = std::string(internal::to_string(reader.error()));
    return false;
  }
  if (fenced_count > out.limits.max_fenced_boots) {
    error = "FENCED_BOOT_LIMIT_EXCEEDED";
    return false;
  }
  for (std::size_t i = 0; i < fenced_count; ++i) {
    const std::string boot = reader.string();
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    const auto parsed_boot = AgentBootId::parse(boot);
    if (!parsed_boot.has_value()) {
      error = "INVALID_IDENTITY";
      return false;
    }
    FencedBootRecord fenced{*parsed_boot};
    const auto worker = reader.optional_string();
    if (worker.has_value()) {
      const auto parsed_worker = WorkerId::parse(*worker);
      if (!parsed_worker.has_value()) {
        error = "INVALID_IDENTITY";
        return false;
      }
      fenced.worker = *parsed_worker;
    }
    fenced.fenced_under_epoch = CoordinatorEpoch::from_value(reader.u64());
    fenced.fenced_at = reader.timestamp();
    fenced.reason = reader.enum8(RevalidationReason::Superseded);
    if (!reader.ok()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    out.fenced_boots.emplace(fenced.boot, std::move(fenced));
  }

  const std::size_t snapshot_count = reader.count(out.limits.max_retained_snapshots);
  if (!reader.ok()) {
    error = std::string(internal::to_string(reader.error()));
    return false;
  }
  if (snapshot_count > out.limits.max_retained_snapshots) {
    error = "SNAPSHOT_LIMIT_EXCEEDED";
    return false;
  }
  for (std::size_t i = 0; i < snapshot_count; ++i) {
    std::optional<RackSnapshot> snapshot;
    if (!SnapshotBuilder::decode(reader, snapshot) || !snapshot.has_value()) {
      error = std::string(internal::to_string(reader.error()));
      return false;
    }
    const auto [it, inserted] = out.snapshots.emplace(snapshot->generation(), std::move(*snapshot));
    (void)it;
    if (!inserted) {
      error = "DUPLICATE_SNAPSHOT_GENERATION";
      return false;
    }
  }

  if (!reader.at_end()) {
    error = reader.ok() ? "TRAILING_DATA" : std::string(internal::to_string(reader.error()));
    return false;
  }
  return true;
}

}  // namespace rack_fabric
