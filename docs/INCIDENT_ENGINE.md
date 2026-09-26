# IncidentEngine (first deterministic stage)

`IncidentEngine` consumes committed typed V2 observations from `EventBus` and
maintains bounded incident lifecycles. It detects observable conditions; it
does not infer a root cause, rank causal candidates, invoke AI/RAG, or expose a
D-Bus API.

## Supported incidents

- `HighTcpRtt`, scoped to a complete `SocketId`, using TCP-estimated RTT in
  microseconds;
- `ElevatedTcpRetransmission`, scoped to a complete `SocketId`, using the
  Phase 5.3 retransmission-segment ratio (not packet loss);
- `RouteUnavailable`, scoped to a `SocketId`, only for explicit
  `NoModeledRoute` evidence from an authoritative socket route context;
- `SocketRouteConflict`, scoped to a `SocketId`, only for explicit
  `diag_ifindex` conflict with modeled route interfaces;
- `UplinkUnavailable`, scoped to a `NetnsId`, only for an authoritative
  NetlinkCollector observation with no selected usable uplink.

Interface-down incidents and collector-observability-gap incidents are
deferred. Unsupported policy routing, non-authoritative topology, multipath
partial attribution, missing `diag_ifindex`, and unavailable metrics are
insufficient evidence rather than incidents.

## Model and lifecycle

`IncidentObservation` contains a typed `IncidentId`, `IncidentType`, typed
scope, `Pending`/`Active`/`Resolved` state, severity, opening/update/resolution
timestamps, condition, validity, evidence completeness, and bounded structured
evidence. Confidence is explicitly `Unknown`, `Partial`, or `High` and is
derived only from evidence completeness. Incident IDs are daemon-local
monotonic values. A reopened condition
receives a new ID; socket generation is part of `SocketId`, so tuple reuse
cannot transfer an incident.

Default numeric rules are engineering defaults, not universal network truth:

- RTT activates at `200000 us` for two consecutive valid samples and recovers
  at or below `150000 us` for two consecutive valid samples;
- retransmission activates at ratio `0.10` and recovers at or below `0.05`,
  with at least ten data segments per interval and two consecutive valid
  intervals.

Values inside the hysteresis band do not advance either streak. Missing,
unavailable, reset, zero-denominator, or partial evidence pauses lifecycle
progress. An active incident is not resolved by telemetry disappearing.

Route and uplink detectors use explicit typed status and authority evidence.
An authoritative no-route result is actionable; an unavailable topology model
is not. An uplink absence is actionable only when the committed Phase 4 policy
says no usable uplink exists.

Evidence retains source event kind/source, typed scope, timestamps, optional
typed value/unit, validity, and the condition used. Opening evidence is
preserved and later evidence is bounded by policy. Resolved history is bounded
(256 entries by default). Unchanged samples do not emit duplicate incident
events.

## Integration and non-goals

The application owns one `IncidentEngine`, which subscribes after the EventBus
is available and unsubscribes before EventBus shutdown. Consumers can query
immutable copies through `listActiveIncidents`, `getIncident`, and
`recentResolvedIncidents`. Incident observations are a separate typed EventBus
payload and are never encoded as fake numeric MetricStore series.

This stage deliberately does not implement RootCauseEngine, causal graphs,
Bayesian/ML/LLM analysis, persistence, notifications, D-Bus V2, weaknetctl,
new collectors, or probe/Wi-Fi/eBPF redesign.
