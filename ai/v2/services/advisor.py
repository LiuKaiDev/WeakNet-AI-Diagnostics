"""Grounded RAG advice orchestration, separate from evidence explanation."""
from __future__ import annotations

import asyncio
import copy
from dataclasses import dataclass, field

from ..errors import AiLayerError, InvalidDiagnosisInput
from ..providers.base import LlmProvider
from ..prompts.rag_advisor import RagAdvisorPromptBuilder
from ..rag.chunking import chunk_documents
from ..rag.knowledge import CorpusManifest, default_manifest_path, load_documents
from ..rag.query_planner import RagQueryPlanner
from ..rag.retriever import HybridRetriever
from ..rag.schemas import RetrievalBundle
from ..schemas.advice import RAG_ADVICE_SCHEMA_VERSION, ProviderRagAdvicePayload, RagAdviceReport
from ..schemas.diagnosis import DiagnosisSnapshot, DiagnosisValidationError, InputTooLarge
from ..schemas.explanation import ProviderExplanationPayload, HypothesisExplanation, LimitationExplanation
from ..validation.citations import CitationGroundingValidator, CitationGroundingViolation, contains_unsafe_advice
from ..validation.grounding import GroundingViolation, GroundingValidator


def default_lexical_retriever() -> HybridRetriever:
    manifest_path = default_manifest_path()
    manifest = CorpusManifest.load(manifest_path)
    documents = load_documents(manifest, root=manifest_path.parent)
    return HybridRetriever(chunk_documents(documents), corpus_version=manifest.corpus_version)


@dataclass
class RagAdvisorService:
    provider: LlmProvider
    retriever: HybridRetriever | None = None
    planner: RagQueryPlanner = field(default_factory=RagQueryPlanner)
    prompt_builder: RagAdvisorPromptBuilder = field(default_factory=RagAdvisorPromptBuilder)
    citation_validator: CitationGroundingValidator = field(default_factory=CitationGroundingValidator)

    async def advise(self, snapshot: DiagnosisSnapshot, bundles: list[RetrievalBundle] | None = None,
                     request_id: str = "") -> RagAdviceReport:
        try:
            normalized = snapshot.normalized_copy(); normalized.validate()
        except InputTooLarge:
            raise
        except (DiagnosisValidationError, TypeError, ValueError) as exc:
            raise InvalidDiagnosisInput(str(exc)) from exc
        queries = self.planner.plan(normalized)
        if not queries:
            return self._not_applicable(normalized, request_id)

        if bundles is not None:
            retrieval = bundles
        else:
            try:
                retriever = self.retriever or default_lexical_retriever()
            except Exception:
                return self._unavailable_retrieval(normalized, request_id)
            try:
                retrieval = [retriever.retrieve(query) for query in queries]
            except Exception:
                return self._retrieval_error(normalized, request_id)
        if not retrieval:
            return self._unavailable_retrieval(normalized, request_id)
        # One provider request can contain all hypothesis bundles; context is
        # still bounded per bundle and citations remain exact.
        bundle = self.prompt_builder.select_bundle(self._merge_bundles(retrieval))
        if not bundle.hits:
            return self._unavailable_retrieval(normalized, request_id)
        request = self.prompt_builder.request(normalized, bundle, request_id)
        try:
            result = await self.provider.generate(request)
        except asyncio.TimeoutError as exc:
            return self._failure_report(normalized, bundle, request_id, "ProviderTimeout", "RAG advisor provider timed out")
        except (ConnectionError, OSError) as exc:
            return self._failure_report(normalized, bundle, request_id, "ProviderUnavailable", "RAG advisor provider unavailable")
        except AiLayerError as exc:
            return self._failure_report(normalized, bundle, request_id, exc.category, "RAG advisor provider failed")
        except Exception as exc:
            return self._failure_report(normalized, bundle, request_id, "ProviderUnavailable", "RAG advisor provider failed")
        if result.error or result.finish_status not in ("completed", "success", "stop"):
            return self._failure_report(normalized, bundle, request_id, "ProviderUnavailable", "RAG advisor provider did not complete")
        try:
            payload = ProviderRagAdvicePayload.from_dict(result.structured_payload)
        except (DiagnosisValidationError, TypeError, ValueError) as exc:
            return self._failure_report(normalized, bundle, request_id, "InvalidProviderOutput", "RAG advice schema validation failed", schema_failure=1)
        try:
            # Keep the existing diagnosis grounding contract in the advice
            # pipeline as well; advice cannot create a parallel authority path.
            GroundingValidator().validate(normalized, self._diagnosis_payload(normalized))
            self.citation_validator.validate(normalized, bundle, payload)
            self._validate_missing_and_authority(normalized, payload)
        except (CitationGroundingViolation, GroundingViolation) as exc:
            if isinstance(exc, CitationGroundingViolation):
                category = "CitationGroundingViolation"
            elif "authority" in str(exc) or "cannot support" in str(exc):
                category = "AdviceAuthorityViolation"
            else:
                category = "GroundingViolation"
            return self._failure_report(normalized, bundle, request_id, category, "RAG advice grounding validation failed", grounding_failure=1,
                                        invalid_citation=int(isinstance(exc, CitationGroundingViolation)))
        cited = {hit.citation_id: hit.to_dict() for hit in bundle.hits}
        used = [citation for item in payload.knowledge_explanations + payload.recommended_checks for citation in item.citation_ids]
        return RagAdviceReport(schema_version=RAG_ADVICE_SCHEMA_VERSION, request_id=request_id,
            deterministic_status=normalized.status, hypotheses=[self._hypothesis(item) for item in normalized.hypotheses],
            retrieval_mode=bundle.retrieval_mode, corpus_version=bundle.corpus_version,
            provider=result.provider, model=result.model, summary=payload.summary,
            knowledge_explanations=payload.knowledge_explanations, recommended_checks=payload.recommended_checks,
            limitations=payload.limitations, cited_knowledge=[cited[item] for item in dict.fromkeys(used)],
            citation_coverage=1.0 if used else 0.0, status="validated")

    def advise_sync(self, snapshot: DiagnosisSnapshot, bundles: list[RetrievalBundle] | None = None,
                    request_id: str = "") -> RagAdviceReport:
        return asyncio.run(self.advise(snapshot, bundles, request_id))

    @staticmethod
    def _diagnosis_payload(snapshot: DiagnosisSnapshot) -> ProviderExplanationPayload:
        return ProviderExplanationPayload(
            schema_version="weaknet.ai.explanation.v1", summary="diagnosis grounding",
            hypothesis_explanations=[HypothesisExplanation(
                hypothesis_id=h.hypothesis_id, explanation="authoritative diagnosis",
                supporting_evidence_ids=[e.evidence_id for e in h.supporting_evidence],
                contradicting_evidence_ids=[e.evidence_id for e in h.contradicting_evidence],
                missing_evidence_ids=[e.evidence_id for e in h.missing_evidence]) for h in snapshot.hypotheses],
            evidence_explanations=[], limitations=[LimitationExplanation(e.evidence_id, "missing")
                for h in snapshot.hypotheses for e in h.missing_evidence])

    @staticmethod
    def _merge_bundles(bundles: list[RetrievalBundle]) -> RetrievalBundle:
        first = bundles[0]
        seen: set[str] = set(); hits = []
        for bundle in bundles:
            for hit in bundle.hits:
                if hit.chunk_id not in seen:
                    hits.append(copy.deepcopy(hit)); seen.add(hit.chunk_id)
        hits.sort(key=lambda hit: (
            0 if hit.rrf_score is not None else 1,
            -(hit.rrf_score if hit.rrf_score is not None else 0.0),
            hit.bm25_rank if hit.bm25_rank is not None else hit.rank,
            hit.chunk_id,
        ))
        return RetrievalBundle(query=first.query, corpus_version=first.corpus_version,
            retrieval_mode="hybrid" if all(item.retrieval_mode == "hybrid" for item in bundles) else "lexical",
            embedding_model=first.embedding_model if first.retrieval_mode == "hybrid" else None,
            reranker_model=first.reranker_model if first.retrieval_mode == "hybrid" else None,
            hits=hits, capabilities={"query_count": len(bundles)}, status="ok" if hits else "empty")

    @staticmethod
    def _hypothesis(hypothesis: object) -> dict[str, object]:
        return {"hypothesis_id": hypothesis.hypothesis_id, "type": hypothesis.type,
                "confidence": hypothesis.confidence, "state": hypothesis.state,
                "supporting_evidence_ids": [e.evidence_id for e in hypothesis.supporting_evidence],
                "contradicting_evidence_ids": [e.evidence_id for e in hypothesis.contradicting_evidence],
                "missing_evidence_ids": [e.evidence_id for e in hypothesis.missing_evidence]}

    @staticmethod
    def _validate_missing_and_authority(snapshot: DiagnosisSnapshot, payload: ProviderRagAdvicePayload) -> None:
        missing = {evidence.evidence_id for h in snapshot.hypotheses for evidence in h.missing_evidence}
        for check in payload.recommended_checks:
            if not set(check.related_missing_evidence_ids).issubset(missing):
                raise GroundingViolation("advice references a non-missing evidence ID")
        # A provider payload has no authoritative fields by schema. The phrase
        # guard is intentionally conservative for obvious escalation attempts.
        for text in [payload.summary, *(item.text for item in payload.knowledge_explanations),
                     *(item.text for item in payload.recommended_checks), *(item.text for item in payload.limitations)]:
            lowered = text.lower()
            if "root cause is" in lowered or "confidence is high" in lowered or contains_unsafe_advice(text):
                raise GroundingViolation("advice attempts to replace authoritative diagnosis")
            if any("rf" in item.kind.lower() or "interference" in item.kind.lower()
                   for h in snapshot.hypotheses for item in h.missing_evidence):
                if any(claim in lowered for claim in ("interference is occurring", "interference exists", "interference is confirmed")):
                    raise GroundingViolation("missing RF evidence cannot support an interference claim")
            if any("health" in item.kind.lower() and "unavailable" in item.kind.lower()
                   for h in snapshot.hypotheses for item in h.missing_evidence):
                if any(claim in lowered for claim in ("ap is unhealthy", "ap health is poor", "access point is failing")):
                    raise GroundingViolation("missing AP health evidence cannot support an AP failure claim")

    @classmethod
    def _failure_report(cls, snapshot: DiagnosisSnapshot, bundle: RetrievalBundle, request_id: str,
                        category: str, message: str, *, schema_failure: int = 0,
                        grounding_failure: int = 0, invalid_citation: int = 0) -> RagAdviceReport:
        return RagAdviceReport(schema_version=RAG_ADVICE_SCHEMA_VERSION, request_id=request_id,
            deterministic_status=snapshot.status, hypotheses=[cls._hypothesis(item) for item in snapshot.hypotheses],
            retrieval_mode=bundle.retrieval_mode, corpus_version=bundle.corpus_version,
            provider="unavailable", model="unavailable", summary="RAG advice is unavailable; retrieved knowledge remains inspectable.",
            knowledge_explanations=[], recommended_checks=[], limitations=[],
            cited_knowledge=[hit.to_dict() for hit in bundle.hits], citation_coverage=0.0,
            invalid_citation_count=invalid_citation, grounding_violation_count=grounding_failure,
            schema_failure_count=schema_failure, status="unavailable",
            error={"category": category, "message": message})

    @classmethod
    def _unavailable_retrieval(cls, snapshot: DiagnosisSnapshot, request_id: str) -> RagAdviceReport:
        return RagAdviceReport(schema_version=RAG_ADVICE_SCHEMA_VERSION, request_id=request_id,
            deterministic_status=snapshot.status, hypotheses=[cls._hypothesis(item) for item in snapshot.hypotheses],
            retrieval_mode="none", corpus_version="unavailable", provider="unavailable", model="unavailable",
            summary="RAG retrieval is unavailable; deterministic diagnosis remains available.",
            knowledge_explanations=[], recommended_checks=[], limitations=[], status="unavailable",
            error={"category": "RagUnavailable", "message": "knowledge retrieval is unavailable"})

    @classmethod
    def _retrieval_error(cls, snapshot: DiagnosisSnapshot, request_id: str) -> RagAdviceReport:
        return RagAdviceReport(schema_version=RAG_ADVICE_SCHEMA_VERSION, request_id=request_id,
            deterministic_status=snapshot.status, hypotheses=[cls._hypothesis(item) for item in snapshot.hypotheses],
            retrieval_mode="none", corpus_version="unavailable", provider="unavailable", model="unavailable",
            summary="RAG retrieval failed; deterministic diagnosis remains available.",
            knowledge_explanations=[], recommended_checks=[], limitations=[], status="unavailable",
            error={"category": "RetrievalError", "message": "knowledge retrieval failed"})

    @classmethod
    def _not_applicable(cls, snapshot: DiagnosisSnapshot, request_id: str) -> RagAdviceReport:
        return RagAdviceReport(schema_version=RAG_ADVICE_SCHEMA_VERSION, request_id=request_id,
            deterministic_status=snapshot.status, hypotheses=[], retrieval_mode="none",
            corpus_version="not-applicable", provider="not-used", model="not-used",
            summary="No active hypothesis is available for knowledge advice.",
            knowledge_explanations=[], recommended_checks=[], limitations=[], citation_coverage=0.0,
            status="not_applicable", error=None)
