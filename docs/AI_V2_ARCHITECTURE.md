# AI Architecture

AI V2 is an optional Python presentation and advisory layer downstream of the
deterministic C++ diagnosis path:

```text
Linux observations -> IncidentEngine -> RootCauseEngine
  -> DiagnosticsQueryService -> DiagnosisSnapshot
  -> EvidenceExplainer (optional)
  -> RagQueryPlanner -> retrieval -> grounded RagAdvisor (optional)
```

The C++ engines remain authoritative. AI cannot create incidents, invent or
change a root cause, change confidence or evidence roles, reinterpret missing
evidence, run commands, mutate network state, or make remediation decisions.
If AI is unavailable, deterministic diagnosis remains available.

## Contracts and package boundary

The implementation lives under `ai/v2`. It consumes structured diagnosis data,
not daemon logs or human-readable CLI output. The legacy raw-log tools in
`optional/experimental/log-analysis-tools/` are isolated and are not part of
the main AI path.

External schema identifiers are explicit:

- `weaknet.ai.diagnosis.v1`
- `weaknet.ai.explanation.v1`
- `weaknet.ai.rag-advice.v1`

`DiagnosisSnapshot` retains status, limitations, topology metadata, active
incidents, root-cause hypotheses, timestamps, scope, provenance, validity and
separate supporting/contradicting/missing evidence. Unknown, unavailable and
missing values remain `None`; they are never changed to zero or healthy.

The D-Bus adapter accepts the V2 `GetDiagnosis` shape and applies bounded input
limits for collections, evidence, strings and serialized bytes. It is a pure
transformation and does not open D-Bus in unit tests.

## Explanation boundary

`EvidenceExplainerPromptBuilder` serializes only the normalized snapshot as
sorted JSON inside `<diagnosis_data>...</diagnosis_data>`. JSON escaping keeps
interface names, SSIDs and provenance data from changing the policy text. The
prompt requires the model to preserve deterministic type, confidence, state,
scope and evidence roles, explain limitations, avoid unsupported ISP/AP/server
or interference claims, and return the requested schema without hidden
reasoning.

`EvidenceExplainerService` validates input, builds the prompt, calls an
`LlmProvider`, validates the response, performs `GroundingValidator` checks,
and builds an `ExplanationReport` by copying authoritative fields from the
snapshot. Provider text may contain only explanation text and references to
existing IDs.

## Provider boundary

`LlmProvider` accepts an `LlmRequest` with prompts, schema version, request ID
and metadata. `FakeLlmProvider` is deterministic, offline and explicitly
marked simulated. `DashScopeProvider` is the optional real provider selected
with `WEAKNET_LLM_PROVIDER=dashscope`; it uses `DASHSCOPE_API_KEY` and
`WEAKNET_LLM_MODEL` and never silently falls back to fake output.

Provider failures are categorized as unavailable, timeout, authentication,
rate limit, invalid output, grounding violation or boundary failure. None of
these errors changes deterministic network state.

## Grounded RAG advisor

`RagQueryPlanner` projects a `DiagnosisSnapshot` into a privacy-aware,
deterministic query. Supporting, contradicting and missing evidence remain
distinct; missing evidence contributes check terms, not claims that a failure
exists. Retrieval preserves source/version, native score, rank and stable
citation IDs in a `RetrievalBundle`.

The advisor validates both diagnosis grounding and citation grounding. It may
return a summary, cited knowledge explanations, cited read-only checks and
limitations. It cannot replace root-cause type/confidence or promote missing
evidence. Retrieval failure is isolated from diagnosis and explanation.

Lexical BM25 is lightweight and always available. Dense BGE/FAISS retrieval,
reciprocal-rank fusion and an optional reranker are lazy, capability-gated
extensions configured by `ai/requirements-rag.txt`; normal imports do not
download models. The verified live path is lexical BM25 plus Qwen advisor.
The hybrid implementation is present, but a real BGE-M3/reranker live pass is
environment-limited when model weights are unavailable.

## Runtime endpoints

`python -m ai.v2.runtime` provides a loopback-only, read-only HTTP adapter:

- `GET /health/live`
- `GET /v2/capabilities`
- `GET /v2/rag/capabilities`
- `POST /v2/explanations` and `/v2/explanations/current`
- `POST /v2/advice` and `/v2/advice/current`

The `current` endpoints read `GetDiagnosis` over the session D-Bus. The CLI
prints deterministic output first and appends optional AI output. Missing
credentials leave liveness available while explanation/advice reports an
explicit provider error.

## Privacy and security

Prompts, API keys, authorization headers, raw model responses and reasoning
content are not logged or returned. Requests have bounded prompt/response
sizes, deadlines and limited retries. AI has no shell, agent, remediation or
network-control capability.
