# V2 Implementation Status and Remaining Limits

This filename is retained for link compatibility. The document describes the
current implementation status rather than an incremental development plan.

## Implemented

- C++20 runtime lifecycle, `NetworkEvent`, `EventBus` and `MetricStore`.
- RTNETLINK topology snapshots and selected-uplink modeling.
- `NETLINK_SOCK_DIAG` socket lifecycle, variable-size `TCP_INFO` parsing,
  reset-safe interval metrics and modeled socket-route attribution.
- `ActiveProbe` and native nl80211 Wi-Fi evidence.
- Deterministic `IncidentEngine` and evidence-driven `RootCauseEngine`.
- Read-only D-Bus V2 diagnostics API and `weaknetctl`.
- Optional structured Qwen/DashScope explanation with diagnosis grounding.
- Optional BM25/hybrid retrieval architecture and citation-grounded RAG
  advisor.
- WeakNet Lab scenarios, structured evaluation and capability-gated cleanup.
- Retained V1 C ABI/D-Bus compatibility tests and legacy staging targets.

## Environment-limited validation

- Privileged namespace scenarios require an approved Linux host with usable
  network namespaces, `CAP_NET_ADMIN`/`CAP_SYS_ADMIN` or equivalent scoped
  privilege. Missing capabilities produce `SKIP`, not a claimed live pass.
- eBPF build/runtime checks depend on Clang BPF support, libbpf, kernel BTF,
  attach permissions and kernel compatibility.
- Physical Wi-Fi evidence needs an nl80211-supported station environment.
- Dense BGE/FAISS and reranker code paths require compatible local model
  weights/runtime. The verified live advisor path is lexical BM25 plus Qwen;
  a real BGE-M3/reranker hybrid live pass is not claimed when weights are
  unavailable.

## Current limitations

- The current product validation path uses session D-Bus rather than a
  packaged system-bus/systemd deployment.
- Modeled route attribution is not full Linux RPDB/fwmark/source-policy/VRF/
  ECMP flow-hash equivalence.
- ActiveProbe currently uses bounded numeric IPv4 targets and does not provide
  DNS, traceroute, jitter or packet-loss windows.
- Root-cause hypotheses are conservative evidence-backed candidates, not proof
  of AP, ISP, interference or remote-server failure.
- AI/RAG is optional and depends on an external provider or available local
  models; it has no remediation or command-execution path.

## Possible future work

- Production system-bus policy, systemd packaging and measured least-
  privilege deployment.
- Broader Linux routing-policy and kernel/environment coverage.
- Broader capability-gated validation across kernels, Wi-Fi devices and eBPF
  attachment strategies.
- Reproducible live dense/reranker validation when model artifacts are
  available.

Historical implementation notes remain in `PHASE2_RUNTIME.md`,
`PHASE3_DATA_PLANE.md`, `PHASE4_NETLINK.md` and `PHASE5_SOCKET_TRACKER.md`.
They are supporting technical records, not the current project overview.
