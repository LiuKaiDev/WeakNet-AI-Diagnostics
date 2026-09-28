# Code Reading Guide

This guide follows the current runtime data path. Historical `PHASE*.md`
documents contain implementation detail, but are not prerequisites for
understanding the final architecture.

## Where to start

Read these files in order:

1. `server/include/network_event.hpp` and `server/src/network_event.cpp` for
   typed observations, identity, validity and provenance.
2. `server/include/event_bus.hpp`, `metric_store.hpp` and their implementations
   for bounded delivery and snapshot storage.
3. `server/include/application.hpp` and `server/src/application.cpp` for
   component ownership, startup and shutdown.
4. `server/include/incident_engine.hpp` and `root_cause_engine.hpp` for the two
   deterministic diagnosis boundaries.
5. `server/include/diagnostics_query.hpp`, `server/src/dbus_service.cpp` and
   `client/weaknetctl.cpp` for the public query path.
6. `ai/v2/` only after the deterministic path is clear.

## End-to-end data path

```text
Linux kernel/network state
  -> topology, socket/TCP, probe, Wi-Fi and optional eBPF collectors
  -> NetworkEvent
  -> EventBus / MetricStore
  -> IncidentEngine
  -> RootCauseEngine
  -> DiagnosticsQueryService
  -> D-Bus V2
  -> weaknetctl
  -> optional EvidenceExplainer or RAG Advisor
```

The key semantic rule is that validity is data. Unknown, unavailable, partial,
stale and reset values are not silently converted to zero or healthy state.

## Topology collector

Start with `server/include/netlink_topology.hpp`,
`server/src/netlink_topology.cpp`, `netlink_parser.*` and
`netlink_collector.cpp`. The collector takes RTNETLINK dumps and notifications,
validates sequence/sender/multipart behavior, commits authoritative snapshots,
and selects an uplink from the modeled route facts. Interface and namespace
identity do not depend on display names.

Modeled route selection is intentionally narrower than the full Linux routing
policy database. It does not claim complete equivalence for `ip rule`, fwmark,
VRF, custom source policy or ECMP hashing.

## Socket and TCP observations

Read `socket_diag_parser.*`, `socket_tracker.*` and
`socket_route_attributor.*`. `NETLINK_SOCK_DIAG` supplies socket snapshots and
`TCP_INFO` fields. The tracker distinguishes socket generations, validates
field availability, calculates deltas only across compatible samples and
resets baselines on counter reset or identity change.

`ElevatedTcpRetransmission` uses a retransmission-segment ratio when the
required counters and denominator are valid. It is not an authoritative packet
loss rate. Route attribution records the modeled route, interface, gateway,
authority and ambiguity rather than asserting the kernel used that path.

## ActiveProbe and Wi-Fi

`active_probe.*` performs bounded IPv4 ICMP datagram probes to the modeled
gateway and a configured numeric remote target. A timeout is one observation,
not a loss percentage. Probe evidence is freshness- and identity-bound before
the root-cause engine uses it.

`wifi_collector.*` and `wifi_nl80211_parser.*` query Generic Netlink/nl80211
for association, signal, bitrate and optional counters. Unsupported hardware,
permission denial, missing attributes and stale observations remain explicit.
RSSI and cumulative retry counters are context, not proof of interference or
packet loss.

## IncidentEngine

`IncidentEngine` converts committed observations into bounded incident state
machines. It detects observable conditions such as `HighTcpRtt`,
`ElevatedTcpRetransmission`, `RouteUnavailable`, `SocketRouteConflict` and
`UplinkUnavailable`. It uses hysteresis, consecutive-sample rules, typed scope
and generation-aware identity. It does not infer root causes or invoke AI.

Read `tests/incident_engine_test.cpp` beside the implementation to understand
activation, recovery, unavailable evidence and recurrence behavior.

## RootCauseEngine

`RootCauseEngine` consumes active incident transitions plus topology, probe and
Wi-Fi context. It emits evidence-backed hypotheses such as
`UplinkAvailabilityProblem`, `LocalRoutingProblem`,
`NetworkPathDegradation`, `RemoteOrUpstreamDegradation`,
`LocalLinkSuspected` and `InsufficientEvidence`.

Supporting, Contradicting and Missing evidence are independent collections.
Confidence is a deterministic enum, not a probability. The rules are designed
to avoid upgrading suspected local/remote conditions into unsupported causal
certainty. `tests/root_cause_engine_test.cpp` is the most direct executable
specification.

## D-Bus and CLI

`DiagnosticsQueryService` copies snapshots from the engines and topology model,
then `dbus_service.cpp` serializes read-only methods on
`com.example.WeakNet.Diagnostics2`. Queries do not trigger scans or probes.

`client/weaknetctl.cpp` implements `status`, `incidents`, `hypotheses` and
`diagnose`. Deterministic output and exit status are produced before any
optional AI request. The retained V1 C library and D-Bus object are compatibility
surfaces and are tested separately.

## Optional AI explanation

Follow `ai/v2/schemas/diagnosis.py`, `adapters/dbus.py`,
`prompts/evidence_explainer.py`, `providers/` and `services/explainer.py`.
The adapter normalizes `GetDiagnosis`; the prompt builder serializes bounded
structured data; the provider returns schema-constrained text; and
`GroundingValidator` ensures every reference maps back to the snapshot.

The final `ExplanationReport` copies authoritative type, confidence, state and
evidence roles from C++, so model output cannot overwrite diagnosis.

## RAG advice

Read `ai/v2/rag/query_planner.py`, `knowledge.py`, `bm25.py`, `retriever.py`,
`fusion.py`, `reranker.py`, then `prompts/rag_advisor.py`,
`services/advisor.py` and `validation/citations.py`.

The planner builds a privacy-aware query from the snapshot. BM25 is the
baseline; dense BGE/FAISS, RRF and reranking are optional. The advisor may use
only chunks in the current `RetrievalBundle`, and every knowledge-backed item
must carry an exact stable citation.

## WeakNet Lab

`lab/weaknet_lab/runner.py` owns isolated namespace/veth/qdisc setup, daemon
and workload processes, structured D-Bus capture, evaluation and cleanup.
Scenario manifests in `lab/scenarios/` declare capability requirements,
faults, expectations and recovery windows. Production C++ engines generate the
observed incidents and hypotheses; the harness does not inject them.

Use `./lab/weaknet-lab doctor` before a live scenario. Missing privileges
produce a recorded `SKIP`. `fixture-demo` is a separate, explicitly labeled
simulation path.

## Tests to read with the code

- `tests/netlink_topology_test.cpp` and
  `tests/netlink_namespace_integration_test.cpp`
- `tests/socket_lifecycle_test.cpp`, `socket_diag_parser_test.cpp`,
  `tcp_metrics_test.cpp`, `socket_route_attribution_test.cpp`
- `tests/active_probe_test.cpp` and `wifi_nl80211_test.cpp`
- `tests/incident_engine_test.cpp`, `root_cause_engine_test.cpp`,
  `diagnostics_query_test.cpp`
- `ai/tests/test_v2.py`, `test_runtime.py`, `test_rag.py`, `test_advisor.py`
- `lab/tests/test_lab.py`
