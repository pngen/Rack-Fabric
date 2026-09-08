// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Race tests use barriers, not sleeps: every thread is released at the same
// instant and every assertion is about a property that must hold under any
// interleaving.

#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rack_fabric/rack_fabric.hpp"
#include "test_framework.hpp"
#include "test_support.hpp"

namespace {

using namespace rack_fabric;
using rf_test::member;
using rf_test::publish_member;

[[nodiscard]] MemberRecord publishable(MemberKind kind, const std::string& id, bool healthy) {
  MemberRecord record = member(kind, id);
  EvidenceValue<HealthState> health;
  health.value = healthy ? HealthState::Healthy : HealthState::Degraded;
  health.provenance = EvidenceProvenance::Measured;
  health.observed_at = rf_test::observed();
  health.ttl = std::chrono::milliseconds{100000};
  health.durability = Durability::Ephemeral;
  record.health = health;
  return record;
}

}  // namespace

RF_TEST(concurrency, parallel_publishers_and_readers_keep_the_state_consistent) {
  constexpr int kWriterCount = 8;
  constexpr int kMembersPerWriter = 40;
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  std::vector<rf_test::Publisher> publishers;
  publishers.reserve(kWriterCount);
  for (int index = 0; index < kWriterCount; ++index) {
    publishers.push_back(rf_test::register_publisher(fabric, "rack-a",
                                                     "worker-" + std::to_string(index),
                                                     "boot-" + std::to_string(index)));
  }

  // Every participant must arrive exactly once: the writers, the readers and
  // the main thread. A barrier with too few participants deadlocks.
  constexpr int kReaderCount = 4;
  std::barrier start(kWriterCount + kReaderCount + 1);
  std::atomic<int> writers_running{kWriterCount};
  std::atomic<bool> invariant_violation{false};
  std::atomic<int> rejected{0};

  std::vector<std::thread> writers;
  writers.reserve(kWriterCount);
  for (int writer = 0; writer < kWriterCount; ++writer) {
    writers.emplace_back([&fabric, &publishers, &start, &writers_running, &rejected, writer]() {
      AuthorityToken token = publishers[writer].token;
      start.arrive_and_wait();
      for (int index = 0; index < kMembersPerWriter; ++index) {
        const std::string id = "node-" + std::to_string(writer) + "-" + std::to_string(index);
        PublishMemberRequest request;
        request.authority = token;
        request.record = publishable(MemberKind::Node, id, index % 2 == 0);
        const MutationResult result = fabric.publish_member(request);
        if (result.accepted()) {
          token.publication_generation = PublicationGeneration::from_value(
              token.publication_generation->value() + 1);
        } else {
          ++rejected;
        }
      }
      writers_running.fetch_sub(1);
    });
  }

  std::atomic<int> reads{0};
  std::vector<std::thread> readers;
  readers.reserve(static_cast<std::size_t>(kReaderCount));
  for (int reader = 0; reader < kReaderCount; ++reader) {
    readers.emplace_back([&fabric, &start, &writers_running, &invariant_violation, &reads]() {
      start.arrive_and_wait();
      while (writers_running.load() > 0) {
        const RackSummary summary = fabric.summary();
        const std::uint64_t count = summary.member_count;
        // A summary that reports members must also report a lifecycle that
        // cannot be UNDECLARED, and every count must be self-consistent.
        if (count > 0 && summary.lifecycle == RackLifecycle::Undeclared) {
          invariant_violation = true;
        }
        if (summary.present_member_count > count || summary.retired_member_count > count) {
          invariant_violation = true;
        }
        if (!fabric.check_invariants().ok()) {
          invariant_violation = true;
        }
        const auto stored = fabric.find_member(MemberKey{MemberKind::Node, "node-0-0"});
        if (stored.has_value() && stored->key.kind != MemberKind::Node) {
          invariant_violation = true;
        }
        reads.fetch_add(1);
      }
    });
  }

  start.arrive_and_wait();
  for (std::thread& writer : writers) {
    writer.join();
  }
  for (std::thread& reader : readers) {
    reader.join();
  }

  RF_CHECK(!invariant_violation.load());
  RF_CHECK(reads.load() > 0);
  RF_CHECK_EQ(rejected.load(), 0);
  RF_CHECK_EQ(fabric.summary().member_count,
              static_cast<std::size_t>(kWriterCount * kMembersPerWriter));
  RF_CHECK(fabric.check_invariants().ok());
  RF_CHECK_EQ(fabric.summary().present_member_count, std::size_t{kWriterCount * kMembersPerWriter});
}

RF_TEST(concurrency, readers_never_observe_a_partially_written_record) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  const MemberKey key{MemberKind::Node, "node-1"};
  publish_member(fabric, publisher.token, publishable(MemberKind::Node, "node-1", true));

  constexpr int kReaders = 4;
  constexpr int kFlips = 200;
  std::barrier start(kReaders + 1);
  std::atomic<bool> stop{false};
  std::atomic<bool> violation{false};
  std::atomic<int> observations{0};

  std::vector<std::thread> readers;
  for (int index = 0; index < kReaders; ++index) {
    readers.emplace_back([&fabric, &start, &stop, &violation, &observations, &key]() {
      start.arrive_and_wait();
      while (!stop.load()) {
        const auto record = fabric.find_member(key);
        if (!record.has_value()) {
          violation = true;
          continue;
        }
        // The record must always be a complete, self-consistent snapshot: a
        // node with either a measured healthy or a measured degraded health
        // value, never a half-written mix.
        if (record->key.kind != MemberKind::Node) {
          violation = true;
        }
        if (record->health.provenance == EvidenceProvenance::Measured &&
            record->health.value != HealthState::Healthy &&
            record->health.value != HealthState::Degraded) {
          violation = true;
        }
        if (record->generation.is_unset()) {
          violation = true;
        }
        observations.fetch_add(1);
      }
    });
  }

  start.arrive_and_wait();
  AuthorityToken token = publisher.token;
  for (int flip = 0; flip < kFlips; ++flip) {
    PublishMemberRequest request;
    request.authority = token;
    request.record = publishable(MemberKind::Node, "node-1", flip % 2 == 0);
    request.expected_generation.reset();
    const MutationResult result = fabric.publish_member(request);
    if (result.accepted()) {
      token.publication_generation = PublicationGeneration::from_value(
          token.publication_generation->value() + 1);
    }
  }
  stop = true;
  for (std::thread& reader : readers) {
    reader.join();
  }

  RF_CHECK(!violation.load());
  RF_CHECK(observations.load() > 0);
  RF_CHECK(fabric.check_invariants().ok());
}

RF_TEST(concurrency, optimistic_concurrency_admits_exactly_one_writer) {
  constexpr int kThreads = 8;
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  const MemberKey key{MemberKind::Node, "node-1"};
  publish_member(fabric, publisher.token, publishable(MemberKind::Node, "node-1", true));

  std::barrier start(kThreads + 1);
  std::atomic<int> accepted{0};
  std::atomic<int> rejected{0};
  std::array<std::string, kThreads> codes;
  std::vector<std::thread> threads;
  for (int index = 0; index < kThreads; ++index) {
    threads.emplace_back([&fabric, &start, &accepted, &rejected, &codes, &publisher, &key,
                          index]() {
      PublishMemberRequest request;
      request.authority = publisher.token;
      request.record = publishable(MemberKind::Node, "node-1", index % 2 == 0);
      request.expected_generation = MemberGeneration::from_value(1);
      start.arrive_and_wait();
      const MutationResult result = fabric.publish_member(request);
      codes[static_cast<std::size_t>(index)] =
          std::string(to_string(result.outcome)) + ":" + result.explanation.code;
      if (result.accepted()) {
        ++accepted;
      } else if (result.rejected()) {
        ++rejected;
      }
    });
  }
  start.arrive_and_wait();
  for (std::thread& thread : threads) {
    thread.join();
  }
  std::size_t race_accepted = 0;
  std::size_t race_stale = 0;
  for (const std::string& code : codes) {
    if (code.rfind("ACCEPTED:", 0) == 0) {
      ++race_accepted;
    } else if (code.rfind("REJECT_STALE_GENERATION:STALE_PUBLICATION_GENERATION", 0) == 0) {
      ++race_stale;
    } else {
      rftest::record_failure(__FILE__, __LINE__, "unexpected race outcome: " + code);
    }
  }
  RF_CHECK_EQ(race_accepted, std::size_t{1});
  RF_CHECK_EQ(race_stale, std::size_t{kThreads - 1});
  RF_CHECK_EQ(accepted.load(), 1);
  RF_CHECK_EQ(rejected.load(), kThreads - 1);
  RF_CHECK_EQ(fabric.find_member(key)->generation.value(), std::uint64_t{2});
  RF_CHECK(fabric.check_invariants().ok());
}

RF_TEST(concurrency, fencing_during_publication_leaves_no_inconsistent_state) {
  RackFabric fabric;
  rf_test::declare_rack(fabric, "rack-a", "epoch-1");
  rf_test::Publisher publisher = rf_test::register_publisher(fabric, "rack-a", "worker-1",
                                                                   "boot-1");
  std::barrier start(2);
  std::atomic<bool> stop{false};

  std::thread writer([&fabric, &publisher, &start, &stop]() {
    AuthorityToken token = publisher.token;
    start.arrive_and_wait();
    for (int index = 0; index < 200 && !stop.load(); ++index) {
      PublishMemberRequest request;
      request.authority = token;
      request.record = publishable(MemberKind::Node, "node-" + std::to_string(index), true);
      const MutationResult result = fabric.publish_member(request);
      if (result.accepted()) {
        token.publication_generation = PublicationGeneration::from_value(
            token.publication_generation->value() + 1);
      }
    }
  });

  std::thread fencer([&fabric, &publisher, &start, &stop]() {
    start.arrive_and_wait();
    FencePublisherRequest request;
    request.coordinator_epoch = fabric.coordinator_epoch();
    request.boot = publisher.boot;
    const MutationResult result = fabric.fence_publisher(request);
    stop = true;
    RF_CHECK(result.accepted() || result.no_change());
  });

  writer.join();
  fencer.join();
  RF_CHECK(fabric.is_boot_fenced(publisher.boot));
  RF_CHECK(fabric.check_invariants().ok());
  // Every member that was accepted before the fence is now flagged.
  for (const MemberRecord& record : fabric.members()) {
    RF_CHECK(record.revalidation_required);
    RF_CHECK_EQ(record.revalidation_reason, RevalidationReason::OwnerProcessLost);
  }
}

RF_TEST(concurrency, instances_can_be_created_and_destroyed_repeatedly) {
  for (int cycle = 0; cycle < 40; ++cycle) {
    RackFabric fabric;
    rf_test::declare_rack(fabric, "rack-a", "epoch-1");
    const AuthorityToken authority = rf_test::operator_token(fabric, "rack-a");
    publish_member(fabric, authority, member(MemberKind::Node, "node-1"));
    RF_CHECK_EQ(fabric.summary().member_count, std::size_t{1});
    RF_CHECK(fabric.check_invariants().ok());
  }
}

RF_TEST(concurrency, independent_instances_are_usable_from_separate_threads) {
  constexpr int kInstances = 4;
  std::vector<std::unique_ptr<RackFabric>> instances;
  for (int index = 0; index < kInstances; ++index) {
    instances.push_back(std::make_unique<RackFabric>());
    rf_test::declare_rack(*instances.back(), "rack-" + std::to_string(index), "epoch-1");
  }
  std::barrier start(kInstances);
  std::atomic<bool> violation{false};
  std::vector<std::thread> threads;
  for (int index = 0; index < kInstances; ++index) {
    threads.emplace_back([&instances, &start, &violation, index]() {
      RackFabric& fabric = *instances[index];
      const AuthorityToken authority = rf_test::operator_token(fabric,
                                                               "rack-" + std::to_string(index));
      start.arrive_and_wait();
      for (int member_index = 0; member_index < 50; ++member_index) {
        PublishMemberRequest request;
        request.authority = authority;
        request.record = publishable(MemberKind::Node,
                                     "node-" + std::to_string(member_index), true);
        if (!fabric.publish_member(request).accepted()) {
          violation = true;
        }
      }
      if (fabric.summary().member_count != 50 || !fabric.check_invariants().ok()) {
        violation = true;
      }
      if (fabric.rack_id()->value() != "rack-" + std::to_string(index)) {
        violation = true;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  RF_CHECK(!violation.load());
  for (const auto& instance : instances) {
    RF_CHECK_EQ(instance->summary().member_count, std::size_t{50});
  }
}
