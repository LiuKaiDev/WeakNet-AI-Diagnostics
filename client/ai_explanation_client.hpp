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

struct ExplanationClientConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8765};
    std::int64_t timeout_ms{38000};
};

class ExplanationClient {
public:
    explicit ExplanationClient(ExplanationClientConfig config = {});

    ExplanationResult explainCurrent() const;
    static ExplanationResult parseResponse(int status_code, const std::string& body);

private:
    ExplanationClientConfig config_;
};

}  // namespace weaknet_ai
