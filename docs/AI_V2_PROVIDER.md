# AI Provider Integration

`DashScopeProvider` implements the replaceable `LlmProvider` protocol used by
the explanation and grounded-advisor services. The provider never rebuilds
diagnosis logic: it submits the structured prompt and returns a bounded,
validated response.

## Selection and secrets

```text
WEAKNET_LLM_PROVIDER=fake|dashscope
WEAKNET_LLM_MODEL=<model-name>
DASHSCOPE_API_KEY=<your-key>
```

The fake provider is deterministic, offline and explicitly marked simulated.
Selecting `dashscope` without a key raises `ProviderUnavailable`; it never
silently falls back to fake output. Credentials are read at construction,
excluded from metadata/errors/logs, and sent only as the HTTPS authorization
header.

## Transport

The default transport is a bounded standard-library HTTPS JSON client using the
OpenAI-compatible DashScope chat-completions endpoint. Requests use the
configured model (the provider default is `qwen-plus`), temperature `0.2`,
non-streaming JSON output, and the exact prompts produced by
`EvidenceExplainerPromptBuilder` or `RagAdvisorPromptBuilder`. Tests inject a
transport and perform no network I/O.

Defaults are intentionally bounded:

- 5 s connect timeout and 25 s read timeout;
- 30 s overall deadline (maximum 35 s);
- 64 KiB response and 256 KiB prompt limits;
- at most two attempts with deterministic backoff.

Only transport errors, HTTP 429 and HTTP 5xx are retried. Authentication,
client, schema and grounding failures are not retried, and oversized output is
rejected rather than truncated.

## Error and grounding behavior

The provider maps missing credentials/connection failure, authentication,
rate-limit, timeout, request, server and invalid-output conditions to explicit
AI-layer errors. No raw response body or secret is included in an error.

The returned JSON object is checked by the same schema and
`GroundingValidator` used for offline tests. Unknown hypothesis/evidence IDs,
wrong evidence roles, invented authoritative fields and omitted missing-
evidence limitations are rejected. The final `ExplanationReport` or
`RagAdviceReport` copies authoritative diagnosis fields from the snapshot.

## Optional live check

The opt-in live test requires all of the following and is skipped otherwise:

```text
WEAKNET_RUN_LIVE_LLM_TESTS=1
WEAKNET_LLM_PROVIDER=dashscope
DASHSCOPE_API_KEY=<your-key>
```

It checks provider/schema/grounding behavior and preservation of deterministic
type and confidence; it does not treat generated prose as a benchmark.

## Runtime service

Start the read-only local service with:

```bash
WEAKNET_LLM_PROVIDER=fake python -m ai.v2.runtime
```

The default listener is `127.0.0.1:8765`. Selecting DashScope without a key
keeps `/health/live` available and reports provider unavailability for model
requests. See [`AI_V2_RUNTIME.md`](AI_V2_RUNTIME.md) for endpoint and CLI
behavior.
