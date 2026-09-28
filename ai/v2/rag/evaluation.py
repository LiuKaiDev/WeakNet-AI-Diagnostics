"""Small, stage-separated retrieval evaluation metrics."""
from __future__ import annotations
from dataclasses import dataclass
from typing import Iterable


def recall_at_k(results: Iterable[str], relevant: set[str], k: int) -> float:
    values = list(results)[:max(k, 0)]
    return 1.0 if relevant and any(item in relevant for item in values) else 0.0


def reciprocal_rank(results: Iterable[str], relevant: set[str]) -> float:
    for rank, item in enumerate(results, 1):
        if item in relevant:
            return 1.0 / rank
    return 0.0


@dataclass(frozen=True)
class EvaluationCase:
    case_id: str
    query: dict
    relevant_chunk_ids: tuple[str, ...] = ()
    relevant_tags: tuple[str, ...] = ()


def evaluate_stage(cases: Iterable[EvaluationCase], outputs: dict[str, list[str]], *, k: int = 5) -> dict[str, float]:
    cases = list(cases)
    if not cases:
        return {"recall_at_k": 0.0, "mrr": 0.0, "cases": 0}
    recalls, ranks = [], []
    for case in cases:
        relevant = set(case.relevant_chunk_ids)
        values = outputs.get(case.case_id, [])
        recalls.append(recall_at_k(values, relevant, k)); ranks.append(reciprocal_rank(values, relevant))
    return {"recall_at_k": sum(recalls) / len(recalls), "mrr": sum(ranks) / len(ranks), "cases": len(cases)}


def evaluate_tags(cases: Iterable[EvaluationCase], outputs: dict[str, list[tuple[str, Iterable[str]]]], *, k: int = 5) -> dict[str, float]:
    """Evaluate expected document tags without matching exact prose."""
    cases = list(cases)
    if not cases:
        return {"recall_at_k": 0.0, "mrr": 0.0, "cases": 0}
    recalls, ranks = [], []
    for case in cases:
        expected = set(case.relevant_tags)
        candidates = outputs.get(case.case_id, [])[:max(k, 0)]
        matching = [rank for rank, (_, tags) in enumerate(candidates, 1) if expected.intersection(tags)]
        recalls.append(1.0 if matching else 0.0)
        ranks.append(1.0 / matching[0] if matching else 0.0)
    return {"recall_at_k": sum(recalls) / len(cases), "mrr": sum(ranks) / len(cases), "cases": len(cases)}
