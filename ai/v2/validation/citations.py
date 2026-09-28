"""Strict citation and conservative advice safety validation."""
from __future__ import annotations

import re
from typing import Iterable

from ..schemas.advice import ProviderRagAdvicePayload
from ..schemas.diagnosis import DiagnosisSnapshot, EvidenceRole
from ..rag.schemas import RetrievalBundle


class CitationGroundingViolation(ValueError):
    pass


_CITATION = re.compile(r"^[^/\s]+/[^\s]+@[^\s]+$")
_FORBIDDEN = re.compile(
    r"(?:\bconfirmed\s+(?:root cause|interference|isp|internet|upstream|outage)|"
    r"\b(?:isp|internet|provider)\s+(?:failure|outage|congestion)\s+confirmed|"
    r"\b(?:interference|rf interference|ap failure|isp failure|isp outage|internet outage)\s+(?:is|has been|was)\s+(?:confirmed|occurring|the root cause|responsible)|"
    r"\b(?:restart|change|disable|reconnect|modify|kill|execute|run)\b.{0,60}\b(?:networkmanager|route|interface|wifi|wi-fi|firewall|sysctl|process|command)\b|"
    r"\b(?:i|we)\s+(?:restarted|changed|disabled|killed|modified|executed|ran)\b)", re.I)


def contains_unsafe_advice(text: str) -> bool:
    return bool(_FORBIDDEN.search(text))


class CitationGroundingValidator:
    def validate(self, snapshot: DiagnosisSnapshot, bundle: RetrievalBundle,
                 payload: ProviderRagAdvicePayload) -> None:
        known = {hit.citation_id: hit for hit in bundle.hits}
        if len(known) != len(bundle.hits):
            raise CitationGroundingViolation("retrieval bundle contains duplicate citation identities")
        if not known and (payload.knowledge_explanations or payload.recommended_checks):
            raise CitationGroundingViolation("advice contains knowledge claims without retrieved chunks")
        missing = {evidence.evidence_id for hypothesis in snapshot.hypotheses
                   for evidence in hypothesis.missing_evidence}
        all_evidence = {evidence.evidence_id for evidence in snapshot.all_evidence()}
        for explanation in payload.knowledge_explanations:
            self._check_citations(explanation.text, explanation.citation_ids, known)
        for check in payload.recommended_checks:
            self._check_citations(check.text, check.citation_ids, known)
            if not set(check.related_missing_evidence_ids).issubset(missing):
                raise CitationGroundingViolation("recommended check references non-missing evidence")
        for limitation in payload.limitations:
            if not set(limitation.evidence_ids).issubset(all_evidence):
                raise CitationGroundingViolation("limitation references unknown evidence")
            if limitation.citation_ids:
                self._check_citations(limitation.text, limitation.citation_ids, known)

    @staticmethod
    def _check_citations(text: str, citations: Iterable[str], known: dict[str, object]) -> None:
        if contains_unsafe_advice(text):
            raise CitationGroundingViolation("advice contains an authority escalation or executed action")
        values = list(citations)
        if len(values) != len(set(values)):
            raise CitationGroundingViolation("duplicate citation identity within a claim")
        if not values:
            raise CitationGroundingViolation("knowledge-backed advice must cite retrieved knowledge")
        for citation in values:
            if not _CITATION.fullmatch(citation):
                raise CitationGroundingViolation("malformed citation identity")
            if citation not in known:
                raise CitationGroundingViolation("citation is not present in the current retrieval bundle")


def validate_citations(snapshot: DiagnosisSnapshot, bundle: RetrievalBundle,
                       payload: ProviderRagAdvicePayload) -> None:
    CitationGroundingValidator().validate(snapshot, bundle, payload)
