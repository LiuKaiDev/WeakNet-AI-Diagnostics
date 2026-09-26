# V2 read-only API and `weaknetctl`

This is the first product-surface stage for the already-computed V2 state. It
is versioned but experimental and additive to the existing V1 interface.

## D-Bus surface

The current implementation uses the existing session bus:

- service: `com.example.WeakNet`
- object: `/com/example/WeakNet/V2`
- interface: `com.example.WeakNet.Diagnostics2`

The V1 object `/com/example/WeakNet` and interface `com.example.WeakNet` are
unchanged.

Methods are read-only and have no input arguments:

| Method | Reply |
| --- | --- |
| `GetStatus` | `a{sv}` |
| `ListActiveIncidents` | `aa{sv}` |
| `ListRootCauseHypotheses` | `aa{sv}` |
| `GetDiagnosis` | `a{sv}` containing status and nested incident/hypothesis arrays |
| `GetTopologySummary` | `a{sv}` |

Dictionary values use D-Bus variants. Enum values are names or documented
numeric evidence-kind values; internal C++ variant layouts and kernel structs
are not part of the ABI. Timestamps are realtime Unix milliseconds. Socket and
namespace scopes include namespace identity and socket generation; interface
names are metadata, never identity.

Incident records include `id`, `type`, `state`, `scope`, `opened_at_ms`, and
`last_updated_at_ms`. Root-cause records include `occurrence`, `type`, `state`,
`confidence`, `scope`, `reason`, and structured `supporting_evidence`,
`contradicting_evidence`, and `missing_evidence` arrays. Missing evidence stays
separate from contradiction.

## Status semantics

`GetStatus` includes `state`, topology authority/degradation, socket-tracker
and runtime degradation flags, active incident/hypothesis counts, selected
uplink (plus ifindex when available), and a realtime timestamp.

- `Healthy`: both diagnosis engines are running, authoritative topology exists,
  no active incidents or hypotheses, and no known degraded runtime state.
- `Degraded`: authoritative state is available but an active incident,
  hypothesis, or known degraded collector/runtime component exists.
- `Unknown`: a required engine is not running or authoritative topology is not
  available. Unavailable telemetry is never silently reported as healthy.

The query facade copies engine snapshots and topology state before assembling a
reply. Queries do not trigger scans, probes, or EventBus publications. The
components are independently owned, so a diagnosis response is a
near-consistent snapshot with one query timestamp rather than a global
transaction.

## `weaknetctl`

The CLI is a synchronous D-Bus client with a three-second call timeout:

```text
weaknetctl status
weaknetctl incidents
weaknetctl hypotheses
weaknetctl diagnose
```

`status` prints the compact status and exits 0 for Healthy, 1 for Degraded,
and 2 for Unknown. `incidents` and `hypotheses` are successful listings and
exit 0 even when entries exist. Daemon/D-Bus/usage errors exit 3 or higher.
`diagnose` prints status, active incidents, and active hypotheses using only
deterministic structured fields; it does not generate LLM prose.

There are no write, configuration, remediation, acknowledgement, or daemon
control commands. JSON output is deferred because the current codebase has no
JSON dependency suitable for a small stable product surface.
