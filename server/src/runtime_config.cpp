#include "runtime_config.hpp"

#include <cstdlib>
#include <system_error>
#include <unistd.h>

namespace weaknet_dbus {
namespace {

std::filesystem::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] != '\0' ? std::filesystem::path(value)
                                     : std::filesystem::path{};
}

std::filesystem::path defaultStateDirectory() {
    if (auto path = environmentPath("XDG_STATE_HOME"); !path.empty()) {
        return path / "weaknet";
    }
    if (auto path = environmentPath("HOME"); !path.empty()) {
        return path / ".local" / "state" / "weaknet";
    }
    return std::filesystem::path("/tmp") /
           ("weaknet-" + std::to_string(static_cast<unsigned long>(::getuid()))) /
           "state";
}

std::filesystem::path defaultRuntimeDirectory() {
    if (auto path = environmentPath("XDG_RUNTIME_DIR"); !path.empty()) {
        return path / "weaknet";
    }
    return std::filesystem::path("/tmp") /
           ("weaknet-" + std::to_string(static_cast<unsigned long>(::getuid()))) /
           "run";
}

bool prepareDirectory(const std::filesystem::path& path, std::string* error) {
    if (path.empty() || !path.is_absolute()) {
        if (error) *error = "runtime directory must be absolute: " + path.string();
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec || !std::filesystem::is_directory(path, ec)) {
        if (error) *error = "cannot create runtime directory '" + path.string() +
                            "': " + ec.message();
        return false;
    }
    return true;
}

}  // namespace

RuntimeConfig RuntimeConfig::fromEnvironment() {
    RuntimeConfig config;
    config.state_dir = environmentPath("WEAKNET_STATE_DIR");
    if (config.state_dir.empty()) config.state_dir = defaultStateDirectory();

    config.log_dir = environmentPath("WEAKNET_LOG_DIR");
    if (config.log_dir.empty()) config.log_dir = config.state_dir / "log";

    config.runtime_dir = environmentPath("WEAKNET_RUNTIME_DIR");
    if (config.runtime_dir.empty()) config.runtime_dir = defaultRuntimeDirectory();

    config.bpf_object_override = environmentPath("WEAKNET_BPF_OBJECT");
    return config;
}

bool RuntimeConfig::validate(std::string* error) const {
    if (!prepareDirectory(state_dir, error) ||
        !prepareDirectory(log_dir, error) ||
        !prepareDirectory(runtime_dir, error)) {
        return false;
    }
    if (!bpf_object_override.empty() && !bpf_object_override.is_absolute()) {
        if (error) *error = "WEAKNET_BPF_OBJECT must be absolute";
        return false;
    }
    return true;
}

std::filesystem::path RuntimeConfig::signalFile() const {
    return state_dir / "signal_changed.bin";
}

std::filesystem::path RuntimeConfig::getReplyFile() const {
    return state_dir / "get_reply.bin";
}

}  // namespace weaknet_dbus
