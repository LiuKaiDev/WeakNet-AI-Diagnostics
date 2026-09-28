#include "ai_explanation_client.hpp"

#include <cassert>
#include <string>

int main() {
    const std::string response = R"({
      "schema_version":"weaknet.ai.explanation.v1",
      "request_id":"test",
      "diagnosis_snapshot_timestamp_ms":1,
      "deterministic_status":"Degraded",
      "provider":"fake",
      "model":"deterministic-fake-v2",
      "simulated":true,
      "summary":"grounded summary",
      "hypotheses":[{"hypothesis_id":"h","type":"LocalLinkSuspected","confidence":"Medium","state":"Active","scope":null,"explanation":"e","supporting_evidence":[],"contradicting_evidence":[],"missing_evidence":[]}],
      "limitations":[{"source":"diagnosis","explanation":"AP health is unavailable"}],
      "validation_status":"validated"
    })";
    const auto parsed = weaknet_ai::ExplanationClient::parseResponse(200, response);
    assert(parsed.success);
    assert(parsed.report.simulated);
    assert(parsed.report.hypotheses.size() == 1);
    assert(parsed.report.hypotheses.front().confidence == "Medium");
    assert(parsed.report.limitations.front() == "AP health is unavailable");

    const auto provider_error = weaknet_ai::ExplanationClient::parseResponse(
        503, R"({"error":{"category":"ProviderTimeout","message":"provider timeout"}})");
    assert(!provider_error.success);
    assert(provider_error.category == "ProviderTimeout");
    assert(provider_error.message == "provider timeout");

    const auto malformed = weaknet_ai::ExplanationClient::parseResponse(200, "not-json");
    assert(!malformed.success);
    assert(malformed.category == "InvalidProviderOutput");

    const std::string advice = R"({
      "schema_version":"weaknet.ai.rag-advice.v1",
      "provider":"fake","model":"deterministic-fake-v2",
      "deterministic_status":"Degraded","retrieval_mode":"lexical",
      "summary":"bounded advice",
      "knowledge_explanations":[{"text":"Knowledge interpretation","citation_ids":["doc/chunk@1"]}],
      "recommended_checks":[{"text":"Collect missing evidence","citation_ids":["doc/chunk@1"]}],
      "limitations":[{"text":"RF evidence is missing"}],
      "cited_knowledge":[{"citation_id":"doc/chunk@1","title":"Knowledge","source":"guide.md"}],
      "status":"validated"
    })";
    const auto parsed_advice = weaknet_ai::ExplanationClient::parseAdviceResponse(200, advice);
    assert(parsed_advice.success);
    assert(parsed_advice.report.retrieval_mode == "lexical");
    assert(parsed_advice.report.knowledge_explanations.size() == 1);
    assert(parsed_advice.report.recommended_checks.size() == 1);
    assert(parsed_advice.report.recommended_checks.front().citation_ids.front() == "doc/chunk@1");
    assert(parsed_advice.report.recommended_checks.front().citation_labels.front().find("guide.md") != std::string::npos);

    const auto not_applicable = weaknet_ai::ExplanationClient::parseAdviceResponse(
        200, R"({"schema_version":"weaknet.ai.rag-advice.v1","provider":"not-used","model":"not-used","deterministic_status":"Unknown","retrieval_mode":"none","summary":"No active hypothesis is available for knowledge advice.","knowledge_explanations":[],"recommended_checks":[],"limitations":[],"status":"not_applicable","error":null})");
    assert(not_applicable.success);
    assert(not_applicable.report.status == "not_applicable");
    assert(not_applicable.report.retrieval_mode == "none");

    const auto retrieval_unavailable = weaknet_ai::ExplanationClient::parseAdviceResponse(
        200, R"({"schema_version":"weaknet.ai.rag-advice.v1","provider":"unavailable","model":"unavailable","deterministic_status":"Degraded","retrieval_mode":"none","summary":"RAG retrieval is unavailable.","knowledge_explanations":[],"recommended_checks":[],"limitations":[],"status":"unavailable","error":{"category":"RagUnavailable","message":"knowledge retrieval is unavailable"}})");
    assert(!retrieval_unavailable.success);
    assert(retrieval_unavailable.category == "RagUnavailable");

    const auto invalid_mode = weaknet_ai::ExplanationClient::parseAdviceResponse(
        200, R"({"schema_version":"weaknet.ai.rag-advice.v1","provider":"fake","model":"m","deterministic_status":"Degraded","retrieval_mode":"hybrid-fake","summary":"x","knowledge_explanations":[],"recommended_checks":[],"limitations":[]})");
    assert(!invalid_mode.success);
    return 0;
}
