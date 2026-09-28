"""Hybrid BM25 + dense + RRF + reranking orchestration."""
from __future__ import annotations
from dataclasses import dataclass
from time import perf_counter
from typing import Any

from .bm25 import BM25Retriever
from .embeddings import EmbeddingModel
from .faiss_index import VectorIndex
from .fusion import reciprocal_rank_fusion, DEFAULT_RRF_K
from .reranker import Reranker
from .schemas import KnowledgeChunk, RetrievalBundle, RetrievalHit, RetrievalQuery


@dataclass(frozen=True)
class RetrievalConfig:
    lexical_top_k: int = 10
    dense_top_k: int = 10
    fusion_top_k: int = 12
    final_top_k: int = 5
    rrf_k: int = DEFAULT_RRF_K


class HybridRetriever:
    def __init__(self, chunks: list[KnowledgeChunk], *, corpus_version: str = "local-v1", embedding_model: EmbeddingModel | None = None, vector_index: VectorIndex | None = None, reranker: Reranker | None = None, config: RetrievalConfig = RetrievalConfig()) -> None:
        self.chunks, self.corpus_version, self.embedding_model = chunks, corpus_version, embedding_model
        self.lexical = BM25Retriever(chunks)
        self.vector_index, self.reranker, self.config = vector_index, reranker, config

    def retrieve(self, query: RetrievalQuery) -> RetrievalBundle:
        started = perf_counter()
        lexical = self.lexical.search(query.search_terms, self.config.lexical_top_k)
        lexical_ms = (perf_counter() - started) * 1000
        dense: list[RetrievalHit] = []
        dense_ms = 0.0
        if self.embedding_model is not None and self.vector_index is not None and self.reranker is not None:
            started = perf_counter()
            vector = self.embedding_model.embed([query.search_text])[0]
            for chunk, score, rank in self.vector_index.search(vector, self.config.dense_top_k):
                dense.append(RetrievalHit.from_chunk(chunk, rank=rank, dense_rank=rank, dense_score=score, matched_query_id=query.query_id))
            dense_ms = (perf_counter() - started) * 1000
        fused = reciprocal_rank_fusion(lexical, dense, rrf_k=self.config.rrf_k, top_k=self.config.fusion_top_k)
        for hit in fused: hit.matched_query_id = query.query_id
        rerank_ms = 0.0
        if self.reranker is not None and fused:
            started = perf_counter(); fused = self.reranker.rerank(query.search_text, fused[:self.config.fusion_top_k]); rerank_ms = (perf_counter() - started) * 1000
        hits = fused[:self.config.final_top_k]
        mode = "hybrid" if dense and self.vector_index is not None and self.reranker is not None else "lexical"
        return RetrievalBundle(query=query, corpus_version=self.corpus_version, retrieval_mode=mode,
            embedding_model=getattr(self.embedding_model, "model_name", None), reranker_model=getattr(self.reranker, "model_name", None),
            hits=hits, status="ok" if hits else "empty", timing_ms={"bm25": lexical_ms, "dense": dense_ms, "reranker": rerank_ms})

    search = retrieve
