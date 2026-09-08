// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// The definition of RackFabric::Impl. It is shared by the runtime and the
// persistence translation units and is never installed.

#ifndef RACK_FABRIC_INTERNAL_IMPL_HPP
#define RACK_FABRIC_INTERNAL_IMPL_HPP

#include <shared_mutex>
#include <string>

#include "core/state.hpp"
#include "rack_fabric/rack_fabric.hpp"

namespace rack_fabric {

class RackFabric::Impl {
 public:
  explicit Impl(RackFabricOptions options_in);

  [[nodiscard]] Timestamp now() const { return clock->now(); }
  [[nodiscard]] std::string summary_digest(const RackSummary& summary) const;

  RackFabricOptions options;
  const Clock* clock = nullptr;
  mutable std::shared_mutex mutex;
  RackState state;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_INTERNAL_IMPL_HPP
