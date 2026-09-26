# RootCauseEngine (deterministic probe enrichment)

`RootCauseEngine` is the diagnosis stage after `IncidentEngine`. It consumes
typed active incident transitions, modeled route/uplink observations, and
typed `ActiveProbe` observations. It publishes bounded
`RootCauseHypothesisObservation` values on the in-process `EventBus` and
through copy-based snapshot methods. Probe evidence refines incident-backed
diagnosis; it never replaces incident evidence, creates an incident, repairs
the network, or invokes AI.

## Model and bounds

The catalog remains `UplinkAvailabilityProblem`, `LocalRoutingProblem`,
`NetworkPathDegradation`, `RemoteOrUpstreamDegradation`,
`LocalLinkSuspected`, and `InsufficientEvidence`. `LocalLinkSuspected` remains
deferred: gateway delay or timeout cannot distinguish a local link, gateway
device, local routing, or ICMP filtering problem.

An incident is a detected condition, not a root cause. A hypothesis contains
typed supporting, contradicting, and missing evidence. Missing capability or
context is never converted into contradiction or healthy state. Confidence is
the enum `Low`, `Medium`, or `High`, not a probability.

Probe state is deliberately small: for each `NetnsId`, the engine retains only
the latest gateway observation and latest configured-remote observation. A new
target replaces the old target; no history, timeout rate, packet-loss rate, or
time series is calculated. Out-of-order older observations cannot replace the
current cache entry.

## Freshness and identity

Probe freshness uses monotonic timestamps. `RootCausePolicy::probe_freshness`
defaults to 15 seconds. Older observations become missing
`ProbeEvidenceStale`; expiration is not positive or contradictory evidence and
does not run a timer that resolves hypotheses. A later relevant event simply
recomputes from the still-active incidents and currently usable context.

Evidence is namespace-isolated. Gateway evidence additionally must match the
current socket route context's binary gateway address, address family, and
selected interface. A gateway change therefore invalidates the previous
gateway sample immediately. Topology generation is retained as provenance but
exact generation equality is not required when gateway identity is unchanged.
The latest remote observation defines the current configured-remote identity,
so target replacement cannot retain the previous target's result.

## Probe status semantics

- `Success` proves reachability only at that sample time. RTT is classified
  against explicit engineering defaults: gateway high RTT is at least 75,000
  microseconds and configured-remote high RTT is at least 175,000
  microseconds. These defaults are reviewable policy, not universal truth.
- One `Timeout` is weak supporting degradation evidence. It is not a loss
  percentage, host-unreachable proof, or ISP-failure proof, and cannot by
  itself produce high confidence.
- `TransportUnavailable` and `NoTarget` are missing capability/context.
  `InvalidReply` and `Error` are low-quality unavailable evidence.
- `Unreachable` is also unavailable for root-cause interpretation because the
  current `ActiveProbe` transport does not parse and validate ICMP unreachable
  replies.

Gateway success means the modeled gateway answered that probe. It does not
prove Wi-Fi, policy routing, every local component, or Internet health.
Configured-remote RTT/reachability describes only the path to that numeric
target. It is not a probe of the TCP socket's endpoint or all Internet traffic.

## Deterministic rules

- Authoritative active `UplinkUnavailable` remains a high-confidence
  `UplinkAvailabilityProblem`; older probe success never overrides topology.
- `RouteUnavailable` and `SocketRouteConflict` remain the only inputs that can
  create `LocalRoutingProblem`. A target-matched gateway success is weak
  contradictory context, while gateway high RTT/timeout is supporting context;
  neither proves routing correctness or misconfiguration.
- `HighTcpRtt` or `ElevatedTcpRetransmission` creates low-confidence
  `NetworkPathDegradation` for the exact generation-aware socket. Both
  independent TCP incident kinds raise it to Medium.
- Gateway high RTT can raise a Low path hypothesis to Medium. Gateway timeout
  adds support without claiming a local-link fault.
- With active TCP degradation, authoritative route/uplink context, fresh normal
  gateway success, and configured-remote high RTT or timeout,
  `RemoteOrUpstreamDegradation` is created at Medium confidence with reason
  `degradation_appears_beyond_local_gateway`. This means only that degradation
  appears beyond the immediate gateway; it does not mean ISP congestion or a
  remote-server fault.
- Fresh configured-remote normal success contradicts the broad upstream
  interpretation, so that candidate is not emitted. The socket-specific path
  hypothesis may remain because the configured target is not the socket peer.
- A remote high-RTT success may raise a Low path hypothesis to Medium. Both TCP
  incident kinds plus normal gateway reachability plus remote high RTT can
  raise the path hypothesis to High. A remote timeout never supplies that High
  upgrade.
- Missing/stale probe context produces low-confidence `InsufficientEvidence`
  alongside the incident-backed path hypothesis when route/uplink context is
  otherwise available. Once fresh gateway and remote evidence is sufficient,
  this candidate resolves without waiting for another incident observation.

Examples:

```text
HighTcpRtt
  -> NetworkPathDegradation Low + InsufficientEvidence when probes are missing

HighTcpRtt + ElevatedTcpRetransmission
+ gateway normal Success + remote Timeout
  -> NetworkPathDegradation Medium
  -> RemoteOrUpstreamDegradation Medium

HighTcpRtt + gateway normal Success + remote normal Success
  -> NetworkPathDegradation Low with RemoteProbeReachable contradiction
  -> no RemoteOrUpstreamDegradation

TCP degradation + gateway high RTT/Timeout
  -> NetworkPathDegradation with gateway-side supporting evidence
  -> no LocalLinkSuspected
```

Correlation uses typed socket/namespace identities, never printable names.
Active incidents only participate in active hypotheses. Recurrence after
resolution gets a new occurrence ID. Probe observations trigger scoped bounded
recomputation immediately. Evidence timestamp or probe-sequence changes do not
publish duplicate hypothesis events when confidence, roles, kinds, values,
capability, and meaning are unchanged.

## Non-goals

No new collector or transport, packet-loss window, jitter, traceroute, DNS,
HTTP/TLS probing, IPv6 probe expansion, Wi-Fi/eBPF redesign, probabilistic
scoring, persistence, AI/RAG, or remediation is part of this stage.
