// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Invariant checking.
//
// Rack Fabric maintains derived indexes for query performance. Canonical
// records remain authoritative; every invariant check compares the indexes
// against the canonical records, so a stale index can never be mistaken for
// truth.

#ifndef RACK_FABRIC_INVARIANT_HPP
#define RACK_FABRIC_INVARIANT_HPP

#include <cstddef>
#include <string>
#include <vector>

namespace rack_fabric {

struct InvariantViolation {
  /// Stable machine-readable code.
  std::string code;
  std::string detail;

  friend bool operator==(const InvariantViolation&, const InvariantViolation&) = default;
};

struct InvariantReport {
  std::size_t checks_run = 0;
  std::vector<InvariantViolation> violations;

  [[nodiscard]] bool ok() const noexcept { return violations.empty(); }
  [[nodiscard]] std::string render() const;

  friend bool operator==(const InvariantReport&, const InvariantReport&) = default;
};

}  // namespace rack_fabric

#endif  // RACK_FABRIC_INVARIANT_HPP
