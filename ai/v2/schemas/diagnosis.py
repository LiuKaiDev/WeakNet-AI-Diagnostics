"""Canonical, versioned diagnosis input for AI V2.

The module intentionally uses only the Python standard library.  The C++
diagnosis remains authoritative; these dataclasses are a bounded, loss-aware
presentation contract and never infer network facts.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from enum import Enum
import json
from typing import Any, Iterable, Mapping, Optional

DIAGNOSIS_SCHEMA_VERSION = "weaknet.ai.diagnosis.v1"


class DiagnosisValidationError(ValueError):
    """The canonical diagnosis contract is malformed."""

    category = "InvalidDiagnosisInput"


class InputTooLarge(DiagnosisValidationError):
    """The bounded AI input contract was exceeded."""

    category = "InputTooLarge"


class EvidenceRole(str, Enum):
    SUPPORTING = "Supporting"
    CONTRADICTING = "Contradicting"
    MISSING = "Missing"


@dataclass(frozen=True)
class DiagnosisLimits:
    max_incidents: int = 64
    max_hypotheses: int = 64
    max_evidence_per_item: int = 32
    max_total_evidence: int = 512
    max_string_length: int = 2048
    max_serialized_bytes: int = 262_144

    def validate(self) -> None:
        values = (
            self.max_incidents,
            self.max_hypotheses,
            self.max_evidence_per_item,
            self.max_total_evidence,
            self.max_string_length,
            self.max_serialized_bytes,
        )
        if any(value <= 0 for value in values):
            raise DiagnosisValidationError("diagnosis limits must be positive")


def _string(value: Any, field_name: str, limits: DiagnosisLimits) -> str:
    if not isinstance(value, str) or not value:
        raise DiagnosisValidationError(f"{field_name} must be a non-empty string")
    if len(value) > limits.max_string_length:
        raise InputTooLarge(f"{field_name} exceeds max_string_length")
    return value


def _optional_string(value: Any, field_name: str, limits: DiagnosisLimits) -> Optional[str]:
    if value is None:
        return None
    return _string(value, field_name, limits)


def _copy_json_value(value: Any, limits: DiagnosisLimits, field_name: str = "value") -> Any:
    """Copy JSON-like metadata while rejecting opaque/unbounded Python objects."""

    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, str):
        return _string(value, field_name, limits)
    if isinstance(value, Mapping):
        return {
            _string(str(key), f"{field_name}.key", limits): _copy_json_value(
                item, limits, f"{field_name}.{key}"
            )
            for key, item in sorted(value.items(), key=lambda pair: str(pair[0]))
        }
    if isinstance(value, (list, tuple)):
        if len(value) > limits.max_evidence_per_item * 4:
            raise InputTooLarge(f"{field_name} contains too many values")
        return [_copy_json_value(item, limits, field_name) for item in value]
    raise DiagnosisValidationError(f"{field_name} must be JSON-compatible")


def _scope(value: Any, limits: DiagnosisLimits) -> Any:
    """Preserve typed scope metadata without treating display names as identity."""

    return _copy_json_value(value, limits, "scope") if value is not None else None


def _timestamp(value: Any, field_name: str, required: bool = False) -> Optional[int]:
    if value is None:
        if required:
            raise DiagnosisValidationError(f"{field_name} is required")
        return None
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise DiagnosisValidationError(f"{field_name} must be a non-negative integer milliseconds value")
    return value


@dataclass
class Evidence:
    kind: str
    role: EvidenceRole
    evidence_id: Optional[str] = None
    source: Optional[str] = None
    provenance: Optional[str] = None
    scope: Any = None
    timestamp_ms: Optional[int] = None
    value: Any = None
    unit: Optional[str] = None
    capability: Optional[str] = None
    validity: Optional[str] = None

    @classmethod
    def from_dict(
        cls, payload: Mapping[str, Any], role: Optional[EvidenceRole] = None,
        limits: DiagnosisLimits = DiagnosisLimits(),
    ) -> "Evidence":
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("evidence must be an object")
        selected_role = role
        if "role" in payload:
            raw_role = payload.get("role")
            try:
                parsed_role = EvidenceRole(raw_role)
            except (TypeError, ValueError) as exc:
                raise DiagnosisValidationError("evidence role is invalid") from exc
            if selected_role is not None and parsed_role is not selected_role:
                raise DiagnosisValidationError("evidence role does not match its structural container")
            selected_role = parsed_role
        if selected_role is None:
            raise DiagnosisValidationError("evidence role is required")
        evidence_id = payload.get("evidence_id", payload.get("id"))
        if evidence_id is not None:
            evidence_id = _string(evidence_id, "evidence_id", limits)
        return cls(
            evidence_id=evidence_id,
            kind=_string(payload.get("kind"), "evidence.kind", limits),
            role=selected_role,
            source=_optional_string(payload.get("source"), "evidence.source", limits),
            provenance=_optional_string(payload.get("provenance"), "evidence.provenance", limits),
            scope=_scope(payload.get("scope"), limits),
            timestamp_ms=_timestamp(payload.get("timestamp_ms"), "evidence.timestamp_ms"),
            value=_copy_json_value(payload.get("value"), limits, "evidence.value"),
            unit=_optional_string(payload.get("unit"), "evidence.unit", limits),
            capability=_optional_string(payload.get("capability"), "evidence.capability", limits),
            validity=_optional_string(payload.get("validity"), "evidence.validity", limits),
        )

    def to_dict(self) -> dict[str, Any]:
        result: dict[str, Any] = {
            "evidence_id": self.evidence_id,
            "kind": self.kind,
            "role": self.role.value,
        }
        optional = {
            "source": self.source,
            "provenance": self.provenance,
            "scope": self.scope,
            "timestamp_ms": self.timestamp_ms,
            "value": self.value,
            "unit": self.unit,
            "capability": self.capability,
            "validity": self.validity,
        }
        result.update({key: value for key, value in optional.items() if value is not None})
        return result


def _evidence_list(
    values: Iterable[Evidence], limits: DiagnosisLimits, field_name: str
) -> list[Evidence]:
    result = list(values)
    if len(result) > limits.max_evidence_per_item:
        raise InputTooLarge(f"{field_name} exceeds max_evidence_per_item")
    for item in result:
        if not isinstance(item, Evidence):
            raise DiagnosisValidationError(f"{field_name} contains a non-evidence item")
    return result


@dataclass
class Incident:
    id: str
    type: str
    state: str
    severity: Optional[str]
    scope: Any
    opened_at_ms: int
    updated_at_ms: int
    evidence: list[Evidence] = field(default_factory=list)

    @classmethod
    def from_dict(cls, payload: Mapping[str, Any], limits: DiagnosisLimits = DiagnosisLimits()) -> "Incident":
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("incident must be an object")
        return cls(
            id=_string(str(payload.get("id")), "incident.id", limits),
            type=_string(payload.get("type"), "incident.type", limits),
            state=_string(payload.get("state"), "incident.state", limits),
            severity=_optional_string(payload.get("severity"), "incident.severity", limits),
            scope=_scope(payload.get("scope"), limits),
            opened_at_ms=_timestamp(payload.get("opened_at_ms"), "incident.opened_at_ms", True),  # type: ignore[arg-type]
            updated_at_ms=_timestamp(payload.get("last_updated_at_ms", payload.get("updated_at_ms")), "incident.updated_at_ms", True),  # type: ignore[arg-type]
            evidence=_evidence_list(
                (Evidence.from_dict(item, EvidenceRole.SUPPORTING, limits) for item in payload.get("evidence", [])),
                limits, "incident.evidence",
            ),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "id": self.id,
            "type": self.type,
            "state": self.state,
            "severity": self.severity,
            "scope": self.scope,
            "opened_at_ms": self.opened_at_ms,
            "updated_at_ms": self.updated_at_ms,
            "evidence": [item.to_dict() for item in self.evidence],
        }


@dataclass
class RootCauseHypothesis:
    hypothesis_id: str
    type: str
    state: str
    confidence: str
    scope: Any
    opened_at_ms: Optional[int]
    updated_at_ms: Optional[int]
    supporting_evidence: list[Evidence] = field(default_factory=list)
    contradicting_evidence: list[Evidence] = field(default_factory=list)
    missing_evidence: list[Evidence] = field(default_factory=list)
    occurrence: Optional[int] = None

    @classmethod
    def from_dict(
        cls, payload: Mapping[str, Any], limits: DiagnosisLimits = DiagnosisLimits(),
        require_timestamps: bool = True,
    ) -> "RootCauseHypothesis":
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("hypothesis must be an object")
        hypothesis_id = payload.get("hypothesis_id", payload.get("id"))
        return cls(
            hypothesis_id=_string(str(hypothesis_id), "hypothesis_id", limits),
            type=_string(payload.get("type"), "hypothesis.type", limits),
            state=_string(payload.get("state"), "hypothesis.state", limits),
            confidence=_string(str(payload.get("confidence")), "hypothesis.confidence", limits),
            scope=_scope(payload.get("scope"), limits),
            opened_at_ms=_timestamp(payload.get("opened_at_ms"), "hypothesis.opened_at_ms", require_timestamps),
            updated_at_ms=_timestamp(payload.get("last_updated_at_ms", payload.get("updated_at_ms")), "hypothesis.updated_at_ms", require_timestamps),
            supporting_evidence=_evidence_list(
                (Evidence.from_dict(item, EvidenceRole.SUPPORTING, limits) for item in payload.get("supporting_evidence", [])),
                limits, "hypothesis.supporting_evidence",
            ),
            contradicting_evidence=_evidence_list(
                (Evidence.from_dict(item, EvidenceRole.CONTRADICTING, limits) for item in payload.get("contradicting_evidence", [])),
                limits, "hypothesis.contradicting_evidence",
            ),
            missing_evidence=_evidence_list(
                (Evidence.from_dict(item, EvidenceRole.MISSING, limits) for item in payload.get("missing_evidence", [])),
                limits, "hypothesis.missing_evidence",
            ),
            occurrence=payload.get("occurrence"),
        )

    def all_evidence(self) -> list[Evidence]:
        return self.supporting_evidence + self.contradicting_evidence + self.missing_evidence

    def to_dict(self) -> dict[str, Any]:
        return {
            "hypothesis_id": self.hypothesis_id,
            "type": self.type,
            "state": self.state,
            "confidence": self.confidence,
            "scope": self.scope,
            "opened_at_ms": self.opened_at_ms,
            "updated_at_ms": self.updated_at_ms,
            "occurrence": self.occurrence,
            "supporting_evidence": [item.to_dict() for item in self.supporting_evidence],
            "contradicting_evidence": [item.to_dict() for item in self.contradicting_evidence],
            "missing_evidence": [item.to_dict() for item in self.missing_evidence],
        }


@dataclass
class DiagnosisSnapshot:
    schema_version: str
    snapshot_timestamp_ms: Optional[int]
    status: str
    limitations: list[str]
    topology: Optional[dict[str, Any]]
    incidents: list[Incident]
    hypotheses: list[RootCauseHypothesis]
    limits: DiagnosisLimits = field(default_factory=DiagnosisLimits, repr=False, compare=False)

    def __post_init__(self) -> None:
        self.validate()

    @classmethod
    def from_dict(
        cls, payload: Mapping[str, Any], limits: DiagnosisLimits = DiagnosisLimits(),
        require_timestamp: bool = True, require_hypothesis_timestamps: bool = True,
    ) -> "DiagnosisSnapshot":
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("diagnosis snapshot must be an object")
        if payload.get("schema_version") != DIAGNOSIS_SCHEMA_VERSION:
            raise DiagnosisValidationError("unsupported diagnosis schema_version")
        incidents = [Incident.from_dict(item, limits) for item in payload.get("incidents", [])]
        hypotheses = [RootCauseHypothesis.from_dict(item, limits, require_hypothesis_timestamps)
                      for item in payload.get("hypotheses", [])]
        snapshot = cls(
            schema_version=payload["schema_version"],
            snapshot_timestamp_ms=_timestamp(payload.get("snapshot_timestamp_ms"), "snapshot_timestamp_ms", require_timestamp),  # type: ignore[arg-type]
            status=_string(payload.get("status"), "status", limits),
            limitations=[_string(item, "limitation", limits) for item in payload.get("limitations", [])],
            topology=_copy_json_value(payload.get("topology"), limits, "topology"),
            incidents=incidents,
            hypotheses=hypotheses,
            limits=limits,
        )
        snapshot.ensure_evidence_ids()
        snapshot.validate()
        return snapshot

    def ensure_evidence_ids(self) -> None:
        for incident in self.incidents:
            for index, evidence in enumerate(incident.evidence):
                if evidence.evidence_id is None:
                    evidence.evidence_id = f"incident:{incident.id}:supporting:{index}"
        for hypothesis in self.hypotheses:
            for role, evidence_items in (
                (EvidenceRole.SUPPORTING, hypothesis.supporting_evidence),
                (EvidenceRole.CONTRADICTING, hypothesis.contradicting_evidence),
                (EvidenceRole.MISSING, hypothesis.missing_evidence),
            ):
                for index, evidence in enumerate(evidence_items):
                    evidence.role = role
                    if evidence.evidence_id is None:
                        evidence.evidence_id = f"hypothesis:{hypothesis.hypothesis_id}:{role.value.lower()}:{index}"

    def all_evidence(self) -> list[Evidence]:
        result: list[Evidence] = []
        for incident in self.incidents:
            result.extend(incident.evidence)
        for hypothesis in self.hypotheses:
            result.extend(hypothesis.all_evidence())
        return result

    def normalized_copy(self) -> "DiagnosisSnapshot":
        return DiagnosisSnapshot.from_dict(
            self.to_dict(), self.limits, require_timestamp=False,
            require_hypothesis_timestamps=False,
        )

    def validate(self) -> None:
        self.limits.validate()
        if self.schema_version != DIAGNOSIS_SCHEMA_VERSION:
            raise DiagnosisValidationError("unsupported diagnosis schema_version")
        _timestamp(self.snapshot_timestamp_ms, "snapshot_timestamp_ms")
        _string(self.status, "status", self.limits)
        if len(self.incidents) > self.limits.max_incidents:
            raise InputTooLarge("too many incidents")
        if len(self.hypotheses) > self.limits.max_hypotheses:
            raise InputTooLarge("too many hypotheses")
        if len(self.limitations) > self.limits.max_evidence_per_item * 4:
            raise InputTooLarge("too many limitations")
        for limitation in self.limitations:
            _string(limitation, "limitation", self.limits)
        total = len(self.all_evidence())
        if total > self.limits.max_total_evidence:
            raise InputTooLarge("too much total evidence")
        serialized = json.dumps(self.to_dict(), sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode()
        if len(serialized) > self.limits.max_serialized_bytes:
            raise InputTooLarge("serialized diagnosis exceeds max_serialized_bytes")

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "snapshot_timestamp_ms": self.snapshot_timestamp_ms,
            "status": self.status,
            "limitations": list(self.limitations),
            "topology": self.topology,
            "incidents": [item.to_dict() for item in self.incidents],
            "hypotheses": [item.to_dict() for item in self.hypotheses],
        }
