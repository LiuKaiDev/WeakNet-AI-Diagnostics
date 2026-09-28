"""Explicit-cost, real-Qwen smoke for real lexical retrieval and citations."""
import os
import re
import unittest

from ai.tests.fixtures import insufficient_evidence, local_link_suspected, remote_upstream_degradation
from ai.v2.providers.factory import create_llm_provider
from ai.v2.services.advisor import RagAdvisorService


OPT_IN = os.environ.get("WEAKNET_RUN_LIVE_RAG_ADVISOR", "") == "1"
HAS_PROVIDER = os.environ.get("WEAKNET_LLM_PROVIDER", "").lower() == "dashscope" and bool(os.environ.get("DASHSCOPE_API_KEY"))


@unittest.skipUnless(OPT_IN and HAS_PROVIDER,
                     "requires WEAKNET_RUN_LIVE_RAG_ADVISOR=1, DashScope selection, and configured credentials")
class LiveRagAdvisorSmokeTest(unittest.TestCase):
    def test_real_lexical_retrieval_and_qwen_citations(self):
        provider = create_llm_provider("dashscope")
        service = RagAdvisorService(provider)
        cases = (local_link_suspected(), remote_upstream_degradation(), insufficient_evidence())
        for snapshot in cases:
            with self.subTest(hypothesis=snapshot.hypotheses[0].type):
                report = service.advise_sync(snapshot)
                self.assertEqual(report.status, "validated")
                self.assertEqual(report.retrieval_mode, "lexical")
                known = {item["citation_id"] for item in report.cited_knowledge}
                self.assertTrue(known)
                for item in report.knowledge_explanations + report.recommended_checks:
                    self.assertTrue(item.citation_ids)
                    self.assertTrue(set(item.citation_ids).issubset(known))
                combined = " ".join([report.summary] + [item.text for item in report.knowledge_explanations + report.recommended_checks]).lower()
                self.assertNotRegex(combined, re.compile(r"(?:confirmed|definite)\s+(?:isp|interference|root cause)"))
                self.assertNotRegex(combined, re.compile(r"isp\s+(?:failure|outage|congestion)\s+confirmed"))
        print("RAG ADVISOR LIVE PASS (LEXICAL)")


if __name__ == "__main__":
    unittest.main()
