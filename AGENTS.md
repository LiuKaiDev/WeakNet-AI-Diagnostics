# WeakNet repository guidance

## Product boundary

WeakNet is a Linux network-observability and root-cause-diagnosis system. The production path is C++20 and Linux-native. It must remain fully useful when every Python and AI component is absent.

The intended production executables are:

- `weaknetd`: privileged or capability-scoped collection, metrics, incidents, deterministic root-cause analysis, and D-Bus service.
- `weaknetctl`: an unprivileged command-line D-Bus client.

Python is allowed only under the optional AI/RAG/evaluation area. It must not collect kernel data for the daemon, replace a C++ collector, be required by `weaknetd` or `weaknetctl`, or sit in the availability/control path.

## Read before changing code

Read these documents in order:

1. `docs/V2_ARCHITECTURE.md`
2. `docs/V2_ROADMAP.md`
3. `docs/V1_AUDIT.md`
4. `docs/LEARNING_GUIDE.md`

The audit describes observed V1 behavior, including defects. It is not a specification to preserve those defects. The architecture is the target boundary, and the roadmap is the migration sequence.

## Current phase guardrail

The repository currently contains the V1 implementation and the V2 design baseline. Do not start a V2 milestone unless the task explicitly authorizes that milestone. Documentation or audit work does not authorize functional implementation.

## Non-negotiable constraints

- Use C++20 for the core runtime, collectors, engines, daemon, CLI, and client compatibility layer.
- Preserve Linux-native implementations: libbpf CO-RE, rtnetlink/sock_diag, `TCP_INFO`, network namespaces, and `tc`/netem.
- Keep AI optional and out of the daemon. The deterministic C++ `IncidentEngine` and `RootCauseEngine` are the authority.
- Migrate incrementally. Do not perform a full rewrite or discard working V1 behavior without a characterized replacement.
- Preserve the useful V1 client surface during migration or provide an explicit, tested compatibility path.
- Treat metric validity, scope, units, provenance, timestamps, and network-namespace identity as part of the data contract.
- Do not report benchmark numbers unless the benchmark command, environment, workload, raw output, and result artifact exist. A benchmark plan is not a benchmark result.
- Do not commit or push unless the user explicitly asks.
- Do not add Python wrappers or shell parsing as substitutes for missing C++/kernel functionality.

## Architecture rules

- Collectors publish immutable, versioned `NetworkEvent` values; they do not call the diagnosis engines or D-Bus layer directly.
- `EventBus` owns delivery and backpressure. Callbacks must not run while internal registry or metric-store locks are held.
- `MetricStore` owns bounded metric history and snapshot/query semantics. Unknown or stale data must not silently become zero.
- `IncidentEngine` turns event/metric windows into deterministic incident state machines.
- `RootCauseEngine` ranks evidence-backed candidates and must expose the evidence and rule that produced each conclusion.
- D-Bus is an adapter over application services, not the internal event bus or state store.
- Kernel/user shared eBPF types have one source of truth, explicit layout checks, and schema/version handling.
- Threads are owned. Prefer `std::jthread` and `std::stop_token`; no detached long-lived threads or fire-and-forget access to stack-owned context.
- Blocking probes and map-sampling windows must run outside shared-state locks.
- Use bounded queues and histories with explicit overflow/drop telemetry.

## Build and dependency rules

- CMake is the target build system; expose real targets and CTest tests. Keep the V1 Make entry points only as temporary compatibility wrappers while the roadmap calls for them.
- Detect required dependencies explicitly. Optional functionality must be controlled by documented CMake options and produce visible degraded-capability status.
- Generate libbpf skeletons and architecture definitions through the build; do not hard-code x86 or depend on an untracked developer-generated header without a documented generation path.
- Keep production runtime dependencies minimal. Python packages must never leak into C++ targets.
- A source change is not complete until the narrowest relevant unit/integration test is run, or the missing prerequisite is reported exactly.

## Test rules

- Unit tests must be deterministic and must not require root, a live Internet endpoint, or the host's default route.
- Linux integration tests belong in isolated network namespaces with explicit veth/topology setup and cleanup.
- Privileged eBPF tests must state their kernel/BTF/capability prerequisites and skip clearly when unavailable.
- Fault scenarios must use reproducible `tc`/netem or namespace fixtures and assert events, metrics, incidents, and root-cause evidence—not log text.
- Contract-test D-Bus signatures and compatibility behavior.
- Test shutdown, queue saturation, counter reset/wrap, interface churn, namespace separation, missing metrics, and degraded eBPF operation.
- Never turn a demonstration CLI that always exits zero into test evidence.

## Safety and operational discipline

- Inspect the active network namespace and exact interface before any command that changes links, routes, qdiscs, BPF attachments, or system-bus policy.
- Run destructive networking tests only inside a disposable namespace created for the test.
- Always remove test qdiscs, namespaces, pins, and processes through scoped cleanup handlers.
- Do not run dependency-install scripts or make external API calls as part of routine validation.
- Never place credentials, packet payloads, raw addresses, hostnames, or customer logs in committed fixtures. Redact optional AI input by default.

## Documentation discipline

When behavior differs from documentation, record the observed behavior and test evidence. Label proposed interfaces as proposed until implemented. Keep V1 compatibility claims separate from V2 target claims, and keep measured results separate from examples.
