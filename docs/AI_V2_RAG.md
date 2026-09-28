# Grounded RAG Architecture

RAG is an optional advisory layer after deterministic C++ diagnosis:

```text
DiagnosisSnapshot
  -> RagQueryPlanner
  -> BM25
  -> optional dense BGE/FAISS
  -> reciprocal-rank fusion (RRF)
  -> optional reranker
  -> RetrievalBundle
  -> grounded advisor
```

It cannot create incidents or root causes, change confidence or status,
promote missing evidence, or perform remediation. Missing evidence contributes
terms for checks and observations; it is never turned into a claim that a
condition exists.

## Query and corpus

`RagQueryPlanner` consumes structured `DiagnosisSnapshot` values and produces
the versioned `weaknet.ai.retrieval-query.v1` query. It does not read logs or
parse CLI prose. Privacy-sensitive identifiers such as SSID, BSSID, addresses,
interface names and socket tuples are excluded by default.

The allowlisted corpus is declared in `ai/knowledge/manifest.json`. Each
`KnowledgeDocument` and bounded Markdown-aware `KnowledgeChunk` retains source,
version and deterministic identity. Arbitrary recursive ingestion is not
supported.

BM25 is the lightweight baseline. Dense embedding (`BGE`/`sentence-transformers`)
and FAISS are lazy optional capabilities; a reranker is another optional
stage. Reciprocal-rank fusion keeps lexical and dense retrieval independent and
retains native scores, ranks and provenance in the `RetrievalBundle`.

## Grounded advisor

`RagAdvisorService` receives a typed bundle and snapshot, then calls the
replaceable `LlmProvider` through `RagAdvisorPromptBuilder`. The provider
contract is `weaknet.ai.rag-advice.v1` and allows only a summary, cited
knowledge explanations, cited read-only checks and limitations.

`CitationGroundingValidator` requires each knowledge explanation and check to
cite an exact `document_id/chunk_id@source_version` present in the current
bundle. Diagnosis grounding remains mandatory as well. The resulting
`RagAdviceReport` copies authoritative type, confidence, state and evidence
roles from the snapshot.

## Validation boundary

Offline tests cover query planning, chunking, BM25, fusion, schema and
grounding. The real validated live path is lexical BM25 plus Qwen advisor. The
hybrid architecture is available, but a real BGE-M3/reranker live run depends
on local model weights and compatible runtime access; fixture results must not
be presented as universal production accuracy.

See [`AI_V2_ARCHITECTURE.md`](AI_V2_ARCHITECTURE.md) and
[`AI_V2_RUNTIME.md`](AI_V2_RUNTIME.md) for the provider and HTTP boundaries.
