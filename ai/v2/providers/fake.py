"""Deterministic, explicitly simulated provider for AI V2."""

from __future__ import annotations

import asyncio
from copy import deepcopy
from typing import Any, Optional

from .base import LlmProviderResult, LlmRequest
from ..schemas.diagnosis import DiagnosisSnapshot
from ..schemas.explanation import EXPLANATION_SCHEMA_VERSION


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
            if not isinstance(snapshot, DiagnosisSnapshot):
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
