"""Versioned and provenance-preserving RAG data contracts."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Mapping
import hashlib
import json
import time

RAG_SCHEMA_VERSION = "weaknet.ai.rag.v1"
QUERY_SCHEMA_VERSION = "weaknet.ai.retrieval-query.v1"
CORPUS_SCHEMA_VERSION = "weaknet.ai.corpus.v1"


def _stable_id(*parts: object) -> str:
    value = "\x1f".join(str(part) for part in parts)
    return hashlib.sha256(value.encode("utf-8")).hexdigest()[:24]


@dataclass(frozen=True)
class RetrievalQuery:
    schema_version: str
    query_id: str
    hypothesis_id: str | None
    hypothesis_type: str
    confidence: str | None
    scope: Any = None
    supporting_evidence: tuple[str, ...] = ()
    contradicting_evidence: tuple[str, ...] = ()
    missing_evidence: tuple[str, ...] = ()
    incidents: tuple[str, ...] = ()
    capability_context: tuple[str, ...] = ()
    search_text: str = ""
    search_terms: tuple[str, ...] = ()
    generated_at: int | None = None

    def __post_init__(self) -> None:
        if self.schema_version != QUERY_SCHEMA_VERSION:
            raise ValueError("unsupported retrieval query schema_version")
        if not self.query_id or not self.hypothesis_type:
            raise ValueError("query identity and hypothesis_type are required")

    @property
    def hypothesis(self) -> dict[str, Any]:
        return {"hypothesis_id": self.hypothesis_id, "type": self.hypothesis_type,
                "confidence": self.confidence, "scope": self.scope}

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version, "query_id": self.query_id,
            "hypothesis_id": self.hypothesis_id, "hypothesis_type": self.hypothesis_type,
            "confidence": self.confidence, "scope": self.scope,
            "supporting_evidence": list(self.supporting_evidence),
            "contradicting_evidence": list(self.contradicting_evidence),
            "missing_evidence": list(self.missing_evidence), "incidents": list(self.incidents),
            "capability_context": list(self.capability_context), "search_text": self.search_text,
            "search_terms": list(self.search_terms), "generated_at": self.generated_at,
        }

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "RetrievalQuery":
        if not isinstance(payload, Mapping):
            raise ValueError("retrieval query must be an object")
        def strings(name: str) -> tuple[str, ...]:
            value = payload.get(name, [])
            if not isinstance(value, (list, tuple)) or not all(isinstance(item, str) for item in value):
                raise ValueError(f"{name} must be a string list")
            return tuple(value)
        return cls(schema_version=str(payload.get("schema_version", "")), query_id=str(payload.get("query_id", "")),
                   hypothesis_id=payload.get("hypothesis_id"), hypothesis_type=str(payload.get("hypothesis_type", "")),
                   confidence=payload.get("confidence"), scope=payload.get("scope"),
                   supporting_evidence=strings("supporting_evidence"), contradicting_evidence=strings("contradicting_evidence"),
                   missing_evidence=strings("missing_evidence"), incidents=strings("incidents"),
                   capability_context=strings("capability_context"), search_text=str(payload.get("search_text", "")),
                   search_terms=strings("search_terms"), generated_at=payload.get("generated_at"))


@dataclass(frozen=True)
class KnowledgeDocument:
    document_id: str
    title: str
    source: str
    source_type: str
    version: str
    content: str
    updated_at: str | None = None
    tags: tuple[str, ...] = ()
    applicable_root_causes: tuple[str, ...] = ()
    platform: str | None = None
    required_capabilities: tuple[str, ...] = ()

    def __post_init__(self) -> None:
        if not all((self.document_id, self.title, self.source, self.source_type, self.version)):
            raise ValueError("knowledge documents require identity and provenance")
        if not self.content.strip():
            raise ValueError("knowledge document content cannot be empty")

    def to_dict(self) -> dict[str, Any]:
        return {"document_id": self.document_id, "title": self.title, "source": self.source,
                "source_type": self.source_type, "version": self.version,
                "updated_at": self.updated_at, "tags": list(self.tags),
                "applicable_root_causes": list(self.applicable_root_causes),
                "platform": self.platform, "required_capabilities": list(self.required_capabilities),
                "content": self.content}


@dataclass(frozen=True)
class KnowledgeChunk:
    chunk_id: str
    document_id: str
    title: str
    section: str
    text: str
    source: str
    source_version: str
    tags: tuple[str, ...] = ()
    ordinal: int = 0

    @classmethod
    def create(cls, document: KnowledgeDocument, section: str, text: str, ordinal: int) -> "KnowledgeChunk":
        chunk_id = f"{document.document_id}:{document.version}:{ordinal}:{_stable_id(section, text)[:12]}"
        return cls(chunk_id, document.document_id, document.title, section, text,
                   document.source, document.version, document.tags, ordinal)

    def to_dict(self) -> dict[str, Any]:
        return {"chunk_id": self.chunk_id, "document_id": self.document_id, "title": self.title,
                "section": self.section, "text": self.text, "source": self.source,
                "source_version": self.source_version, "tags": list(self.tags), "ordinal": self.ordinal}


@dataclass
class RetrievalHit:
    rank: int
    chunk_id: str
    document_id: str
    title: str
    section: str
    text: str
    source: str
    source_version: str
    bm25_rank: int | None = None
    bm25_score: float | None = None
    dense_rank: int | None = None
    dense_score: float | None = None
    rrf_score: float | None = None
    reranker_score: float | None = None
    matched_query_id: str | None = None

    @classmethod
    def from_chunk(cls, chunk: KnowledgeChunk, **kwargs: Any) -> "RetrievalHit":
        return cls(rank=kwargs.pop("rank", 0), chunk_id=chunk.chunk_id, document_id=chunk.document_id,
                   title=chunk.title, section=chunk.section, text=chunk.text, source=chunk.source,
                   source_version=chunk.source_version, **kwargs)

    @property
    def citation_id(self) -> str:
        return f"{self.document_id}/{self.chunk_id}@{self.source_version}"

    def to_dict(self) -> dict[str, Any]:
        return {"rank": self.rank, "chunk_id": self.chunk_id, "document_id": self.document_id,
                "title": self.title, "section": self.section, "text": self.text, "source": self.source,
                "source_version": self.source_version, "bm25_rank": self.bm25_rank,
                "bm25_score": self.bm25_score, "dense_rank": self.dense_rank,
                "dense_score": self.dense_score, "rrf_score": self.rrf_score,
                "reranker_score": self.reranker_score, "matched_query_id": self.matched_query_id,
                "citation_id": self.citation_id}


@dataclass
class RetrievalBundle:
    query: RetrievalQuery
    corpus_version: str
    retrieval_mode: str
    embedding_model: str | None
    reranker_model: str | None
    hits: list[RetrievalHit] = field(default_factory=list)
    capabilities: dict[str, Any] = field(default_factory=dict)
    timing_ms: dict[str, float] = field(default_factory=dict)
    status: str = "ok"
    schema_version: str = RAG_SCHEMA_VERSION

    def __post_init__(self) -> None:
        if self.schema_version != RAG_SCHEMA_VERSION:
            raise ValueError("unsupported retrieval bundle schema_version")
        for index, hit in enumerate(self.hits, 1):
            hit.rank = index

    def to_dict(self) -> dict[str, Any]:
        return {"schema_version": self.schema_version, "query": self.query.to_dict(),
                "corpus_version": self.corpus_version, "retrieval_mode": self.retrieval_mode,
                "embedding_model": self.embedding_model, "reranker_model": self.reranker_model,
                "hits": [hit.to_dict() for hit in self.hits], "capabilities": self.capabilities,
                "timing_ms": self.timing_ms, "status": self.status}

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "RetrievalBundle":
        if not isinstance(payload, Mapping) or payload.get("schema_version") != RAG_SCHEMA_VERSION:
            raise ValueError("unsupported retrieval bundle schema_version")
        query = RetrievalQuery.from_dict(payload.get("query", {}))
        hits: list[RetrievalHit] = []
        for item in payload.get("hits", []):
            if not isinstance(item, Mapping):
                raise ValueError("retrieval hit must be an object")
            allowed = {field for field in RetrievalHit.__dataclass_fields__ if field != "rank"}
            values = {key: item[key] for key in allowed if key in item}
            for required in ("chunk_id", "document_id", "title", "section", "text", "source", "source_version"):
                if required not in values:
                    raise ValueError(f"retrieval hit missing {required}")
            hits.append(RetrievalHit(rank=int(item.get("rank", 0)), **values))
        return cls(query=query, corpus_version=str(payload.get("corpus_version", "")),
                   retrieval_mode=str(payload.get("retrieval_mode", "")), embedding_model=payload.get("embedding_model"),
                   reranker_model=payload.get("reranker_model"), hits=hits,
                   capabilities=dict(payload.get("capabilities", {})), timing_ms=dict(payload.get("timing_ms", {})),
                   status=str(payload.get("status", "ok")), schema_version=str(payload["schema_version"]))


@dataclass
class RagCapabilities:
    corpus_available: bool = False
    bm25_available: bool = True
    embedding_model_configured: bool = False
    embedding_model_loaded: bool = False
    faiss_available: bool = False
    index_available: bool = False
    index_compatible: bool = False
    reranker_configured: bool = False
    reranker_available: bool = False
    hybrid_ready: bool = False
    details: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        result = {key: value for key, value in self.__dict__.items() if key != "details"}
        result["details"] = dict(self.details)
        return result


def query_id_for(hypothesis_id: str | None, hypothesis_type: str, terms: list[str]) -> str:
    return "rq-" + _stable_id(hypothesis_id or "snapshot", hypothesis_type, *terms)
