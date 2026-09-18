#pragma once

#include <filesystem>
#include <string>

namespace weaknet_dbus {

struct RuntimeConfig {
    std::filesystem::path state_dir;
    std::filesystem::path log_dir;
    std::filesystem::path runtime_dir;
    std::filesystem::path bpf_object_override;

    static RuntimeConfig fromEnvironment();
    bool validate(std::string* error) const;

    std::filesystem::path signalFile() const;
    std::filesystem::path getReplyFile() const;
};

}  // namespace weaknet_dbus
