# AI Runtime

The optional runtime is deliberately outside the C++ daemon:

```text
weaknet-dbus-server -- D-Bus V2 GetDiagnosis --> ai.v2.runtime --> Qwen
     ^                                      ^
     |                                      |
weaknetctl diagnose                  weaknetctl diagnose --explain/--advise
```

`weaknetctl diagnose` always queries deterministic C++ state first. `--explain`
adds an `ExplanationReport`; `--advise` calls the separate grounded advisor
and adds cited knowledge, safe checks and limitations. Neither mode can alter
diagnosis fields or the deterministic exit code.

## Start the service

```bash
WEAKNET_LLM_PROVIDER=fake python -m ai.v2.runtime
```

The service binds to `127.0.0.1:8765` by default and exposes:

```text
GET  /health/live
GET  /v2/capabilities
GET  /v2/rag/capabilities
POST /v2/explanations
POST /v2/explanations/current
POST /v2/advice
POST /v2/advice/current
```

The `current` endpoints use the session-D-Bus V2 source. Install
`dbus-next` for that source; unit tests inject a source/client and do not need
a live bus. A caller-supplied snapshot can be used without `dbus-next`.

## Failure and security behavior

The source distinguishes D-Bus unavailability, timeout and malformed payload;
the provider distinguishes authentication, rate limit, timeout, server,
schema and grounding errors. The CLI reports AI failure separately while
retaining Healthy/Degraded/Unknown from C++.

Only structured diagnosis fields used by the applicable prompt cross the AI
boundary. Knowledge is delimited as untrusted data and every advice item must
cite an exact stable chunk identity. Logs, environment dumps, API keys,
authorization headers, raw responses and model reasoning are not returned or
logged. There is no shell execution, remediation or mutating action.

When no active root-cause hypothesis exists, advice is `not_applicable`: the
runtime does not retrieve knowledge or call a provider.

An explicit live smoke test additionally requires a configured DashScope
provider, key, session bus and its opt-in flags. Ordinary tests make no
external provider calls.
