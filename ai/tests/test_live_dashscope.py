"""Explicit opt-in live smoke test; never runs in normal unit test runs."""

from __future__ import annotations

import os
import unittest

from ai.tests.fixtures import local_link_suspected
from ai.v2.providers.dashscope import DashScopeProvider
from ai.v2.services.explainer import EvidenceExplainerService


LIVE = os.environ.get("WEAKNET_RUN_LIVE_LLM_TESTS") == "1"
HAS_KEY = bool(os.environ.get("DASHSCOPE_API_KEY"))
SELECTED = os.environ.get("WEAKNET_LLM_PROVIDER", "").lower() == "dashscope"


@unittest.skipUnless(LIVE and HAS_KEY and SELECTED,
                     "live DashScope test requires explicit opt-in, dashscope selection, and DASHSCOPE_API_KEY")
class LiveDashScopeSmokeTest(unittest.TestCase):
    def test_local_link_fixture(self):
        report = EvidenceExplainerService(DashScopeProvider.from_env()).explain_sync(
            local_link_suspected(), "live-smoke"
        )
        self.assertEqual(report.provider, "dashscope")
        self.assertEqual(report.hypotheses[0].type, "LocalLinkSuspected")
        self.assertEqual(report.hypotheses[0].confidence, "Medium")

