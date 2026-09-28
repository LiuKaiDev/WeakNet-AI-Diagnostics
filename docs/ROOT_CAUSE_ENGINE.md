# RootCauseEngine (deterministic probe enrichment)

`RootCauseEngine` is the deterministic diagnosis layer after `IncidentEngine`. It consumes
typed active incident transitions, modeled route/uplink observations, and
typed `ActiveProbe` observations. It publishes bounded
`RootCauseHypothesisObservation` values on the in-process `EventBus` and
through copy-based snapshot methods. Probe evidence refines incident-backed
diagnosis; it never replaces incident evidence, creates an incident, repairs
the network, or invokes AI.

## Model and bounds

The catalog remains `UplinkAvailabilityProblem`, `LocalRoutingProblem`,
`NetworkPathDegradation`, `RemoteOrUpstreamDegradation`,
`LocalLinkSuspected`, and `InsufficientEvidence`. `LocalLinkSuspected` is a
conservative Wi-Fi enrichment: it opens only when active path degradation is
correlated with a fresh current-interface association failure, or weak signal
plus degraded gateway evidence. It is capped at Medium confidence; gateway
delay, RSSI, and a single timeout do not prove a Wi-Fi fault.

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

## Wi-Fi enrichment

The engine retains only the latest `WifiObservation` per `NetnsId + ifindex`.
Interface names are display metadata and are never used for correlation. A
sample is usable only when it is fresh under the named ten-second monotonic
`wifi_freshness` window, matches the current route/uplink interface (and its
generation when both are present), and reports an applicable station state.
Switching uplinks or receiving a no-target observation prevents old interface
evidence from leaking into the new diagnosis. Namespaces are isolated.

`Available + Associated` contributes association, signal, and optional low
TX-bitrate context. `NotAssociated` is meaningful only for the authoritative
current Wi-Fi uplink. `NotWifi`, `Unsupported`, `PermissionDenied`,
`TransportUnavailable`, `Error`, and `NoTarget` are missing/not-applicable
evidence, never link failures. Stale and generation-mismatched samples are
missing evidence. Cumulative retry/failure counters are not converted to
interval ratios or packet loss here.

Signal categories use average signal when available, otherwise signal: weak
enters at `<= -70 dBm` and very weak at `<= -80 dBm`; they leave at `>= -67`
and `>= -77 dBm`. These are reviewable heuristics with small hysteresis, not
universal radio limits. RSSI alone never opens a hypothesis. Healthy Wi-Fi
means only fresh association plus a non-weak signal; it does not prove the
gateway or local network healthy.
TX bitrate below the named `wifi_tx_bitrate_low_kbps` default of 6,000 kbps is
only weak context; PHY and driver reporting make it unsuitable as a failure
threshold.

The local-link rule is deterministic and capped at Medium: active TCP/path
degradation plus authoritative current `NotAssociated` opens Medium; active
degradation plus weak signal and gateway high RTT/timeout opens Low, or Medium
when very-weak signal or another independent path incident is present. Weak
signal without degradation, or with a healthy gateway, does not open it.
Route/uplink authority is preserved. Healthy associated/normal Wi-Fi enriches
the existing beyond-gateway pattern, while association failure or very weak
signal prevents that pattern from being over-claimed. Missing Wi-Fi does not
contradict a remote diagnosis and does not resolve hypotheses by itself.

AP/device health, RF interference, roaming history, retry interval metrics, and
endpoint-specific validation remain unobserved.

## Non-goals

The engine does not provide packet-loss windows, jitter, traceroute, DNS,
HTTP/TLS probing, probabilistic scoring, persistence, AI/RAG or remediation.
Those omissions are explicit capability boundaries rather than evidence of a
healthy or failed network.
