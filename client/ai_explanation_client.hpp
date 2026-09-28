#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace weaknet_ai {

struct ExplainedHypothesis {
    std::string type;
    std::string confidence;
    std::string state;
    std::string explanation;
};

struct ExplanationReport {
    std::string provider;
    std::string model;
    bool simulated{false};
    std::string deterministic_status;
    std::string summary;
    std::vector<ExplainedHypothesis> hypotheses;
    std::vector<std::string> limitations;
};

struct ExplanationResult {
    bool success{false};
    ExplanationReport report;
    std::string category;
    std::string message;
};

struct AdviceCheck {
    std::string text;
    std::vector<std::string> citation_ids;
    std::vector<std::string> citation_labels;
};

struct AdviceReport {
    std::string status;
    std::string provider;
    std::string model;
    std::string deterministic_status;
    std::string retrieval_mode;
    std::string summary;
    std::vector<AdviceCheck> knowledge_explanations;
    std::vector<AdviceCheck> recommended_checks;
    std::vector<std::string> limitations;
};

struct AdviceResult {
    bool success{false};
    AdviceReport report;
    std::string category;
    std::string message;
};

struct ExplanationClientConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8765};
    std::int64_t timeout_ms{38000};
};

class ExplanationClient {
public:
    explicit ExplanationClient(ExplanationClientConfig config = {});

    ExplanationResult explainCurrent() const;
    AdviceResult adviseCurrent() const;
    static ExplanationResult parseResponse(int status_code, const std::string& body);
    static AdviceResult parseAdviceResponse(int status_code, const std::string& body);

private:
    ExplanationClientConfig config_;
};

}  // namespace weaknet_ai
