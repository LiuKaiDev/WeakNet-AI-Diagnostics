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
    return 0;
}
