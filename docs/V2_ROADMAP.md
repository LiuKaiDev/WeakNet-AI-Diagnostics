# WeakNet V2 roadmap

## Roadmap rules

This roadmap migrates V1 incrementally. A milestone's recorded completion does not authorize work on any later milestone; phase authorization remains task-specific.

Every phase must satisfy these standing gates:

- the C++ core builds and works without Python, AI credentials, or Internet access;
- no useful V1 behavior is removed before replacement/compatibility tests pass or an explicit deprecation is approved;
- missing privileges/capabilities are reported as degraded, not converted to zero metrics;
- privileged tests run only in disposable network namespaces or clearly documented isolated hosts;
- test processes return nonzero on assertion failure;
- no benchmark or evaluation number is published without stored raw output and environment/workload metadata;
- documentation labels proposed behavior separately from implemented behavior.

## Phase 0: V1 audit and V2 design baseline

Status: documentation phase represented by this document set.

Deliverables:

- repository guidance in `AGENTS.md`;
- source-level V1 audit;
- target V2 architecture;
- incremental roadmap;
- post-implementation learning guide.

Acceptance criteria:

- all tracked areas are accounted for, including build, daemon, collectors, D-Bus, eBPF, client, tests, and optional AI;
- core/AI boundaries and “no full rewrite” migration strategy are explicit;
- risks and unverified assumptions are labeled;
- no V2 functional code is introduced.

Validation:

- clean Markdown/diff inspection;
- shell/Python syntax inspection where tools are available;
- record why a native build could or could not run.

## Phase 1: Reproducible baseline and CMake/C++20 foundation

Status: implemented in the current working tree; validated on the documented Ubuntu 24.04 x86_64 WSL2 environment with GCC and Clang. This status covers build/test foundations only and does not authorize or imply Phase 2 work.

Objective: make the existing product build and fail predictably before changing its architecture.

Scope:

- add top-level CMake targets for the existing daemon, client library, CLI/manual tool, and BPF object;
- select C++20 and declare compiler/kernel/library prerequisites;
- add options for eBPF and tests with explicit required/optional behavior;
- generate or acquire `vmlinux.h` through a documented path and map target architecture correctly;
- preserve existing Make entry points temporarily as thin compatibility wrappers if needed;
- add install layout, runtime path configuration, and compile commands;
- introduce CTest with a minimal deterministic smoke/unit target;
- capture a baseline of V1 public D-Bus signatures and C ABI symbols;
- fix only blockers needed to compile/test the baseline, with each fix separately characterized.

Acceptance criteria:

- clean configure/build succeeds on each declared reference environment with Python absent;
- `weaknet-dbus-server`, `libweaknet`, and the existing CLI are produced by named CMake targets;
- eBPF-enabled and explicitly disabled configurations have clear configure results;
- no path requires a checked-in host-specific generated file;
- examples selected as supported compile, or unsupported examples are clearly excluded/documented;
- CTest reports real failures through exit status;
- V1 D-Bus signature and exported-C-symbol snapshots are stored as contract fixtures.

Tests:

- configure matrix: eBPF on/off, tests on/off, supported compilers/architectures;
- compilation with warnings enabled and a documented warnings-as-errors policy for project code;
- C ABI symbol comparison;
- D-Bus signature fixture test without privileged collectors;
- deterministic serializer and quality-assessor smoke tests;
- `bash -n`/static checks for retained scripts.

Out of scope:

- renaming the daemon, new event bus, collector rewrite, incident logic, system-bus migration, or AI work.

## Phase 2: Runtime ownership, shutdown, and health

Status: implemented in the current working tree. The V1 session-bus/API and collector semantics remain in place; the implementation adds owned lifecycle, bounded Ping dispatch, direct runtime health, resource cleanup, and non-CWD runtime paths only.

Objective: make the V1 daemon lifecycle safe without changing collector semantics.

Scope:

- introduce an RAII application/composition object;
- replace detached long-lived monitor and signal threads with owned `std::jthread` workers and stop tokens;
- implement SIGINT/SIGTERM stop propagation and reverse-order teardown;
- add RAII wrappers for FDs, D-Bus connection/registration, and existing libbpf links/object;
- remove per-signal thread creation; serialize or queue signal output;
- add startup rollback and collector health/degraded status;
- move blocking Ping execution off the sole D-Bus dispatch context or make it an explicit bounded asynchronous operation;
- replace CWD-dependent runtime paths with validated configuration/state directories.

Acceptance criteria:

- idle and active daemon runs stop within a documented deadline on SIGTERM;
- no long-lived detached thread remains;
- partial startup failure releases all already-acquired resources;
- D-Bus and BPF resources detach/unref on shutdown;
- current V1 method results remain compatible unless an approved bug fix is documented;
- thread/race sanitizers pass the supported non-BPF test subset, with suppressions reviewed.

Tests:

- lifecycle unit tests with fake workers and injected failure at each startup step;
- process tests for SIGINT/SIGTERM during idle, active request, and collector failure;
- repeated start/stop and service-restart tests;
- FD/thread count leak checks;
- concurrent D-Bus request/shutdown test;
- sanitizer jobs for core ownership and client lifetime.

## Phase 3: Unified `NetworkEvent`, `EventBus`, and `MetricStore`

Status: implemented in the current working tree as an additive internal data plane. V1 remains authoritative for the existing D-Bus/C ABI surface. See `PHASE3_DATA_PLANE.md` for exact ordering, bounds, validity, mirroring, and validation semantics. This status does not authorize Phase 4 work.

Objective: establish the V2 internal data plane alongside V1 state.

Scope:

- define typed IDs, validity/status, realtime/monotonic timestamps, namespace identity, event header, and typed payloads;
- implement a bounded multi-producer `EventBus` with subscription tokens and drop/coalesce telemetry;
- implement bounded `MetricStore` latest-value and time-window queries;
- add adapters that mirror V1 observations into events/metrics while V1 D-Bus output still reads its existing model;
- define retention/cardinality limits and immutable snapshot semantics;
- introduce injected clock support and schema/version tests.

Acceptance criteria:

- every mirrored sample distinguishes valid, unavailable, stale, partial, and reset states;
- event ordering is specified and verified for one and multiple producers;
- callbacks execute outside registry/store locks;
- queue saturation follows the documented policy and exposes exact drop/coalesce counters;
- MetricStore never treats missing data as numeric zero;
- V1-facing behavior remains available through the compatibility path.

Tests:

- event schema round-trip and forward-compatible fixture tests;
- concurrent publish/subscribe/unsubscribe/reentrant-callback tests;
- bounded-queue saturation and priority/coalescing tests;
- MetricStore retention, eviction, staleness, namespace, and snapshot-consistency tests;
- deterministic clock/time-window tests;
- ThreadSanitizer stress test for bus/store.

## Phase 4: Netlink topology collector and uplink state

Status: implementation is present in the current working tree as one active Phase 4 collector layered on the Phase 3 data plane; final acceptance remains evidence-gated because privileged namespace integration and TSan execution may be supported skips. V1 interface-list/current-uplink compatibility is backed by its reconciled snapshot; the old listener is no longer started. See `PHASE4_NETLINK.md` for exact parser, reconciliation, policy, and validation semantics.

Objective: replace duplicated V1 route/link discovery with one correct collector.

Scope:

- build one netlink transport/parser for initial dumps, notifications, and reconciliation;
- model link, address, route table, route priority, on-link defaults, IPv4/IPv6, and multipath;
- include network namespace and ifindex in identity;
- implement deterministic selected-uplink policy with explicit evidence;
- adapt legacy interface listing/current-uplink behavior to the new state;
- remove V1 snapshot/listener implementations only after parity tests.

Acceptance criteria:

- no immediate-`EAGAIN` empty-dump behavior;
- sequence/sender validation, multipart completion, `NLMSG_ERROR`, truncation, and interrupted dumps are handled;
- route add/delete multiplicity and link rename/delete are correct;
- selection is deterministic for equal/multiple routes and documented for policy routing limitations;
- interface names with valid punctuation work;
- V1 interface-list compatibility tests pass for covered behavior.

Tests:

- parser fixtures for links, addresses, routes, errors, truncation, multipath, and delete cases;
- fake netlink transport unit tests for dump/reconcile races;
- namespace integration tests for link add/remove/rename, route add/remove/metric changes, IPv4/IPv6, and multiple namespaces;
- event sequence and MetricStore snapshot assertions;
- lab cleanup/idempotence test.

## Phase 5: SocketTracker and trustworthy TCP metrics

Status: Phase 5.1 through the authorized Phase 5.4 modeled route-attribution
increment are implemented. Socket identity, transactional IPv4+IPv6
NETLINK_SOCK_DIAG snapshots, variable-size TCP_INFO parsing, and same-generation
interval metrics are present. TCP state and `idiag_if` remain evidence, not
guaranteed routed-egress attribution. Phase 5.4 adds deterministic attribution
from the committed Phase 4 topology model.

Objective: turn existing sock_diag/TCP_INFO inspection into stable per-socket observability.

Scope:

- define `SocketId` and socket generation rules using netns/socket cookie where supported;
- enumerate IPv4/IPv6 TCP sockets and maintain lifecycle state;
- record TCP_INFO field availability per kernel and calculate only valid compatible deltas;
- retain TCP_INFO field availability across kernel payload sizes;
- reset baselines on socket/generation changes and counter resets;
- publish socket lifecycle/TCP metric events and store bounded history;
- retire the V1 aggregate “TCP loss” implementation after compatibility output is mapped to valid data or marked unavailable.

Phase 5.4 additions:

- match socket destinations against committed modeled IPv4/IPv6 routes using
  longest prefix and Phase 4 table/metric policy;
- retain route/interface/gateway, multipath, source-address, `diag_ifindex`,
  selected-uplink, authority, and ambiguity evidence as a typed context;
- recompute active contexts after committed topology changes without a second
  per-socket route query.

The result is explicitly not Linux policy-routing completeness: `ip rule`,
fwmark, source-policy routing, VRF semantics, custom policy table selection,
and ECMP flow hashing remain out of scope.

Acceptance criteria:

- opening/closing/reusing tuple sockets does not create negative or cross-generation deltas;
- unavailable denominators yield unavailable metrics;
- interface/global/socket scopes are labeled and never mixed in one ratio;
- IPv4/IPv6 and namespace-identical tuples remain distinct;
- partial permission visibility is reported;
- legacy health output no longer presents invalid precision as authoritative.

Tests:

- recorded sock_diag/TCP_INFO fixtures for field variants and malformed messages;
- counter delta/reset/wrap and socket-generation unit tests;
- namespace tests with multiple TCP flows, tuple reuse, and one impaired endpoint;
- comparison assertions against controlled application byte/counter behavior (with tolerances defined before running);
- permission-denied/degraded tests.

## Phase 6: libbpf CO-RE observability

Objective: replace the V1 eBPF experiment with portable, owned, attributable observations.

Scope:

- move BPF code to a dedicated target and generate a libbpf skeleton;
- share versioned kernel/user structs with layout assertions;
- remove hard-coded x86 and support the declared architecture/kernel matrix;
- choose tracepoint/fentry/fexit/kprobe attachments by capability and semantic tests;
- add IPv6 and the required ingress/egress/socket lifecycle/retransmission observations;
- include namespace/socket/interface/cgroup identity where valid;
- bound maps/ring buffers and expose loss/pressure;
- integrate events with SocketTracker rather than maintaining a competing flow truth;
- implement explicit load/attach failure degradation and cleanup.

Acceptance criteria:

- BPF object and skeleton are reproducibly generated by CMake;
- all supported architectures use correct target macros;
- verifier/attachment status is visible through daemon health;
- interface attribution tests prove behavior for TCP and UDP or mark it unavailable;
- no payload collection occurs;
- all links/maps/buffers are released on normal stop and failed startup;
- the daemon remains operational with eBPF disabled or denied.

Tests:

- compile/BTF/CO-RE relocation checks across the declared matrix;
- shared ABI size/offset static assertions;
- privileged namespace tests for TCP/UDP, IPv4/IPv6, ingress/egress, multiple interfaces/namespaces, retransmission, and map pressure;
- ring-buffer loss and bounded-overload tests;
- load denial, unsupported attachment fallback, and no-BTF behavior tests;
- repeated attach/detach leak checks.

## Phase 7: Remaining collectors and validity-aware quality

The first Wi-Fi evidence slice is implemented: `WifiCollector` uses native
Generic Netlink/nl80211 with typed optional fields, signed RSSI parsing,
exact-kbps bitrate conversion, bounded polling, selected-interface identity,
and degraded capability states. It is additive to the V1 RSSI path. The
RootCauseEngine now consumes this typed evidence conservatively; no Wi-Fi
incidents are added. See `docs/WIFI_EVIDENCE.md`.

Objective: migrate active probing and Wi-Fi facts, then expose a trustworthy deterministic health view.

Scope:

- implement bounded cancellable IPv4/IPv6 probes with correct reply matching and monotonic timing;
- implement nl80211 signal/state collection with read-only wpa_supplicant fallback if necessary;
- separate link state, route state, target reachability, and application/socket health;
- replace sentinel values with typed not-applicable/unavailable/stale states;
- convert V1 quality rules into tested, versioned, validity-aware health indicators;
- make external probe targets and intervals configurable; disable by default where policy requires.

Acceptance criteria:

- unrelated ICMP packets do not terminate a probe;
- probe cancellation/shutdown meets the lifecycle deadline;
- wired interfaces are not penalized for absent RSSI;
- target failure alone does not set physical link down;
- quality output lists used and missing metrics and never claims more precision than inputs support;
- no collector starts or reconfigures the host's network manager/wpa_supplicant.

Tests:

- probe packet parsing/matching, timeout, DNS, IPv4/IPv6, cancellation, and capability tests;
- fake nl80211/wpa response tests and wired/not-applicable cases;
- lab target-down versus link-down/path-loss scenarios;
- table-driven quality/missing/stale metric tests;
- configuration validation and rate-limit tests.

## Phase 8: IncidentEngine

Status: the first deterministic detection/lifecycle stage is implemented in
the current working tree. Root-cause correlation is documented and implemented
as the bounded first stage of Phase 9.

Objective: turn valid event/metric windows into deterministic incident lifecycles.

Scope:

- define versioned incident schema and stable type/scope/deduplication IDs;
- implement rule/state-machine framework with hysteresis, minimum sample count, cooldown, and resolution;
- add initial incidents for link/uplink, latency, valid retransmission elevation, Wi-Fi signal, and collector degradation;
- persist recent incident history only if needed through a versioned bounded store;
- publish incident transitions on EventBus.

The implemented first stage supports bounded `HighTcpRtt`,
`ElevatedTcpRetransmission`, `RouteUnavailable`, `UplinkUnavailable`, and
`SocketRouteConflict` state machines with typed scope/evidence, consecutive
sample hysteresis, socket-generation isolation, and bounded current/history
state. It does not implement causal inference or RootCauseEngine behavior.

Acceptance criteria:

- replaying an identical ordered stream/configuration produces identical incident transitions and IDs (where IDs are designed deterministic);
- missing/stale metrics cannot open a healthy/negative incident; they can open observability-gap incidents;
- transient samples below rule duration do not flap incidents;
- incident evidence contains the exact event/sample IDs and rule version;
- memory and incident cardinality remain bounded.

Tests:

- table-driven state-machine tests for open/update/resolve/hysteresis/cooldown;
- deterministic replay and out-of-order policy tests;
- missing/stale/reset/partial evidence tests;
- multi-interface/multi-namespace deduplication tests;
- EventBus saturation/recovery interaction;
- initial lab scenario assertions against incident transitions.

## Phase 9: RootCauseEngine

Status: the bounded deterministic stage consumes IncidentEngine, route/uplink
context, and the latest fresh target-matched ActiveProbe and Wi-Fi evidence. It
distinguishes gateway-side path evidence from conservative local-link and
beyond-the-gateway patterns without claiming ISP or remote-server failure.
Wi-Fi cache identity, freshness, association semantics, RSSI hysteresis, and
the capped `LocalLinkSuspected` rule are implemented. Collector-health
correlation, hop/endpoint-specific probes, and replay tooling remain future
work.

Objective: produce explainable evidence-backed cause candidates entirely in C++.

Scope:

- define root-cause candidate/report schema;
- implement temporal/topological correlation over incidents, sockets, routes, interfaces, probes, and collector health;
- encode supporting, contradicting, missing evidence, alternatives, confidence, and next deterministic checks;
- version rules and make thresholds/configuration reviewable;
- add replay tooling for structured event/incident fixtures.

Acceptance criteria:

- each candidate cites structured evidence and a rule version;
- contradictory evidence reduces confidence or produces alternatives;
- global versus endpoint-specific faults are distinguished in controlled scenarios;
- insufficient visibility yields abstention/observability-gap candidates rather than fabricated causes;
- results are deterministic for the same ordered input/configuration;
- no AI service is invoked.

Tests:

- unit fixtures for each cause rule and competing-candidate case;
- golden structured reports with schema-aware comparisons, not prose snapshots;
- property tests for evidence references and confidence bounds;
- replay tests for ordering, clock boundaries, and rule versions;
- lab scenarios for link down, route change, Wi-Fi degradation (where hardware simulation is feasible), path latency/loss, endpoint-specific retransmissions, and collector loss.

## Phase 10: Versioned D-Bus API, `weaknetd`, and `weaknetctl`

Status: the first read-only query/API/CLI slice is implemented in the current
working tree. The V1 surface remains unchanged; deployment migration,
write/configuration operations, and API freeze remain future work.

The ActiveProbe slice is implemented alongside this surface: typed
gateway/remote IPv4 probe observations, bounded native ICMP transport,
lifecycle integration, and freshness/identity-aware RootCauseEngine
interpretation. Probe incidents, loss windows, and transport expansion remain
deferred.

Objective: expose V2 as a production service while preserving explicit compatibility.

Scope:

- rename/package the daemon as `weaknetd`;
- publish introspection XML for `org.weaknet.WeakNet1` on the system bus;
- expose status/capabilities, interface snapshots, bounded metric queries, incidents, root-cause reports, and redacted exports;
- implement stable errors, pagination/limits, and service generation/version;
- build `weaknetctl` commands and follow mode;
- provide systemd service and D-Bus policy with least-privilege capability configuration;
- keep a tested V1 adapter/library where required, with deprecation documentation.

Acceptance criteria:

- `weaknetd` and `weaknetctl` perform all core user workflows without Python installed;
- introspection matches implementation in an automated contract test;
- every request has bounded input, work, output, and timeout behavior;
- system-bus policy permits intended read-only clients and rejects unauthorized sensitive/active operations;
- daemon restart/disappearance is correctly surfaced by CLI/library;
- systemd start, readiness, stop, restart, and degraded collector status are tested;
- supported V1 calls pass compatibility tests or return documented deprecation errors.

Tests:

- private-bus contract tests for all signatures/errors/limits;
- system-bus policy tests in an isolated image/VM as appropriate;
- CLI output/exit-code tests for success, no data, degraded, timeout, and service absent;
- service restart and signal-follow reconnect tests;
- C ABI buffer/null/concurrency/retry tests;
- systemd hardening/capability audit and shutdown tests.

## Phase 11: Reproducible WeakNet Lab

Objective: make end-to-end behavior repeatable and reviewable.

Scope:

- add scoped namespace/veth/router fixtures with idempotent cleanup;
- define versioned scenario manifests for healthy, latency, jitter, loss, reordering, duplication, bandwidth/queue, link, route, IPv4/IPv6, and visibility-degraded cases;
- drive real C++ `weaknetd` and query with `weaknetctl`;
- store machine-readable expected events/incidents/cause codes and time bounds;
- record kernel/tool versions and scenario parameters in artifacts.

Acceptance criteria:

- scenarios do not change the host default route or qdisc outside uniquely named lab interfaces/namespaces;
- cleanup succeeds after pass, assertion failure, timeout, or interruption;
- two consecutive runs start from a clean state and produce schema-equivalent required evidence;
- timing assertions use explicit windows/tolerances and do not depend on public Internet services;
- skips identify the exact missing kernel feature/capability.

Tests:

- fixture self-tests and forced-failure cleanup tests;
- scenario schema validation;
- core healthy/fault scenario suite;
- parallel or serialized execution behavior as declared;
- artifact completeness check.

## Phase 12: Evaluation and benchmarks

Objective: measure correctness and cost without inventing results.

Scope:

- define replay correctness corpus from lab plus reviewed synthetic fixtures;
- define microbenchmarks for EventBus, MetricStore, rule evaluation, map/ring consumption, and serialization;
- define end-to-end workloads for idle, many interfaces/sockets/flows, event bursts, and degraded collectors;
- capture CPU, RSS, queue lag/drops, collection-to-event latency, incident detection latency, D-Bus latency, map pressure, and shutdown time where meaningful;
- store commands, configuration, kernel/tool/hardware metadata, raw output, and analysis scripts.

Acceptance criteria:

- benchmark definitions and pass/fail correctness assertions are reviewed before measurement;
- each published number links to a raw artifact and environment/workload record;
- warmup, sample count, variance/percentiles, and clock source are documented;
- correctness is verified during performance runs (no “fast” run that dropped required events unnoticed);
- no historical README example is reused as a V2 result.

Tests:

- benchmark harness self-test with deterministic dummy workload;
- artifact schema/completeness validation;
- regression threshold mechanism based only on an approved measured baseline;
- stress/soak jobs kept distinct from microbenchmarks.

## Phase 13: Optional LLM explanation and AI evaluation

Status: the End-to-End AI Explanation Product Wiring stage is implemented.
AI V2.2 provides the advisory structured-diagnosis contract, grounding
framework, and opt-in real `DashScopeProvider`; this stage adds the optional
loopback service, real D-Bus V2 diagnosis source, and explicit
`weaknetctl diagnose --explain` path. Normal tests use the deterministic fake
provider; live requests require explicit provider selection, an opt-in flag,
and `DASHSCOPE_API_KEY`. No RAG, embeddings, or agent behavior is implemented.
The C++ diagnosis remains authoritative.

AI V2.3A is implemented as a separate retrieval-only increment under
`ai/v2/rag`. It plans semantic queries from `DiagnosisSnapshot`, indexes a
small explicit corpus, and provides offline-testable BM25, optional dense
embeddings/FAISS, RRF, reranking, capabilities, and evaluation metrics. It
does not inject context into Qwen; that remains AI V2.3B.

Objective: provide a read-only advisory explanation product after the
deterministic core and export schema are stable.

Scope:

- keep AI V2 isolated from the legacy experimental raw-log/RAG scripts;
- consume a versioned `DiagnosisSnapshot` derived from D-Bus V2 output;
- integrate a bounded, injectable DashScope/Qwen transport with explicit error
  mapping, retry, timeout, and response-size policies;
- consume the structured D-Bus V2 `GetDiagnosis` result through an injectable
  `DiagnosisSource`, rather than presentation logs;
- use provider-neutral configuration and secret loading; support a no-provider/offline mode;
- require evidence citations, uncertainty, and explicit separation of core findings from model inference;
- add prompt-injection defenses, data policy, timeout/retry/token/cost limits, and output schema validation;
- expose health/capability/explanation endpoints and keep the service loopback-only.

Acceptance criteria:

- deleting or not installing the optional package has no effect on core build/runtime/tests;
- the explanation layer has no privileged control API and cannot mutate daemon incident/root-cause state;
- D-Bus input and HTTP requests are bounded and size-bounded according to tested policy;
- reports distinguish deterministic core facts from model inference and cite valid bundle evidence IDs;
- provider/network failure produces a clear optional-layer failure while core output remains available;
- AI quality claims are made only from recorded evaluation runs.

Tests and evaluation:

- diagnosis-source adapter, timeout, malformed-payload, service endpoint, and structured-report tests;
- offline deterministic parser and grounding tests;
- malicious/prompt-injection fixture tests;
- evidence-citation validity and unsupported-claim checks;
- cause-code top-k, groundedness, abstention, latency, and cost metrics from a recorded dataset run;
- provider adapter contract tests using fakes by default; live calls only in an explicit, credentialed, cost-bounded job.

## Phase 14: Release hardening and V1 retirement

Objective: remove obsolete V1 internals only after demonstrated parity and operational readiness.

Scope:

- audit compatibility usage and approve deprecations;
- remove duplicate collectors, old daemon name, ad-hoc files, manual test framework, and copied AI variants in small reviewed changes;
- finalize packaging, upgrade/rollback notes, configuration migration, support matrix, and operator runbook;
- run the full unit/integration/lab/evaluation matrix and a documented soak.

Acceptance criteria:

- every removed component has a replacement test or an approved deprecation record;
- upgrade and rollback are tested on supported packages/environments;
- no production C++ target depends on Python;
- no detached core thread, CWD runtime dependency, host-mutating default test, or unversioned IPC remains;
- release notes contain only measured/verified claims.

## Dependency map

```text
Phase 1 build baseline
  -> Phase 2 lifecycle
    -> Phase 3 events/store
      -> Phase 4 netlink
      -> Phase 5 SocketTracker
      -> Phase 6 eBPF (integrates with Phase 5)
      -> Phase 7 probes/Wi-Fi/quality
        -> Phase 8 IncidentEngine
          -> Phase 9 RootCauseEngine
            -> Phase 10 D-Bus/daemon/CLI
              -> Phase 11 Lab completion
                -> Phase 12 measured evaluation/benchmarks
                  -> Phase 13 optional LLM
                    -> Phase 14 retirement/release
```

Some test-fixture work for the lab should begin alongside collector phases, but Phase 11 is the gate where the complete reproducible suite becomes a product artifact. Optional AI deliberately follows the stable deterministic report/export contract.

## Definition of done for any phase

A phase is done only when:

1. its implementation and negative/error paths are present;
2. its listed automated tests pass or supported skips state exact prerequisites;
3. ownership, resource bounds, privilege needs, and degraded behavior are documented;
4. compatibility impact is tested and documented;
5. no claimed result exceeds the available evidence;
6. the next phase can build on a stable public/internal contract rather than private implementation details.
