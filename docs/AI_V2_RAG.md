# AI V2.3A hybrid RAG retrieval

This stage adds advisory knowledge retrieval after deterministic C++
diagnosis:

```text
DiagnosisSnapshot -> RagQueryPlanner -> RetrievalQuery
  -> BM25 + optional BGE/FAISS -> reciprocal-rank fusion -> optional reranker
  -> RetrievalBundle
```

It cannot create incidents, root causes, confidence, status, confirmed claims,
or remediation. Missing evidence means “retrieve checks and observations,” not
that a failure exists. The planner is pure and deterministic, and does not
consume raw logs or parse `weaknetctl` prose. Scope/topology identifiers (SSID,
BSSID, addresses, interface names, and socket tuples) are excluded by default.

## Contracts and corpus

`RetrievalQuery` is `weaknet.ai.retrieval-query.v1`; `RetrievalBundle` is
`weaknet.ai.rag.v1`. Queries retain hypothesis ID/type, optional confidence,
supporting/contradicting/missing evidence kinds, incidents, stable search terms,
and a query ID. `KnowledgeDocument` requires document ID, title, source, source
type, version, and content. `KnowledgeChunk` retains document/source metadata
and gets a deterministic ID derived from document/version/section/ordinal and
content hash. Every indexed source is explicitly listed in
`ai/knowledge/manifest.json`; arbitrary recursive ingestion is not supported.

Chunking is Markdown-section aware, paragraph preserving, bounded by
`max_chars`, and uses bounded overlap only for long paragraphs. Ordering is
stable.

## V2.3B grounded advisor

The separate `RagAdvisorService` consumes a typed `RetrievalBundle` and a
`DiagnosisSnapshot`, then calls the existing replaceable `LlmProvider` through
`RagAdvisorPromptBuilder`. It does not perform corpus searches inside prompt
construction. The provider contract is `weaknet.ai.rag-advice.v1` and permits
only a summary, cited knowledge explanations, cited read-only recommended
checks, and limitations. Root-cause, diagnosis, confidence, incident, and
evidence-role fields are rejected as provider fields; authoritative hypothesis
type/confidence/roles are copied from the snapshot into `RagAdviceReport`.

`CitationGroundingValidator` requires every knowledge explanation and check to
cite an exact `document_id/chunk_id@source_version` identity present in the
current bundle. It rejects invented, malformed, positional, or wrong-bundle
citations, missing-evidence relationship violations, obvious authority
escalation, and claims that an action was executed. This is a conservative
support boundary: citation presence does not prove full semantic entailment,
so obvious overclaims are rejected and deeper semantic judging is deferred.

The advisor prompt separates `<diagnosis_data>` from
`<retrieved_knowledge>` and treats knowledge as untrusted DATA. Context is
bounded by chunk count, per-chunk characters, total characters, and a small
document-diversity pass. Lexical-only retrieval is reported as `lexical`;
`hybrid` is emitted only when every bundle has dense retrieval.

`POST /v2/advice` and `POST /v2/advice/current` are additive read-only endpoints.
The existing `/v2/explanations` behavior is unchanged. `weaknetctl diagnose
--advise` is an explicit product mode and prints deterministic diagnosis first,
then advice, citations, limitations, and retrieval mode. No commands are
executed and no remediation is implemented. A RAG/provider failure remains
isolated from deterministic diagnosis and normal evidence explanation.
An empty hypothesis set produces the non-error `not_applicable` report state
before retrieval or provider execution; it is not reported as RAG unavailable.

The curated root-cause coverage matrix is:

| Root-cause type | Corpus coverage |
| --- | --- |
| `UplinkAvailabilityProblem` | `topology-path-semantics` — authoritative uplink state and topology checks |
| `LocalRoutingProblem` | `topology-path-semantics` — route conflict/unavailable boundaries |
| `NetworkPathDegradation` | `topology-path-semantics`, `root-cause-semantics` — TCP/path evidence |
| `RemoteOrUpstreamDegradation` | `root-cause-semantics`, `probe-wifi-semantics`, `topology-path-semantics` — beyond-gateway attribution |
| `LocalLinkSuspected` | `root-cause-semantics`, `probe-wifi-semantics` — Wi-Fi/link evidence |
| `InsufficientEvidence` | `root-cause-semantics`, `probe-wifi-semantics` — missing observations and uncertainty |

The matrix is represented in the manifest through each document's
`applicable_root_causes`; it is not inferred from arbitrary repository files.

## Retrieval stages

The standard-library BM25 implementation uses deterministic tokenization,
stable chunk ordering, bounded `top_k`, and exposes native scores. Dense
retrieval is isolated behind `EmbeddingModel`; `BgeEmbeddingModel` defaults to
`BAAI/bge-m3` and loads only when explicitly constructed. `VectorIndex` stores
metadata separately and rejects missing, malformed, stale, count-mismatched, or
dimension-mismatched indexes. It can persist FAISS when installed and has a
small JSON fallback for local tests.

`HybridRetriever` runs BM25 and dense search independently. It never adds their
incomparable scores. `reciprocal_rank_fusion` uses named `rrf_k` (default 60),
preserves native ranks/scores, and tie-breaks by stable chunk ID. A bounded
reranker receives only the fusion pool; fake and lazy BGE implementations are
provided. Final output is a versioned `RetrievalBundle` whose citation identity
is `document_id/chunk_id@source_version`.

`BgeEmbeddingModel` and `BgeReranker` accept either a Hugging Face repository ID
or an explicit local filesystem path through the existing model environment
variables. Absolute and explicit relative paths are loaded with
`local_files_only=True`; a missing local path raises immediately and never
falls back to a repository download or a different checkpoint.

In the current validation environment, the CPU packages installed successfully
but the Hugging Face endpoint was unreachable, so neither configured BGE
checkpoint was loaded. The real end-to-end smoke therefore remains explicitly
`RAG LIVE SKIP` until those exact weights are placed in a trusted local cache or
an approved model source becomes available.

## Capabilities and lifecycle

`RagCapabilities` distinguishes corpus availability, BM25, model configured vs
loaded, FAISS installed, index available vs compatible, reranker configured vs
available, and `hybrid_ready`. Normal `import ai.v2` and the explanation path
remain usable without RAG dependencies. Optional dependencies are listed in
`ai/requirements-rag.txt`; CPU use is supported and model download/cache
behavior is controlled by the sentence-transformers installation. The CLI
supports explicit `build` and lightweight lexical `query`; full indexes are
not rebuilt on normal requests.

For CPU-only WSL, install with `pip install -r ai/requirements-rag.txt`.
The requirements file explicitly selects the PyTorch CPU wheel index and pins
the CPU torch variant, avoiding accidental CUDA toolkit downloads. The default
embedding model is `BAAI/bge-m3`; the default reranker is
`BAAI/bge-reranker-v2-m3`. Both names remain configurable through
`WEAKNET_RAG_EMBEDDING_MODEL` and `WEAKNET_RAG_RERANKER_MODEL`. Model files are
downloaded only when the corresponding adapter is constructed and reside in
the normal Hugging Face cache outside the repository; generated indexes belong
in the ignored `.weaknet-rag-index/` directory.

The read-only `GET /v2/rag/capabilities` endpoint reports capability state
without loading models. RAG errors never break deterministic diagnosis or
non-RAG Qwen explanations.

## Evaluation and live validation

`ai/v2/rag/evaluation.py` provides Recall@K, reciprocal rank/MRR, hit/miss, and
stage-separated result aggregation. Offline tests use fake embeddings, indexes,
and rerankers; a real local BGE/FAISS/reranker smoke is explicit and must be
reported separately as `RAG LIVE PASS`, `RAG LIVE FAIL`, or `RAG LIVE SKIP`.
AI V2.3B has an explicit opt-in Qwen lexical-advisor test guarded by
`WEAKNET_RUN_LIVE_RAG_ADVISOR=1`, DashScope provider selection, and configured
credentials. It uses actual local-corpus BM25 retrieval and exact citations;
when BGE is unavailable it must report `RAG ADVISOR LIVE PASS (LEXICAL)`, never
a hybrid pass. Real hybrid must be revalidated once BGE embedding and reranker
weights are available.
