# Learning WeakNet V2

## Purpose

This is the intended code-reading order for a developer learning the system after V2 implementation. At the time this guide was written, the repository still contains V1 and the V2 target is a design. If actual paths differ later, follow the equivalent CMake target and component name rather than assuming an illustrative path is exact.

The central learning idea is to follow one observation from the kernel to an explanation, then follow one query from `weaknetctl` back to stored state. Read the optional AI code last; it is not part of core correctness.

## Current implementation checkpoint

Phase 1 through Phase 3 are now the implemented baseline. The top-level `CMakeLists.txt`, `cmake/`, and `tests/` describe the current V1 products built as C++20 with an owned runtime lifecycle plus an additive V2 internal data plane:

- `weaknet-dbus-server` (name intentionally unchanged);
- `libweaknet.so`;
- `test-client` and supported examples;
- the optional existing `flow_rate.bpf.o`;
- deterministic CTest and V1 ABI/D-Bus contract fixtures;
- `DaemonApplication`, direct `RuntimeHealth`, owned `std::jthread` workers, signal-driven reverse shutdown, bounded asynchronous V1 Ping, and validated runtime/state paths.
- `network_event.hpp`, `event_bus.hpp`, `metric_store.hpp`, injected clocks, and `v1_observation_adapter.hpp` in the `weaknet_data_plane` target;
- deterministic Phase 3 schema, saturation/concurrency, store retention, and V1 bridge tests;
- exact Phase 3 semantics in `docs/PHASE3_DATA_PLANE.md`.
- the Phase 4 `netlink_parser.hpp`, `netlink_topology.hpp`, and `netlink_collector.hpp` path with deterministic parser/policy tests;
- exact Phase 4 semantics in `docs/PHASE4_NETLINK.md`.
- the Phase 5.1–5.3 `socket_tracker.hpp`/`socket_tracker.cpp` and
  `socket_diag_parser.hpp` path with deterministic identity, lifecycle,
  NETLINK_SOCK_DIAG inventory, variable-size TCP_INFO, and interval-metric
  semantics;
- exact Phase 5.1–5.4 semantics in `docs/PHASE5_SOCKET_TRACKER.md`, including
  committed-topology socket route attribution and its uncertainty model.

The later names `weaknetd` and `weaknetctl` remain target architecture.
RootCauseEngine now provides the first bounded deterministic hypothesis stage.
The additive experimental V2 read-only D-Bus surface and `weaknetctl` query
commands are implemented; read `docs/V2_API_AND_CLI.md` after the diagnosis
engine documents.
The first ActiveProbe evidence collector and RootCauseEngine enrichment are
implemented; read `docs/ACTIVE_PROBE.md` and `docs/ROOT_CAUSE_ENGINE.md` before
treating probe observations as diagnosis.
The first native Wi-Fi evidence collector is also implemented; read
`docs/WIFI_EVIDENCE.md` before treating RSSI, association, retry counters, or
bitrate as network-health conclusions.
SocketTracker now provides the Phase 5.3 TCP_INFO metric path and Phase 5.4
modeled route context. The first deterministic IncidentEngine stage is
implemented; its incidents are not root causes and
its route context is not a claim of kernel FIB/RPDB equivalence.
Read `server/src/application.cpp` after the Phase 1 targets, then read
`docs/PHASE2_RUNTIME.md`, `docs/PHASE3_DATA_PLANE.md`,
`docs/PHASE4_NETLINK.md`, and `docs/PHASE5_SOCKET_TRACKER.md`; use later
sections as the intended post-implementation order.

## Prerequisites

Before reading implementation details, be comfortable with:

- C++20 RAII, `std::variant`, `std::chrono`, `std::jthread`, `std::stop_token`, atomics, mutexes, and condition variables;
- Linux file descriptors, `poll`/`epoll`, signals, capabilities, and network namespaces;
- rtnetlink message framing and route/link concepts;
- TCP state and important `TCP_INFO` counter/gauge distinctions;
- eBPF maps, BTF, CO-RE relocations, libbpf skeletons, and ring buffers;
- D-Bus names, paths, interfaces, methods, signals, introspection, and bus policy;
- veth, routing, qdiscs, and `tc`/netem.

You do not need Python, LangChain, or an LLM to understand or operate the core.

## Read the design record first

Read these documents in order:

1. `V2_ARCHITECTURE.md` — the intended boundaries and invariants.
2. `V2_ROADMAP.md` — why the code arrived in increments and which compatibility layers may remain.
3. `V1_AUDIT.md` — the origin of reused code and the defects the new boundaries prevent.
4. `AGENTS.md` — repository engineering and safety rules.

Questions to answer before opening source:

- Which features work with Python entirely absent?
- Which component owns event ordering and overload behavior?
- Which component owns metric history?
- What is the difference between an observation, an incident, and a root-cause candidate?
- How does the system represent “unknown” rather than zero?

## Code-reading path

### 1. Start at build targets, not individual files

Read the top-level `CMakeLists.txt`, relevant files under `cmake/`, and target definitions for:

- the current `weaknet_server_core`, `weaknet-dbus-server`, `weaknet_client`, and example targets;
- `weaknet_bpf`, `ENABLE_EBPF`, `BUILD_TESTING`, and build-tree generated files;
- `tests/contracts/` and the Phase 1 deterministic tests;

- `weaknet_core` (or the equivalent domain library);
- collector libraries;
- the BPF object and generated skeleton;
- `weaknetd`;
- `weaknetctl`;
- unit, integration, lab, and benchmark targets;
- the optional AI package boundary.

The first three bullets are implemented now. The following target-architecture bullets become applicable as their roadmap phases land.

Record which dependencies are required, optional, privileged only at runtime, or test-only. Notice how the eBPF-disabled build is represented and verify that no Python dependency reaches a production C++ target.

### 2. Read the two executable composition roots

Read `weaknetd`'s `main` and application/composition object first. Do not descend into collectors yet.

Trace:

1. configuration load and validation;
2. capability detection;
3. component construction order;
4. worker start order;
5. readiness/degraded reporting;
6. signal handling;
7. reverse-order shutdown and deadlines.

Then read `weaknetctl`'s command dispatch and D-Bus client construction. Identify which commands are pure snapshots, which follow signals, and which export bounded data.

At this point you should be able to draw the process boundary and explain why the daemon does not launch Python.

### 3. Learn the domain language through `NetworkEvent`

Read typed identifiers, status/validity, timestamps, `EventKind`, and every `NetworkEvent` payload. This is the most important internal contract.

For each payload, note:

- its scope: namespace, interface, socket, flow, or daemon;
- units and whether the value is a counter, gauge, delta, ratio, or state transition;
- provenance/source;
- valid/unavailable/stale/partial/reset behavior;
- realtime versus monotonic timestamp use;
- schema version and external serialization mapping.

Do not continue until you can explain why interface name alone is not identity and why a missing metric must not be stored as zero.

### 4. Read `EventBus`

Follow publication through the bounded queue to subscriber delivery. Find:

- ordering guarantees;
- subscription token lifetime;
- filtering;
- queue-full policy and event coalescing;
- drop/high-water/lag metrics;
- callback lock boundaries;
- stop/drain behavior.

Read EventBus concurrency and saturation tests immediately after the implementation. Tests often express its contract more clearly than helper classes.

### 5. Read `MetricStore`

Start with the key and sample types, then updates, latest snapshots, window queries, retention, and eviction. Check how namespace/interface/socket cardinality is bounded.

Read tests for:

- missing versus zero;
- stale and reset samples;
- snapshot consistency;
- retention boundaries;
- concurrent readers and writers;
- injected clock behavior.

The EventBus transports facts; MetricStore answers state/window questions. Keep those roles distinct.

### 6. Read topology collection (`NetlinkCollector`)

Read netlink transport and message parsing separately from topology/uplink policy.

Trace an initial dump:

```text
request -> sequence/sender validation -> multipart parser
        -> link/address/route facts -> reconciled topology
        -> NetworkEvent -> EventBus -> MetricStore
```

Then trace an incremental route notification and its reconciliation. Study route table, priority, multipath, IPv4/IPv6, on-link default, link rename/delete, and namespace tests. Use lab scenarios to see real kernel messages only after fixture tests make the parser understandable.

Treat transport startup and topology reconciliation as separate lifecycle stages. A socket create/configure/bind failure leaves the collector stopped, while a dump failure after the socket opens keeps the worker running with degraded telemetry and an empty non-authoritative or last-known-good snapshot. During a dump, sequence-0 link/address/route notifications are recorded but deliberately not merged; the candidate is rejected and resynced if any relevant notification races it. ENOBUFS and malformed/ambiguous notification application follow the same bounded recovery path. Follow the bounded 250 ms-to-five-second recovery backoff through a successful transactional commit; collector telemetry recovers immediately, although the current direct `RuntimeHealth` startup entry is not automatically refreshed.

### 7. Read the Phase 5.1 socket identity model

Read `socket_tracker.hpp` and `socket_tracker.cpp` before any future kernel
collector. Follow `SocketId`, `SocketTuple`, cookie availability, generation
allocation, and the authoritative/partial snapshot boundary. Confirm that
tuple reuse and cookie reuse receive new generations, while failed snapshots
preserve active lifecycles. `SocketObservation` is a typed future event
payload; this phase intentionally emits no live socket events.

### 8. Read `SocketTracker` and TCP_INFO next

Read socket identity/generation before reading metric calculations. Trace:

1. sock_diag request, `INET_DIAG_INFO`, and response parsing;
2. cookie/netns/tuple identity;
3. socket create/update/close reconciliation;
4. field-size/version availability checks;
5. compatible monotonic counter deltas and reset handling;
6. emitted raw socket/TCP events and MetricStore samples.

Pay special attention to fields that are gauges versus cumulative counters. Read counter reset, socket reuse, IPv4/IPv6, and partial-permission tests alongside the code.

### 9. Read modeled socket route attribution

Read `socket_route_attributor.hpp` and its tests after the topology and socket
models.  The attributor is pure: it performs binary longest-prefix matching
over the authoritative committed snapshot, applies the Phase 4 table/metric
policy, and keeps ambiguity and multipath uncertainty explicit.  Treat
`diag_ifindex` and local/source addresses as evidence rather than conclusions.
Then follow `SocketTracker::recomputeRouteContexts` and the topology collector
commit callback to see why rejected candidates cannot publish route context
and why active sockets are refreshed when topology generation changes.

### 10. Cross the kernel boundary through eBPF

Read in this order:

1. the shared kernel/user ABI header and layout assertions;
2. BPF map and event definitions;
3. each BPF attachment and the semantic reason it was chosen;
4. the generated skeleton target;
5. userspace load/attach/capability fallback;
6. ring-buffer/map consumption;
7. conversion to `NetworkEvent`;
8. resource cleanup and health reporting.

For each kernel event, ask:

- Is the hook stable on the supported kernel matrix?
- In which execution context does it run?
- Is process/interface/namespace attribution actually valid there?
- What happens when the buffer/map is full?
- How is the event correlated with SocketTracker?
- What happens when BPF cannot load?

Run or inspect the namespace integration tests; do not infer correctness solely from successful verifier loading.

### 10. Read active probe and Wi-Fi collectors

For probes, follow binding, request IDs, reply matching, monotonic timing, cancellation, rate limiting, and validity. Confirm that target reachability does not overwrite physical link state.

For Wi-Fi, read nl80211 first and any read-only wpa_supplicant fallback second. Confirm that wired RSSI is “not applicable” and that the daemon never starts or reconfigures a network manager.

In the current V2 implementation, follow `WifiCollector` through its
Phase-4-selected `NetnsId + ifindex` target, Generic Netlink family
resolution, typed parser, bounded worker, and `WifiObservation` EventBus
event. Study signed dBm parsing, 100-kbit/s-to-kbps conversion, station
absence versus query failure, optional counters, and explicit capability
states. The V1 wpa_supplicant path remains compatibility code and is not the
V2 source of truth.

### 11. Read `IncidentEngine`

Read `incident_engine.hpp`, `incident_engine.cpp`, and
`docs/INCIDENT_ENGINE.md`. Begin with incident schema and a single rule/state
machine. Follow a metric window through:

```text
normal -> pending -> open -> updated -> resolving -> resolved
```

Locate hysteresis, minimum sample count, cooldown, deduplication key, evidence references, rule version, and missing-data behavior. Read deterministic replay tests and one matching lab scenario before reading all rules.

Remember: an incident is a detected condition (“latency elevated”), not yet a cause (“Wi-Fi signal caused it”).

### 12. Read `RootCauseEngine`

Start with the candidate/report schema, then one correlation rule. Trace supporting, contradicting, and missing evidence into confidence/ranking. Confirm the engine can abstain and can return alternatives.

Use three contrasting fixtures:

- a link/route problem affecting many sockets;
- one impaired endpoint while other sockets are healthy;
- insufficient collector visibility.

Then follow `ProbeObservation` through the engine's bounded per-namespace
cache and `WifiObservation` through its bounded per-interface cache. Check the
15-second probe and ten-second Wi-Fi monotonic freshness policies, gateway
target/interface matching, remote target replacement, current Wi-Fi interface
identity, RSSI hysteresis, and the distinction between `Timeout` and
`TransportUnavailable`. Compare these combinations:

- TCP degradation plus normal gateway and degraded configured remote;
- TCP degradation plus degraded gateway;
- TCP degradation plus normal gateway and normal configured remote.

The configured remote is path context, not the socket endpoint. Confirm that
no timeout ratio or retry percentage is calculated. A fresh authoritative
Wi-Fi `NotAssociated` observation can refine active path degradation into a
Medium-capped `LocalLinkSuspected`; weak RSSI requires gateway degradation and
never opens a hypothesis alone. Associated/normal Wi-Fi can strengthen the
relative beyond-gateway interpretation, while missing or stale Wi-Fi remains
missing evidence.

You should be able to reproduce every result from the rule version and cited event/sample IDs without consulting an LLM.

### 13. Read application services and D-Bus last among core modules

Now read the service facade and D-Bus introspection XML. Map each method/signal to a snapshot/query rather than collector internals.

Check:

- version and generation fields;
- stable error names;
- array/time-range/string limits;
- authorization and bus policy;
- system-bus versus private test-bus setup;
- incident/root-cause signal schema;
- service restart behavior;
- V1 compatibility adapter and C ABI translation.

Read contract tests beside the XML. Then use `weaknetctl` to follow the same structured result.

### 14. Read the WeakNet Lab

Read fixture creation and cleanup before scenarios. Verify that names, namespaces, qdiscs, and processes are uniquely scoped and always cleaned.

For one scenario, trace:

```text
topology setup
  -> traffic starts
  -> tc/netem fault is applied
  -> collector observations
  -> NetworkEvents / metrics
  -> incident transition
  -> root-cause candidates
  -> weaknetctl assertion
  -> cleanup
```

Scenario assertions should target structured schemas/evidence, not logs or prose.

### 15. Read benchmarks and evaluation artifacts

Read benchmark source before results. Verify workload, warmup, sample count, clock, environment metadata, correctness assertions, and raw artifact linkage.

Keep these separate:

- unit/integration correctness;
- performance microbenchmarks;
- end-to-end stress/soak;
- deterministic diagnosis evaluation;
- optional AI evaluation.

A number in a README without its run artifact is an example, not evidence.

### 16. Read optional AI/RAG code last

Begin with `docs/AI_V2_ARCHITECTURE.md` and `ai/v2`: the versioned
`DiagnosisSnapshot`, pure D-Bus-shaped adapter, deterministic prompt builder,
provider protocol, fake provider, and grounding validator. The V2 explainer
consumes structured diagnosis only and cannot change deterministic type or
confidence. It makes zero external LLM calls in this stage.

The older `optional/experimental/log-analysis-tools` directory is legacy V1
raw-log/RAG experimentation. It is intentionally not routed through AI V2 and
may contain optional DashScope/OpenAI, embeddings, and FAISS dependencies.
Read it only after the V2 contract. Do not begin with provider SDK code.

Verify that:

- the package consumes structured exported bundles rather than scraping daemon logs;
- evidence citations refer to real bundle IDs;
- core facts and model inference are visibly separated;
- provider failure leaves deterministic C++ reports available;
- secrets and network access are optional/configured;
- prompt-injection and sensitive-data fixtures exist;
- evaluation scores have recorded runs.

## Three end-to-end traces to perform

### Trace A: route switch

1. Find the lab route-switch scenario.
2. Follow rtnetlink messages into topology events.
3. Observe selected-uplink state and MetricStore updates.
4. Follow incident open/update/resolve.
5. Inspect root-cause route evidence.
6. Find the D-Bus signal and `weaknetctl` representation.

### Trace B: socket retransmission

1. Start from controlled TCP traffic in the lab.
2. Follow SocketTracker identity and TCP_INFO deltas.
3. Correlate any eBPF retransmission event.
4. Confirm metric scope/denominator validity.
5. Compare endpoint-specific versus interface-wide evidence.
6. Read the incident and ranked cause candidates.

### Trace C: collector degradation

1. Run/inspect the eBPF-disabled or permission-denied test.
2. Follow `CollectorHealthEvent` publication.
3. Confirm missing traffic evidence is unavailable, not zero.
4. Observe any observability-gap incident/candidate.
5. Query capability status through `weaknetctl`.

This trace is as important as a successful collection path.

## V1-to-V2 orientation map

| V1 concept/path | V2 place to study |
|---|---|
| `server.cpp` orchestration | `weaknetd` application composition/lifecycle |
| `net_iface.cpp` + `using_iface.cpp` | unified `NetlinkCollector` and uplink policy |
| `net_tcp.cpp` | `SocketTracker` sock_diag/TCP_INFO adapter |
| `flow_rate.bpf.c` + `net_traffic.cpp` | BPF target, generated skeleton, `EbpfCollector` |
| `net_ping.cpp` | `ProbeCollector` |
| `net_wifiriss.cpp` | `WifiCollector` fallback adapter |
| `NetInfo` | typed topology entities plus MetricStore samples |
| `NetworkEventManager` | unified `NetworkEvent` plus `EventBus` |
| `NetworkQualityAssessor` | validity-aware health rules and inputs to IncidentEngine |
| `DbusService` | versioned D-Bus adapter/application services |
| `libweaknet.so` | temporary compatibility client over V2 API |
| `test-client` | `weaknetctl` plus real CTest/lab targets |
| Python log parsers | structured IncidentBundle consumer/evaluation fixtures |

## Common reading mistakes

- Treating a successful BPF attachment as proof that fields are correctly attributed.
- Treating a quality score as a root cause.
- Treating no sample as zero.
- Treating an interface name as globally unique.
- Reading D-Bus handlers as the source of truth instead of adapters over application services.
- Reading optional AI before understanding deterministic evidence.
- Trusting log wording or historical example output over structured tests.
- Skipping shutdown and degraded-mode code; those paths define production reliability.

## Suggested first contributions after learning

Good starter changes are bounded and test-first:

- add a missing parser fixture for an already supported netlink/TCP_INFO message;
- add a MetricStore validity/retention edge-case test;
- add a `weaknetctl` error/exit-code contract test;
- improve one lab cleanup failure path;
- add documentation for an existing cause rule with links to its structured tests;
- add an optional-AI redaction/evidence-citation fixture without changing the core.

Avoid beginning with a new collector, changing event schema, altering root-cause confidence, or adding a model provider until the corresponding contracts and tests are understood.
