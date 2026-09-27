#include "ai_explanation_client.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <map>
#include <stdexcept>
#include <utility>
#include <variant>

namespace weaknet_ai {
namespace {

struct JsonValue;
using Json = std::variant<std::nullptr_t, bool, double, std::string,
                          std::vector<JsonValue>, std::map<std::string, JsonValue>>;
struct JsonValue { Json value; };
using JsonArray = std::vector<JsonValue>;
using JsonObject = std::map<std::string, JsonValue>;

// A deliberately small bounded JSON reader.  The service is local, but its
// response is still treated as untrusted and never parsed with string grep.
class JsonReader {
public:
    explicit JsonReader(const std::string& input) : input_(input) {}

    JsonValue parse() {
        if (input_.size() > 1024 * 1024) throw std::runtime_error("JSON response too large");
        skip();
        JsonValue result = value(0);
        skip();
        if (position_ != input_.size()) throw std::runtime_error("trailing JSON data");
        return result;
    }

private:
    void skip() { while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_; }
    char take() { if (position_ >= input_.size()) throw std::runtime_error("unexpected end of JSON"); return input_[position_++]; }
    void expect(const char* text) {
        while (*text) if (take() != *text++) throw std::runtime_error("invalid JSON token");
    }
    JsonValue value(unsigned depth) {
        if (depth > 64) throw std::runtime_error("JSON nesting too deep");
        skip();
        if (position_ >= input_.size()) throw std::runtime_error("missing JSON value");
        switch (input_[position_]) {
            case '{': return object(depth + 1);
            case '[': return array(depth + 1);
            case '"': return JsonValue{string()};
            case 't': expect("true"); return JsonValue{true};
            case 'f': expect("false"); return JsonValue{false};
            case 'n': expect("null"); return JsonValue{nullptr};
            default: return JsonValue{number()};
        }
    }
    JsonValue object(unsigned depth) {
        take();
        JsonObject result;
        skip();
        if (position_ < input_.size() && input_[position_] == '}') { ++position_; return JsonValue{result}; }
        while (true) {
            skip();
            if (take() != '"') throw std::runtime_error("JSON object key is not a string");
            --position_;
            std::string key = string();
            skip(); if (take() != ':') throw std::runtime_error("missing JSON colon");
            result.emplace(std::move(key), value(depth));
            skip(); const char delimiter = take();
            if (delimiter == '}') break;
            if (delimiter != ',') throw std::runtime_error("invalid JSON object delimiter");
        }
        return JsonValue{result};
    }
    JsonValue array(unsigned depth) {
        take();
        JsonArray result;
        skip();
        if (position_ < input_.size() && input_[position_] == ']') { ++position_; return JsonValue{result}; }
        while (true) {
            result.push_back(value(depth));
            skip(); const char delimiter = take();
            if (delimiter == ']') break;
            if (delimiter != ',') throw std::runtime_error("invalid JSON array delimiter");
        }
        return JsonValue{result};
    }
    std::string string() {
        if (take() != '"') throw std::runtime_error("invalid JSON string");
        std::string result;
        while (position_ < input_.size()) {
            const char ch = take();
            if (ch == '"') return result;
            if (static_cast<unsigned char>(ch) < 0x20) throw std::runtime_error("control character in JSON string");
            if (ch != '\\') { result += ch; continue; }
            const char escaped = take();
            switch (escaped) {
                case '"': result += '"'; break; case '\\': result += '\\'; break;
                case '/': result += '/'; break; case 'b': result += '\b'; break;
                case 'f': result += '\f'; break; case 'n': result += '\n'; break;
                case 'r': result += '\r'; break; case 't': result += '\t'; break;
                case 'u':
                    // Product text is UTF-8 in practice.  Preserve ASCII and
                    // use a replacement marker for escaped non-ASCII text.
                    for (int i = 0; i < 4; ++i) {
                        const char digit = take();
                        if (!std::isxdigit(static_cast<unsigned char>(digit))) throw std::runtime_error("invalid JSON unicode escape");
                    }
                    result += '?'; break;
                default: throw std::runtime_error("invalid JSON escape");
            }
        }
        throw std::runtime_error("unterminated JSON string");
    }
    double number() {
        const std::size_t start = position_;
        if (input_[position_] == '-') ++position_;
        while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        if (position_ < input_.size() && input_[position_] == '.') { ++position_; while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_; }
        if (position_ < input_.size() && (input_[position_] == 'e' || input_[position_] == 'E')) {
            ++position_; if (position_ < input_.size() && (input_[position_] == '+' || input_[position_] == '-')) ++position_;
            while (position_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[position_]))) ++position_;
        }
        if (start == position_) throw std::runtime_error("invalid JSON number");
        try { return std::stod(input_.substr(start, position_ - start)); }
        catch (...) { throw std::runtime_error("invalid JSON number"); }
    }
    const std::string& input_;
    std::size_t position_{0};
};

const JsonObject& object(const JsonValue& value) {
    auto* result = std::get_if<JsonObject>(&value.value);
    if (!result) throw std::runtime_error("expected JSON object");
    return *result;
}
const JsonArray& array(const JsonValue& value) {
    auto* result = std::get_if<JsonArray>(&value.value);
    if (!result) throw std::runtime_error("expected JSON array");
    return *result;
}
std::string text(const JsonObject& value, const char* key, bool required = true) {
    const auto it = value.find(key);
    if (it == value.end()) { if (required) throw std::runtime_error("missing report field"); return {}; }
    auto* result = std::get_if<std::string>(&it->second.value);
    if (!result || (required && result->empty())) throw std::runtime_error("invalid report field");
    return *result;
}
bool boolean(const JsonObject& value, const char* key) {
    const auto it = value.find(key); if (it == value.end()) throw std::runtime_error("missing report boolean");
    auto* result = std::get_if<bool>(&it->second.value); if (!result) throw std::runtime_error("invalid report boolean"); return *result;
}

ExplanationResult parseReport(const JsonObject& root) {
    if (text(root, "schema_version") != "weaknet.ai.explanation.v1") throw std::runtime_error("unsupported report schema");
    ExplanationReport report;
    report.provider = text(root, "provider");
    report.model = text(root, "model");
    report.simulated = boolean(root, "simulated");
    report.deterministic_status = text(root, "deterministic_status");
    report.summary = text(root, "summary");
    if (text(root, "validation_status") != "validated") throw std::runtime_error("report was not validated");
    for (const auto& item : array(root.at("hypotheses"))) {
        const auto& value = object(item);
        report.hypotheses.push_back({text(value, "type"), text(value, "confidence"), text(value, "state"), text(value, "explanation")});
    }
    for (const auto& item : array(root.at("limitations"))) {
        const auto& value = object(item);
        std::string explanation = text(value, "explanation");
        const auto missing = value.find("missing_evidence_id");
        if (missing != value.end()) explanation = text(value, "missing_evidence_id") + ": " + explanation;
        report.limitations.push_back(std::move(explanation));
    }
    return ExplanationResult{true, std::move(report), {}, {}};
}

std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool waitFd(int fd, short events, std::int64_t deadline) {
    while (true) {
        const auto remaining = deadline - nowMs();
        if (remaining <= 0) return false;
        pollfd descriptor{fd, events, 0};
        const int result = ::poll(&descriptor, 1,
                                  static_cast<int>(std::min<std::int64_t>(remaining, 1000)));
        if (result > 0) return true;
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return false;
        // A one-second poll slice expired; retain the overall deadline.
    }
}
enum class IoStatus { Success, Timeout, Error };
IoStatus sendAll(int fd, const std::string& request, std::int64_t deadline) {
    std::size_t sent = 0;
    while (sent < request.size()) {
        if (!waitFd(fd, POLLOUT, deadline)) return IoStatus::Timeout;
        const ssize_t count = ::send(fd, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) return IoStatus::Error;
        sent += static_cast<std::size_t>(count);
    }
    return IoStatus::Success;
}

}  // namespace

ExplanationClient::ExplanationClient(ExplanationClientConfig config) : config_(std::move(config)) {
    if (config_.timeout_ms < 1 || config_.timeout_ms > 40000) config_.timeout_ms = 38000;
}

ExplanationResult ExplanationClient::parseResponse(int status_code, const std::string& body) {
    try {
        const auto root = object(JsonReader(body).parse());
        if (status_code < 200 || status_code >= 300) {
            const auto error = root.find("error");
            if (error != root.end()) {
                const auto& details = object(error->second);
                return {false, {}, text(details, "category"), text(details, "message")};
            }
            return {false, {}, "AiServiceError", "AI service request failed"};
        }
        return parseReport(root);
    } catch (...) {
        return {false, {}, "InvalidProviderOutput", "AI service returned an invalid explanation report"};
    }
}

ExplanationResult ExplanationClient::explainCurrent() const {
    if (config_.host != "127.0.0.1" && config_.host != "localhost" && config_.host != "::1")
        return {false, {}, "AiServiceUnavailable", "AI service must use a loopback address"};
    if (config_.port == 0) return {false, {}, "AiServiceUnavailable", "AI service address is invalid"};
    const std::int64_t deadline = nowMs() + config_.timeout_ms;
    addrinfo hints{}; hints.ai_socktype = SOCK_STREAM; hints.ai_family = AF_UNSPEC;
    addrinfo* addresses = nullptr;
    const std::string port = std::to_string(config_.port);
    if (::getaddrinfo(config_.host.c_str(), port.c_str(), &hints, &addresses) != 0)
        return {false, {}, "AiServiceUnavailable", "AI service is unavailable"};
    int fd = -1;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        fd = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd < 0) continue;
        const int flags = ::fcntl(fd, F_GETFL, 0); if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        const int result = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (result == 0) break;
        if (result < 0 && errno == EINPROGRESS && waitFd(fd, POLLOUT, deadline)) {
            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &socket_error_size) == 0 && socket_error == 0)
                break;
        }
        ::close(fd); fd = -1;
    }
    ::freeaddrinfo(addresses);
    if (fd < 0) return {false, {}, nowMs() >= deadline ? "AiServiceTimeout" : "AiServiceUnavailable", nowMs() >= deadline ? "AI service request timed out" : "AI service is unavailable"};
    const std::string request = "POST /v2/explanations/current HTTP/1.1\r\nHost: " + config_.host + "\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: 2\r\n\r\n{}";
    const auto send_status = sendAll(fd, request, deadline);
    if (send_status != IoStatus::Success) {
        ::close(fd);
        return send_status == IoStatus::Timeout
            ? ExplanationResult{false, {}, "AiServiceTimeout", "AI service request timed out"}
            : ExplanationResult{false, {}, "AiServiceUnavailable", "AI service is unavailable"};
    }
    std::string response;
    char buffer[4096];
    while (response.size() <= 1024 * 1024 && waitFd(fd, POLLIN, deadline)) {
        const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        response.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    if (response.empty()) return {false, {}, nowMs() >= deadline ? "AiServiceTimeout" : "AiServiceUnavailable", nowMs() >= deadline ? "AI service request timed out" : "AI service is unavailable"};
    const auto line_end = response.find("\r\n");
    const auto body_start = response.find("\r\n\r\n");
    if (line_end == std::string::npos || body_start == std::string::npos) return {false, {}, "AiServiceError", "AI service returned an invalid HTTP response"};
    const auto first_space = response.find(' ');
    int status = 0;
    try { status = std::stoi(response.substr(first_space + 1, line_end - first_space - 1)); } catch (...) { return {false, {}, "AiServiceError", "AI service returned an invalid HTTP status"}; }
    return parseResponse(status, response.substr(body_start + 4));
}

}  // namespace weaknet_ai
