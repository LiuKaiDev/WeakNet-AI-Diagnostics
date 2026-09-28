# AI 与 RAG

WeakNet 的 AI runtime 是可选、只读、独立进程。它消费确定性 C++ 诊断产生的结构化 `DiagnosisSnapshot`，不会采集 kernel data、创建 incident、修改 root-cause state 或执行 remediation。

## 边界总览

```text
DiagnosticsQueryService
  -> D-Bus GetDiagnosis
  -> DiagnosisSnapshot
       ├─ EvidenceExplainer -> LlmProvider -> ExplanationReport
       └─ RagQueryPlanner -> RetrievalBundle -> RagAdvisor -> RagAdviceReport
```

核心 schema identifiers：

- `weaknet.ai.diagnosis.v1`
- `weaknet.ai.explanation.v1`
- `weaknet.ai.retrieval-query.v1`
- `weaknet.ai.rag.v1`
- `weaknet.ai.rag-advice.v1`

这些是 wire/data contract 名称，不表示文档中的版本演进叙事。

## DiagnosisSnapshot

`DiagnosisSnapshot` 保留：

- snapshot time 与 overall status；
- limitations 和 optional topology metadata；
- active incidents；
- active root-cause hypotheses；
- structured Supporting、Contradicting、Missing evidence；
- scope、timestamps、provenance、validity、capability 和 optional value/unit。

Unknown、unavailable 和 missing value 保持 `None`，不会被改写为 zero 或 healthy。Evidence 使用 snapshot-local deterministic ID，例如 hypothesis + role + ordinal；ID 不依赖 random UUID，也不声明跨 snapshot 永久稳定。

D-Bus adapter 是纯转换层，接受 `GetDiagnosis` decoded dictionary，并应用 incident/hypothesis/evidence count、string length 和 serialized byte bounds。缺少 required identity 或超过 bounds 会显式失败，不静默截断 authoritative evidence。

## EvidenceExplainer

`EvidenceExplainerPromptBuilder` 只把 normalized snapshot 作为 sorted JSON 放入明确的 `<diagnosis_data>` boundary。Interface、SSID、provenance 或 address-like values 都是 data；JSON escaping 防止其中的 prompt-like text 改变 system policy。

Prompt policy 要求：

- C++ diagnosis 为 authority；
- root-cause type、confidence、state、scope 不可修改；
- Missing evidence 不能被用作 support 或 contradiction；
- `suspected` 不能升级为 `confirmed`；
- 不生成 unsupported ISP/AP/server/interference claim；
- 不返回 command、shell action 或 hidden reasoning；
- 输出必须符合指定 JSON schema。

`EvidenceExplainerService` 执行 input validation、prompt build、provider call、provider schema validation、`GroundingValidator` 和 report construction。最终 `ExplanationReport` 的 authoritative fields 从 snapshot 复制，模型只贡献 summary/explanation text 和 existing IDs 的引用。

## LlmProvider 与 DashScope/Qwen

`LlmProvider` 是可替换 async boundary。`LlmRequest` 包含 system/user prompt、response schema version、request ID 和 metadata。

实现包括：

- `FakeLlmProvider`：deterministic、无网络、无需 key，明确标记 `simulated: true`，仅用于测试和 simulation；
- `DashScopeProvider`：真实 provider，通过 OpenAI-compatible DashScope chat-completions endpoint 调用配置的 Qwen model，标记 `simulated: false`。

配置：

```text
WEAKNET_LLM_PROVIDER=fake|dashscope
WEAKNET_LLM_MODEL=<model>
DASHSCOPE_API_KEY=<your-key>
```

选择 `dashscope` 但缺少 key 时返回 `ProviderUnavailable`，不会 fallback 到 fake result。Key 不进入 `repr`、capability metadata、report、exception 或 log，只作为 HTTPS authorization header 使用。

默认 transport bounds：

- connect timeout 5 秒；
- read timeout 25 秒；
- overall deadline 30 秒，允许上限 35 秒；
- raw response 64 KiB；
- prompt 256 KiB；
- 最多两次 request attempt。

只有 transport unavailable、HTTP 429 和 5xx 会 retry。Authentication、4xx request、invalid JSON、schema 和 grounding error 不 retry。Oversized response 会被拒绝，不截断成看似有效的 JSON。

## GroundingValidator

Provider payload 不允许携带 authoritative diagnosis fields。`GroundingValidator` 检查：

- hypothesis/evidence ID 必须存在；
- evidence 必须属于引用的 hypothesis；
- role 必须与 snapshot 一致；
- Missing evidence 不能变成 support；
- contradiction 不能被隐藏成 support；
- limitations 必须覆盖关键 missing evidence；
- provider 不能升级或修改 type/confidence/state。

错误分类包括 invalid diagnosis、input too large、provider unavailable/authentication/rate-limit/timeout/server error、invalid provider output、grounding violation 和 internal boundary failure。所有 AI errors 都与 network health state 分离。

## RAG query 与 corpus

`RagQueryPlanner` 从每个 root-cause hypothesis 生成 deterministic、privacy-aware `RetrievalQuery`。它不读取 daemon logs，也不解析 `weaknetctl` prose。Supporting、Contradicting、Missing evidence 保持分离；Missing evidence 只增加 check/procedure terms，不声明缺失条件真实存在。

SSID、BSSID、addresses、interface name 和 socket tuple 等 scope identifiers 默认不进入 retrieval query。

允许索引的知识源由 `ai/knowledge/manifest.json` 明确列出。`KnowledgeDocument` 保留 document ID、title、source、source type、version 和 content；Markdown-aware chunker 生成 bounded `KnowledgeChunk`，其 ID 由 document/version/section/ordinal/content hash 决定。不会递归摄取任意仓库文件。

`ai/knowledge/` 是 runtime RAG corpus/data，可保持英文；它不作为普通展示文档翻译，以免改变 retrieval/evaluation behavior。

## Retrieval pipeline

```text
RetrievalQuery
  -> BM25 lexical retrieval
  -> optional BGE embedding + FAISS vector search
  -> reciprocal-rank fusion (RRF)
  -> optional reranker
  -> RetrievalBundle
```

BM25 不需要模型权重，是始终可用的 baseline。Dense embedding、FAISS 和 reranker 通过 `ai/requirements-rag.txt` 提供，lazy import/load，普通 runtime startup 不下载模型。

Lexical 与 dense stage 独立运行，RRF 使用 rank 而不是把不同 score scale 直接相加。`RetrievalBundle` 保存 native score、rank、source/version、stable citation ID、timing 和 capability state。

Retrieval mode 只有在 dense result 与 reranker 都实际参与时才标记 `hybrid`，否则为 `lexical`。真实 lexical BM25 + Qwen advisor 已验证；真实 BGE-M3/reranker hybrid execution 取决于本地模型权重和环境，不能用 fixture 模拟结果代替 live pass。

## RagAdvisor 与 CitationGroundingValidator

`RagAdvisorService` 接收 `DiagnosisSnapshot` 和 typed `RetrievalBundle`，通过 `RagAdvisorPromptBuilder` 调用同一个 replaceable `LlmProvider`。Provider output 只允许：

- summary；
- cited knowledge explanations；
- cited read-only recommended checks；
- limitations。

它不能返回 authoritative root cause、confidence、incident 或 evidence-role fields。

`CitationGroundingValidator` 要求每个知识性说明和检查项都引用当前 bundle 中精确的：

```text
document_id/chunk_id@source_version
```

Invented、malformed、wrong-version、wrong-bundle 或遗漏 citation 会被拒绝。`RagAdviceReport` 同样从 snapshot 复制 type、confidence、state 和 evidence roles。

Retrieved knowledge 是运维参考，不是 observed evidence。RAG 不会把文档陈述写回 deterministic diagnosis。

## Runtime service

```bash
.venv/bin/python -m ai.v2.runtime
```

默认 listener 为 `127.0.0.1:8765`：

```text
GET  /health/live
GET  /v2/capabilities
GET  /v2/rag/capabilities
POST /v2/explanations
POST /v2/explanations/current
POST /v2/advice
POST /v2/advice/current
```

`current` endpoints 通过 session D-Bus 获取当前 diagnosis，需要 optional `dbus-next`。Caller-supplied snapshot endpoints 和 fake provider 可在无 live D-Bus、无 key、无网络环境中测试。

Liveness 与 provider availability 分离：缺少 DashScope key 时 service 仍可报告 live，但 model request 返回明确 unavailable error。没有 active hypothesis 时 advice 返回 `not_applicable`，不进行 retrieval 或 provider call。

## Privacy 与安全

- Prompt、diagnosis payload、API key、authorization header、raw provider response 和 reasoning content 默认不记录。
- 所有输入、输出、collection size、string、timeout 和 retry 均有 bounds。
- Knowledge context 被标记为 untrusted data。
- AI runtime 只读，不包含 agent loop、tool execution、shell 或自动 remediation。
- 普通 test suite 不调用 DashScope/Qwen；live tests 需要显式 opt-in environment variables。

AI/provider/RAG failure 只影响 explanation/advice availability，不影响 `weaknetctl diagnose` 的确定性结果。
