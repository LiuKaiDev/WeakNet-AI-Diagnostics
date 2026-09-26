# RootCauseEngine (first deterministic stage)

`RootCauseEngine` is the diagnosis stage after `IncidentEngine`. It consumes
typed active incident transitions and already-modeled route/uplink
observations, then produces bounded `RootCauseHypothesisObservation` values on
the in-process `EventBus` and through copy-based snapshot methods. It does not
collect data, repair the network, or call an AI service.

## Model

The initial catalog is deliberately small: `UplinkAvailabilityProblem`,
`LocalRoutingProblem`, `NetworkPathDegradation`,
`RemoteOrUpstreamDegradation`, `LocalLinkSuspected`, and
`InsufficientEvidence`. `LocalLinkSuspected` is catalogued but deferred until
trustworthy local-link evidence exists.

An incident is a detected condition, not a root cause. A hypothesis contains
typed supporting, contradicting, and missing evidence. Each evidence entry
identifies its source incident or observation, typed scope, timestamps,
validity, capability state, and optional value/unit. Missing evidence means a
useful observation is unavailable or not implemented; it is never converted
to contradiction or healthy state.

Confidence is the enum `Low`, `Medium`, or `High`, not a probability. High is
reserved for explicit authoritative direct evidence; two semantically
independent compatible sources can produce Medium; one passive source is Low.
There are no floating scores or Bayesian/ML rules.

## Deterministic rules

- Active authoritative `UplinkUnavailable` opens high-confidence
  `UplinkAvailabilityProblem` for its `NetnsId`; resolution follows the
  incident. No ISP failure is inferred.
- `RouteUnavailable` opens high-confidence `LocalRoutingProblem`.
  `SocketRouteConflict` opens medium-confidence `LocalRoutingProblem`.
  Policy-routing, `ip rule`, and VRF details are missing capability; conflict
  does not prove kernel routing failure.
- `HighTcpRtt` or `ElevatedTcpRetransmission` opens low-confidence
  `NetworkPathDegradation` for the exact generation-aware `SocketId`. Both
  independent incident kinds for the same socket raise confidence to Medium;
  different sockets never combine.
- An explicitly authoritative modeled route and selected uplink are positive
  evidence only when observed. Otherwise topology is listed as missing.
  Gateway and remote probes are missing in this phase.
- `RemoteOrUpstreamDegradation` is an optional low-confidence alternative,
  emitted only with explicitly healthy route/uplink state and TCP degradation.
  It does not mean ISP congestion or server overload.
- `LocalLinkSuspected` is deferred: retransmission alone is not Wi-Fi/link
  evidence.

Correlation uses typed socket/namespace identities, never printable names.
Active incidents only participate in active hypotheses. A hypothesis ID
contains type, scope, and a daemon-local occurrence number; recurrence after
resolution gets a new ID. Resolved history and evidence are bounded, and
unchanged input does not publish duplicate events.

The EventBus payload is `RootCauseHypothesisObservation` with
`EventSource::RootCauseEngine`. The engine ignores its own output. The
application unsubscribes it before stopping `IncidentEngine` and EventBus.

## Non-goals

No AI/RAG, embeddings, probability, persistence, D-Bus V2, `weaknetctl`,
remediation, DNS diagnosis, new netlink/sock_diag/eBPF collectors, or
probe/Wi-Fi redesign is part of this stage. Future evidence sources may make
the deferred causes stronger; an AI explainer may describe the structured
result but cannot become authoritative.

