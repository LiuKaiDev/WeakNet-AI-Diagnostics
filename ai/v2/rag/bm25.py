"""Small deterministic BM25 implementation with no third-party dependency."""
from __future__ import annotations

import math
import re
from dataclasses import dataclass
from typing import Iterable

from .schemas import KnowledgeChunk, RetrievalHit

TOKEN_RE = re.compile(r"[A-Za-z0-9]+")


def tokenize(value: str) -> list[str]:
    expanded = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", value).replace("_", " ").replace("-", " ")
    return [token.lower() for token in TOKEN_RE.findall(expanded)]


@dataclass(frozen=True)
class LexicalResult:
    chunk: KnowledgeChunk
    score: float
    rank: int


class BM25Retriever:
    def __init__(self, chunks: Iterable[KnowledgeChunk] = (), *, k1: float = 1.5, b: float = 0.75) -> None:
        self.k1, self.b = k1, b
        self.chunks = sorted(list(chunks), key=lambda chunk: chunk.chunk_id)
        self._tokens = [tokenize(f"{chunk.title} {chunk.section} {chunk.text} {' '.join(chunk.tags)}") for chunk in self.chunks]
        self._length = [len(item) for item in self._tokens]
        self._avgdl = sum(self._length) / len(self._length) if self._length else 0.0
        self._df: dict[str, int] = {}
        for terms in self._tokens:
            for term in set(terms):
                self._df[term] = self._df.get(term, 0) + 1

    def search(self, query: str | Iterable[str], top_k: int = 10) -> list[RetrievalHit]:
        if top_k <= 0:
            return []
        query_terms = tokenize(query) if isinstance(query, str) else tokenize(" ".join(map(str, query)))
        n = len(self.chunks)
        scores: list[tuple[float, KnowledgeChunk]] = []
        for chunk, terms, length in zip(self.chunks, self._tokens, self._length):
            counts = {term: terms.count(term) for term in set(query_terms)}
            score = 0.0
            for term, frequency in counts.items():
                if not frequency:
                    continue
                df = self._df.get(term, 0)
                idf = math.log(1 + (n - df + 0.5) / (df + 0.5))
                denominator = frequency + self.k1 * (1 - self.b + self.b * length / self._avgdl) if self._avgdl else 1.0
                score += idf * frequency * (self.k1 + 1) / denominator
            scores.append((score, chunk))
        scores.sort(key=lambda pair: (-pair[0], pair[1].chunk_id))
        return [RetrievalHit.from_chunk(chunk, rank=index, bm25_rank=index, bm25_score=score)
                for index, (score, chunk) in enumerate(scores[:top_k], 1) if score > 0 or query_terms == []]

    retrieve = search
