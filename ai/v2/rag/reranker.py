"""Bounded reranker abstraction."""
from __future__ import annotations
from typing import Iterable, Protocol
import re
import os
from pathlib import Path
from .schemas import RetrievalHit


class Reranker(Protocol):
    model_name: str
    def rerank(self, query: str, hits: Iterable[RetrievalHit]) -> list[RetrievalHit]: ...


class FakeReranker:
    model_name = "fake"
    def rerank(self, query: str, hits: Iterable[RetrievalHit]) -> list[RetrievalHit]:
        terms = set(re.findall(r"[a-z0-9]+", query.lower()))
        scored = []
        for hit in hits:
            score = sum(1 for term in re.findall(r"[a-z0-9]+", hit.text.lower()) if term in terms)
            hit.reranker_score = float(score)
            scored.append(hit)
        result = sorted(scored, key=lambda hit: (-(hit.reranker_score or 0), hit.chunk_id))
        for rank, hit in enumerate(result, 1): hit.rank = rank
        return result


class BgeReranker:
    def __init__(self, model_name: str | None = None) -> None:
        self.model_name = model_name or os.environ.get("WEAKNET_RAG_RERANKER_MODEL", "BAAI/bge-reranker-v2-m3")
        local_path = Path(self.model_name).expanduser()
        is_local = local_path.is_absolute() or self.model_name.startswith((".", "~")) or local_path.exists()
        if is_local and not local_path.is_dir():
            raise FileNotFoundError(f"local reranker model path does not exist: {local_path}")
        try:
            from sentence_transformers import CrossEncoder  # type: ignore
        except ImportError as exc:
            raise RuntimeError("sentence-transformers is not installed") from exc
        self._model = CrossEncoder(self.model_name, local_files_only=is_local)

    def rerank(self, query: str, hits: Iterable[RetrievalHit]) -> list[RetrievalHit]:
        result = list(hits)
        scores = self._model.predict([(query, hit.text) for hit in result], show_progress_bar=False)
        for hit, score in zip(result, scores): hit.reranker_score = float(score)
        result.sort(key=lambda hit: (-(hit.reranker_score or 0), hit.chunk_id))
        for rank, hit in enumerate(result, 1): hit.rank = rank
        return result
