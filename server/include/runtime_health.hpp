#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace weaknet_dbus {

enum class RuntimeHealthState {
    Starting,
    Running,
    Disabled,
    Degraded,
    Failed,
    Stopped,
};

struct RuntimeHealthEntry {
    std::string component;
    RuntimeHealthState state = RuntimeHealthState::Starting;
    std::string reason;
};

class RuntimeHealth {
public:
    void set(std::string component, RuntimeHealthState state, std::string reason = {});
    std::vector<RuntimeHealthEntry> snapshot() const;
    bool degraded() const;
    std::string toJson() const;

private:
    mutable std::mutex mutex_;
    std::map<std::string, RuntimeHealthEntry> entries_;
};

const char* runtimeHealthStateName(RuntimeHealthState state);

}  // namespace weaknet_dbus
