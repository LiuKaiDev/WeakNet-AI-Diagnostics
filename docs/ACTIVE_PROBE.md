# ActiveProbe and deterministic evidence consumers

`ActiveProbe` is an evidence collector. It periodically emits typed
`ProbeObservation` values for one modeled gateway and one configured numeric
remote IPv4 target. It does not create incidents or root-cause claims.

## Targets and topology

Gateway targets come only from an authoritative committed `TopologySnapshot`
and the selected `UplinkSelection`. The first implementation accepts a
modeled IPv4 default route with one explicit gateway and the selected output
interface. On-link routes, multipath routes without a unique representative
gateway, unavailable topology, and no selected uplink produce `NoTarget`; no
gateway is invented. IPv6 gateway probing is intentionally deferred.

When topology becomes temporarily non-authoritative, an existing gateway is
retained as a last-known-good target but its provenance is marked stale. A
freshly authoritative topology replaces it. An explicitly authoritative
no-uplink state clears gateway and remote targets. Target identity includes
namespace, family, binary address, interface metadata, and topology generation,
so a gateway change cannot silently share identity with its predecessor.

The remote target defaults to numeric IPv4 `198.51.100.1` and is configured by
`ProbeConfig::remote_ipv4`; it never performs DNS resolution. Invalid literals
are rejected and reported as degraded capability. Remote probing is skipped
when authoritative topology has no selected uplink or when topology is
unknown.

## Transport and result semantics

The native transport uses a bounded Linux IPv4 ICMP datagram socket, validates
source address, echo identifier, and sequence, and polls in stop-aware slices.
Permission/capability failures are `TransportUnavailable`, not host
unreachable. Matching replies are `Success`; no matching reply before the
deadline is `Timeout`; malformed/wrong replies yield `InvalidReply` after the
bounded attempt. `Unreachable` parsing is reserved for a later stage.

RTT is measured from monotonic send/receive timestamps and reported as
`rtt_us`. Timeout, unavailable, no-target, and invalid results have no RTT
value. A timeout is one observation, not a packet-loss percentage; this stage
does not calculate loss windows, jitter, or infer path health.

Every observation carries `NetnsId`, target kind (`Gateway` or `Remote`),
sequence, realtime and monotonic timestamps, validity, transport, and a typed
status/failure reason. The component operates only in the daemon's current
network namespace and does not bind to an interface or call `setns`.

## Lifecycle and integration

One stop-aware worker sequences gateway then remote probes at a bounded default
five-second interval with a one-second timeout. `ActiveProbe` publishes
`EventKind::ProbeObservation` from `EventSource::ActiveProbe` and exposes
bounded telemetry for attempts, successes, timeouts, invalid replies,
transport/capability failures, no-target results, and last success. It is
started and stopped by `DaemonApplication`; a transport failure degrades probe
capability without stopping the daemon.

`RootCauseEngine` now consumes these observations directly as bounded,
freshness-limited evidence. It retains only the latest gateway and remote
sample per namespace. Gateway samples must match the current modeled binary
gateway address, family, and interface; remote target replacement replaces the
old cached identity. The default freshness window is 15 seconds and uses
monotonic time.

Successful RTTs are compared with reviewable engineering defaults (75 ms for
the gateway and 175 ms for the configured remote), not learned baselines or
universal quality claims. One timeout is weak evidence only. `NoTarget`,
`TransportUnavailable`, `InvalidReply`, and transport `Error` remain missing or
low-quality evidence. Although the schema reserves `Unreachable`, the current
transport does not parse validated ICMP unreachable replies, so the diagnosis
engine does not treat that status as path-failure evidence.

Gateway success does not establish Internet health or prove every local
component healthy. The configured numeric remote is path context only: it is
not the observed TCP socket's endpoint and cannot prove ISP or remote-server
failure. No packet-loss rate, timeout percentage, or probe incident is
calculated. See `ROOT_CAUSE_ENGINE.md` for exact confidence rules.

This stage deliberately does not store RTT in MetricStore, diagnose DNS,
implement traceroute, redesign Wi-Fi/eBPF, or perform remediation.
