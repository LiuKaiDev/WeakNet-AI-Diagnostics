#include "runtime_health.hpp"

#include <sstream>

namespace weaknet_dbus {
namespace {

std::string jsonEscape(const std::string& value) {
    std::ostringstream output;
    for (unsigned char ch : value) {
        switch (ch) {
            case '\\': output << "\\\\"; break;
            case '"': output << "\\\""; break;
            case '\n': output << "\\n"; break;
            case '\r': output << "\\r"; break;
            case '\t': output << "\\t"; break;
            default:
                if (ch < 0x20) {
                    static constexpr char hex[] = "0123456789abcdef";
                    output << "\\u00" << hex[ch >> 4] << hex[ch & 0xf];
                } else {
                    output << static_cast<char>(ch);
                }
        }
    }
    return output.str();
}

}  // namespace

const char* runtimeHealthStateName(RuntimeHealthState state) {
    switch (state) {
        case RuntimeHealthState::Starting: return "starting";
        case RuntimeHealthState::Running: return "running";
        case RuntimeHealthState::Disabled: return "disabled";
        case RuntimeHealthState::Degraded: return "degraded";
        case RuntimeHealthState::Failed: return "failed";
        case RuntimeHealthState::Stopped: return "stopped";
    }
    return "failed";
}

void RuntimeHealth::set(std::string component, RuntimeHealthState state, std::string reason) {
    std::lock_guard lock(mutex_);
    RuntimeHealthEntry entry{component, state, std::move(reason)};
    entries_[component] = std::move(entry);
}

std::vector<RuntimeHealthEntry> RuntimeHealth::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<RuntimeHealthEntry> result;
    result.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        (void)name;
        result.push_back(entry);
    }
    return result;
}

bool RuntimeHealth::degraded() const {
    for (const auto& entry : snapshot()) {
        if (entry.state == RuntimeHealthState::Degraded ||
            entry.state == RuntimeHealthState::Failed) {
            return true;
        }
    }
    return false;
}

std::string RuntimeHealth::toJson() const {
    const auto entries = snapshot();
    bool has_degradation = false;
    for (const auto& entry : entries) {
        has_degradation = has_degradation || entry.state == RuntimeHealthState::Degraded ||
                          entry.state == RuntimeHealthState::Failed;
    }
    std::ostringstream output;
    output << "{\"overall\":\"" << (has_degradation ? "degraded" : "running")
           << "\",\"components\":[";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) output << ',';
        const auto& entry = entries[index];
        output << "{\"name\":\"" << jsonEscape(entry.component)
               << "\",\"state\":\"" << runtimeHealthStateName(entry.state) << '"';
        if (!entry.reason.empty()) {
            output << ",\"reason\":\"" << jsonEscape(entry.reason) << '"';
        }
        output << '}';
    }
    output << "]}";
    return output.str();
}

}  // namespace weaknet_dbus
