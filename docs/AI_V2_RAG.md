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

## Evaluation and next stage

`ai/v2/rag/evaluation.py` provides Recall@K, reciprocal rank/MRR, hit/miss, and
stage-separated result aggregation. Offline tests use fake embeddings, indexes,
and rerankers; a real local BGE/FAISS/reranker smoke is explicit and must be
reported separately as `RAG LIVE PASS`, `RAG LIVE FAIL`, or `RAG LIVE SKIP`. No
DashScope call is part of this stage. AI V2.3B may later pass a retrieval bundle
to Qwen with exact citation IDs; V2.3A intentionally does not.
