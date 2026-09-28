"""Lazy dense embedding abstractions and deterministic test double."""
from __future__ import annotations

from typing import Iterable, Protocol
import hashlib
import math
import os


class EmbeddingModel(Protocol):
    model_name: str
    dimension: int
    def embed(self, texts: Iterable[str]) -> list[list[float]]: ...


class FakeEmbeddingModel:
    def __init__(self, dimension: int = 32, model_name: str = "fake") -> None:
        self.dimension, self.model_name = dimension, model_name

    def embed(self, texts: Iterable[str]) -> list[list[float]]:
        result: list[list[float]] = []
        for text in texts:
            vector = [0.0] * self.dimension
            for token in text.lower().split():
                digest = hashlib.sha256(token.encode()).digest()
                index = int.from_bytes(digest[:4], "big") % self.dimension
                vector[index] += 1.0 if digest[4] & 1 else -1.0
            norm = math.sqrt(sum(value * value for value in vector)) or 1.0
            result.append([value / norm for value in vector])
        return result


class BgeEmbeddingModel:
    """Sentence-transformers BGE adapter; imports the heavy stack only on use."""
    def __init__(self, model_name: str | None = None, *, device: str | None = None) -> None:
        self.model_name = model_name or os.environ.get("WEAKNET_RAG_EMBEDDING_MODEL", "BAAI/bge-m3")
        try:
            from sentence_transformers import SentenceTransformer  # type: ignore
        except ImportError as exc:
            raise RuntimeError("sentence-transformers is not installed") from exc
        kwargs = {"device": device} if device else {}
        self._model = SentenceTransformer(self.model_name, **kwargs)
        self.dimension = int(self._model.get_sentence_embedding_dimension())

    def embed(self, texts: Iterable[str]) -> list[list[float]]:
        values = self._model.encode(list(texts), normalize_embeddings=True, show_progress_bar=False)
        return values.tolist() if hasattr(values, "tolist") else [list(item) for item in values]
