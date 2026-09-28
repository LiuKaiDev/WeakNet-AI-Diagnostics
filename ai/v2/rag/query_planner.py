"""Pure projection from deterministic diagnosis to semantic retrieval queries."""
from __future__ import annotations

import re
from typing import Iterable

from ..schemas.diagnosis import DiagnosisSnapshot, RootCauseHypothesis, Evidence
from .schemas import RetrievalQuery, QUERY_SCHEMA_VERSION, query_id_for


def _words(value: str) -> list[str]:
    # Keep the canonical token as well as readable words.  This helps a small
    # corpus match both ``HighTcpRtt`` and prose such as ``TCP RTT``.
    chunks = re.sub(r"([a-z0-9])([A-Z])", r"\1 \2", value).replace("_", " ").replace("-", " ").split()
    return [value.lower()] + [item.lower() for item in chunks if item.lower() != value.lower()]


class RagQueryPlanner:
    """Deterministic, privacy-aware query planner; it never reads raw logs."""

    def __init__(self, *, include_confidence: bool = False) -> None:
        self.include_confidence = include_confidence

    @staticmethod
    def _evidence_kinds(items: Iterable[Evidence]) -> tuple[str, ...]:
        return tuple(sorted({item.kind for item in items}))

    def _plan_hypothesis(self, hypothesis: RootCauseHypothesis, incidents: tuple[str, ...], generated_at: int | None) -> RetrievalQuery:
        supporting = self._evidence_kinds(hypothesis.supporting_evidence)
        contradicting = self._evidence_kinds(hypothesis.contradicting_evidence)
        missing = self._evidence_kinds(hypothesis.missing_evidence)
        terms: list[str] = []
        terms.extend(_words(hypothesis.type))
        for kind in supporting:
            terms.extend(_words(kind))
        for kind in incidents:
            terms.extend(_words(kind))
        for kind in contradicting:
            terms.extend(["contradicting", *_words(kind)])
        for kind in missing:
            # Missing evidence asks for procedures/observations, not a claim
            # that the absent condition exists.
            terms.extend(["missing", "check", "evidence", *_words(kind)])
        # Stable de-duplication avoids accidental term weighting from repeated
        # evidence while preserving semantic priority ordering.
        terms = list(dict.fromkeys(item for item in terms if item))
        search_text = " ".join(terms)
        capabilities = tuple(sorted({item.capability for item in hypothesis.missing_evidence if item.capability}))
        query_id = query_id_for(hypothesis.hypothesis_id, hypothesis.type, terms)
        return RetrievalQuery(
            schema_version=QUERY_SCHEMA_VERSION, query_id=query_id,
            hypothesis_id=hypothesis.hypothesis_id, hypothesis_type=hypothesis.type,
            confidence=hypothesis.confidence if self.include_confidence else None,
            # Scope is intentionally excluded: it frequently contains SSID,
            # addresses, interface names, and socket identity.
            scope=None, supporting_evidence=supporting, contradicting_evidence=contradicting,
            missing_evidence=missing, incidents=incidents, capability_context=capabilities,
            search_text=search_text, search_terms=tuple(terms), generated_at=generated_at,
        )

    def plan(self, snapshot: DiagnosisSnapshot) -> list[RetrievalQuery]:
        incident_types = tuple(sorted({item.type for item in snapshot.incidents}))
        return [self._plan_hypothesis(item, incident_types, snapshot.snapshot_timestamp_ms)
                for item in sorted(snapshot.hypotheses, key=lambda h: h.hypothesis_id)]

    def build(self, snapshot: DiagnosisSnapshot) -> list[RetrievalQuery]:
        return self.plan(snapshot)
