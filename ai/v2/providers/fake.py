"""Deterministic, explicitly simulated provider for AI tests and demos."""

from __future__ import annotations

import asyncio
from copy import deepcopy
from typing import Any, Optional

from .base import LlmProviderResult, LlmRequest
from ..schemas.diagnosis import DiagnosisSnapshot
from ..schemas.explanation import EXPLANATION_SCHEMA_VERSION
from ..schemas.advice import RAG_ADVICE_SCHEMA_VERSION


class FakeLlmProvider:
    provider_name = "fake"
    model_name = "deterministic-fake-v2"

    def __init__(self, response: Optional[dict[str, Any]] = None, malformed: Any = None) -> None:
        self.response = deepcopy(response) if response is not None else None
        self.malformed = deepcopy(malformed)
        self.requests: list[LlmRequest] = []

    async def generate(self, request: LlmRequest) -> LlmProviderResult:
        self.requests.append(request)
        if self.malformed is not None:
            payload = deepcopy(self.malformed)
        elif self.response is not None:
            payload = deepcopy(self.response)
        else:
            snapshot = request.snapshot
            if request.response_schema_version == RAG_ADVICE_SCHEMA_VERSION and isinstance(snapshot, DiagnosisSnapshot):
                citations = []
                raw = request.metadata.get("retrieval_bundle", {}) if isinstance(request.metadata, dict) else {}
                if isinstance(raw, dict):
                    citations = [item.get("citation_id") for item in raw.get("hits", []) if isinstance(item, dict) and item.get("citation_id")]
                citation = citations[0] if citations else None
                knowledge = [{"text": "Retrieved knowledge is advisory and does not establish a new diagnosis.", "citation_ids": [citation]}] if citation else []
                payload = {"schema_version": RAG_ADVICE_SCHEMA_VERSION,
                           "summary": "The deterministic diagnosis remains authoritative; retrieved knowledge provides bounded next checks.",
                           "knowledge_explanations": knowledge,
                           "recommended_checks": ([{"text": "Collect the missing evidence before attributing a cause.", "citation_ids": [citation],
                                                    "related_missing_evidence_ids": [e.evidence_id for h in snapshot.hypotheses for e in h.missing_evidence]}] if citation else []),
                           "limitations": [{"text": "Missing evidence remains unresolved.", "evidence_ids": [e.evidence_id for h in snapshot.hypotheses for e in h.missing_evidence], "citation_ids": []}]}
            elif not isinstance(snapshot, DiagnosisSnapshot):
                # The service supplies a snapshot in metadata.  A missing one
                # is deliberately malformed rather than an invented report.
                payload = {"schema_version": EXPLANATION_SCHEMA_VERSION}
            else:
                hypotheses = []
                limitations = []
                for hypothesis in snapshot.hypotheses:
                    support = [item.evidence_id for item in hypothesis.supporting_evidence]
                    contradicting = [item.evidence_id for item in hypothesis.contradicting_evidence]
                    missing = [item.evidence_id for item in hypothesis.missing_evidence]
                    hypotheses.append({
                        "hypothesis_id": hypothesis.hypothesis_id,
                        "explanation": (
                            f"The deterministic diagnosis reports {hypothesis.type} at "
                            f"{hypothesis.confidence} confidence; this text explains "
                            "the supplied evidence without changing that conclusion."
                        ),
                        "supporting_evidence_ids": support,
                        "contradicting_evidence_ids": contradicting,
                        "missing_evidence_ids": missing,
                    })
                    for evidence_id in missing:
                        limitations.append({
                            "missing_evidence_id": evidence_id,
                            "explanation": "This evidence is unavailable, so the diagnosis remains bounded by that limitation.",
                        })
                payload = {
                    "schema_version": EXPLANATION_SCHEMA_VERSION,
                    "provider": self.provider_name,
                    "simulated": True,
                    "summary": (
                        f"Deterministic diagnosis status: {snapshot.status}. "
                        "This is a simulated evidence explanation."
                    ),
                    "hypothesis_explanations": hypotheses,
                    "evidence_explanations": [],
                    "limitations": limitations,
                }
        return LlmProviderResult(
            provider=self.provider_name,
            model=self.model_name,
            structured_payload=payload,
            latency_ms=0,
            finish_status="completed",
            request_id=request.request_id,
        )

    def generate_sync(self, request: LlmRequest) -> LlmProviderResult:
        return asyncio.run(self.generate(request))
