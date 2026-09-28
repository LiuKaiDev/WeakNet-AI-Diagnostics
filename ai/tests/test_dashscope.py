import asyncio
import json
import os
import unittest

from ai.tests.test_v2 import snapshot_with_hypothesis
from ai.v2.errors import (
    InvalidProviderOutput,
    ProviderAuthenticationError,
    ProviderRateLimited,
    ProviderRequestError,
    ProviderServerError,
    ProviderTimeout,
    ProviderUnavailable,
)
from ai.v2.schemas.diagnosis import InputTooLarge
from ai.v2.providers.base import LlmRequest
from ai.v2.providers.dashscope import (
    DashScopeConfig,
    DashScopeProvider,
    DashScopeResponse,
)
from ai.v2.providers.factory import create_llm_provider, provider_capabilities
from ai.v2.providers.fake import FakeLlmProvider
from ai.v2.prompts.evidence_explainer import EvidenceExplainerPromptBuilder
from ai.v2.services.explainer import EvidenceExplainerService


class SequenceTransport:
    def __init__(self, *responses):
        self.responses = list(responses)
        self.calls = []

    def post_json(self, endpoint, headers, payload, timeout_seconds, max_response_bytes):
        self.calls.append((endpoint, dict(headers), payload, timeout_seconds, max_response_bytes))
        response = self.responses.pop(0)
        if isinstance(response, BaseException):
            raise response
        return response


def response_for(snapshot, finish_reason="stop"):
    request = EvidenceExplainerPromptBuilder().request(snapshot, "request-7")
    fake = FakeLlmProvider()
    payload = asyncio.run(fake.generate(request)).structured_payload
    return DashScopeResponse(
        status_code=200,
        body=json.dumps({
            "id": "provider-request-1",
            "choices": [{"message": {"content": json.dumps(payload)}, "finish_reason": finish_reason}],
        }).encode(),
        provider_request_id="provider-request-1",
    )


class DashScopeProviderTests(unittest.TestCase):
    def config(self, **kwargs):
        values = {"api_key": "k", "max_attempts": 1, "retry_backoff_seconds": 0}
        values.update(kwargs)
        return DashScopeConfig(**values)

    def test_missing_key_is_explicit_and_no_import_requirement(self):
        with self.assertRaises(ProviderUnavailable):
            DashScopeProvider(DashScopeConfig.from_env({}))

    def test_secret_is_not_in_repr_or_public_metadata(self):
        provider = DashScopeProvider(self.config())
        self.assertNotIn("k", repr(provider.config.public_dict()))
        self.assertNotIn("k", repr(provider))

    def test_request_uses_prompt_model_temperature_and_request_id(self):
        snapshot = snapshot_with_hypothesis()
        transport = SequenceTransport(response_for(snapshot))
        provider = DashScopeProvider(self.config(model="qwen-test", temperature=0.2), transport=transport)
        request = EvidenceExplainerPromptBuilder().request(snapshot, "request-7")
        result = asyncio.run(provider.generate(request))
        endpoint, headers, payload, timeout, _ = transport.calls[0]
        self.assertEqual(payload["model"], "qwen-test")
        self.assertEqual(payload["temperature"], 0.2)
        self.assertFalse(payload["enable_thinking"])
        self.assertEqual(payload["messages"][1]["content"], request.user_prompt)
        self.assertEqual(result.request_id, "request-7")
        self.assertEqual(result.provider_request_id, "provider-request-1")
        self.assertLessEqual(timeout, self.config().read_timeout_seconds)
        self.assertNotIn("k", result.__dict__)

    def test_malformed_json_and_oversized_output_are_rejected(self):
        snapshot = snapshot_with_hypothesis()
        malformed = DashScopeResponse(200, b"not-json")
        with self.assertRaises(InvalidProviderOutput):
            asyncio.run(DashScopeProvider(self.config(), SequenceTransport(malformed)).generate(
                EvidenceExplainerPromptBuilder().request(snapshot)))

    def test_prompt_size_limit_rejects_without_truncation(self):
        snapshot = snapshot_with_hypothesis()
        with self.assertRaises(InputTooLarge):
            asyncio.run(DashScopeProvider(self.config(max_prompt_bytes=1), SequenceTransport()).generate(
                EvidenceExplainerPromptBuilder().request(snapshot)))
        oversized = DashScopeResponse(200, b"x" * 33)
        with self.assertRaises(InvalidProviderOutput):
            asyncio.run(DashScopeProvider(self.config(max_response_bytes=32), SequenceTransport(oversized)).generate(
                EvidenceExplainerPromptBuilder().request(snapshot)))

    def test_status_mapping_and_retry_policy(self):
        snapshot = snapshot_with_hypothesis()
        request = EvidenceExplainerPromptBuilder().request(snapshot)
        sleeper_calls = []
        transient = SequenceTransport(DashScopeResponse(500, b"{}"), response_for(snapshot))
        provider = DashScopeProvider(self.config(max_attempts=2), transient, sleeper_calls.append)
        result = asyncio.run(provider.generate(request))
        self.assertEqual(result.provider, "dashscope")
        self.assertEqual(len(transient.calls), 2)
        self.assertEqual(len(sleeper_calls), 1)
        for status, error_type in ((401, ProviderAuthenticationError), (429, ProviderRateLimited),
                                   (400, ProviderRequestError), (500, ProviderServerError)):
            with self.subTest(status=status):
                transport = SequenceTransport(DashScopeResponse(status, b"{}"))
                with self.assertRaises(error_type):
                    asyncio.run(DashScopeProvider(self.config(), transport).generate(request))
                self.assertEqual(len(transport.calls), 1)

    def test_timeout_and_connection_are_explicit(self):
        snapshot = snapshot_with_hypothesis()
        request = EvidenceExplainerPromptBuilder().request(snapshot)
        with self.assertRaises(ProviderTimeout):
            asyncio.run(DashScopeProvider(self.config(), SequenceTransport(TimeoutError())).generate(request))
        with self.assertRaises(ProviderUnavailable):
            asyncio.run(DashScopeProvider(self.config(), SequenceTransport(ConnectionError())).generate(request))

    def test_service_uses_same_grounding_contract_and_no_fake_fallback(self):
        snapshot = snapshot_with_hypothesis()
        provider = DashScopeProvider(self.config(), SequenceTransport(response_for(snapshot)))
        report = EvidenceExplainerService(provider).explain_sync(snapshot, "request-7")
        self.assertEqual(report.provider, "dashscope")
        self.assertFalse(report.simulated)
        self.assertEqual(report.hypotheses[0].type, snapshot.hypotheses[0].type)
        self.assertEqual(report.hypotheses[0].confidence, snapshot.hypotheses[0].confidence)
        with self.assertRaises(ProviderUnavailable):
            create_llm_provider("dashscope", environ={})
        self.assertEqual(create_llm_provider("fake", environ={}).provider_name, "fake")

    def test_qwen_alternate_envelope_is_strictly_normalized(self):
        snapshot = snapshot_with_hypothesis()
        hypothesis = snapshot.hypotheses[0]
        structured = {
            "response_schema_version": "weaknet.ai.explanation.v1",
            "summary": "grounded",
            "root_cause_hypotheses": [{
                "hypothesis_id": hypothesis.hypothesis_id,
                "type": hypothesis.type,
                "confidence": hypothesis.confidence,
                "state": hypothesis.state,
                "explanation": "grounded explanation",
            }],
            "supporting_evidence": [
                {"evidence_id": item.evidence_id, "kind": item.kind}
                for item in hypothesis.supporting_evidence
            ],
            "contradicting_evidence": [
                {"evidence_id": item.evidence_id, "kind": item.kind}
                for item in hypothesis.contradicting_evidence
            ],
            "missing_evidence": [
                {"evidence_id": item.evidence_id, "kind": item.kind}
                for item in hypothesis.missing_evidence
            ],
            "limitations": [
                {"evidence_id": item.evidence_id, "description": "unavailable"}
                for item in hypothesis.missing_evidence
            ],
        }
        response = DashScopeResponse(200, json.dumps({
            "choices": [{"message": {"content": json.dumps(structured)}, "finish_reason": "stop"}]
        }).encode())
        provider = DashScopeProvider(self.config(), SequenceTransport(response))
        report = EvidenceExplainerService(provider).explain_sync(snapshot)
        self.assertEqual(report.validation_status, "validated")
        self.assertEqual(report.hypotheses[0].type, hypothesis.type)
        self.assertEqual(report.hypotheses[0].confidence, hypothesis.confidence)

    def test_non_explanation_versioned_payload_passes_through_unchanged(self):
        from ai.v2.schemas.advice import RAG_ADVICE_SCHEMA_VERSION
        payload = {
            "schema_version": RAG_ADVICE_SCHEMA_VERSION,
            "summary": "bounded",
            "knowledge_explanations": [],
            "recommended_checks": [],
            "limitations": [{"text": "missing RF", "evidence_ids": ["e"], "citation_ids": []}],
        }
        self.assertEqual(DashScopeProvider._normalize_qwen_explanation(payload), payload)

    def test_capabilities_do_not_claim_reachability(self):
        without_key = provider_capabilities({})
        with_key = provider_capabilities({"DASHSCOPE_API_KEY": "k", "WEAKNET_LLM_MODEL": "qwen-test"})
        self.assertFalse(without_key["providers"]["dashscope"]["configured"])
        self.assertTrue(with_key["providers"]["dashscope"]["configured"])
        self.assertFalse(with_key["providers"]["dashscope"]["reachable"])


if __name__ == "__main__":
    unittest.main()
