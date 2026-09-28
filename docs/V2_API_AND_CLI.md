# V2 read-only API and `weaknetctl`

This is the current read-only product surface for V2 state. It is versioned,
additive to the retained V1 compatibility interface, and does not expose
network mutation or remediation operations.

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

Dictionary values use D-Bus variants. Root-cause evidence kinds are serialized
as stable descriptive names such as `GatewayProbeReachable`,
`RemoteProbeTimeout`, `ProbeEvidenceStale`, `WifiAssociated`,
`WifiSignalWeak`, and `WifiNotAssociated`; other enum fields remain names
or documented numeric values. Internal C++ variant layouts and kernel structs
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
weaknetctl diagnose --advise
```

`status` prints the compact status and exits 0 for Healthy, 1 for Degraded,
and 2 for Unknown. `incidents` and `hypotheses` are successful listings and
exit 0 even when entries exist. Daemon/D-Bus/usage errors exit 3 or higher.
`diagnose` prints status, active incidents, and active hypotheses using only
deterministic structured fields; it does not generate LLM prose. The explicit
`weaknetctl diagnose --explain` form then calls the optional loopback AI V2
service and renders its structured `ExplanationReport`. The deterministic
section is printed first and is never replaced by model text.
The explicit `weaknetctl diagnose --advise` form calls the separate grounded
RAG-advisor endpoint. It renders structured advice, exact citation IDs, safe
checks, limitations, and `lexical`/`hybrid` mode after the deterministic
section. It cannot alter diagnosis fields.
If there is no active root-cause hypothesis, the advisor returns
`status=not_applicable` without retrieval or a provider call, and the CLI
reports that state after the deterministic section without changing its exit
code. Retrieval and provider failures remain separate unavailable states.
Probe-derived supporting, contradicting, and missing evidence appears through
the existing hypothesis evidence fields and summaries, including the Wi-Fi
cache's interface/freshness limitations; there is no diagnosis logic or
separate probe/Wi-Fi command in the CLI.

There are no write, configuration, remediation, acknowledgement, or daemon
control commands. JSON output is deferred because the current codebase has no
JSON dependency suitable for a small stable product surface.

AI explanation requests have a 38-second default total timeout, configurable
through `WEAKNET_AI_TIMEOUT_SECONDS` and capped at 40 seconds. An unavailable
AI service, provider timeout, invalid model output, or grounding violation is
printed separately and leaves the deterministic Healthy/Degraded/Unknown exit
code unchanged. `diagnose` without `--explain` never contacts AI.

The advice endpoints are `POST /v2/advice` and `POST /v2/advice/current`.
Provider advice uses `weaknet.ai.rag-advice.v1`; citation grounding rejects
unknown or wrong-bundle chunk identities. Advice failure is reported separately
and does not change deterministic exit status. `--advise` is never an alias for
`--explain`.
