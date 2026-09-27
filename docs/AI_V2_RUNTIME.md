# AI V2 runtime wiring

The optional explanation product is deliberately outside `weaknetd`:

```text
weaknetd -- D-Bus V2 GetDiagnosis --> ai.v2.runtime -- provider --> Qwen
     ^                                      ^
     |                                      |
weaknetctl diagnose                  weaknetctl diagnose --explain
```

`weaknetctl diagnose` queries the deterministic C++ service and has no Python,
HTTP, or provider dependency. With `--explain`, it prints that same result
first, then makes one bounded request to the loopback AI service. The AI result
is an `ExplanationReport`; it cannot alter the type, confidence, evidence
roles, status, or exit code printed by the deterministic section.

## Service

```bash
WEAKNET_LLM_PROVIDER=fake python3 -m ai.v2.runtime
```

The default listener is `127.0.0.1:8765`. Endpoints are `GET /health/live`,
`GET /v2/capabilities`, `POST /v2/explanations` (caller-supplied canonical
snapshot), and `POST /v2/explanations/current` (D-Bus V2 source). Install the
optional `dbus-next` package for the current-diagnosis endpoint. Unit tests
inject a source/client and do not require a live bus.

`WEAKNET_LLM_PROVIDER=fake` starts without credentials. Selecting `dashscope`
without `DASHSCOPE_API_KEY` leaves liveness available but returns an explicit
`ProviderUnavailable` explanation error. There is no fake fallback. Model
selection remains generic through `WEAKNET_LLM_MODEL`.

## Failure and security behavior

The source distinguishes D-Bus unavailability, timeout, and malformed payload;
the provider distinguishes authentication, rate limit, timeout, server,
schema, and grounding errors. The CLI displays a concise AI error while
retaining the deterministic status exit code. The default CLI timeout is 38
seconds and is capped at 40 seconds.

Only structured diagnosis fields used by `PromptBuilder` cross the AI boundary.
No logs, environment dump, API key, authorization header, or model reasoning
content is logged or returned. No RAG, embeddings, FAISS, BM25, reranking,
agent, remediation, or deterministic-core changes are part of this stage.

The explicit live smoke test additionally requires
`WEAKNET_RUN_LIVE_LLM_TESTS=1`, `WEAKNET_RUN_LIVE_DBUS_AI_TEST=1`,
`WEAKNET_LLM_PROVIDER=dashscope`, `DASHSCOPE_API_KEY`, and a running session-
bus daemon. It is skipped in ordinary tests.
