"""Optional, advisory hybrid retrieval for structured diagnosis.

The package deliberately has no eager imports of torch, transformers, numpy, or
faiss.  Importing :mod:`ai.v2` therefore remains safe on the minimal runtime.
"""

from .schemas import (
    RAG_SCHEMA_VERSION,
    RetrievalQuery,
    KnowledgeDocument,
    KnowledgeChunk,
    RetrievalHit,
    RetrievalBundle,
    RagCapabilities,
)
from .query_planner import RagQueryPlanner
from .bm25 import BM25Retriever, tokenize
from .fusion import reciprocal_rank_fusion
from .retriever import HybridRetriever, RetrievalConfig

__all__ = [
    "RAG_SCHEMA_VERSION", "RetrievalQuery", "KnowledgeDocument", "KnowledgeChunk",
    "RetrievalHit", "RetrievalBundle", "RagCapabilities", "RagQueryPlanner",
    "BM25Retriever", "tokenize", "reciprocal_rank_fusion", "HybridRetriever",
    "RetrievalConfig",
]
