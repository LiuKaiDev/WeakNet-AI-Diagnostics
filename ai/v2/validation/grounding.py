"""Reject provider output that escapes the deterministic diagnosis contract."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any

from ..schemas.diagnosis import DiagnosisSnapshot, EvidenceRole
from ..schemas.explanation import ProviderExplanationPayload


class GroundingViolation(ValueError):
    """Provider output references or invents facts outside the snapshot."""


@dataclass(frozen=True)
class _EvidenceLocation:
    hypothesis_id: str | None
    role: EvidenceRole


class GroundingValidator:
    def validate(self, snapshot: DiagnosisSnapshot, payload: ProviderExplanationPayload) -> None:
        hypothesis_by_id = {item.hypothesis_id: item for item in snapshot.hypotheses}
        locations: dict[str, _EvidenceLocation] = {}
        for incident in snapshot.incidents:
            for evidence in incident.evidence:
                if evidence.evidence_id is not None:
                    locations[evidence.evidence_id] = _EvidenceLocation(None, EvidenceRole.SUPPORTING)
        for hypothesis in snapshot.hypotheses:
            for role, evidence_items in (
                (EvidenceRole.SUPPORTING, hypothesis.supporting_evidence),
                (EvidenceRole.CONTRADICTING, hypothesis.contradicting_evidence),
                (EvidenceRole.MISSING, hypothesis.missing_evidence),
            ):
                for evidence in evidence_items:
                    if evidence.evidence_id is None:
                        raise GroundingViolation("snapshot contains evidence without a local evidence ID")
                    if evidence.evidence_id in locations:
                        raise GroundingViolation(f"duplicate evidence ID: {evidence.evidence_id}")
                    locations[evidence.evidence_id] = _EvidenceLocation(hypothesis.hypothesis_id, role)

        limitation_ids = [item.missing_evidence_id for item in payload.limitations]
        if len(limitation_ids) != len(set(limitation_ids)):
            raise GroundingViolation("duplicate limitation reference")
        for limitation_id in limitation_ids:
            location = locations.get(limitation_id)
            if location is None:
                raise GroundingViolation(f"limitation references unknown evidence: {limitation_id}")
            if location.role is not EvidenceRole.MISSING:
                raise GroundingViolation("limitations may reference missing evidence only")

        explained_ids: set[str] = set()
        for explanation in payload.hypothesis_explanations:
            if explanation.hypothesis_id in explained_ids:
                raise GroundingViolation("hypothesis is explained more than once")
            explained_ids.add(explanation.hypothesis_id)
            hypothesis = hypothesis_by_id.get(explanation.hypothesis_id)
            if hypothesis is None:
                raise GroundingViolation(f"unknown hypothesis ID: {explanation.hypothesis_id}")
            expected = {
                EvidenceRole.SUPPORTING: {item.evidence_id for item in hypothesis.supporting_evidence},
                EvidenceRole.CONTRADICTING: {item.evidence_id for item in hypothesis.contradicting_evidence},
                EvidenceRole.MISSING: {item.evidence_id for item in hypothesis.missing_evidence},
            }
            actual = {
                EvidenceRole.SUPPORTING: set(explanation.supporting_evidence_ids),
                EvidenceRole.CONTRADICTING: set(explanation.contradicting_evidence_ids),
                EvidenceRole.MISSING: set(explanation.missing_evidence_ids),
            }
            for role in EvidenceRole:
                unknown = actual[role] - set(locations)
                if unknown:
                    raise GroundingViolation(f"unknown evidence reference: {sorted(unknown)}")
                for evidence_id in actual[role]:
                    location = locations[evidence_id]
                    if location.hypothesis_id not in (None, hypothesis.hypothesis_id):
                        raise GroundingViolation("evidence belongs to a different hypothesis")
                    if location.hypothesis_id == hypothesis.hypothesis_id and location.role is not role:
                        raise GroundingViolation("evidence is referenced under the wrong role")
                if actual[role] != expected[role]:
                    raise GroundingViolation(
                        f"{role.value} evidence must be preserved exactly for {hypothesis.hypothesis_id}"
                    )
            for missing_id in expected[EvidenceRole.MISSING]:
                if missing_id not in limitation_ids:
                    raise GroundingViolation(
                        f"missing evidence limitation omitted: {missing_id}"
                    )

        for evidence_explanation in payload.evidence_explanations:
            if evidence_explanation.evidence_id not in locations:
                raise GroundingViolation(
                    f"unknown evidence explanation reference: {evidence_explanation.evidence_id}"
                )

        # An output with no hypothesis explanations cannot silently claim to
        # explain an active diagnosis. Empty snapshots are valid and produce a
        # summary without this requirement.
        if snapshot.hypotheses and not explained_ids:
            raise GroundingViolation("active hypotheses were omitted from provider output")


def validate_grounding(snapshot: DiagnosisSnapshot, payload: ProviderExplanationPayload) -> None:
    """Functional convenience wrapper for the stateless validator."""
    GroundingValidator().validate(snapshot, payload)
