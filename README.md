# Rack Fabric

Rack Fabric is a vendor-neutral rack-level infrastructure runtime. It owns the
authoritative description of **one rack**: which members exist, how they are
arranged, what they are connected to, which failure domains they belong to,
what evidence supports each statement, how fresh that evidence is, and which
process is allowed to change any of it.

It is deliberately not a scheduler, not a cluster manager, not a collective
library, and not a hardware runtime. It answers "what is in this rack and how do
we know?" — never "what should run on it?".

* Language: C++20
* Namespace: `rack_fabric`
* License: Apache License 2.0
* Version: 1.0.0 (wire protocol 1, persistence format 1)
* Dependencies: none beyond the C++ standard library, Winsock, and an optional
  CUDA toolkit for the accelerator evidence module
* Telemetry: none

## Contents

* [What it owns](#what-it-owns)
* [What it must never absorb](#what-it-must-never-absorb)
* [Doctrine](#doctrine)
* [Architecture](#architecture)
* [Building](#building)
* [Installing and consuming](#installing-and-consuming)
* [Tools](#tools)
* [Library API sketch](#library-api-sketch)
* [Rack model](#rack-model)
* [Authority and fencing](#authority-and-fencing)
* [Snapshots](#snapshots)
* [Persistence and recovery](#persistence-and-recovery)
* [Wire protocol](#wire-protocol)
* [Tests](#tests)
* [Benchmarks](#benchmarks)
* [Limits](#limits)
* [Project layout](#project-layout)
* [Limitations](#limitations)
* [License](#license)

## What it owns

Rack Fabric is the single authority for:

* **Rack identity and epoch** — the rack id, its epoch, and the coordinator
  epoch that fences every mutation.
* **Membership** — compute nodes, accelerators, CPUs, memory domains, NICs,
  DPUs, switches, storage endpoints, power domains, cooling domains, and any
  other member kind, with a stable key (`kind` + `id`) and a monotonic
  per-record generation.
* **Hierarchy and topology** — parent/child containment and typed relationships
  (`contains`, `attached_to`, `connected_to`, `reachable_through`,
  `same_node`, `same_numa_domain`, ...) with their own generations and
  ownership.
* **Locality** — NUMA and node locality expressed as relationships, not as a
  fixed topology shape.
* **Connectivity and reachability** — as evidence, never as assumption. An
  unreported link is UNKNOWN, not connected.
* **Failure domains** — declared domains (node, rack, power feed, cooling zone,
  switch, host, ...) and member membership in them.
* **Power and cooling envelopes** — declared and observed envelopes with
  explicit headroom, never inferred from a favourable aggregate.
* **Evidence provenance, freshness and generations** — every fact carries how it
  was learned, when, how long it stays meaningful, and whether it is durable or
  ephemeral.
* **Snapshots** — immutable, digest-verified, monotonically numbered
  publications of rack state, with explicit supersession.
* **Mutation authority** — who may change what, enforced by coordinator epoch,
  process incarnation, publisher lease, and per-record generation CAS.
* **Stale fencing** — dead processes, replayed identities, and superseded
  generations are refused, never silently accepted.
* **Distributed agents** — real OS processes registering over the wire protocol
  with boot identities and incarnation fencing.
* **Persistence and conservative recovery** — durable state survives a restart;
  live observations never do.
* **Deterministic inspection and explanation** — every mutation result carries a
  stable code and factors; every rejection is explainable.

## What it must never absorb

Rack Fabric does not implement, and will not grow into, any of the following.
They are separate systems that consume Rack Fabric's description:

* Cluster Fabric (multi-rack/cluster membership and scheduling domains)
* Fabric Scheduler / Resource Broker / Capacity Fabric
* Communication Planner / Collective Scheduler / Collective Fabric
* Congestion Fabric
* Power Fabric / Thermal Governor
* Accelerator Health
* Runtime Registry / Hardware Capability Registry
* NVLink, NVSwitch, PCIe, DPU, GPUDirect, CXL, NUMA, storage, or RDMA runtimes

Rack Fabric may *record* evidence about those things. It never *drives* them.

## Doctrine

These rules are enforced by the implementation and asserted by the test suite.

1. **UNKNOWN is a first-class value.** It never silently becomes PRESENT,
   HEALTHY, READY, REACHABLE, CONNECTED, COMPATIBLE,
   WITHIN_POWER_ENVELOPE, WITHIN_COOLING_ENVELOPE, or CURRENT.
2. **Absence of evidence is not positive evidence.** A missing observation
   produces a deficit, not a pass.
3. **Hard validity before interpretation.** Structural validity, bounds, and
   integrity are checked before any semantic reading of a payload.
4. **Favourable aggregates never rescue stale or invalid state.** A rack is not
   READY because most members are fine.
5. **Provenance is explicit.** MEASURED, REPORTED, DERIVED, ESTIMATED,
   SYNTHETIC, RECONSTRUCTED, UNKNOWN. Synthetic evidence is never relabelled as
   measured, and synthetic evidence can never make a physical rack READY.
6. **Reconstructed evidence is not fresh measurement.** State recovered from
   disk is marked RECONSTRUCTED and flagged for revalidation.
7. **Heterogeneity is the norm.** No NVIDIA-only paths, no fixed GPU or NIC
   counts, no single switch layer or topology shape, no NVLink/NVSwitch
   assumption, no homogeneous nodes, no uniform power or cooling, no identical
   failure domains.
8. **Mutations return typed outcomes.** Never a bare `false`. Every rejection
   has a stable code and factors.
9. **Fencing is permanent within an epoch.** A fenced boot identity can never
   mutate again; reincarnation requires a new boot identity.
10. **No telemetry.** The library opens no outbound connection that is not an
    explicit coordinator/agent session.

## Architecture

```
+--------------------------------------------------------------------------+
| consumer (cluster fabric, scheduler, operator tooling, ...)              |
+--------------------------------------------------------------------------+
        |  C++ API (rack_fabric::RackFabric)   |  wire protocol (TCP)
        v                                      v
+---------------------------------+   +------------------------------------+
| RackFabric                      |   | rack_fabric_coordinator            |
|  - authoritative RackState      |<--|  - owns the only RackFabric that   |
|  - validation + invariants      |   |    may be mutated                  |
|  - canonical codec              |   |  - sessions, leases, fencing       |
|  - persistence (atomic, CRC+    |   +------------------------------------+
|    SHA-256, generation-checked) |            ^        ^        ^
+---------------------------------+            |        |        |
        ^                                      |        |        |
        | in-process use                       |        |        |
+---------------------------------+   +---------+--+  +--+---------+  +-------+
| examples, tests, benchmarks     |   | agent #1   |  | agent #2   |  | ...  |
+---------------------------------+   | (separate  |  | (separate  |  |      |
                                      |  process)  |  |  process)  |  |      |
                                      +------------+  +------------+  +------+
```

* `rack_fabric` — the library. One `RackFabric` instance owns one rack's
  authoritative state. Queries take a shared lock; mutations take an exclusive
  lock. No lock is ever held across I/O, a join, or a callback.
* `rack_fabric_cuda` — optional accelerator evidence module. It probes a real
  CUDA device and produces MEASURED evidence; it never schedules or allocates
  work for consumers.
* `rack_fabric_coordinator` — a process that owns a `RackFabric` and serves
  agents over the wire protocol. It is the only process that accepts mutations
  from remote processes.
* `rack_fabric_agent` — a process that registers with a coordinator using a
  boot identity, publishes what it can observe, heartbeats, and exits cleanly on
  request.
* `rack_fabric_inspect` — deterministic inspection and explanation, including
  the synthetic rack laboratory and the real hardware probe.
* `rack_fabric_benchmarks` — component/relationship scale measurements.

## Building

Requirements:

* CMake 3.20 or newer
* A C++20 compiler. On Windows: MSVC 19.4x (Visual Studio 2022 or Build Tools)
* Ninja (recommended; the Visual Studio generator is supported but slower)
* Optional: CUDA Toolkit 12.x/13.x for `rack_fabric_cuda`

```powershell
# Configure and build the default (Release) preset.
cmake --preset release
cmake --build --preset release

# Run the tests.
ctest --preset release
```

Manual configuration, equivalent to the preset:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DRACK_FABRIC_BUILD_TESTS=ON `
  -DRACK_FABRIC_WARNINGS_AS_ERRORS=ON
cmake --build build
```

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `RACK_FABRIC_BUILD_LIBRARY` | `ON` | Build `rack_fabric` |
| `RACK_FABRIC_BUILD_TOOLS` | `ON` | Build coordinator, agent, inspection tool |
| `RACK_FABRIC_BUILD_TESTS` | `ON` | Build the test suites |
| `RACK_FABRIC_BUILD_EXAMPLES` | `ON` | Build the examples |
| `RACK_FABRIC_BUILD_BENCHMARKS` | `ON` | Build the benchmarks |
| `RACK_FABRIC_ENABLE_CUDA` | `ON` | Build the CUDA module when a toolkit is found |
| `RACK_FABRIC_WARNINGS_AS_ERRORS` | `ON` | Treat first-party warnings as errors |
| `RACK_FABRIC_ENABLE_ASAN` | `OFF` | Build with AddressSanitizer |

First-party code compiles warning-free at `/W4 /WX` (MSVC) and at
`-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
-Wnon-virtual-dtor -Werror` (GCC/Clang). Third-party headers are not a
dependency of this project.

## Installing and consuming

```powershell
cmake --install build/release --prefix C:/RackFabric
```

The install tree contains the static libraries, public headers, the CMake
package config, the tools, and the license/readme. A consumer uses it with:

```cmake
find_package(rack_fabric 1.0.0 REQUIRED)

add_executable(my_consumer main.cpp)
target_link_libraries(my_consumer PRIVATE rack_fabric::rack_fabric)
# Optional CUDA evidence module:
# target_link_libraries(my_consumer PRIVATE rack_fabric::rack_fabric_cuda)
```

```cpp
#include <rack_fabric/rack_fabric.hpp>

int main() {
  rack_fabric::RackFabric fabric;
  rack_fabric::DeclareRackRequest declare;
  declare.authority.coordinator_epoch = fabric.coordinator_epoch();
  declare.rack = rack_fabric::RackId{"rack-1"};
  declare.epoch = rack_fabric::RackEpochId{"epoch-1"};
  if (!fabric.declare_rack(declare).accepted()) {
    return 1;
  }
  const auto summary = fabric.summary();
  return summary.member_count == 0 ? 0 : 1;
}
```

## Tools

### `rack_fabric_coordinator`

```
rack_fabric_coordinator --rack <id> [--rack-epoch <id>] [--rack-label <text>]
                        [--port <n>] [--state <file>] [--persist <file>]
                        [--stop-file <file>] [--version] [--help]
```

* `--port 0` binds an ephemeral port; the chosen port is printed as
  `listening port=<n> epoch=<n> version=<v>`.
* `--persist <file>` loads durable state on start (advancing the coordinator
  epoch and applying conservative recovery) and writes it on clean shutdown.
* `--stop-file <file>` requests a deterministic shutdown when the file appears.

### `rack_fabric_agent`

```
rack_fabric_agent --port <n> [--worker <id>] [--boot <id>] [--rack <id>]
                  [--label <text>] [--heartbeat-ms <n>] [--no-heartbeat]
                  [--no-hardware] [--stop-file <file>] [--status-file <file>]
                  [--version] [--help]
```

The agent registers a boot identity, publishes what it can observe (hardware
inventory unless `--no-hardware`), heartbeats inside the publisher lease, and
exits non-zero if the coordinator refuses it.

### `rack_fabric_inspect`

```
rack_fabric_inspect [--state <file>] [--validate] [--host] [--cuda]
                    [--synthetic --seed <n> --nodes <n> --accelerators <n>
                     --switches <n>] [--json] [--version] [--help]
```

* `--state <file> --validate` decodes a persisted state file and checks
  invariants without mutating anything.
* `--host` inspects the real host: CPUs, NUMA domains, memory, NICs, and the
  members it can actually measure.
* `--cuda` runs the real CUDA evidence path (device query, allocation, H2D,
  kernel, sync, D2H, compare, free) and reports MEASURED evidence.
* `--synthetic` builds a labelled SYNTHETIC rack laboratory of the requested
  shape. Synthetic evidence can never satisfy a physical readiness contract.

## Library API sketch

```cpp
rack_fabric::RackFabric fabric(rack_fabric::RackFabricOptions{});

// Declarations (durable).
fabric.declare_rack(declare);
fabric.redeclare_rack(redeclare);
fabric.retire_rack(retire);

// Processes.
fabric.register_publisher(register_request);   // boot identity, worker id
fabric.heartbeat(heartbeat);
fabric.expire_publishers();                    // lease enforcement
fabric.fence_publisher(fence);                 // explicit fencing
fabric.deregister_publisher(deregister);

// Structure (per-record generations, ownership, supersession).
fabric.publish_member(publish_member);
fabric.publish_relationship(publish_relationship);
fabric.publish_failure_domain(publish_failure_domain);
fabric.publish_power_envelope(publish_power);
fabric.publish_cooling_envelope(publish_cooling);

// Evidence.
fabric.publish_evidence(publish_evidence);
fabric.withdraw_evidence(withdraw);
fabric.revalidate_members(revalidate);
fabric.retire_member(retire_member);

// Publication.
fabric.publish_snapshot(snapshot_request);
fabric.supersede_snapshot(supersede_request);

// Inspection (shared lock, no mutation).
fabric.summary();
fabric.lifecycle();
fabric.members();
fabric.find_member(key);
fabric.relationships();
fabric.failure_domains();
fabric.publishers();
fabric.fenced_boots();
fabric.snapshots();
fabric.readiness();
fabric.explain(mutation_result);
fabric.check_invariants();

// Persistence.
fabric.save_state(path);
fabric.load_state(path);
```

Every mutation returns `MutationResult`: a typed `MutationOutcome`
(`Accepted`, `NoChange`, `Reject...`), a stable `explanation.code`, a
`subject`, and ordered `factors` with the exact generation numbers involved.

## Rack model

**Member kinds** are open: node, accelerator, cpu, memory_domain, nic, dpu,
switch, storage_endpoint, power_domain, cooling_domain, fabric, and any other
kind the consumer needs. Nothing in the runtime assumes a fixed count or shape.

**Member lifecycle:** `Unknown`, `Absent`, `Present`, `Unavailable`,
`Retired`. A retired identity can never be resurrected.

**Rack lifecycle:** `Undeclared`, `Declared`, `Discovering`, `Partial`,
`Ready`, `Degraded`, `RevalidationRequired`, `Retired`.

**Readiness** is a contract, not a heuristic: required member counts, required
health, required active publishers, and a requirement that satisfying evidence
has physical provenance. The evaluation reports every deficit with the required
and observed counts.

**Envelopes** carry declared and observed values separately, with explicit
headroom. A rack with unknown draw is not "within the power envelope".

## Authority and fencing

A mutation carries an `AuthorityToken`: rack id, rack epoch, coordinator
epoch, worker id, boot id, and publication generation. The coordinator validates,
in order:

1. hard validity of the payload (bounds, structure, integrity);
2. rack identity and rack epoch;
3. coordinator epoch (a stale coordinator is refused);
4. process registration and lease;
5. fenced boot set (a fenced incarnation is refused, permanently);
6. per-record ownership (a process may not steal another process's record);
7. publication generation (a stale or ahead generation is refused);
8. per-record generation CAS (`expected_generation`) or explicit
   `supersede`.

Fencing marks the boot identity, flags every record that process owned as
requiring revalidation, marks ephemeral evidence as no longer current, and keeps
durable identity as history. Recovery after a coordinator restart does the same
for every owner-bearing record and advances the coordinator epoch.

## Snapshots

A snapshot is an immutable, digest-verified view with an id of the form
`snap-<rack>-<generation>`. Publication advances a generation and never mutates
a previous snapshot. A snapshot becomes `Stale` when the membership generation
moves past it, `Superseded` when explicitly replaced, and `Reconstructed`
when it came from disk. Retained snapshots are bounded
(`max_retained_snapshots`, default 256).

## Persistence and recovery

The persisted file is:

```
offset  size  field
0       8     magic 'R' 'K' 'F' 'S' 'T' 'A' 'T' '1'
8       4     format version
12      4     flags
16      8     payload length
24      8     payload CRC-32C
32      32    payload SHA-256
64      8     reserved (zero)
72      N     payload
72+N    4     trailer 'R' 'K' 'F' 'E'
76+N    4     CRC-32C of the header and payload
```

Writes are atomic: a temporary file is written, flushed, and renamed over the
target with `MoveFileExW`. Reads decode into a candidate and only commit if the
whole file is valid, so a truncated or corrupt file changes nothing.

Recovery is deliberately conservative:

* durable declarations are restored and flagged RECONSTRUCTED;
* every owner-bearing record is marked `revalidation_required` (the process
  that published it is gone);
* ephemeral evidence is cleared and never restored as current;
* publishers become `Lost` and cannot resume without re-registering;
* the coordinator epoch advances, so every token from the previous incarnation
  is refused;
* fenced boot identities stay fenced across restarts.

## Wire protocol

Little-endian frames:

```
offset  size  field
0       4     magic 'R' 'K' 'F' '1'
4       2     protocol version (1)
6       2     message type
8       2     flags (bit 0: response)
10      2     reserved (must be zero)
12      8     correlation id
20      4     payload length
24      N     payload
24+N    4     CRC-32C of bytes 0..24+N-1
```

Every length is validated against a bound before any buffer is allocated; all
length arithmetic is checked; a malformed frame is rejected without touching
authoritative state. Payload codecs are versioned, bounded, and re-encode to the
exact bytes they decoded from.

## Tests

```powershell
ctest --preset release --output-on-failure
# or run the executables directly
.\build\tests\rack_fabric_tests.exe
.\build\tests\rack_fabric_multiprocess_tests.exe
```

The test runner supports `--list`, `--filter=<substring>`, and
`--repeat <n>`. There is no CTest
`TIMEOUT` property, no shell timeout wrapper, and no watchdog that kills a
running test. Race tests use `std::barrier`, not sleeps. Property tests use
fixed seeds and reproduce deterministically. A failing test always produces a
non-zero exit code and a structured failure line.

Coverage includes: identity and generation semantics; membership, topology and
envelope rules; snapshot publication, supersession and staleness; authority,
fencing and reincarnation; protocol round-trips, bounds, corruption, and a
fixed-seed fuzz run; persistence round-trip, corruption, truncation at every
offset, hostile lengths, and atomic replacement; conservative recovery across
restarts; barrier-based concurrency with no partial observation; property tests
over random mutation sequences; the synthetic rack laboratory; and a
multiprocess suite that starts real coordinator and agent processes and drives
them over TCP loopback.

## Benchmarks

```powershell
.\build\rack_fabric_benchmarks.exe
```

The benchmark builds racks of 100, 1,000 and 10,000 members with up to 100,000
relationships, then measures publication, query, snapshot, and persistence
costs. Numbers are reported per run and are not committed as guarantees.

## Limits

All limits are in `rack_fabric::ResourceLimits` and are configurable per
instance. The collection limits bound a single protocol message; durable state
is bounded by the rack limits of the collection it encodes (`max_members`,
`max_relationships`, `max_failure_domains`, `max_retained_snapshots`,
`max_fenced_boots`) and by `max_persisted_bytes`, so a rack may hold far more
members than one message can carry:

| Limit | Default |
| --- | --- |
| `max_frame_bytes` | 1 MiB |
| `max_payload_bytes` | 1 MiB - 64 |
| `max_string_bytes` | 256 |
| `max_collection_items` | 4096 |
| `max_relationships_per_message` | 4096 |
| `max_members` | 1,000,000 |
| `max_relationships` | 4,000,000 |
| `max_failure_domains` | 100,000 |
| `max_capabilities_per_member` | 256 |
| `max_failure_domains_per_member` | 64 |
| `max_connections` | 64 |
| `max_outstanding_requests` | 64 |
| `max_send_queue_frames` | 1024 |
| `max_persisted_bytes` | 512 MiB |
| `max_retained_snapshots` | 256 |
| `max_fenced_boots` | 65,536 |
| `max_explanation_factors` | 64 |
| `max_session_threads` | 32 |
| `publisher_lease_millis` | 5000 |

## Project layout

```
include/rack_fabric/   public headers (one per concern)
src/core/              state, model, canonical codec, invariants
src/protocol/          framing and message codecs
src/persistence/       atomic persistence and recovery
src/transport/         TCP loopback transport
src/coordinator/       coordinator server
src/agent/             agent client
src/hardware/          host and CUDA evidence
src/synthetic/         synthetic rack laboratory
tools/                 coordinator, agent, inspect entry points
examples/              quick start and multiprocess examples
benchmarks/            scale benchmarks
tests/                 unit, protocol, persistence, concurrency, property,
                       synthetic, and multiprocess suites
cmake/                 package config template
```

## Limitations

* The coordinator serves one rack. Multi-rack and cluster-level composition is
  deliberately out of scope.
* Transport is loopback TCP; there is no TLS, authentication, or multi-host
  transport. Sessions are trusted on the local host.
* The synthetic rack laboratory produces SYNTHETIC evidence. It is labelled as
  such everywhere and can never satisfy a physical readiness contract.
* The CUDA module reports evidence about the devices it can see. It does not
  manage them, schedule them, or allocate them for consumers.
* Persistence is a single file per rack instance. There is no replication or
  distributed consensus; correctness relies on the coordinator being the only
  writer and on epoch-based fencing of stale processes.
* Hardware discovery is best-effort and reports UNKNOWN where the operating
  system does not expose a fact. That is intentional.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
