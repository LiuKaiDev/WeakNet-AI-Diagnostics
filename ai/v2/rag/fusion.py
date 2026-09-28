"""Reciprocal rank fusion preserving native-stage observability."""
from __future__ import annotations
from typing import Iterable
from .schemas import RetrievalHit

DEFAULT_RRF_K = 60


def reciprocal_rank_fusion(lexical: Iterable[RetrievalHit], dense: Iterable[RetrievalHit], *, rrf_k: int = DEFAULT_RRF_K, top_k: int = 20) -> list[RetrievalHit]:
    if rrf_k <= 0:
        raise ValueError("rrf_k must be positive")
    merged: dict[str, RetrievalHit] = {}
    for hit in lexical:
        current = merged.setdefault(hit.chunk_id, hit)
        current.bm25_rank = hit.bm25_rank or hit.rank; current.bm25_score = hit.bm25_score
        current.rrf_score = (current.rrf_score or 0.0) + 1.0 / (rrf_k + (hit.bm25_rank or hit.rank))
    for hit in dense:
        current = merged.setdefault(hit.chunk_id, hit)
        current.dense_rank = hit.dense_rank or hit.rank; current.dense_score = hit.dense_score
        current.rrf_score = (current.rrf_score or 0.0) + 1.0 / (rrf_k + (hit.dense_rank or hit.rank))
    result = sorted(merged.values(), key=lambda hit: (-(hit.rrf_score or 0), hit.chunk_id))[:max(top_k, 0)]
    for rank, hit in enumerate(result, 1): hit.rank = rank
    return result
