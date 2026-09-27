"""Explicit live end-to-end smoke test; skipped in ordinary test runs.

It expects a running repository weaknetd on the session bus and an explicitly
enabled DashScope configuration.  No prose is asserted and no secret is
printed.
"""

from __future__ import annotations

import asyncio
import os
import unittest

from ai.v2.providers.dashscope import DashScopeProvider
from ai.v2.services.explainer import EvidenceExplainerService
from ai.v2.sources.dbus import DbusDiagnosisSource


ENABLED = (
    os.environ.get("WEAKNET_RUN_LIVE_LLM_TESTS") == "1"
    and os.environ.get("WEAKNET_RUN_LIVE_DBUS_AI_TEST") == "1"
    and os.environ.get("WEAKNET_LLM_PROVIDER", "").lower() == "dashscope"
    and bool(os.environ.get("DASHSCOPE_API_KEY"))
)


@unittest.skipUnless(
    ENABLED,
    "live runtime test requires an explicit flag, running weaknetd, dashscope, and an API key",
)
class LiveRuntimeSmokeTest(unittest.TestCase):
    def test_dbus_to_dashscope_report(self):
        async def run():
            snapshot = await DbusDiagnosisSource().current()
            return await EvidenceExplainerService(DashScopeProvider.from_env()).explain(snapshot, "live-runtime")

        report = asyncio.run(run())
        self.assertEqual(report.provider, "dashscope")
        self.assertEqual(report.validation_status, "validated")
        for hypothesis in report.hypotheses:
            self.assertTrue(hypothesis.type)
            self.assertTrue(hypothesis.confidence)
            self.assertIsNotNone(hypothesis.missing_evidence)

