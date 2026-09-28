# Legacy Log-Analysis RAG Guide

This directory contains optional experiments that analyze WeakNet log text.
It is retained for historical reference and is not the current AI V2 product
path. Current explanation and grounded advice consume structured
`DiagnosisSnapshot` data under `ai/v2/`; see
[`../../../docs/AI_V2_ARCHITECTURE.md`](../../../docs/AI_V2_ARCHITECTURE.md).

## Isolated setup

Use a dedicated virtual environment and the dependency file in this directory:

```bash
python3 -m venv .venv
.venv/bin/pip install -r requirements.txt
```

If an experiment is configured for DashScope, provide credentials only through
the environment and use an explicit placeholder in documentation:

```bash
export DASHSCOPE_API_KEY="<your-key>"
```

Keys, captured customer logs, addresses, hostnames and packet payloads must not
be committed. These scripts are not loaded by `weaknet-dbus-server`, `weaknetctl`
or `ai.v2.runtime`, and their output is not authoritative diagnosis.

## Semantic limits

Legacy log fields may include names such as `tcp_loss_rate`. Those names are
compatibility data and must not be interpreted as an authoritative packet-loss
measurement. TCP retransmission, timeouts and missing observations retain
different meanings.

For current provider, RAG, grounding and citation behavior, use:

- [`../../../docs/AI_V2_PROVIDER.md`](../../../docs/AI_V2_PROVIDER.md)
- [`../../../docs/AI_V2_RAG.md`](../../../docs/AI_V2_RAG.md)
- [`../../../docs/AI_V2_RUNTIME.md`](../../../docs/AI_V2_RUNTIME.md)
