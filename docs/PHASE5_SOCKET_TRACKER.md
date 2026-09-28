# Phase 5.1–5.4 socket identity, inventory, metrics, and modeled route context

> Historical implementation note. IncidentEngine, RootCauseEngine, D-Bus V2,
> `weaknetctl`, ActiveProbe, Wi-Fi evidence and optional AI/RAG were implemented
> after the socket-tracking increments documented here.

## Phase 5.1 scope

Phase 5.1 defines the in-process V2 data model used by future socket
observation work. It does not open `NETLINK_SOCK_DIAG`, request `TCP_INFO`,
sample live sockets, calculate retransmission/RTT deltas, or replace the V1
TCP monitor.

The implementation is `SocketLifecycleTable` in
`server/include/socket_tracker.hpp`. It is a deterministic, bounded model with
explicit snapshot boundaries and no worker thread or kernel dependency.

## Identity

A four-tuple (local/remote address and port) is descriptive metadata, not a
persistent identity. `SocketTuple` includes the namespace, TCP protocol,
address family, and typed IPv4/IPv6 endpoints. Endpoint addresses remain raw
bytes; string conversion is diagnostic only.

`SocketId` contains:

- `NetnsId`, so equal cookies or tuples in different namespaces cannot match;
- an optional `KernelSocketCookie` when Linux supplies one; cookie value zero
  is still an available cookie, not an unavailable sentinel;
- a daemon-local `SocketGeneration` allocated monotonically from one.

Generation values are process-local and are not persisted across daemon
restarts. The generation is the final discriminator when a tuple is reused,
when cookie availability changes, or when a cookie is observed with
incompatible metadata.

## Reconciliation boundaries

The table is driven as:

```text
beginSnapshot(disposition)
    -> observe(tuple, optional cookie, timestamps)
    -> commitSnapshot() or abortSnapshot()
```

For an authoritative snapshot, active identities observed in the candidate
remain active. Previously active identities absent from the complete candidate
are closed and removed from the active table. Their bounded closed records are
retained only for recent lifecycle/history inspection; they do not cause a
future observation to reuse the identity.

Partial and failed snapshots discard their candidate observations and preserve
all active lifecycle state. Absence in an incomplete or failed observation is
never interpreted as a close. An explicit `close(SocketId)` is an authoritative
removal indication and closes only that exact identity.

## Cookie and tuple behavior

When a cookie is available, namespace plus cookie is the strongest anchor. A
matching tuple reuses the active identity. If the same active cookie appears
with incompatible tuple metadata, the table conservatively allocates a new
generation and reports `inconsistent_metadata`; it does not silently mutate
the old identity.

When a cookie is unavailable, an active tuple may match only within the current
live lifecycle. A complete authoritative disappearance terminates that weak
identity. Reappearance of the same tuple receives a new generation. A
cookie-backed identity is never guessed from a cookie-free observation.

Cookie reuse after authoritative disappearance also receives a new generation.
This intentionally favors an identity reset over unsafe history merging.

## Bounds and events

The active table and recently closed history have explicit configurable bounds
(`4096` active and `1024` closed entries by default). Closed records are
discarded oldest-first once the history bound is reached. No persistent cache or
global singleton is used.

`SocketObservation` is an additive Phase 3 event payload containing the resolved
`SocketId`, tuple, namespace, timestamps, source, validity, TCP-state metadata,
and minimal active/closed lifecycle state. Phase 5.1 alone emits no live socket
events; Phase 5.2 publishes only committed authoritative changes.

## Phase 5.2 NETLINK_SOCK_DIAG inventory

`SocketTracker` owns one nonblocking `AF_NETLINK`/`NETLINK_SOCK_DIAG` socket in
the daemon's current network namespace. It requests all TCP states for both
IPv4 and IPv6 and validates kernel sender PID 0, request sequence, multipart
completion, errors, truncation, interruption, and bounded receive deadlines.
`EAGAIN` is only a wait condition and never an empty-table result.

`SocketDiagParser` extracts binary endpoints, network-order ports, Linux TCP
state, `idiag_if`, and the two-word kernel cookie. Linux stores the native
low 32 bits in word 0 and high 32 bits in word 1, so they are combined as
`word0 | (word1 << 32)` without byte-order conversion. Two
`INET_DIAG_NOCOOKIE` words mean unavailable while
zero remains valid. Unknown attributes are ignored; malformed inventory
attributes reject the candidate, while malformed optional TCP_INFO is marked
unavailable for that socket. TCP state is metadata only. `idiag_if` is retained as
`diag_ifindex` evidence, not routed egress or selected uplink attribution.

IPv4 and IPv6 dumps are transactional: both must complete before observations
are passed to the lifecycle table and committed as one authoritative snapshot.
Failure or partial visibility aborts the candidate and preserves the
last-known-good active state. Events are emitted only after commit. Startup
transport failure is degraded but optional to the daemon; post-start dump
failures keep the worker alive and use bounded recovery backoff. Active
capacity exhaustion rejects the entire candidate rather than committing a
partial table. The one socket observes only its creation namespace; traversal
of other namespaces is future work. Inventory cadence is five seconds and is
not a TCP metric cadence.

## Phase 5.3 TCP_INFO semantic model and interval metrics

The same `SocketTracker` dump requests `INET_DIAG_INFO`; no second socket dump
pipeline is introduced. The `INET_DIAG_INFO` attribute is treated as a bounded
byte payload. Known `struct tcp_info` fields are copied individually only when
`payload_size >= offsetof(field) + sizeof(field)`. Shorter kernel payloads keep
later fields unavailable, while larger payloads safely ignore unknown tail
bytes. A malformed TCP_INFO attribute marks TCP telemetry unavailable for that
socket but does not discard otherwise valid socket inventory.

`TcpInfoRaw` uses optionals so an observed zero remains available zero and an
absent field is not silently filled with zero. RTT and RTT variance (`rtt_us`
and `rttvar_us`) are current gauges in microseconds. Congestion window,
slow-start threshold, unacked, reordering, and receive-space values are current
gauges/counts. Bytes, segment, and retransmission fields are cumulative
counters. The collector does not delta gauges.

Interval metrics are derived only for exactly the same `SocketId`, including
the same lifecycle generation, with strictly increasing monotonic timestamps
and compatible available counters. Counter decreases are treated as resets;
wrap is not guessed. Missing fields, identity changes, non-increasing
timestamps, and zero denominators produce unavailable fields/reasons rather
than zero or negative rates.

The current metric set is deliberately small: TCP-estimated `rtt_us` and
`rttvar_us`, `snd_cwnd`/`snd_ssthresh`, acked/received byte rates, segment and
data-segment rates, `delta_total_retrans`, and
`delta_total_retrans / delta_data_segs_out` named as a retransmission-segment
ratio. That ratio is not a packet-loss rate; repeated retransmissions may make
it exceed one. A zero data-segment delta makes the ratio unavailable.

The successful five-second authoritative inventory cycle is also the initial
TCP sampling point. Failed or partial IPv4/IPv6 reconciliation does not alter
lifecycle state or TCP baselines and emits no interval metrics. Baselines are
bounded by active `SocketId` state; disappearance removes a baseline and a new
generation starts with a baseline-only observation. Raw TCP observations and
valid interval metrics are published through typed EventBus payloads and valid
values are stored in MetricStore with socket identity and interval duration.

## Phase 5.4 modeled socket route attribution

Phase 5.4 adds `SocketRouteAttributor` and the typed
`SocketRouteContextObservation` EventBus payload.  It consumes a committed,
authoritative `TopologySnapshot` and the existing `UplinkPolicy`; it never
issues an RTM_GETROUTE query per socket.  Matching is binary longest-prefix
matching for IPv4 (0..32) and IPv6 (0..128), including non-byte-aligned
prefixes.  Ties use the Phase 4 modeled table order (main, default, then
other modeled tables), lower priority/metric, and route identity only as a
stable representative.  Tied routes with different possible interfaces are
reported as `Ambiguous`, not silently selected.

The context retains route identity/summary, table, prefix, metric, gateway or
on-link evidence, all possible interface identities, topology generation,
`diag_ifindex` evidence, local/source-address evidence, and the relationship
to the selected uplink.  A multipath route retains every modeled nexthop and
is `Partial`; no ECMP flow hash is implemented.  `diag_ifindex` is evidence
only: agreement supports the model, zero is unavailable, and conflict makes
the result partial.  Local address checks are corroborating evidence and do
not implement Linux source-address selection.

Only authoritative topology can produce an attribution.  A degraded collector
may continue using its last-known-good authoritative snapshot; its generation
and degraded/partial staleness evidence remain visible.  Uncommitted topology and
rejected socket candidates publish no route context.  SocketTracker bounds
contexts to active socket generations, removes them on authoritative
disappearance, and recomputes them after a committed topology change.
Route context is retained as bounded current SocketTracker state and EventBus
metadata; it is intentionally not encoded as fake numeric MetricStore series.

This remains a modeled explanation, not guaranteed kernel forwarding.  Linux
`ip rule`/RPDB policy, fwmarks, source-policy routing, VRFs, and custom table
selection are not reproduced; such tables carry policy-limitation evidence.
Loopback/local routes are represented as non-uplink where modeled.
IncidentEngine and RootCauseEngine consume these observations in the current
runtime. Process/cgroup attribution and full eBPF correlation remain outside
this modeled route-attribution component.

## Remaining boundaries

Process/cgroup attribution, packet capture, full Linux policy routing and
broader eBPF correlation remain outside this component. Current deterministic
diagnosis consumes the implemented socket/TCP/route observations without
turning retransmission into packet-loss claims.
