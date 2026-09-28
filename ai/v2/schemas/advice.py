"""Strict provider and final-report contracts for grounded RAG advice."""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Mapping

from .diagnosis import DiagnosisValidationError

RAG_ADVICE_SCHEMA_VERSION = "weaknet.ai.rag-advice.v1"


def _text(value: Any, name: str) -> str:
    if not isinstance(value, str) or not value.strip():
        raise DiagnosisValidationError(f"{name} must be a non-empty string")
    return value


def _list(value: Any, name: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or any(not isinstance(item, str) or not item.strip() for item in value):
        raise DiagnosisValidationError(f"{name} must be a list of non-empty strings")
    return list(value)


@dataclass
class AdviceKnowledgeExplanation:
    text: str
    citation_ids: list[str]

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "AdviceKnowledgeExplanation":
        allowed = {"text", "citation_ids"}
        unknown = set(payload) - allowed
        if unknown:
            raise DiagnosisValidationError(f"knowledge explanation has unsupported fields: {sorted(unknown)}")
        citations = _list(payload.get("citation_ids"), "knowledge_explanation.citation_ids")
        if not citations:
            raise DiagnosisValidationError("knowledge explanation requires citations")
        return cls(_text(payload.get("text"), "knowledge_explanation.text"), citations)

    def to_dict(self) -> dict[str, Any]:
        return {"text": self.text, "citation_ids": list(self.citation_ids)}


@dataclass
class AdviceRecommendedCheck:
    text: str
    citation_ids: list[str]
    related_missing_evidence_ids: list[str] = field(default_factory=list)

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "AdviceRecommendedCheck":
        allowed = {"text", "citation_ids", "related_missing_evidence_ids"}
        unknown = set(payload) - allowed
        if unknown:
            raise DiagnosisValidationError(f"recommended check has unsupported fields: {sorted(unknown)}")
        citations = _list(payload.get("citation_ids"), "recommended_check.citation_ids")
        if not citations:
            raise DiagnosisValidationError("recommended check requires citations")
        return cls(_text(payload.get("text"), "recommended_check.text"), citations,
                   _list(payload.get("related_missing_evidence_ids"), "related_missing_evidence_ids"))

    def to_dict(self) -> dict[str, Any]:
        return {"text": self.text, "citation_ids": list(self.citation_ids),
                "related_missing_evidence_ids": list(self.related_missing_evidence_ids)}


@dataclass
class AdviceLimitation:
    text: str
    evidence_ids: list[str] = field(default_factory=list)
    citation_ids: list[str] = field(default_factory=list)

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "AdviceLimitation":
        allowed = {"text", "evidence_ids", "citation_ids"}
        unknown = set(payload) - allowed
        if unknown:
            raise DiagnosisValidationError(f"advice limitation has unsupported fields: {sorted(unknown)}")
        return cls(_text(payload.get("text"), "limitation.text"),
                   _list(payload.get("evidence_ids"), "limitation.evidence_ids"),
                   _list(payload.get("citation_ids"), "limitation.citation_ids"))

    def to_dict(self) -> dict[str, Any]:
        return {"text": self.text, "evidence_ids": list(self.evidence_ids),
                "citation_ids": list(self.citation_ids)}


@dataclass
class ProviderRagAdvicePayload:
    schema_version: str
    summary: str
    knowledge_explanations: list[AdviceKnowledgeExplanation]
    recommended_checks: list[AdviceRecommendedCheck]
    limitations: list[AdviceLimitation]

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "ProviderRagAdvicePayload":
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("RAG advice provider output must be an object")
        allowed = {"schema_version", "summary", "knowledge_explanations", "recommended_checks", "limitations"}
        unknown = set(payload) - allowed
        if unknown:
            raise DiagnosisValidationError(f"RAG advice contains unsupported authoritative fields: {sorted(unknown)}")
        if payload.get("schema_version") != RAG_ADVICE_SCHEMA_VERSION:
            raise DiagnosisValidationError("unsupported RAG advice schema_version")
        def objects(name: str) -> list[Mapping[str, Any]]:
            value = payload.get(name, [])
            if not isinstance(value, list) or any(not isinstance(item, Mapping) for item in value):
                raise DiagnosisValidationError(f"{name} must be an object list")
            return value
        return cls(payload["schema_version"], _text(payload.get("summary"), "summary"),
                   [AdviceKnowledgeExplanation.from_dict(item) for item in objects("knowledge_explanations")],
                   [AdviceRecommendedCheck.from_dict(item) for item in objects("recommended_checks")],
                   [AdviceLimitation.from_dict(item) for item in objects("limitations")])

    def to_dict(self) -> dict[str, Any]:
        return {"schema_version": self.schema_version, "summary": self.summary,
                "knowledge_explanations": [item.to_dict() for item in self.knowledge_explanations],
                "recommended_checks": [item.to_dict() for item in self.recommended_checks],
                "limitations": [item.to_dict() for item in self.limitations]}


@dataclass
class RagAdviceReport:
    schema_version: str
    request_id: str
    deterministic_status: str
    hypotheses: list[dict[str, Any]]
    retrieval_mode: str
    corpus_version: str
    provider: str
    model: str
    summary: str
    knowledge_explanations: list[AdviceKnowledgeExplanation]
    recommended_checks: list[AdviceRecommendedCheck]
    limitations: list[AdviceLimitation]
    cited_knowledge: list[dict[str, Any]] = field(default_factory=list)
    citation_coverage: float = 1.0
    invalid_citation_count: int = 0
    grounding_violation_count: int = 0
    schema_failure_count: int = 0
    status: str = "validated"
    error: dict[str, str] | None = None

    def to_dict(self) -> dict[str, Any]:
        return {"schema_version": self.schema_version, "request_id": self.request_id,
                "deterministic_status": self.deterministic_status, "hypotheses": self.hypotheses,
                "retrieval_mode": self.retrieval_mode, "corpus_version": self.corpus_version,
                "provider": self.provider, "model": self.model, "summary": self.summary,
                "knowledge_explanations": [item.to_dict() for item in self.knowledge_explanations],
                "recommended_checks": [item.to_dict() for item in self.recommended_checks],
                "limitations": [item.to_dict() for item in self.limitations],
                "cited_knowledge": self.cited_knowledge, "citation_coverage": self.citation_coverage,
                "invalid_citation_count": self.invalid_citation_count,
                "grounding_violation_count": self.grounding_violation_count,
                "schema_failure_count": self.schema_failure_count, "status": self.status,
                "error": self.error}
