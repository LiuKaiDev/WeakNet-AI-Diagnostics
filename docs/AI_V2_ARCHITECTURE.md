# AI V2.3A: structured diagnosis, evidence explainer, and advisory retrieval

AI V2.2 is an optional Python presentation layer downstream of the
deterministic C++ diagnosis pipeline:

```text
Linux observations -> IncidentEngine -> RootCauseEngine
    -> DiagnosticsQueryService -> DiagnosisSnapshot -> AI V2 explanation
    -> RetrievalQuery -> optional hybrid retrieval -> grounded advisor (explicit mode)
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
unknown string enum values, and fails explicitly when required collections or
identity fields are missing. Fields not exposed by the current serializer (for
example the snapshot timestamp and hypothesis timestamps) remain `None` rather
than being fabricated. It does not open D-Bus and does not depend on a live
daemon.

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
or malformed payload for validator tests. `DashScopeProvider` is the optional
real provider, selected explicitly with `WEAKNET_LLM_PROVIDER=dashscope` and
configured through `DASHSCOPE_API_KEY` / `WEAKNET_LLM_MODEL`. It uses an
injectable bounded HTTPS transport, defaults to `qwen-plus`, and reports
`simulated: false`. Missing credentials never fall back to fake output.

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

## AI V2.3A retrieval boundary

`ai/v2/rag` is an optional, advisory retrieval layer. `RagQueryPlanner`
projects each structured `DiagnosisSnapshot` hypothesis into a deterministic,
privacy-aware `RetrievalQuery`; it never reads daemon logs, human-readable CLI
output, or user-controlled scope identifiers. Supporting, contradicting, and
missing evidence remain distinct. Missing evidence adds check/procedure terms,
not claims that the missing condition exists.

The allowlisted corpus in `ai/knowledge/manifest.json` produces provenance-
preserving `KnowledgeDocument` and deterministic `KnowledgeChunk` records.
BM25 is always lightweight and available. Dense retrieval is behind lazy
`EmbeddingModel`, `VectorIndex`/FAISS, and `Reranker` abstractions. Hybrid
retrieval runs lexical and dense stages independently, combines ranks with
named reciprocal-rank fusion, then applies a bounded reranker. Native scores,
ranks, source/version, and stable citation IDs are retained in
`RetrievalBundle`.

Model/index configuration is optional (`ai/requirements-rag.txt`); normal
imports never download models. Capabilities distinguish configured, loaded,
installed, compatible, and ready states. The additive
`GET /v2/rag/capabilities` endpoint is read-only. Retrieval failure cannot
affect deterministic diagnosis or the existing explanation endpoint.

AI V2.3A deliberately stopped at `RetrievalBundle`. AI V2.3B now adds the
separate grounded advisor below. No agent, shell execution, or remediation is
implemented.

AI V2.3B adds an explicit grounded-advisor mode after retrieval. Advice is
validated twice: the existing diagnosis grounding contract remains mandatory,
and `CitationGroundingValidator` verifies stable knowledge citations. Advice
cannot replace deterministic root-cause type/confidence or promote missing
evidence. It is available through `/v2/advice` and `weaknetctl diagnose
--advise`; `/v2/explanations` and `--explain` retain their prior behavior.

## Runtime product path

The optional `ai.v2.runtime` module provides a standard-library,
loopback-only HTTP adapter around `EvidenceExplainerService`:

- `GET /health/live` is provider-independent liveness;
- `GET /v2/capabilities` reports selection and configuration truthfully;
- `GET /v2/rag/capabilities` reports optional retrieval state;
- `POST /v2/explanations` accepts a canonical `DiagnosisSnapshot`;
- `POST /v2/explanations/current` obtains the snapshot through D-Bus V2;
- `POST /v2/advice` and `/v2/advice/current` provide separate grounded advice.

The C++ CLI calls only the explicit `--explain` path and uses a bounded
localhost request. A missing DashScope key leaves the service live but makes
explanation calls return a structured `ProviderUnavailable` error. There is
no fake fallback when DashScope is selected. The real source uses
`com.example.WeakNet`, `/com/example/WeakNet/V2`,
`com.example.WeakNet.Diagnostics2`, and `GetDiagnosis`; `dbus-next` is loaded
only when that source is used, and injected clients keep tests independent of
a live bus. Legacy V1 raw-log/RAG scripts remain isolated.

## Privacy and next stage

Complete prompts are not logged by default. If future debug logging is added it
must be explicit and disclose that addresses, interface metadata, and SSIDs may
be present. DashScope requests use a 30-second overall deadline, a 64 KiB
response bound, and at most two attempts for transport/429/5xx failures.
Authentication, client, schema, and grounding failures are not retried. AI
V2.2 makes no provider calls during normal tests; an opt-in live smoke test
requires explicit selection, a flag, and an environment key. The provider must
not move authority out of the deterministic diagnosis engines.
