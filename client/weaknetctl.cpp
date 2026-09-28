#include <dbus/dbus.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "ai_explanation_client.hpp"
#include "common.hpp"

namespace {

using Dict = std::map<std::string, std::string>;

std::string basicValue(DBusMessageIter* value) {
    switch (dbus_message_iter_get_arg_type(value)) {
        case DBUS_TYPE_STRING: {
            const char* text = "";
            dbus_message_iter_get_basic(value, &text);
            return text ? text : "";
        }
        case DBUS_TYPE_UINT64: {
            dbus_uint64_t number{};
            dbus_message_iter_get_basic(value, &number);
            return std::to_string(number);
        }
        case DBUS_TYPE_UINT32: {
            dbus_uint32_t number{};
            dbus_message_iter_get_basic(value, &number);
            return std::to_string(number);
        }
        case DBUS_TYPE_DOUBLE: {
            double number{};
            dbus_message_iter_get_basic(value, &number);
            return std::to_string(number);
        }
        case DBUS_TYPE_BOOLEAN: {
            dbus_bool_t value_bool = false;
            dbus_message_iter_get_basic(value, &value_bool);
            return value_bool ? "yes" : "no";
        }
        default:
            return "unavailable";
    }
}

Dict parseDict(DBusMessageIter* array) {
    Dict result;
    DBusMessageIter entry;
    while (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(array) != DBUS_TYPE_DICT_ENTRY) {
            dbus_message_iter_next(array);
            continue;
        }
        dbus_message_iter_recurse(array, &entry);
        const char* key = "";
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            DBusMessageIter variant;
            dbus_message_iter_recurse(&entry, &variant);
            result[key ? key : ""] = basicValue(&variant);
        }
        dbus_message_iter_next(array);
    }
    return result;
}

bool call(DBusConnection* connection, const char* method, DBusMessage** reply,
          std::string& error) {
    DBusMessage* request = dbus_message_new_method_call(
        weaknet_dbus::kBusName, weaknet_dbus::kV2ObjectPath,
        weaknet_dbus::kV2Interface, method);
    if (!request) { error = "unable to allocate D-Bus request"; return false; }
    DBusError dbus_error;
    dbus_error_init(&dbus_error);
    *reply = dbus_connection_send_with_reply_and_block(connection, request, 3000, &dbus_error);
    dbus_message_unref(request);
    if (!*reply) {
        error = dbus_error_is_set(&dbus_error) && dbus_error.message
            ? dbus_error.message : "D-Bus call failed";
        dbus_error_free(&dbus_error);
        return false;
    }
    dbus_error_free(&dbus_error);
    return true;
}

bool parseStatus(DBusMessage* reply, Dict& status, std::string& error) {
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        error = dbus_message_get_error_name(reply);
        return false;
    }
    DBusMessageIter args;
    if (!dbus_message_iter_init(reply, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        error = "malformed GetStatus reply";
        return false;
    }
    DBusMessageIter dict;
    dbus_message_iter_recurse(&args, &dict);
    status = parseDict(&dict);
    return true;
}

std::vector<Dict> parseList(DBusMessage* reply, std::string& error) {
    std::vector<Dict> result;
    if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
        error = dbus_message_get_error_name(reply);
        return result;
    }
    DBusMessageIter args;
    if (!dbus_message_iter_init(reply, &args) ||
        dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_ARRAY) {
        error = "malformed list reply";
        return result;
    }
    DBusMessageIter array;
    dbus_message_iter_recurse(&args, &array);
    while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID) {
        if (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_ARRAY) {
            DBusMessageIter dict;
            dbus_message_iter_recurse(&array, &dict);
            result.push_back(parseDict(&dict));
        }
        dbus_message_iter_next(&array);
    }
    return result;
}

void printStatus(const Dict& status) {
    const auto get = [&](const char* key, const char* fallback = "unknown") {
        const auto it = status.find(key);
        return it == status.end() ? std::string(fallback) : it->second;
    };
    std::cout << "WeakNet status: " << get("state") << "\n"
              << "Topology authoritative: " << get("topology_authoritative") << "\n"
              << "Topology degraded: " << get("topology_degraded") << "\n"
              << "Socket tracker degraded: " << get("socket_tracker_degraded") << "\n"
              << "Selected uplink: " << get("uplink", "unavailable") << "\n"
              << "Active incidents: " << get("active_incidents", "0") << "\n"
              << "Root-cause hypotheses: " << get("active_hypotheses", "0") << "\n";
}

void printList(const std::vector<Dict>& values, const char* title) {
    std::cout << title << ": " << values.size() << "\n";
    for (const auto& value : values) {
        const auto find = [&](const char* key, const char* fallback = "unknown") {
            const auto it = value.find(key);
            return it == value.end() ? std::string(fallback) : it->second;
        };
        const auto identifier = value.contains("id") ? find("id") : find("occurrence", "?");
        std::cout << "  [" << identifier << "] "
                  << find("type") << "\n"
                  << "      state: " << find("state") << "\n"
                  << "      scope: " << find("scope") << "\n";
        if (value.contains("confidence"))
            std::cout << "      confidence: " << find("confidence") << "\n";
        if (value.contains("supporting_summary"))
            std::cout << "      supporting evidence: " << find("supporting_summary") << "\n"
                      << "      contradicting evidence: " << find("contradicting_summary", "none") << "\n"
                      << "      missing evidence: " << find("missing_summary", "none") << "\n";
    }
}

int statusExit(const Dict& status) {
    const auto it = status.find("state");
    if (it == status.end() || it->second == "Unknown") return 2;
    return it->second == "Healthy" ? 0 : 1;
}

void usage() {
    std::cout << "Usage: weaknetctl <status|incidents|hypotheses|diagnose [--explain|--advise]>\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        usage();
        return argc >= 2 && argc <= 3 ? 0 : 3;
    }
    const std::string command = argv[1];
    const bool explain = argc == 3 && std::string(argv[2]) == "--explain";
    const bool advise = argc == 3 && std::string(argv[2]) == "--advise";
    if ((command != "status" && command != "incidents" && command != "hypotheses" && command != "diagnose") ||
        (argc == 3 && (!(explain || advise) || command != "diagnose"))) {
        usage();
        return 3;
    }
    DBusError dbus_error;
    dbus_error_init(&dbus_error);
    DBusConnection* connection = dbus_bus_get(DBUS_BUS_SESSION, &dbus_error);
    if (!connection) {
        std::cerr << "weaknetctl: D-Bus unavailable: "
                  << (dbus_error.message ? dbus_error.message : "unknown error") << '\n';
        dbus_error_free(&dbus_error);
        return 3;
    }

    std::string error;
    int result = 0;
    if (command == "status" || command == "diagnose") {
        DBusMessage* reply = nullptr;
        Dict status;
        if (!call(connection, weaknet_dbus::kV2MethodGetStatus, &reply, error) ||
            !parseStatus(reply, status, error)) {
            std::cerr << "weaknetctl: " << error << '\n';
            if (reply) dbus_message_unref(reply);
            dbus_connection_unref(connection);
            return 3;
        }
        dbus_message_unref(reply);
        printStatus(status);
        result = statusExit(status);
        if (command == "status") {
            dbus_connection_unref(connection);
            return result;
        }
        std::cout << "\nDiagnosis:\n";
    }
    if (command == "incidents" || command == "diagnose") {
        DBusMessage* reply = nullptr;
        if (!call(connection, weaknet_dbus::kV2MethodListActiveIncidents, &reply, error)) {
            std::cerr << "weaknetctl: " << error << '\n';
            dbus_connection_unref(connection);
            return 3;
        }
        auto values = parseList(reply, error);
        dbus_message_unref(reply);
        if (!error.empty()) { std::cerr << "weaknetctl: " << error << '\n'; dbus_connection_unref(connection); return 3; }
        printList(values, "Active incidents");
        if (command == "incidents") { dbus_connection_unref(connection); return 0; }
    }
    if (command == "hypotheses" || command == "diagnose") {
        DBusMessage* reply = nullptr;
        if (!call(connection, weaknet_dbus::kV2MethodListRootCauseHypotheses, &reply, error)) {
            std::cerr << "weaknetctl: " << error << '\n';
            dbus_connection_unref(connection);
            return 3;
        }
        auto values = parseList(reply, error);
        dbus_message_unref(reply);
        if (!error.empty()) { std::cerr << "weaknetctl: " << error << '\n'; dbus_connection_unref(connection); return 3; }
        printList(values, "Root-cause hypotheses");
    }
    dbus_connection_unref(connection);
    if (explain || advise) {
        weaknet_ai::ExplanationClientConfig config;
        if (const char* host = std::getenv("WEAKNET_AI_HOST")) config.host = host;
        if (const char* port = std::getenv("WEAKNET_AI_PORT")) {
            try {
                const auto parsed = std::stoul(port);
                if (parsed > 0 && parsed <= 65535) config.port = static_cast<std::uint16_t>(parsed);
            } catch (...) {
                // Keep the safe default; malformed optional configuration must
                // not make deterministic diagnosis unavailable.
            }
        }
        if (const char* timeout = std::getenv("WEAKNET_AI_TIMEOUT_SECONDS")) {
            try {
                const auto seconds = std::stoll(timeout);
                if (seconds > 0 && seconds <= 40) config.timeout_ms = seconds * 1000;
            } catch (...) {
                // Keep the bounded default.
            }
        }
        if (advise) {
            const auto ai = weaknet_ai::ExplanationClient(config).adviseCurrent();
            if (!ai.success) {
                std::cout << "\nRAG advice: unavailable\nRAG error:\n  " << ai.message << "\n";
            } else if (ai.report.status == "not_applicable") {
                std::cout << "\nRAG advice: not applicable\n  " << ai.report.summary << "\n";
            } else {
                std::cout << "\nRAG advice (" << ai.report.retrieval_mode << "):\n  " << ai.report.summary << "\n";
                if (!ai.report.knowledge_explanations.empty()) {
                    std::cout << "Knowledge-backed interpretation:\n";
                    for (const auto& explanation : ai.report.knowledge_explanations) {
                        std::cout << "  - " << explanation.text;
                        for (const auto& citation : explanation.citation_labels) std::cout << "\n      source: " << citation;
                        std::cout << "\n";
                    }
                }
                if (!ai.report.recommended_checks.empty()) {
                    std::cout << "Recommended checks:\n";
                    for (const auto& check : ai.report.recommended_checks) {
                        std::cout << "  - " << check.text;
                        for (const auto& citation : check.citation_labels) std::cout << "\n      source: " << citation;
                        std::cout << "\n";
                    }
                }
                for (const auto& limitation : ai.report.limitations) std::cout << "Limitation: " << limitation << "\n";
            }
            return result;
        }
        const auto ai = weaknet_ai::ExplanationClient(config).explainCurrent();
        if (!ai.success) {
            std::cout << "\nAI explanation:\n  unavailable\n"
                      << "AI error:\n  " << ai.message << "\n";
        } else {
            const auto& report = ai.report;
            std::cout << "\nAI explanation" << (report.simulated ? " (simulated)" : "") << ":\n"
                      << "  " << report.summary << "\n";
            for (const auto& hypothesis : report.hypotheses) {
                std::cout << "  " << hypothesis.explanation << "\n";
            }
            if (!report.limitations.empty()) {
                std::cout << "\nLimitations:\n";
                for (const auto& limitation : report.limitations)
                    std::cout << "  - " << limitation << "\n";
            }
        }
    }
    return result;
}
