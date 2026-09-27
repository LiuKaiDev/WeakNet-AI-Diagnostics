"""Evidence explanation orchestration with explicit AI-layer failures."""

from __future__ import annotations

import asyncio
import copy
from dataclasses import dataclass, field
from typing import Optional

from ..errors import (
    AiLayerError,
    GroundingValidationError,
    InvalidDiagnosisInput,
    InvalidProviderOutput,
    ProviderAuthenticationError,
    ProviderRateLimited,
    ProviderRequestError,
    ProviderServerError,
    ProviderTimeout,
    ProviderUnavailable,
)
from ..providers.base import LlmProvider
from ..prompts.evidence_explainer import EvidenceExplainerPromptBuilder
from ..schemas.diagnosis import DiagnosisSnapshot, DiagnosisValidationError, InputTooLarge
from ..schemas.explanation import (
    EXPLANATION_SCHEMA_VERSION,
    ExplainedHypothesis,
    ExplanationReport,
    ProviderExplanationPayload,
)
from ..validation.grounding import GroundingValidator, GroundingViolation


@dataclass
class EvidenceExplainerService:
    provider: LlmProvider
    prompt_builder: EvidenceExplainerPromptBuilder = field(default_factory=EvidenceExplainerPromptBuilder)
    grounding_validator: GroundingValidator = field(default_factory=GroundingValidator)

    async def explain(self, snapshot: DiagnosisSnapshot, request_id: str = "") -> ExplanationReport:
        try:
            normalized = snapshot.normalized_copy()
            normalized.validate()
        except InputTooLarge:
            raise
        except (DiagnosisValidationError, TypeError, ValueError) as exc:
            raise InvalidDiagnosisInput(str(exc)) from exc

        request = self.prompt_builder.request(normalized, request_id)
        try:
            result = await self.provider.generate(request)
        except asyncio.TimeoutError as exc:
            raise ProviderTimeout("LLM provider timed out") from exc
        except (ConnectionError, OSError) as exc:
            raise ProviderUnavailable(str(exc)) from exc
        except InputTooLarge:
            raise
        except AiLayerError:
            raise
        except Exception as exc:  # provider boundary: never leak arbitrary provider errors
            raise ProviderUnavailable(str(exc)) from exc

        if result.error or result.finish_status not in ("completed", "success", "stop"):
            raise ProviderUnavailable(result.error or "provider did not complete")
        try:
            payload = ProviderExplanationPayload.from_dict(result.structured_payload)
        except (DiagnosisValidationError, TypeError, ValueError) as exc:
            raise InvalidProviderOutput(str(exc)) from exc
        try:
            self.grounding_validator.validate(normalized, payload)
        except GroundingViolation as exc:
            raise GroundingValidationError(str(exc)) from exc
        return self._report(normalized, payload, result.provider, result.model, request_id,
                            result.latency_ms, result.provider_request_id, result.finish_status)

    def explain_sync(self, snapshot: DiagnosisSnapshot, request_id: str = "") -> ExplanationReport:
        return asyncio.run(self.explain(snapshot, request_id))

    async def explain_snapshot(self, snapshot: DiagnosisSnapshot, request_id: str = "") -> ExplanationReport:
        return await self.explain(snapshot, request_id)

    @staticmethod
    def _report(snapshot: DiagnosisSnapshot, payload: ProviderExplanationPayload,
                provider: str, model: str, request_id: str, latency_ms: int | None = None,
                provider_request_id: str | None = None, finish_status: str = "completed") -> ExplanationReport:
        provider_by_id = {item.hypothesis_id: item for item in payload.hypothesis_explanations}
        explained: list[ExplainedHypothesis] = []
        for hypothesis in snapshot.hypotheses:
            item = provider_by_id[hypothesis.hypothesis_id]
            by_id = {evidence.evidence_id: evidence.to_dict() for evidence in hypothesis.all_evidence()}
            explained.append(ExplainedHypothesis(
                hypothesis_id=hypothesis.hypothesis_id,
                type=hypothesis.type,
                confidence=hypothesis.confidence,
                state=hypothesis.state,
                scope=copy.deepcopy(hypothesis.scope),
                explanation=item.explanation,
                supporting_evidence=[by_id[evidence_id] for evidence_id in item.supporting_evidence_ids],
                contradicting_evidence=[by_id[evidence_id] for evidence_id in item.contradicting_evidence_ids],
                missing_evidence=[by_id[evidence_id] for evidence_id in item.missing_evidence_ids],
            ))
        limitations = [{"source": "diagnosis", "explanation": item} for item in snapshot.limitations]
        limitations.extend(item.to_dict() for item in payload.limitations)
        return ExplanationReport(
            schema_version=EXPLANATION_SCHEMA_VERSION,
            request_id=request_id,
            diagnosis_snapshot_timestamp_ms=snapshot.snapshot_timestamp_ms,
            deterministic_status=snapshot.status,
            provider=provider,
            model=model,
            # Provider metadata, not model-controlled payload fields, defines
            # whether the execution was simulated.
            simulated=provider == "fake",
            summary=payload.summary,
            hypotheses=explained,
            limitations=limitations,
            latency_ms=latency_ms,
            provider_request_id=provider_request_id,
            finish_status=finish_status,
        )
