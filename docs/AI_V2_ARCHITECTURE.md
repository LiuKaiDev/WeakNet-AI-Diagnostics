# AI V2.1: structured diagnosis contract and evidence explainer

AI V2.1 is an optional Python presentation layer downstream of the
deterministic C++ diagnosis pipeline:

```text
Linux observations -> IncidentEngine -> RootCauseEngine
    -> DiagnosticsQueryService -> DiagnosisSnapshot -> AI V2 explanation
```

The C++ engines remain authoritative. AI V2 cannot create incidents, invent a
root cause, change a type or confidence, reinterpret missing evidence, run
commands, mutate network state, or make remediation decisions. AI unavailable
therefore means only that an explanation is unavailable; deterministic
diagnosis remains available.

## Package and versioned contracts

The new implementation is isolated under `ai/v2`. It does not mix with the
legacy experimental scripts in `optional/experimental/log-analysis-tools`,
which analyze raw logs and may optionally call DashScope/OpenAI-compatible
services. AI V2 consumes structured diagnosis data only.

The external contract identifiers are explicit and experimental:

- `weaknet.ai.diagnosis.v1`
- `weaknet.ai.explanation.v1`

They are not declared a frozen ABI. Domain type, incident, and evidence-kind
values are non-empty strings so future C++ enum additions can pass through
without Python ordinal coupling. `EvidenceRole` is a strict enum with the
independent `Supporting`, `Contradicting`, and `Missing` values.

`DiagnosisSnapshot` contains the snapshot timestamp, overall status,
limitations, optional topology metadata, active incidents, and active root
cause hypotheses. Each incident and hypothesis retains typed-ish scope
metadata, timestamps, and structured evidence fields: kind, role, source,
provenance, scope, timestamp, optional value/unit, capability, and validity.
Unknown, unavailable, and missing values remain `None`; they are never changed
to zero or healthy values.

Evidence receives deterministic snapshot-local IDs such as
`hypothesis:<id>:supporting:0` and `hypothesis:<id>:missing:0`. IDs are not
globally persistent and never use random UUIDs. Supporting, contradicting, and
missing evidence remain separate in both the input and final report.

## D-Bus adapter and bounds

`DbusDiagnosisAdapter` is a pure transformation from a decoded V2
`GetDiagnosis`-shaped dictionary to `DiagnosisSnapshot`. It accepts either the
current top-level `state` shape or a nested `status.state` shape, preserves
unknown string enum values, and fails explicitly when required fields are
missing. It does not open D-Bus and does not depend on a live daemon.

Input is bounded by named limits: incident count, hypothesis count, evidence per
item, total evidence, string length, and serialized bytes. Exceeding a limit
raises `InputTooLarge`; no authoritative evidence is silently truncated.

## Prompt and data boundary

`EvidenceExplainerPromptBuilder` is pure and deterministic. It serializes only
the normalized `DiagnosisSnapshot` as sorted JSON inside
`<diagnosis_data>...</diagnosis_data>`. Interface names, SSIDs, provenance, and
other values are data, not instructions; JSON escaping prevents prompt-like
strings from changing the policy text. Raw daemon logs are never accepted.

The system instructions state that deterministic C++ diagnosis is authoritative,
confidence and type cannot change, suspected cannot become confirmed, missing
evidence is not support/contradiction, unsupported ISP/AP/server/interference
claims are forbidden, limitations must be explained, commands are forbidden,
and output must be the requested structure without hidden reasoning.

## Provider and grounding boundary

`LlmProvider` has one small asynchronous operation accepting an `LlmRequest`
with system/user prompt, response schema version, request ID, and metadata.
`FakeLlmProvider` is deterministic, makes no network calls, needs no API key,
marks output `provider: fake` and `simulated: true`, and can inject a supplied
or malformed payload for validator tests. No DashScope, Qwen, OpenAI, model
download, embeddings, or RAG dependency is present.

Provider output may contain only summary/explanation text and references to
existing IDs. `ProviderExplanationPayload` rejects unknown fields such as
authoritative type or confidence. `GroundingValidator` rejects unknown
hypothesis/evidence IDs, cross-hypothesis references, wrong roles, missing
evidence used as support, and omitted missing-evidence limitations. Explicit
contradictions must remain represented.

`EvidenceExplainerService` validates a normalized snapshot, builds the prompt,
calls the provider, validates its schema, performs grounding validation, and
builds `ExplanationReport`. The final report copies hypothesis type,
confidence, state, scope, and evidence details from the snapshot rather than
trusting provider text. The input snapshot is not mutated.

Error categories are explicit: invalid diagnosis input, input too large,
provider unavailable/timeout, invalid provider output, grounding violation,
and internal/provider-boundary failure. AI failures never map to network
health state.

## HTTP and legacy relationship

FastAPI is not installed in this repository, so no HTTP surface is added in
V2.1. A future read-only `/health/live`, `/v2/capabilities`, and
`/v2/explanations` adapter may wrap the same service without making a real
provider the default. The legacy V1 raw-log/RAG scripts remain untouched and
are not part of the C++ daemon availability path.

## Privacy and next stage

Complete prompts are not logged by default. If future debug logging is added it
must be explicit and disclose that addresses, interface metadata, and SSIDs may
be present. AI V2.1 makes zero external LLM calls. The next stage may add a
real DashScope/Qwen provider behind the same contract; it must not move
authority out of the deterministic diagnosis engines.

