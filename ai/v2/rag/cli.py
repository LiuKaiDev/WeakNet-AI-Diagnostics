"""Small explicit developer CLI for local corpus/index work."""
from __future__ import annotations
import argparse
import json
from pathlib import Path

from .chunking import chunk_documents
from .embeddings import BgeEmbeddingModel
from .knowledge import CorpusManifest, default_manifest_path, load_documents
from .schemas import RetrievalQuery
from .evaluation import EvaluationCase, evaluate_tags


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="python -m ai.v2.rag.cli")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("build", help="build an explicit real dense index")
    query = sub.add_parser("query", help="inspect lexical retrieval")
    query.add_argument("text")
    sub.add_parser("evaluate", help="evaluate available local retrieval stages")
    args = parser.parse_args(argv)
    manifest = CorpusManifest.load(default_manifest_path())
    documents = load_documents(manifest, root=default_manifest_path().parent)
    chunks = chunk_documents(documents)
    if args.command == "build":
        model = BgeEmbeddingModel()
        from .faiss_index import VectorIndex
        index = VectorIndex(chunks, model.embed([item.text for item in chunks]), model_name=model.model_name, corpus_version=manifest.corpus_version)
        index.save(Path(".weaknet-rag-index")); print(json.dumps(index.metadata(), sort_keys=True)); return 0
    if args.command == "evaluate":
        dataset = json.loads((default_manifest_path().parent / "evaluation-v1.json").read_text(encoding="utf-8"))
        cases = [EvaluationCase(item["case_id"], item,
                                relevant_tags=tuple(item.get("tags", []))) for item in dataset["cases"]]
        from .bm25 import BM25Retriever
        retriever = BM25Retriever(chunks)
        outputs = {}
        for item in dataset["cases"]:
            terms = [item["hypothesis_type"]]
            for role in ("supporting", "contradicting", "missing"):
                terms.extend(item.get(role, []))
            outputs[item["case_id"]] = [(hit.chunk_id, next((chunk.tags for chunk in chunks if chunk.chunk_id == hit.chunk_id), ()))
                                         for hit in retriever.search(terms, 5)]
        report = {"dataset": dataset["schema_version"], "bm25": evaluate_tags(cases, outputs, k=5),
                  "dense": {"status": "SKIP", "reason": "no real model/index configured by default"},
                  "rrf": {"status": "SKIP", "reason": "dense stage unavailable"},
                  "reranked_hybrid": {"status": "SKIP", "reason": "dense/reranker unavailable"}}
        print(json.dumps(report, sort_keys=True, indent=2)); return 0
    from .bm25 import BM25Retriever
    print(json.dumps([hit.to_dict() for hit in BM25Retriever(chunks).search(args.text)], indent=2)); return 0


if __name__ == "__main__":
    raise SystemExit(main())
