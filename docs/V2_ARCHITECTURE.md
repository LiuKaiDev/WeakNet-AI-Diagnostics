# WeakNet V2 architecture

## Status

This document is the proposed V2 architecture. It defines component boundaries and migration constraints; it does not claim that these components are implemented yet. The observed V1 baseline is in `V1_AUDIT.md`, and implementation order is in `V2_ROADMAP.md`.

## Design goals

WeakNet V2 is a C++20/Linux network-observability and root-cause-diagnosis system with these properties:

- complete collection, incident detection, diagnosis, local query, and explanation without Python or AI;
- Linux-native data collection through libbpf CO-RE, rtnetlink, sock_diag/`TCP_INFO`, and optional active probes;
- explicit metric scope, validity, provenance, timestamps, and network-namespace identity;
- bounded resources and deterministic behavior under overload;
- graceful startup, degradation, reconfiguration, and shutdown;
- a versioned D-Bus API served by `weaknetd` and consumed by `weaknetctl`;
- reproducible fault scenarios in disposable network namespaces with `tc`/netem;
- evidence-backed incident and root-cause results that can be tested without an LLM;
- an optional, unprivileged LLM diagnostic layer that consumes a redacted structured bundle and cannot control the core.

## Non-goals

- Replacing kernel collectors with shell commands or Python.
- Capturing packet payloads by default.
- Claiming that retransmissions, RTT, or traffic alone prove a root cause.
- Requiring a cloud service, Python runtime, model, vector database, or Internet access for core operation.
- Rewriting every V1 module at once.
- Promising benchmark performance before a reproducible benchmark has been run.

## System context

```text
                         optional, unprivileged
                    +-----------------------------+
                    | Python diagnostic agent     |
                    | RAG / report / evaluation   |
                    +--------------^--------------+
                                   | redacted, versioned
                                   | incident bundle (read only)
                                   |
+-------------+       system D-Bus |       +---------------------------+
| weaknetctl  | <------------------------> | weaknetd (C++20)          |
+-------------+                            |                           |
                                           |  D-Bus adapter            |
                                           |  RootCauseEngine          |
                                           |  IncidentEngine           |
                                           |  MetricStore              |
                                           |  EventBus                 |
                                           |  SocketTracker            |
                                           |  Linux collectors         |
                                           +------------+--------------+
                                                        |
                         +------------------------------+------------------+
                         | rtnetlink | sock_diag/TCP_INFO | eBPF | probes |
                         +------------------------------+------------------+
                                                        |
                                                     Linux kernel

WeakNet Lab: isolated netns + veth + tc/netem drives the same collectors and APIs.
```

## Process and language boundary

### Core process: `weaknetd`

`weaknetd` contains all required production behavior:

- capability discovery and collector lifecycle;
- link, route, socket, TCP, active-probe, Wi-Fi, and eBPF observations;
- normalization into `NetworkEvent`;
- bounded event delivery;
- time-series/latest-value storage;
- incident state machines;
- deterministic root-cause candidates with evidence;
- D-Bus query, status, and signal APIs;
- structured logs and health/degraded-capability reporting.

No Python interpreter, Python subprocess, model client, or RAG package is loaded or launched by `weaknetd`.

### Core client: `weaknetctl`

`weaknetctl` is an unprivileged C++20 command-line client. It queries status, interfaces, sockets/flows where authorized, metrics, active incidents, and root-cause reports. It can follow versioned signals and request an export bundle. It is not a test runner, although integration tests may invoke it.

### Compatibility library

The existing C ABI may remain as a separately versioned compatibility target during migration. It should translate legacy calls onto the V2 D-Bus API, report truncation/errors accurately, and have an explicit deprecation policy. It must not dictate the internal V2 model.

### Optional Python layer

Python is limited to:

- retrieval over reviewed diagnostic knowledge;
- generation of an advisory human-readable report from an exported incident bundle;
- AI-specific evaluation and dataset tooling.

It does not collect kernel data, mutate network state, determine core incident state, or provide a dependency of `weaknetd`/`weaknetctl`.

## C++ core layers

### 1. Platform foundation

Common facilities should be small and independently testable:

- `Status`/error categories with stable machine codes and human context;
- typed IDs (`InterfaceId`, `SocketId`, `NetnsId`, `IncidentId`, event sequence);
- injected realtime and monotonic clocks for deterministic tests;
- configuration parsing and validation;
- capability/feature detection;
- structured logging fields and rate limiting;
- bounded queue and shutdown primitives;
- shared kernel/user ABI definitions with layout assertions.

The code should use RAII for file descriptors, libbpf objects/links, D-Bus objects, threads, and temporary namespace/lab resources.

### 2. Collectors

Collectors are Linux adapters. Each collector publishes observations and health events; it does not update incidents, call D-Bus, or own cross-collector policy.

#### `NetlinkCollector`

Responsibilities:

- subscribe to link, address, neighbor (when needed), and route changes;
- take initial and periodic reconciliation dumps;
- validate netlink sender, sequence, multipart completion, errors, truncation, and dump interruption;
- model IPv4/IPv6 routes, table, priority, protocol, scope, type, multipath, and on-link defaults;
- expose interface ifindex/name/type/state and selected-route evidence per network namespace;
- coalesce event bursts without losing the final reconciled state.

One parser/transport should serve both snapshots and notifications. Uplink selection is a deterministic policy over route facts rather than a second netlink implementation.

#### `SocketTracker`

Responsibilities:

- enumerate TCP sockets with sock_diag and request `INET_DIAG_INFO`;
- maintain a stable best-effort socket identity using network namespace plus socket cookie when available, otherwise a documented tuple/generation fallback;
- track family, endpoints, state, bound/observed interface, UID, cgroup/process attribution where permitted, and TCP_INFO availability;
- calculate deltas only from compatible monotonic fields for the same socket generation;
- handle socket creation/close, counter reset/wrap, partial visibility, permission denial, and missing kernel fields;
- publish socket lifecycle and timestamped TCP metric events;
- reconcile eBPF lifecycle/flow observations with periodic sock_diag snapshots.

“Loss” must be named and scoped precisely. For example, a retransmission ratio can be `delta_total_retrans / delta_data_segs_out` only when both compatible counters are available for the same interval and population. Otherwise the metric is unavailable, not zero.

#### `EbpfCollector`

Responsibilities:

- load a generated libbpf skeleton built from CO-RE source;
- select supported attachment strategies by detected kernel capability;
- observe the minimum events needed to complement netlink and SocketTracker, such as socket lifecycle, retransmission, and flow byte/packet counters;
- include direction, address family, ifindex where valid, network namespace/cgroup/socket identity where available, and event timestamps;
- consume ring-buffer/perf-buffer events and read maps without blocking unrelated state;
- report verifier/load/attach/map/ring-loss status and degrade cleanly;
- own and destroy all links, maps, buffers, and objects on stop.

Kernel tracepoints or stable BTF-based fentry/fexit attachments should be preferred where they satisfy the observation. Kprobes are capability-gated fallbacks, not assumed portable. The design must test the semantics of any interface/process attribution rather than infer it from a convenient field.

The eBPF program must not collect application payloads. Map sizes and event rates are configurable and bounded.

#### `ProbeCollector`

Optional active measurements run outside core state locks:

- IPv4/IPv6 ICMP or another explicitly configured probe type;
- interface/source binding and a configured target set;
- correct request/reply matching and monotonic RTT measurement;
- rate limits, timeouts, cancellation, and privilege status;
- separation of target reachability from link state.

Active probes supplement passive evidence; a target outage must not be diagnosed as a local link failure without corroboration.

#### `WifiCollector`

Use nl80211 for link/signal facts where available. A wpa_supplicant control socket may be a read-only fallback. The daemon must not start or reconfigure wpa_supplicant. Wired interfaces carry “not applicable” RSSI, not a sentinel that affects quality.

### 3. Unified `NetworkEvent`

All collectors and engines exchange one envelope with typed payloads. A proposed conceptual schema is:

```cpp
struct NetworkEventHeader {
    std::uint16_t schema_version;
    EventId event_id;
    std::uint64_t sequence;
    EventKind kind;
    EventSource source;
    std::chrono::system_clock::time_point observed_at;
    std::chrono::steady_clock::time_point monotonic_at;
    NetnsId netns;
    std::optional<InterfaceId> interface;
    std::optional<SocketId> socket;
    Validity validity;       // valid, unavailable, stale, partial, reset
    std::optional<Status> status;
};

using NetworkEventPayload = std::variant<
    LinkEvent,
    AddressEvent,
    RouteEvent,
    UplinkEvent,
    SocketLifecycleEvent,
    TcpMetricEvent,
    FlowMetricEvent,
    ProbeMetricEvent,
    WifiMetricEvent,
    CollectorHealthEvent,
    IncidentEvent,
    RootCauseEvent>;

struct NetworkEvent {
    NetworkEventHeader header;
    NetworkEventPayload payload;
};
```

Required semantics:

- unit and scope are encoded by the typed payload;
- unknown/unavailable/stale/reset are distinct from numeric zero;
- realtime supports human correlation, monotonic time supports durations/order;
- namespace is never inferred from interface name alone;
- schema changes are additive where possible and versioned at external boundaries;
- raw pointers and unbounded JSON blobs are not part of the internal event contract.

### 4. `EventBus`

`EventBus` is an in-process delivery mechanism, not D-Bus.

Recommended behavior:

- a bounded multi-producer input queue;
- one owned dispatcher thread to establish a documented publication order;
- subscription tokens with RAII unsubscribe;
- filters by event kind/scope;
- callbacks invoked without registry locks held;
- explicit policies for full queues (for example, preserve state/incident events, aggregate replaceable gauges, count all drops);
- per-source sequence/drop/lag metrics;
- no detached worker per event.

Collectors publish immutable values. Slow consumers must not stall kernel polling indefinitely. Expensive consumers receive work through their own bounded queues or snapshot APIs.

### 5. `MetricStore`

`MetricStore` is a bounded in-memory store for latest values and recent windows. It is not the D-Bus layer and it is not the experimental `.bin` file.

Data key dimensions include:

- network namespace;
- interface/socket/flow identity as appropriate;
- metric name and unit;
- source/collector;
- observation interval.

Each sample includes value, validity, observed time, monotonic time, and provenance. The store offers immutable snapshots and time-window queries. Retention, cardinality, and memory limits are configurable and observable. Updates are short critical sections; collectors perform I/O and delta calculation before committing a sample.

Persistence is optional. If added, it uses a versioned schema, size limits, atomic replacement/transactions, and explicit retention. Core correctness cannot depend on a writable current working directory.

### 6. `IncidentEngine`

The incident engine is deterministic C++. It consumes normalized events and metric windows and runs explicit state machines such as:

- interface/link unavailable;
- default-route/uplink change or route flap;
- latency elevation relative to configured and learned baseline;
- retransmission elevation with a valid denominator;
- Wi-Fi signal degradation;
- collector blind spot/degraded observability;
- traffic saturation/anomaly only when capacity/baseline evidence exists.

An incident has an ID, type, scope, opened/updated/resolved times, state, severity, confidence, contributing observations, missing-evidence flags, rule version, and deduplication key. Hysteresis, minimum sample count, cooldown, and stale-data handling are part of each rule. Reprocessing the same ordered event stream with the same configuration must yield the same incident transitions.

### 7. `RootCauseEngine`

The root-cause engine is also deterministic C++. It correlates incidents and evidence across time and topology, then returns ranked candidates rather than an unsupported single answer.

A candidate contains:

- a stable cause code, such as `local_link_down`, `wifi_signal`, `route_change`, `path_latency`, `tcp_retransmission`, `remote_endpoint`, or `observability_gap`;
- confidence and severity computed by a versioned rule;
- supporting and contradicting event/sample IDs;
- scope and time window;
- missing evidence and alternative candidates;
- recommended next deterministic checks.

Example reasoning pattern:

```text
route change + many sockets impacted + probes fail on old uplink
    -> high-confidence local route/uplink candidate

one socket retransmits + other sockets and interface probes are healthy
    -> endpoint/path-specific candidate, not global network failure

eBPF unavailable + no TCP_INFO visibility
    -> observability-gap incident, never “0% loss”
```

Rules should be data-driven where useful but compiled/executed by the C++ engine with schema validation. Every result must be explainable without AI.

### 8. Application services and D-Bus adapter

Application services expose immutable snapshots and commands to adapters. D-Bus code must not reach into collector internals or hold MetricStore locks while marshaling.

#### Proposed V2 endpoint

- Bus: system bus for production; an explicitly selected private/session bus for tests.
- Name: `org.weaknet.WeakNet1`
- Root path: `/org/weaknet/WeakNet1`
- Manager interface: `org.weaknet.WeakNet1.Manager`

The exact XML is delivered and contract-tested in its roadmap milestone. At minimum, it should provide:

- daemon/API version and capability/degraded status;
- interface and collector snapshots;
- bounded metric queries;
- active/recent incident queries;
- root-cause report queries;
- a redacted incident export method;
- versioned incident/status signals.

Rules:

- publish introspection XML and stable error names;
- bound array sizes, time ranges, strings, and response work;
- do not perform raw probes or long collection synchronously in a generic query handler;
- apply D-Bus policy and, where needed, polkit authorization to active operations or sensitive data;
- rate-limit subscriptions/exports and expose service restart/generation;
- keep a temporary `com.example.WeakNet` compatibility adapter only while required and test it explicitly.

## Threading and shutdown model

The target model uses owned, cancellable workers:

| Execution context | Responsibility |
|---|---|
| Main/application thread | Construct components, install signal handling, start in dependency order, wait, stop in reverse order. |
| D-Bus event context | Parse/bound requests and marshal snapshots; never run long collectors. |
| Netlink collector worker | Blocking poll/receive plus reconciliation timer. |
| eBPF worker | Ring-buffer polling/map maintenance. |
| SocketTracker worker | Scheduled sock_diag reconciliation and TCP_INFO delta production. |
| Probe worker/pool | Bounded cancellable active probes. |
| EventBus dispatcher | Ordered fan-out and drop accounting. |
| Engine worker or dispatcher-owned state | Deterministic incident/root-cause state mutation in one serialized context. |

Prefer `std::jthread` with `std::stop_token`. Every blocking syscall has a wakeup/cancellation strategy (eventfd, finite poll timeout, fd close, or library-specific stop). No detached thread may retain application objects. Shutdown has a deadline and is tested under idle and busy collectors.

Startup is transactional: if a required component fails, already-started components stop in reverse order. Optional collector failure produces a `CollectorHealthEvent` and explicit degraded capability.

## Privilege and deployment model

`weaknetd` runs under systemd with the least capabilities demonstrated by the supported kernel matrix. Candidate capabilities may include `CAP_BPF`, `CAP_PERFMON`, `CAP_NET_ADMIN`, and `CAP_NET_RAW`; older kernels may require broader fallback. The final set must be measured per feature and documented, not assumed.

Deployment should include:

- a dedicated service account where feasible;
- `CapabilityBoundingSet`/`AmbientCapabilities` scoped to enabled collectors;
- `NoNewPrivileges`, filesystem protections, private temporary space where compatible, and syscall/address-family restrictions tested against libbpf/netlink;
- system-bus policy and service activation decision;
- explicit writable state/log directories, not repository-relative paths;
- startup capability report and systemd watchdog/readiness integration if selected.

If privileges are insufficient, the daemon remains useful with supported collectors and reports exactly what is missing. It must not silently convert missing visibility to healthy zeroes.

## WeakNet Lab

The lab is a reproducible integration environment, not a replacement implementation. Shell/CMake/Python test orchestration may create disposable namespaces, while observations and diagnoses continue to come from the C++ daemon and Linux/eBPF collectors.

Baseline topology:

```text
client netns -- veth -- router netns -- veth -- server netns
                         |
                         `-- tc/netem fault injection
```

Scenario definitions specify topology, kernel prerequisites, traffic generator, fault parameters, expected event/incident evidence, time bounds, and cleanup. Initial scenarios should cover:

- healthy baseline;
- fixed latency and jitter;
- packet loss and burst loss;
- reordering/duplication where supported;
- bandwidth constraint/queue buildup using appropriate qdiscs;
- link down/up;
- default-route switch/flap;
- IPv4 and IPv6;
- multiple simultaneous sockets with one endpoint impaired;
- eBPF unavailable or capability denied;
- namespace/interface-name collisions.

Tests assert structured API output and event evidence, not localized log strings. Cleanup is idempotent and limited to uniquely named test namespaces/qdiscs.

## Optional LLM diagnostic layer

### Input contract

The core exports a versioned `IncidentBundle` containing:

- incident and deterministic root-cause candidates;
- referenced, bounded metric/event evidence;
- topology and capability context;
- rule/schema versions;
- missing/contradictory evidence;
- redaction metadata.

It excludes packet payloads and redacts/hashes addresses, hostnames, PIDs, UIDs, cgroups, and interface labels according to policy. Export is explicit, auditable, size-bounded, and safe to store locally.

### Agent behavior

The optional agent may retrieve reviewed operational knowledge and produce a human explanation or propose additional checks. It must:

- cite incident/event/sample IDs from the bundle for factual claims;
- label inference and uncertainty;
- never override the core incident/root-cause result in storage;
- never invoke privileged network changes through `weaknetd`;
- operate offline with deterministic fallback messaging when no model is configured;
- enforce provider timeouts, cost/token limits, secret loading from environment/secret stores, and data-handling policy;
- treat bundle/log text as untrusted input and resist prompt injection.

### Evaluation

AI evaluation is separate from core correctness. A versioned dataset maps lab/curated incident bundles to acceptable cause codes, required evidence citations, forbidden unsupported claims, and redaction expectations. Report retrieval relevance, cause-code accuracy/top-k coverage, evidence-groundedness, abstention, latency, and cost only from recorded runs. Never invent a score.

## Observability of WeakNet itself

The daemon should expose:

- enabled/disabled/degraded collectors and reasons;
- last successful sample and reconciliation time;
- event queue depth, high-water mark, drops/coalesces, and consumer lag;
- MetricStore cardinality, memory, eviction, and stale samples;
- incident counts/transitions and rule version;
- D-Bus request counts/errors/latency buckets with bounded labels;
- eBPF verifier/attach status, ring-buffer loss, and map pressure;
- shutdown duration and stuck component diagnostics.

These internal health metrics must not be confused with observed network metrics.

## Build and source organization

The target build is CMake with explicit production, BPF, test, lab, and optional Python boundaries. A possible layout is illustrative, not yet implemented:

```text
CMakeLists.txt
cmake/
src/
  daemon/           weaknetd composition
  cli/              weaknetctl
  core/             NetworkEvent, EventBus, MetricStore
  collectors/       netlink, socket, probe, wifi
  ebpf/             userspace loader/adapters
  incidents/        IncidentEngine
  root_cause/       RootCauseEngine
  ipc/              D-Bus adapter and generated contract data
bpf/                CO-RE programs and shared ABI
include/weaknet/
tests/unit/
tests/integration/
lab/
optional/ai/
benchmarks/
```

Likely C++ dependencies are the standard library, threads, libdbus-1 (initially preserved to minimize churn), libbpf and its transitive ELF/zlib requirements, and a tested serialization/configuration choice. Adding a large framework requires a concrete benefit and migration plan.

## Compatibility and migration

V2 should be introduced behind V1-compatible surfaces in small steps:

1. Characterize current externally useful behavior.
2. Establish CMake/C++20 without changing behavior.
3. Add owned lifecycle and the new event/metric foundation alongside V1.
4. Migrate one collector at a time through adapters.
5. Introduce new versioned D-Bus names while keeping a compatibility adapter.
6. Move the CLI/library onto V2, then retire V1 internals only after parity tests pass.
7. Add deterministic engines and lab evidence before optional AI integration.

No milestone should require a simultaneous replacement of daemon, collectors, IPC, client, and tests.

## Architecture invariants

The following are release-blocking invariants:

1. With the optional Python directory removed, the C++ build, daemon, CLI, lab core assertions, incidents, and root-cause reports still work.
2. A missing collector produces explicit unavailable/degraded state, never a healthy zero.
3. Every incident and root-cause candidate refers to structured evidence and a rule version.
4. No long-lived detached threads exist in the core.
5. Network namespace is part of object identity.
6. Collector I/O and sampling sleeps do not occur under shared state-store locks.
7. eBPF resources and network-lab resources are released on normal and failed startup/shutdown.
8. D-Bus and export inputs/outputs are versioned and bounded.
9. Benchmarks and AI evaluation report only captured results with reproducible metadata.
10. V1 behavior is removed only after its replacement has acceptance evidence or an explicit deprecation decision.
