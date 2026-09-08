// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include <cstdint>
#include <string>
#include <vector>

#include "rack_fabric/rack_fabric.hpp"
#include "rack_fabric/synthetic.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;

[[nodiscard]] SyntheticRackConfig config_for(std::uint64_t seed) {
  SyntheticRackConfig config;
  config.seed = seed;
  config.rack = RackId{"synth-rack-" + std::to_string(seed)};
  config.rack_epoch = RackEpochId{"synth-epoch-" + std::to_string(seed)};
  config.node_count = 6;
  config.min_accelerators_per_node = 0;
  config.max_accelerators_per_node = 4;
  config.min_nics_per_node = 1;
  config.max_nics_per_node = 3;
  config.dpu_count = 2;
  config.switch_count = 3;
  config.storage_endpoint_count = 2;
  config.power_domain_count = 2;
  config.cooling_domain_count = 2;
  config.cpu_packages_per_node = 2;
  config.memory_domains_per_node = 2;
  config.synthetic_accelerator_peer_links = true;
  return config;
}

}  // namespace

RF_TEST(synthetic, generation_is_reproducible_from_the_seed) {
  const SyntheticRack first = generate_synthetic_rack(config_for(4242));
  const SyntheticRack second = generate_synthetic_rack(config_for(4242));
  RF_CHECK_EQ(first.members.size(), second.members.size());
  RF_CHECK_EQ(first.reproduction_parameters, second.reproduction_parameters);
  RF_CHECK_EQ(first.members.size(), second.members.size());
  for (std::size_t index = 0; index < first.members.size(); ++index) {
    RF_CHECK_EQ(first.members[index].key, second.members[index].key);
    RF_CHECK(first.members[index].details == second.members[index].details);
  }
  RF_CHECK_EQ(first.relationships.size(), second.relationships.size());
  RF_CHECK_EQ(first.failure_domains.size(), second.failure_domains.size());
  RF_CHECK(first.power.has_value());
  RF_CHECK(first.cooling.has_value());

  const SyntheticRack other = generate_synthetic_rack(config_for(4243));
  RF_CHECK_NE(first.reproduction_parameters, other.reproduction_parameters);
}

RF_TEST(synthetic, every_generated_rack_publishes_without_rejection) {
  for (const std::uint64_t seed : {1ULL, 7ULL, 99ULL, 2026ULL, 0xABCDEFULL}) {
    const SyntheticRackConfig config = config_for(seed);
    const SyntheticRack rack = generate_synthetic_rack(config);
    RackFabric fabric;
    const AuthorityToken authority = rf_test::operator_token(fabric, rack.rack.value());
    const SyntheticPublishSummary summary = publish_synthetic_rack(fabric, rack, authority);
    if (!summary.rejections.empty()) {
      rftest::record_failure(__FILE__, __LINE__,
                             "seed " + std::to_string(seed) + " rejected " +
                                 std::to_string(summary.rejections.size()) +
                                 " mutations; first: " + summary.rejections[0].explanation.render() +
                                 " parameters: " + rack.reproduction_parameters);
    }
    RF_CHECK_EQ(summary.members_accepted, rack.members.size());
    RF_CHECK_EQ(summary.relationships_accepted, rack.relationships.size());
    RF_CHECK_EQ(summary.failure_domains_accepted, rack.failure_domains.size());
    RF_CHECK(fabric.check_invariants().ok());
    RF_CHECK_EQ(fabric.summary().member_count, rack.members.size());
    RF_CHECK(fabric.power_envelope().has_value());
    RF_CHECK(fabric.cooling_envelope().has_value());
  }
}

RF_TEST(synthetic, racks_are_heterogeneous) {
  const SyntheticRack rack = generate_synthetic_rack(config_for(31337));
  std::vector<std::size_t> accelerators_per_node;
  std::vector<std::string> accelerator_models;
  std::vector<std::size_t> nics_per_node;
  for (const MemberRecord& record : rack.members) {
    if (record.key.kind == MemberKind::Accelerator) {
      const auto* details = std::get_if<AcceleratorDetails>(&record.details);
      RF_REQUIRE(details != nullptr);
      if (details->model.has_value()) {
        accelerator_models.push_back(*details->model);
      }
    }
  }
  for (const MemberRecord& node : rack.members) {
    if (node.key.kind != MemberKind::Node) {
      continue;
    }
    std::size_t accelerator_count = 0;
    std::size_t nic_count = 0;
    for (const MemberRecord& record : rack.members) {
      if (!record.parent.has_value() || !(*record.parent == node.key)) {
        continue;
      }
      if (record.key.kind == MemberKind::Accelerator) {
        ++accelerator_count;
      }
      if (record.key.kind == MemberKind::Nic) {
        ++nic_count;
      }
    }
    accelerators_per_node.push_back(accelerator_count);
    nics_per_node.push_back(nic_count);
  }
  RF_CHECK(!accelerators_per_node.empty());
  const bool accelerator_variety =
      std::adjacent_find(accelerators_per_node.begin(), accelerators_per_node.end(),
                         std::not_equal_to<>()) != accelerators_per_node.end();
  const bool nic_variety =
      std::adjacent_find(nics_per_node.begin(), nics_per_node.end(), std::not_equal_to<>()) !=
      nics_per_node.end();
  const bool model_variety = accelerator_models.size() >= 2 &&
                             std::adjacent_find(accelerator_models.begin(),
                                                accelerator_models.end(),
                                                std::not_equal_to<>()) != accelerator_models.end();
  RF_CHECK(accelerator_variety || nic_variety || model_variety);
}

RF_TEST(synthetic, partial_evidence_is_represented_not_fabricated) {
  SyntheticRackConfig config = config_for(555);
  config.missing_evidence = 1.0;
  config.degraded_fraction = 1.0;
  const SyntheticRack rack = generate_synthetic_rack(config);
  RackFabric fabric;
  const AuthorityToken authority = rf_test::operator_token(fabric, rack.rack.value());
  const SyntheticPublishSummary summary = publish_synthetic_rack(fabric, rack, authority);
  RF_CHECK_EQ(summary.rejections.size(), std::size_t{0});
  std::size_t unknown_provenance = 0;
  for (const MemberRecord& record : fabric.members()) {
    if (record.provenance == EvidenceProvenance::Unknown) {
      ++unknown_provenance;
      RF_CHECK_NE(record.lifecycle, MemberLifecycle::Present);
    }
  }
  RF_CHECK(unknown_provenance > 0);
  RF_CHECK(fabric.check_invariants().ok());
}

RF_TEST(synthetic, physical_readiness_is_never_satisfied_by_synthetic_evidence) {
  const SyntheticRack rack = generate_synthetic_rack(config_for(8));
  RackFabricOptions options;
  options.readiness.require_physical_provenance = true;
  RackFabric fabric(options);
  const AuthorityToken authority = rf_test::operator_token(fabric, rack.rack.value());
  (void)publish_synthetic_rack(fabric, rack, authority);
  const ReadinessEvaluation evaluation = fabric.evaluate_readiness();
  RF_CHECK(!evaluation.satisfied);
  RF_CHECK_NE(fabric.lifecycle(), RackLifecycle::Ready);
  const Explanation explanation = fabric.explain_readiness();
  RF_CHECK(!explanation.ok);
  RF_CHECK_EQ(explanation.code, std::string("RACK_NOT_READY"));
}

RF_TEST(synthetic, reproduction_parameters_are_complete_and_parseable) {
  const SyntheticRackConfig config = config_for(11);
  const std::string parameters = synthetic_reproduction_parameters(config);
  for (const char* key : {"seed=", "nodes=", "accelerators=", "nics=", "switches=", "storage=",
                          "power_domains=", "cooling_domains=", "heterogeneity=",
                          "missing_evidence=", "degraded_fraction=", "missing_links="}) {
    RF_CHECK(parameters.find(key) != std::string::npos);
  }
  const SyntheticRack rack = generate_synthetic_rack(config);
  RF_CHECK_EQ(rack.reproduction_parameters, parameters);
}

RF_TEST(synthetic, generated_relationships_are_publishable_in_order) {
  SyntheticRackConfig config = config_for(777);
  config.synthetic_accelerator_peer_links = true;
  config.missing_links = 0.0;
  const SyntheticRack rack = generate_synthetic_rack(config);
  RackFabric fabric;
  const AuthorityToken authority = rf_test::operator_token(fabric, rack.rack.value());
  const SyntheticPublishSummary summary = publish_synthetic_rack(fabric, rack, authority);
  RF_CHECK_EQ(summary.rejections.size(), std::size_t{0});
  RF_CHECK(summary.relationships_accepted > 0);
  for (const RelationshipRecord& record : fabric.relationships()) {
    RF_CHECK(fabric.find_member(record.key.from).has_value());
    RF_CHECK(fabric.find_member(record.key.to).has_value());
  }
  RF_CHECK(fabric.check_invariants().ok());
}
