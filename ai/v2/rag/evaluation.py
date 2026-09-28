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


def evaluate_advice_reports(reports: Iterable[object]) -> dict[str, float | int]:
    """Aggregate advisor validation metrics separately from retrieval quality."""
    reports = list(reports)
    total = len(reports)
    if not total:
        return {"cases": 0, "schema_success": 0, "diagnosis_grounding_success": 0,
                "citation_grounding_success": 0, "citation_coverage": 0.0,
                "invalid_citation_count": 0, "grounding_violation_count": 0,
                "schema_failure_count": 0, "forbidden_overclaim_failures": 0}
    validated = [report for report in reports if getattr(report, "status", "") == "validated"]
    return {"cases": total, "schema_success": len(validated),
            "diagnosis_grounding_success": len(validated), "citation_grounding_success": len(validated),
            "citation_coverage": sum(float(getattr(report, "citation_coverage", 0.0)) for report in reports) / total,
            "invalid_citation_count": sum(int(getattr(report, "invalid_citation_count", 0)) for report in reports),
            "grounding_violation_count": sum(int(getattr(report, "grounding_violation_count", 0)) for report in reports),
            "schema_failure_count": sum(int(getattr(report, "schema_failure_count", 0)) for report in reports),
            "forbidden_overclaim_failures": sum(1 for report in reports
                if getattr(report, "error", None) and report.error.get("category") == "AdviceAuthorityViolation")}
