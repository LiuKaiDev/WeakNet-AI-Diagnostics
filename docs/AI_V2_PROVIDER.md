# AI V2.2 DashScope/Qwen provider

AI V2.2 adds `DashScopeProvider` behind the existing generic `LlmProvider`
protocol. The provider is replaceable: `EvidenceExplainerService` and
`GroundingValidator` do not know whether a response came from the fake
provider or DashScope.

## Selection and secrets

Provider selection is configuration-driven:

```text
WEAKNET_LLM_PROVIDER=fake|dashscope
WEAKNET_LLM_MODEL=qwen-plus
DASHSCOPE_API_KEY=<your-key>
```

The factory defaults to `fake` to keep offline development safe. Selecting
`dashscope` without `DASHSCOPE_API_KEY` raises `ProviderUnavailable`; it never
silently falls back to fake output. The key is read only at provider
construction, is excluded from `repr`, public metadata, reports, exceptions,
and logs, and is sent only as the HTTPS authorization header.

Capability reporting distinguishes configured from reachable. A configured key
does not trigger a network probe and does not claim that DashScope is
reachable. The fake provider remains explicitly simulated.

## Transport and request

The default transport is a small standard-library HTTPS JSON client. A local
`DashScopeTransport` seam is injected in tests, so unit tests perform no
network I/O. Requests use the OpenAI-compatible DashScope chat-completions
endpoint with:

- configured model, default `qwen-plus`;
- temperature default `0.2`;
- `stream: false`;
- JSON-object response format;
- the exact system and user prompts produced by
  `EvidenceExplainerPromptBuilder`.

The provider never rebuilds diagnosis logic or transforms evidence. It submits
the safely delimited structured prompt unchanged.

## Timeouts, retries, and limits

Defaults are bounded and reviewable:

- connect timeout: 5 seconds;
- read timeout: 25 seconds;
- overall request deadline: 30 seconds (maximum allowed: 35 seconds);
- maximum raw response: 64 KiB;
- maximum prompt bytes: 256 KiB;
- maximum attempts: 2;
- deterministic exponential retry backoff starting at 250 ms.

Retries are limited to transport unavailability, HTTP 429, and HTTP 5xx. Test
sleep is injectable. Authentication failures, client errors, malformed JSON,
schema errors, and grounding failures are never retried. Output is rejected if
it exceeds the response bound; it is never truncated into apparently valid
JSON.

## Error mapping

The provider maps failures to explicit AI-layer categories:

| Condition | Error |
| --- | --- |
| Missing key / connection failure | `ProviderUnavailable` |
| 401/403 | `ProviderAuthenticationError` |
| 429 | `ProviderRateLimited` |
| timeout / overall deadline | `ProviderTimeout` |
| other 4xx or invalid endpoint | `ProviderRequestError` |
| 5xx | `ProviderServerError` |
| oversized or malformed response | `InvalidProviderOutput` |

No raw response body, authorization header, or secret is included in errors.

## Structured output and grounding

The provider accepts one JSON object from the Qwen response and rejects
surrounding prose. It then passes the result through the same
`ProviderExplanationPayload` schema and `GroundingValidator` used by the fake
provider. Unknown hypothesis/evidence references, role mismatches, invented
authoritative fields, and omitted missing-evidence limitations remain errors.

The final `ExplanationReport` copies root-cause type, confidence, state, scope,
and evidence details from `DiagnosisSnapshot`. Model text cannot upgrade,
downgrade, confirm, or invent deterministic diagnosis. A DashScope report is
marked `provider: dashscope`, `simulated: false`, and includes latency,
finish-status, request correlation ID, and provider request ID when exposed.

## Live smoke test and privacy

The live smoke test is skipped unless all three conditions hold:

```text
WEAKNET_RUN_LIVE_LLM_TESTS=1
WEAKNET_LLM_PROVIDER=dashscope
DASHSCOPE_API_KEY=<your-key>
```

Normal Python tests never contact DashScope. The smoke test checks provider
success, schema/grounding validity, and preservation of the deterministic
`LocalLinkSuspected` type/confidence; it does not assert exact prose. Prompts
and diagnosis payloads are not logged by default. Future debug logging must be
explicit because payloads may include addresses, endpoints, interface names,
or SSIDs.

## Service operation

Start the optional read-only service with:

```text
WEAKNET_LLM_PROVIDER=fake python3 -m ai.v2.runtime
```

It binds to `127.0.0.1:8765` by default (`WEAKNET_AI_HOST` and
`WEAKNET_AI_PORT` may select another loopback address/port). The service stays
live and reports an unavailable capability when DashScope is selected without
`DASHSCOPE_API_KEY`; it does not silently construct `FakeLlmProvider`.
Provider errors are mapped to safe categories and messages, and authorization
headers, keys, raw model responses, and reasoning content are never returned
to the CLI.

## Non-goals

This stage does not add RAG, FAISS, BGE, embeddings, BM25, reranking, agents,
tool calls, shell execution, remediation, network probes, or C++ diagnosis
changes. The next stage is retrieval/RAG only after this provider contract and
grounding boundary have been evaluated.
