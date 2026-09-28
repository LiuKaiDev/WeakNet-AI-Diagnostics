"""Pure adapter for decoded D-Bus ``GetDiagnosis`` dictionaries."""

from __future__ import annotations

import json
from typing import Any, Mapping

from ..schemas.diagnosis import (
    DIAGNOSIS_SCHEMA_VERSION,
    DiagnosisLimits,
    DiagnosisSnapshot,
    DiagnosisValidationError,
    Evidence,
    EvidenceRole,
    Incident,
    RootCauseHypothesis,
)


def _required_mapping(payload: Mapping[str, Any], key: str) -> Mapping[str, Any]:
    value = payload.get(key)
    if not isinstance(value, Mapping):
        raise DiagnosisValidationError(f"D-Bus diagnosis field {key!r} must be an object")
    return value


def _scope_key(scope: Any) -> str:
    return json.dumps(scope, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


class DbusDiagnosisAdapter:
    """Convert decoded values; it never opens a bus or invents unavailable data."""

    def __init__(self, limits: DiagnosisLimits = DiagnosisLimits()) -> None:
        self.limits = limits

    def from_dict(self, payload: Mapping[str, Any]) -> DiagnosisSnapshot:
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("D-Bus diagnosis payload must be an object")
        raw_status = payload.get("status")
        status_payload = raw_status if isinstance(raw_status, Mapping) else payload
        status = status_payload.get("state")
        if not isinstance(status, str) or not status:
            raise DiagnosisValidationError("D-Bus status.state is required")
        timestamp = payload.get(
            "snapshot_timestamp_ms",
            payload.get("timestamp_ms", status_payload.get("timestamp_ms")),
        )

        raw_incidents = payload.get("incidents")
        raw_hypotheses = payload.get("hypotheses")
        if not isinstance(raw_incidents, list) or not isinstance(raw_hypotheses, list):
            raise DiagnosisValidationError("D-Bus incidents and hypotheses arrays are required")

        incidents = [self._incident(item) for item in raw_incidents]
        hypotheses = [self._hypothesis(item) for item in raw_hypotheses]
        snapshot = DiagnosisSnapshot(
            schema_version=DIAGNOSIS_SCHEMA_VERSION,
            snapshot_timestamp_ms=timestamp,
            status=status,
            limitations=list(payload.get("limitations", [])),
            topology=payload.get("topology") or (
                {"selected_uplink": payload["selected_uplink"]}
                if "selected_uplink" in payload else None
            ),
            incidents=incidents,
            hypotheses=hypotheses,
            limits=self.limits,
        )
        snapshot.ensure_evidence_ids()
        snapshot.validate()
        return snapshot

    adapt = from_dict

    def _incident(self, payload: Mapping[str, Any]) -> Incident:
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("D-Bus incident must be an object")
        evidence = []
        for item in payload.get("evidence", []):
            if not isinstance(item, Mapping):
                raise DiagnosisValidationError("D-Bus incident evidence must be an object")
            normalized_item = dict(item)
            if "kind" not in normalized_item and "source_kind" in normalized_item:
                normalized_item["kind"] = normalized_item["source_kind"]
            evidence.append(Evidence.from_dict(normalized_item, EvidenceRole.SUPPORTING, self.limits))
        return Incident.from_dict({
            "id": payload.get("id"),
            "type": payload.get("type"),
            "state": payload.get("state"),
            "severity": payload.get("severity"),
            "scope": payload.get("scope"),
            "opened_at_ms": payload.get("opened_at_ms"),
            "updated_at_ms": payload.get("last_updated_at_ms", payload.get("updated_at_ms")),
            "evidence": [item.to_dict() for item in evidence],
        }, self.limits)

    def _hypothesis(self, payload: Mapping[str, Any]) -> RootCauseHypothesis:
        if not isinstance(payload, Mapping):
            raise DiagnosisValidationError("D-Bus hypothesis must be an object")
        type_name = payload.get("type")
        occurrence = payload.get("occurrence")
        if not isinstance(type_name, str) or not type_name:
            raise DiagnosisValidationError("D-Bus hypothesis.type is required")
        if isinstance(occurrence, bool) or not isinstance(occurrence, int) or occurrence < 0:
            raise DiagnosisValidationError("D-Bus hypothesis.occurrence is required")
        hypothesis_id = payload.get("hypothesis_id", payload.get("id"))
        if hypothesis_id is None:
            hypothesis_id = f"hypothesis:{type_name}:{_scope_key(payload.get('scope'))}:{occurrence}"
        return RootCauseHypothesis.from_dict({
            "hypothesis_id": str(hypothesis_id),
            "type": type_name,
            "state": payload.get("state"),
            "confidence": payload.get("confidence"),
            "scope": payload.get("scope"),
            "opened_at_ms": payload.get("opened_at_ms"),
            "updated_at_ms": payload.get("last_updated_at_ms", payload.get("updated_at_ms")),
            "occurrence": occurrence,
            "supporting_evidence": payload.get("supporting_evidence", []),
            "contradicting_evidence": payload.get("contradicting_evidence", []),
            "missing_evidence": payload.get("missing_evidence", []),
        }, self.limits, require_timestamps=False)
