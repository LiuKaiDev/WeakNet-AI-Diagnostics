"""Truthful optional dependency/index capability reporting."""
from __future__ import annotations
import importlib.util
import os
from .schemas import RagCapabilities


def capabilities(*, corpus_available: bool = False, index_available: bool = False, index_compatible: bool = False, embedding_loaded: bool = False, reranker_available: bool = False) -> RagCapabilities:
    configured = bool(os.environ.get("WEAKNET_RAG_EMBEDDING_MODEL", "").strip())
    reranker_configured = bool(os.environ.get("WEAKNET_RAG_RERANKER_MODEL", "").strip())
    faiss_available = importlib.util.find_spec("faiss") is not None
    return RagCapabilities(corpus_available=corpus_available, embedding_model_configured=configured,
        embedding_model_loaded=embedding_loaded, faiss_available=faiss_available,
        index_available=index_available, index_compatible=index_compatible,
        reranker_configured=reranker_configured, reranker_available=reranker_available,
        hybrid_ready=bool(corpus_available and index_available and index_compatible and embedding_loaded and reranker_available and faiss_available))
