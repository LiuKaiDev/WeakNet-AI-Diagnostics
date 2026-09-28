# WeakNet V2 Architecture

WeakNet V2 is the current deterministic C++20/Linux diagnosis architecture.
It remains useful without Python, a model provider, dense retrieval or Internet
access. V1 D-Bus/C ABI components are retained as compatibility surfaces and do
not define the V2 internal model.

## System context

```text
Linux kernel/network state
  -> RTNETLINK topology
  -> NETLINK_SOCK_DIAG / TCP_INFO
  -> ActiveProbe
  -> nl80211 Wi-Fi evidence
  -> optional eBPF observations
  -> typed NetworkEvent values
  -> EventBus / MetricStore
  -> IncidentEngine
  -> RootCauseEngine
  -> DiagnosticsQueryService
  -> session D-Bus V2
  -> weaknetctl

GetDiagnosis / DiagnosisSnapshot
  -> optional EvidenceExplainer
  -> optional grounded RAG Advisor
```

The executable is `weaknet-dbus-server`. It owns collectors, stores, engines
and the D-Bus adapter. `weaknetctl` is the read-only C++ client. The optional
Python runtime runs in a separate unprivileged process and cannot control the
daemon.

## Data contracts

`NetworkEvent` is the internal typed envelope. Its header carries schema
version, event identity and sequence, realtime and monotonic timestamps,
network namespace, optional interface/socket scope, source, validity and
status. Payloads cover topology, socket lifecycle, TCP metrics, route context,
probes, Wi-Fi, collector health, incidents and root-cause hypotheses.

Metric values are scoped by namespace and relevant interface/socket identity.
Unknown, unavailable, stale, partial and reset are distinct states. Units,
provenance and observation interval are part of the contract. A tuple without
socket generation is not stable identity.

## Collection layer

### RTNETLINK topology

`NetlinkCollector` owns a nonblocking `NETLINK_ROUTE` socket for link, address
and route dumps/notifications. It validates kernel sender, sequence,
multipart completion, errors, truncation and interrupted dumps. Reconciliation
commits a complete candidate atomically and preserves the last authoritative
snapshot after failed or raced updates.

Uplink selection is deterministic over modeled route facts. IPv4/IPv6,
on-link defaults and multipath metadata are retained. The model does not claim
complete Linux RPDB, fwmark, source-policy, VRF or ECMP-flow-hash equivalence.

### SocketTracker and TCP_INFO

`SocketTracker` performs transactional IPv4/IPv6 `NETLINK_SOCK_DIAG` snapshots,
uses namespace/cookie/generation-aware identity, and treats disappearance as a
close only after an authoritative complete snapshot. `TCP_INFO` fields are
read only when the kernel payload contains them.

Interval metrics require the same socket generation, compatible available
counters and increasing monotonic time. Counter decrease resets the baseline.
The retransmission-segment ratio is a retransmission metric, not packet loss.

`SocketRouteAttributor` applies longest-prefix matching to the committed
topology model and records route/interface/gateway, selected-uplink relation,
`diag_ifindex`, ambiguity and authority. It is a modeled explanation, not a
guaranteed kernel forwarding decision.

### ActiveProbe

`ActiveProbe` uses a bounded Linux IPv4 ICMP datagram transport for one modeled
gateway and one configured numeric remote target. It validates replies and
reports typed success, timeout, invalid, unavailable and no-target outcomes.
One timeout is not a packet-loss percentage. Probe identity and monotonic
freshness are required before evidence is correlated.

### Wi-Fi and eBPF

`WifiCollector` queries Generic Netlink/nl80211 for current station association,
signal, bitrate and optional counters on the selected interface. Not-Wi-Fi,
unsupported, permission denied and missing attributes remain explicit; RSSI or
cumulative retries alone do not prove a fault.

The eBPF observer is optional and capability-gated. Build/load/attach failures
are visible as degraded capability and do not make missing traffic evidence
appear healthy.

## Event and state layer

`EventBus` is a bounded in-process queue with one owned dispatcher, explicit
drop/coalesce telemetry and callbacks invoked outside registry locks.
`MetricStore` retains bounded latest/history snapshots with namespace and
object identity. Both components use owned lifecycle and support deterministic
tests with injected clocks.

The application starts dependencies in order and stops them in reverse order.
Workers are owned; startup rollback and repeated shutdown are tested. Optional
collector failure degrades capability instead of aborting all diagnosis.

## Deterministic diagnosis

`IncidentEngine` detects conditions from committed observations using typed
scope, hysteresis and consecutive-sample rules. Current incident types include
`HighTcpRtt`, `ElevatedTcpRetransmission`, `RouteUnavailable`,
`SocketRouteConflict` and `UplinkUnavailable`.

`RootCauseEngine` is separate. It combines active incidents with authoritative
topology and fresh probe/Wi-Fi context to emit evidence-backed candidates:
`UplinkAvailabilityProblem`, `LocalRoutingProblem`,
`NetworkPathDegradation`, `RemoteOrUpstreamDegradation`,
`LocalLinkSuspected` and `InsufficientEvidence`.

Each candidate contains independent Supporting, Contradicting and Missing
evidence. Confidence is a deterministic enum, not a probability. A suspected
local link or beyond-gateway pattern is never presented as confirmed AP, ISP,
interference or remote-server failure.

## Query and compatibility surfaces

`DiagnosticsQueryService` copies engine and topology snapshots; queries do not
trigger scans, probes or event publications. D-Bus V2 uses:

```text
service:   com.example.WeakNet
object:    /com/example/WeakNet/V2
interface: com.example.WeakNet.Diagnostics2
```

Methods are read-only: `GetStatus`, `ListActiveIncidents`,
`ListRootCauseHypotheses`, `GetDiagnosis` and `GetTopologySummary`.
`weaknetctl` exposes `status`, `incidents`, `hypotheses` and `diagnose`.

The V1 object, C library and old quality fields remain separately tested for
compatibility. Literal V1 names such as `tcp_loss_rate` do not change the V2
semantic rule that retransmission is not packet loss.

## Optional AI/RAG boundary

The Python runtime consumes the versioned `DiagnosisSnapshot`. The
`EvidenceExplainer` can add human-readable explanation, while the RAG path uses
a deterministic query planner, allowlisted corpus, BM25, optional BGE/FAISS,
RRF, optional reranking and stable citations.

`GroundingValidator` and `CitationGroundingValidator` reject invented
diagnosis/evidence/citation references. Provider output cannot overwrite
root-cause type, confidence, state or evidence roles. No AI component has
shell, remediation or network-mutation capability.

## WeakNet Lab

WeakNet Lab creates owned network namespaces, veths and bounded `tc/netem`
faults, runs the real daemon and workload, reads structured D-Bus diagnosis and
writes versioned evaluation artifacts. Missing host capabilities produce a
recorded `SKIP`; fixture demos are explicitly labeled `SIMULATION`.

## Operational limits

- The current product path uses session D-Bus; packaged system-bus/systemd
  deployment remains operational work.
- Namespace, eBPF and physical Wi-Fi coverage depends on host capabilities,
  kernel/BTF and devices.
- Dense/reranker live validation depends on compatible local model artifacts.
- The deterministic engines report evidence-backed hypotheses, not universal
  causal certainty.

See `INCIDENT_ENGINE.md`, `ROOT_CAUSE_ENGINE.md`, `V2_API_AND_CLI.md`,
`AI_V2_ARCHITECTURE.md` and `BUILDING.md` for focused details.
