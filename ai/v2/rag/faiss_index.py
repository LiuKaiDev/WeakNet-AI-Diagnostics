"""FAISS vector index with explicit compatibility metadata and lazy imports."""
from __future__ import annotations

import json
from pathlib import Path
from typing import Iterable
import math

from .schemas import KnowledgeChunk, RetrievalHit


class IndexErrorRag(RuntimeError):
    pass


class VectorIndex:
    def __init__(self, chunks: Iterable[KnowledgeChunk], embeddings: Iterable[Iterable[float]], *, model_name: str, corpus_version: str, dimension: int | None = None, chunking_version: str = "v1") -> None:
        pairs = sorted(zip(list(chunks), embeddings), key=lambda pair: pair[0].chunk_id)
        self.chunks = [pair[0] for pair in pairs]
        self.vectors = [list(map(float, pair[1])) for pair in pairs]
        self.model_name, self.corpus_version, self.chunking_version = model_name, corpus_version, chunking_version
        self.dimension = dimension or (len(self.vectors[0]) if self.vectors else 0)
        if len(self.chunks) != len(self.vectors) or any(len(item) != self.dimension for item in self.vectors):
            raise IndexErrorRag("vector count or dimension mismatch")

    def search(self, vector: Iterable[float], top_k: int = 10) -> list[tuple[KnowledgeChunk, float, int]]:
        query = list(map(float, vector))
        if len(query) != self.dimension:
            raise IndexErrorRag("query dimension mismatch")
        norm = math.sqrt(sum(x * x for x in query)) or 1.0
        query = [x / norm for x in query]
        scores = []
        for chunk, candidate in zip(self.chunks, self.vectors):
            scores.append((sum(a * b for a, b in zip(query, candidate)), chunk))
        scores.sort(key=lambda item: (-item[0], item[1].chunk_id))
        return [(chunk, score, rank) for rank, (score, chunk) in enumerate(scores[:max(top_k, 0)], 1)]

    def metadata(self) -> dict[str, object]:
        return {"corpus_version": self.corpus_version, "embedding_model": self.model_name,
                "embedding_dimension": self.dimension, "chunk_count": len(self.chunks),
                "chunking_version": self.chunking_version, "chunk_ids": [item.chunk_id for item in self.chunks]}

    def save(self, directory: str | Path) -> None:
        path = Path(directory); path.mkdir(parents=True, exist_ok=True)
        (path / "metadata.json").write_text(json.dumps(self.metadata(), sort_keys=True, indent=2), encoding="utf-8")
        try:
            import faiss  # type: ignore
            import numpy as np  # type: ignore
            index = faiss.IndexFlatIP(self.dimension)
            index.add(np.asarray(self.vectors, dtype="float32"))
            faiss.write_index(index, str(path / "index.faiss"))
        except ImportError:
            (path / "vectors.json").write_text(json.dumps(self.vectors), encoding="utf-8")

    @classmethod
    def load(cls, directory: str | Path, chunks: Iterable[KnowledgeChunk], *, expected: dict[str, object] | None = None) -> "VectorIndex":
        path = Path(directory)
        try:
            metadata = json.loads((path / "metadata.json").read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise IndexErrorRag("missing or malformed vector metadata") from exc
        if expected and any(metadata.get(key) != value for key, value in expected.items()):
            raise IndexErrorRag("vector index metadata is incompatible")
        selected_chunks = sorted(list(chunks), key=lambda item: item.chunk_id)
        if metadata.get("chunk_count") != len(selected_chunks) or metadata.get("chunk_ids") != [item.chunk_id for item in selected_chunks]:
            raise IndexErrorRag("vector index chunk mapping is incompatible")
        try:
            vectors = json.loads((path / "vectors.json").read_text(encoding="utf-8"))
        except (OSError, ValueError):
            try:
                import faiss  # type: ignore
                import numpy as np  # type: ignore
                index = faiss.read_index(str(path / "index.faiss"))
                vectors = np.asarray(index.reconstruct_n(0, index.ntotal)).tolist()
            except Exception as exc:
                raise IndexErrorRag("missing or unreadable vector index") from exc
        return cls(selected_chunks, vectors, model_name=str(metadata.get("embedding_model", "")),
                   corpus_version=str(metadata.get("corpus_version", "")), dimension=int(metadata.get("embedding_dimension", 0)),
                   chunking_version=str(metadata.get("chunking_version", "v1")))
