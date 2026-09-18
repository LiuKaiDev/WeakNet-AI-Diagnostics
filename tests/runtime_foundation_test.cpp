#include "runtime_config.hpp"
#include "runtime_health.hpp"
#include "scoped_fd.hpp"

#include <cerrno>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

}  // namespace

int main() {
    bool ok = true;
    char temporary_template[] = "/tmp/weaknet-runtime-test-XXXXXX";
    char* temporary = ::mkdtemp(temporary_template);
    if (!temporary) return 1;
    const std::filesystem::path root(temporary);

    weaknet_dbus::RuntimeConfig config;
    config.state_dir = root / "state";
    config.log_dir = root / "log";
    config.runtime_dir = root / "run";
    std::string error;
    ok &= expect(config.validate(&error), "absolute runtime paths should validate");
    ok &= expect(std::filesystem::is_directory(config.state_dir), "state directory missing");
    ok &= expect(config.signalFile().parent_path() == config.state_dir,
                 "signal file must live under state directory");

    auto invalid = config;
    invalid.state_dir = "relative-state";
    ok &= expect(!invalid.validate(&error), "relative runtime path must be rejected");

    weaknet_dbus::RuntimeHealth health;
    health.set("dbus", weaknet_dbus::RuntimeHealthState::Running);
    health.set("ebpf", weaknet_dbus::RuntimeHealthState::Degraded, "permission_denied");
    const std::string json = health.toJson();
    ok &= expect(health.degraded(), "degraded component must affect overall health");
    ok &= expect(json.find("\"overall\":\"degraded\"") != std::string::npos,
                 "health JSON missing degraded overall state");
    ok &= expect(json.find("permission_denied") != std::string::npos,
                 "health JSON missing reason");

    int descriptors[2]{};
    if (::pipe(descriptors) != 0) return 1;
    const int owned = descriptors[0];
    {
        weaknet_dbus::ScopedFd descriptor(owned);
        ok &= expect(descriptor.get() == owned, "ScopedFd lost descriptor");
    }
    errno = 0;
    ok &= expect(::fcntl(owned, F_GETFD) == -1 && errno == EBADF,
                 "ScopedFd did not close descriptor");
    ::close(descriptors[1]);

    std::filesystem::remove_all(root);
    return ok ? 0 : 1;
}
