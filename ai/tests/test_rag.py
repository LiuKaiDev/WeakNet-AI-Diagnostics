import unittest
import tempfile
from unittest.mock import patch

from ai.tests.fixtures import insufficient_evidence, local_link_suspected, remote_upstream_degradation
from ai.v2.schemas.diagnosis import Evidence, EvidenceRole
from ai.v2.rag.bm25 import BM25Retriever
from ai.v2.rag.chunking import chunk_documents
from ai.v2.rag.embeddings import FakeEmbeddingModel
from ai.v2.rag.embeddings import BgeEmbeddingModel
from ai.v2.rag.evaluation import recall_at_k, reciprocal_rank
from ai.v2.rag.fusion import reciprocal_rank_fusion
from ai.v2.rag.knowledge import CorpusManifest, load_documents, default_manifest_path
from ai.v2.rag.schemas import KnowledgeDocument
from ai.v2.rag.query_planner import RagQueryPlanner
from ai.v2.rag.reranker import FakeReranker
from ai.v2.rag.reranker import BgeReranker
from ai.v2.rag.retriever import HybridRetriever, RetrievalConfig
from ai.v2.rag.faiss_index import VectorIndex
from ai.v2.rag.faiss_index import IndexErrorRag
from ai.v2.rag.schemas import RetrievalBundle


class RagTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        manifest = CorpusManifest.load(default_manifest_path())
        cls.chunks = chunk_documents(load_documents(manifest, root=default_manifest_path().parent))

    def test_queries_are_deterministic_and_private(self):
        snapshot = local_link_suspected()
        snapshot.topology["ssid"] = "SecretWifi"
        snapshot.topology["interface"] = "wlan0"
        query = RagQueryPlanner().plan(snapshot)[0]
        self.assertEqual(query, RagQueryPlanner().plan(snapshot)[0])
        self.assertNotIn("secretwifi", query.search_text.lower())
        self.assertNotIn("wlan0", query.search_text.lower())
        self.assertIn("locallinksuspected", query.search_text)
        self.assertIn("missing", query.search_text)

    def test_insufficient_and_contradicting_evidence_are_explicit(self):
        query = RagQueryPlanner().plan(insufficient_evidence())[0]
        self.assertIn("check", query.search_text)
        self.assertIn("GatewayProbeUnavailable", query.missing_evidence)
        snapshot = local_link_suspected()
        snapshot.hypotheses[0].contradicting_evidence.append(Evidence(kind="WifiSignalNormal", role=EvidenceRole.CONTRADICTING))
        query = RagQueryPlanner().plan(snapshot)[0]
        self.assertIn("WifiSignalNormal", query.contradicting_evidence)
        self.assertIn("contradicting", query.search_text)

    def test_manifest_allowlist_and_stable_chunks(self):
        first = [chunk.chunk_id for chunk in self.chunks]
        second = [chunk.chunk_id for chunk in chunk_documents(load_documents(CorpusManifest.load(default_manifest_path()), root=default_manifest_path().parent))]
        self.assertEqual(first, second)
        self.assertTrue(all(chunk.source for chunk in self.chunks))
        self.assertTrue(all(len(chunk.text) <= 1800 for chunk in self.chunks))
        original = load_documents(CorpusManifest.load(default_manifest_path()), root=default_manifest_path().parent)[0]
        changed = KnowledgeDocument(document_id=original.document_id, title=original.title, source=original.source,
                                    source_type=original.source_type, version="changed", content=original.content,
                                    tags=original.tags, applicable_root_causes=original.applicable_root_causes)
        self.assertNotEqual(chunk_documents([original])[0].chunk_id, chunk_documents([changed])[0].chunk_id)

    def test_root_cause_coverage_matrix_includes_all_engine_types(self):
        manifest = CorpusManifest.load(default_manifest_path())
        documents = load_documents(manifest, root=default_manifest_path().parent)
        covered = {item for document in documents for item in document.applicable_root_causes}
        expected = {"UplinkAvailabilityProblem", "LocalRoutingProblem", "NetworkPathDegradation",
                    "RemoteOrUpstreamDegradation", "LocalLinkSuspected", "InsufficientEvidence"}
        self.assertTrue(expected.issubset(covered))
        uplink = next(document for document in documents if document.document_id == "topology-path-semantics")
        self.assertIn("UplinkAvailabilityProblem", uplink.applicable_root_causes)
        self.assertIn("authoritative", uplink.content.lower())

    def test_local_model_paths_fail_without_network_or_fallback(self):
        with self.assertRaises(FileNotFoundError):
            BgeEmbeddingModel("/definitely/missing/weaknet-bge")
        with self.assertRaises(FileNotFoundError):
            BgeReranker("/definitely/missing/weaknet-reranker")
        with patch.dict("os.environ", {"WEAKNET_RAG_EMBEDDING_MODEL": "/definitely/missing/from-env"}, clear=False):
            with self.assertRaises(FileNotFoundError):
                BgeEmbeddingModel()
        with patch.dict("os.environ", {"WEAKNET_RAG_RERANKER_MODEL": "/definitely/missing/reranker-from-env"}, clear=False):
            with self.assertRaises(FileNotFoundError):
                BgeReranker()

    def test_bm25_and_ties(self):
        hits = BM25Retriever(self.chunks).search("weak wifi signal", top_k=2)
        self.assertLessEqual(len(hits), 2)
        self.assertEqual(hits, BM25Retriever(self.chunks).search("weak wifi signal", top_k=2))

    def test_vector_index_and_hybrid_with_fakes(self):
        model = FakeEmbeddingModel(16)
        index = VectorIndex(self.chunks, model.embed([chunk.text for chunk in self.chunks]), model_name=model.model_name, corpus_version="test")
        query = RagQueryPlanner().plan(remote_upstream_degradation())[0]
        bundle = HybridRetriever(self.chunks, corpus_version="test", embedding_model=model, vector_index=index,
                                  reranker=FakeReranker(), config=RetrievalConfig(final_top_k=2)).retrieve(query)
        self.assertLessEqual(len(bundle.hits), 2)
        self.assertEqual(bundle.retrieval_mode, "hybrid")
        self.assertTrue(all(hit.citation_id for hit in bundle.hits))

    def test_rrf_and_metrics(self):
        lexical = BM25Retriever(self.chunks).search("wifi", top_k=2)
        fused = reciprocal_rank_fusion(lexical, [], top_k=1)
        self.assertEqual(len(fused), 1)
        self.assertEqual(recall_at_k(["a", "b"], {"b"}, 2), 1.0)
        self.assertEqual(reciprocal_rank(["a", "b"], {"b"}), 0.5)

    def test_index_metadata_mapping_and_bundle_round_trip(self):
        model = FakeEmbeddingModel(8)
        index = VectorIndex(self.chunks, model.embed([chunk.text for chunk in self.chunks]), model_name="fake", corpus_version="test")
        query = RagQueryPlanner().plan(local_link_suspected())[0]
        with tempfile.TemporaryDirectory() as directory:
            index.save(directory)
            loaded = VectorIndex.load(directory, self.chunks, expected={"corpus_version": "test", "embedding_model": "fake"})
            self.assertEqual(loaded.metadata()["chunk_ids"], index.metadata()["chunk_ids"])
            with self.assertRaises(IndexErrorRag):
                VectorIndex.load(directory, self.chunks, expected={"corpus_version": "stale"})
        bundle = HybridRetriever(self.chunks).retrieve(query)
        round_trip = RetrievalBundle.from_dict(bundle.to_dict())
        self.assertEqual(round_trip.to_dict(), bundle.to_dict())


if __name__ == "__main__":
    unittest.main()
