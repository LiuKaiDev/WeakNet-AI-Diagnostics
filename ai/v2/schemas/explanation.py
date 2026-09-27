"""Provider and application-level explanation contracts."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Mapping, Optional

from .diagnosis import DiagnosisValidationError, EvidenceRole, _string

EXPLANATION_SCHEMA_VERSION = "weaknet.ai.explanation.v1"


def _text(value: Any, field_name: str) -> str:
    if not isinstance(value, str) or not value:
        raise DiagnosisValidationError(f"{field_name} must be a non-empty string")
    return value


def _refs(value: Any, field_name: str) -> list[str]:
    if value is None:
        return []
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise DiagnosisValidationError(f"{field_name} must be a list of non-empty strings")
    return list(value)


@dataclass
class HypothesisExplanation:
    hypothesis_id: str
    explanation: str
    supporting_evidence_ids: list[str] = field(default_factory=list)
    contradicting_evidence_ids: list[str] = field(default_factory=list)
    missing_evidence_ids: list[str] = field(default_factory=list)

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "HypothesisExplanation":
        allowed = {
            "hypothesis_id", "explanation", "supporting_evidence_ids",
            "contradicting_evidence_ids", "missing_evidence_ids",
        }
        unknown = set(payload) - allowed
        if unknown:
            raise DiagnosisValidationError(
                f"provider hypothesis explanation has unsupported fields: {sorted(unknown)}"
            )
        return cls(
            hypothesis_id=_text(payload.get("hypothesis_id"), "hypothesis_id"),
            explanation=_text(payload.get("explanation"), "explanation"),
            supporting_evidence_ids=_refs(payload.get("supporting_evidence_ids"), "supporting_evidence_ids"),
            contradicting_evidence_ids=_refs(payload.get("contradicting_evidence_ids"), "contradicting_evidence_ids"),
            missing_evidence_ids=_refs(payload.get("missing_evidence_ids"), "missing_evidence_ids"),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "hypothesis_id": self.hypothesis_id,
            "explanation": self.explanation,
            "supporting_evidence_ids": list(self.supporting_evidence_ids),
            "contradicting_evidence_ids": list(self.contradicting_evidence_ids),
            "missing_evidence_ids": list(self.missing_evidence_ids),
        }


@dataclass
class EvidenceExplanation:
    evidence_id: str
    explanation: str

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "EvidenceExplanation":
        allowed = {"evidence_id", "explanation"}
        unknown = set(payload) - allowed
        if unknown:
            raise DiagnosisValidationError("provider evidence explanation has unsupported fields")
        return cls(_text(payload.get("evidence_id"), "evidence_id"), _text(payload.get("explanation"), "explanation"))

    def to_dict(self) -> dict[str, Any]:
        return {"evidence_id": self.evidence_id, "explanation": self.explanation}


@dataclass
class LimitationExplanation:
    missing_evidence_id: str
    explanation: str

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "LimitationExplanation":
        allowed = {"missing_evidence_id", "explanation"}
        unknown = set(payload) - allowed
        if unknown:
            raise DiagnosisValidationError("provider limitation has unsupported fields")
        return cls(
            _text(payload.get("missing_evidence_id"), "missing_evidence_id"),
            _text(payload.get("explanation"), "limitation.explanation"),
        )

    def to_dict(self) -> dict[str, Any]:
        return {"missing_evidence_id": self.missing_evidence_id, "explanation": self.explanation}


@dataclass
class ProviderExplanationPayload:
    schema_version: str
    summary: str
    hypothesis_explanations: list[HypothesisExplanation]
    evidence_explanations: list[EvidenceExplanation]
    limitations: list[LimitationExplanation]
    provider: str = ""
    simulated: bool = False

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any]) -> "ProviderExplanationPayload":
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("provider output must be an object")
        allowed = {
            "schema_version", "provider", "simulated", "summary",
            "hypothesis_explanations", "evidence_explanations", "limitations",
        }
        unknown = set(payload) - allowed
        if unknown:
            raise DiagnosisValidationError(
                f"provider output contains unsupported authoritative fields: {sorted(unknown)}"
            )
        if payload.get("schema_version") != EXPLANATION_SCHEMA_VERSION:
            raise DiagnosisValidationError("unsupported explanation schema_version")
        hypotheses = payload.get("hypothesis_explanations", [])
        evidence = payload.get("evidence_explanations", [])
        limitations = payload.get("limitations", [])
        if not isinstance(hypotheses, list) or not isinstance(evidence, list) or not isinstance(limitations, list):
            raise DiagnosisValidationError("provider explanation arrays must be lists")
        simulated = payload.get("simulated", False)
        if not isinstance(simulated, bool):
            raise DiagnosisValidationError("simulated must be boolean")
        return cls(
            schema_version=payload["schema_version"],
            summary=_text(payload.get("summary"), "summary"),
            hypothesis_explanations=[HypothesisExplanation.from_dict(item) for item in hypotheses],
            evidence_explanations=[EvidenceExplanation.from_dict(item) for item in evidence],
            limitations=[LimitationExplanation.from_dict(item) for item in limitations],
            provider=str(payload.get("provider", "")),
            simulated=simulated,
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "provider": self.provider,
            "simulated": self.simulated,
            "summary": self.summary,
            "hypothesis_explanations": [item.to_dict() for item in self.hypothesis_explanations],
            "evidence_explanations": [item.to_dict() for item in self.evidence_explanations],
            "limitations": [item.to_dict() for item in self.limitations],
        }


@dataclass
class ExplainedHypothesis:
    hypothesis_id: str
    type: str
    confidence: str
    state: str
    scope: Any
    explanation: str
    supporting_evidence: list[dict[str, Any]]
    contradicting_evidence: list[dict[str, Any]]
    missing_evidence: list[dict[str, Any]]

    def to_dict(self) -> dict[str, Any]:
        return {
            "hypothesis_id": self.hypothesis_id,
            "type": self.type,
            "confidence": self.confidence,
            "state": self.state,
            "scope": self.scope,
            "explanation": self.explanation,
            "supporting_evidence": self.supporting_evidence,
            "contradicting_evidence": self.contradicting_evidence,
            "missing_evidence": self.missing_evidence,
        }


@dataclass
class ExplanationReport:
    schema_version: str
    request_id: str
    diagnosis_snapshot_timestamp_ms: int
    deterministic_status: str
    provider: str
    model: str
    simulated: bool
    summary: str
    hypotheses: list[ExplainedHypothesis]
    limitations: list[dict[str, Any]]
    validation_status: str = "validated"

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "request_id": self.request_id,
            "diagnosis_snapshot_timestamp_ms": self.diagnosis_snapshot_timestamp_ms,
            "deterministic_status": self.deterministic_status,
            "provider": self.provider,
            "model": self.model,
            "simulated": self.simulated,
            "summary": self.summary,
            "hypotheses": [item.to_dict() for item in self.hypotheses],
            "limitations": self.limitations,
            "validation_status": self.validation_status,
        }

