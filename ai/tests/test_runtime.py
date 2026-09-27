import asyncio
import json
import unittest

from ai.tests.fixtures import local_link_suspected
from ai.v2.errors import DiagnosisSourceTimeout, MalformedDiagnosisSourcePayload
from ai.v2.providers.fake import FakeLlmProvider
from ai.v2.runtime import AiExplanationApplication
from ai.v2.services.explainer import EvidenceExplainerService
from ai.v2.schemas.explanation import EXPLANATION_SCHEMA_VERSION
from ai.v2.sources.dbus import DbusDiagnosisSource


class _Source:
    def __init__(self, snapshot=None, error=None):
        self.snapshot = snapshot
        self.error = error

    async def current(self):
        if self.error:
            raise self.error
        return self.snapshot


class _Client:
    async def get_diagnosis(self):
        return {"state": "Healthy"}


class RuntimeServiceTests(unittest.TestCase):
    def test_health_and_fake_capabilities_need_no_dbus_or_provider(self):
        app = AiExplanationApplication.from_env(
            {"WEAKNET_LLM_PROVIDER": "fake"}, diagnosis_source=_Source()
        )
        self.assertEqual(app.handle("GET", "/health/live"), (200, {"status": "live"}))
        status, capabilities = app.handle("GET", "/v2/capabilities")
        self.assertEqual(status, 200)
        self.assertEqual(capabilities["selected_provider"], "fake")
        self.assertTrue(capabilities["explanations_available"])

    def test_dashscope_without_key_stays_live_but_explanation_is_explicit(self):
        app = AiExplanationApplication.from_env(
            {"WEAKNET_LLM_PROVIDER": "dashscope"}, diagnosis_source=_Source()
        )
        status, capabilities = app.handle("GET", "/v2/capabilities")
        self.assertEqual(status, 200)
        self.assertFalse(capabilities["explanations_available"])
        self.assertEqual(capabilities["provider_error"]["category"], "ProviderUnavailable")
        status, body = app.handle(
            "POST", "/v2/explanations", json.dumps(local_link_suspected().to_dict()).encode()
        )
        self.assertEqual(status, 503)
        self.assertEqual(body["error"]["category"], "ProviderUnavailable")

    def test_explanation_endpoint_returns_structured_report(self):
        app = AiExplanationApplication.from_env(
            {"WEAKNET_LLM_PROVIDER": "fake"}, diagnosis_source=_Source()
        )
        status, report = app.handle(
            "POST", "/v2/explanations", json.dumps(local_link_suspected().to_dict()).encode()
        )
        self.assertEqual(status, 200)
        self.assertEqual(report["schema_version"], "weaknet.ai.explanation.v1")
        self.assertEqual(report["hypotheses"][0]["type"], "LocalLinkSuspected")
        self.assertTrue(report["simulated"])

    def test_invalid_snapshot_is_rejected(self):
        app = AiExplanationApplication.from_env(
            {"WEAKNET_LLM_PROVIDER": "fake"}, diagnosis_source=_Source()
        )
        status, body = app.handle("POST", "/v2/explanations", b'{"status":"Degraded"}')
        self.assertEqual(status, 400)
        self.assertEqual(body["error"]["category"], "InvalidDiagnosisInput")

    def test_current_endpoint_surfaces_source_timeout(self):
        app = AiExplanationApplication.from_env(
            {"WEAKNET_LLM_PROVIDER": "fake"},
            diagnosis_source=_Source(error=DiagnosisSourceTimeout("timed out")),
        )
        status, body = app.handle("POST", "/v2/explanations/current", b"{}")
        self.assertEqual(status, 503)
        self.assertEqual(body["error"]["category"], "DiagnosisSourceTimeout")

    def test_grounding_failure_is_an_explicit_service_error(self):
        snapshot = local_link_suspected()
        evidence_id = snapshot.hypotheses[0].supporting_evidence[0].evidence_id
        bad = FakeLlmProvider(response={
            "schema_version": EXPLANATION_SCHEMA_VERSION,
            "provider": "fake", "simulated": True, "summary": "bad",
            "hypothesis_explanations": [{
                "hypothesis_id": snapshot.hypotheses[0].hypothesis_id,
                "explanation": "bad",
                "supporting_evidence_ids": [], "contradicting_evidence_ids": [],
                "missing_evidence_ids": [evidence_id],
            }],
            "evidence_explanations": [], "limitations": [],
        })
        app = AiExplanationApplication(
            explainer=EvidenceExplainerService(bad), diagnosis_source=_Source(snapshot),
            selected_provider="fake",
        )
        status, body = app.handle(
            "POST", "/v2/explanations", json.dumps(snapshot.to_dict()).encode()
        )
        self.assertEqual(status, 503)
        self.assertEqual(body["error"]["category"], "GroundingViolation")

    def test_dbus_source_adapter_and_timeout_are_injectable(self):
        payload = local_link_suspected().to_dict()
        payload["state"] = payload.pop("status")
        source = DbusDiagnosisSource(client=type("Client", (), {
            "get_diagnosis": lambda self: asyncio.sleep(0, result=payload)
        })())
        snapshot = asyncio.run(source.current())
        self.assertEqual(snapshot.hypotheses[0].type, "LocalLinkSuspected")

        class Slow:
            async def get_diagnosis(self):
                await asyncio.sleep(0.02)

        with self.assertRaises(DiagnosisSourceTimeout):
            asyncio.run(DbusDiagnosisSource(client=Slow(), timeout_seconds=0.001).current())

        class Malformed:
            async def get_diagnosis(self):
                return {"state": "Degraded", "hypotheses": []}

        with self.assertRaises(MalformedDiagnosisSourcePayload):
            asyncio.run(DbusDiagnosisSource(client=Malformed()).current())


if __name__ == "__main__":
    unittest.main()
