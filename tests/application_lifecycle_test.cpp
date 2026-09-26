#include "application.hpp"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::size_t countEntries(const std::filesystem::path& path) {
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        (void)entry;
        ++count;
    }
    return count;
}

weaknet_dbus::RuntimeConfig makeConfig(const std::filesystem::path& root,
                                       const std::string& suffix) {
    weaknet_dbus::RuntimeConfig config;
    config.state_dir = root / suffix / "state";
    config.log_dir = root / suffix / "log";
    config.runtime_dir = root / suffix / "run";
    return config;
}

bool hasHealth(const weaknet_dbus::RuntimeHealth& health, const std::string& component,
               weaknet_dbus::RuntimeHealthState state, const std::string& reason) {
    for (const auto& entry : health.snapshot()) {
        if (entry.component == component) {
            return entry.state == state && entry.reason == reason;
        }
    }
    return false;
}

}  // namespace

int main() {
    bool ok = true;
    char temporary_template[] = "/tmp/weaknet-lifecycle-test-XXXXXX";
    char* temporary = ::mkdtemp(temporary_template);
    if (!temporary) return 1;
    const std::filesystem::path root(temporary);

    const char* required_steps[] = {"logger", "signals", "manager", "dbus", "events", "workers"};
    for (const char* step : required_steps) {
        weaknet_dbus::ApplicationTestHooks hooks;
        hooks.fail_required_step = step;
        weaknet_dbus::DaemonApplication application(makeConfig(root, step), hooks);
        ok &= expect(!application.start(), std::string("injected startup step did not fail: ") + step);
        application.stop();
        application.stop();
    }

    {
        weaknet_dbus::ApplicationTestHooks hooks;
        hooks.fail_optional_component = "rssi";
        weaknet_dbus::DaemonApplication application(makeConfig(root, "degraded"), hooks);
        ok &= expect(application.start(), "optional collector failure killed startup");
        ok &= expect(application.eventBus().running(), "Phase 3 EventBus did not start with application");
        ok &= expect(application.health().degraded(), "optional collector failure was not degraded");
        const auto started = std::chrono::steady_clock::now();
        application.stop();
        ok &= expect(!application.eventBus().running(), "Phase 3 EventBus survived application stop");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        ok &= expect(elapsed < 5s, "degraded application stop exceeded five seconds");
        application.stop();
    }

    {
        weaknet_dbus::ApplicationTestHooks hooks;
        hooks.netlink_collector.open_socket = [] {
            return ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        };
        hooks.netlink_collector.reconcile =
            [](std::stop_token) -> std::optional<weaknet_dbus::v2::TopologySnapshot> {
                return std::nullopt;
            };
        hooks.netlink_collector.recovery_retry_initial = 10s;
        hooks.netlink_collector.recovery_retry_max = 10s;
        weaknet_dbus::DaemonApplication application(
            makeConfig(root, "netlink-reconcile-degraded"), hooks);
        ok &= expect(application.start(),
                     "initial netlink reconciliation failure killed application startup");
        ok &= expect(hasHealth(application.health(), "netlink_topology",
                               weaknet_dbus::RuntimeHealthState::Degraded,
                               "initial_reconciliation_failed"),
                     "initial reconciliation failure was not reported as degraded health");
        application.stop();
    }

    {
        weaknet_dbus::ApplicationTestHooks hooks;
        hooks.netlink_collector.open_socket = [] { return -1; };
        weaknet_dbus::DaemonApplication application(
            makeConfig(root, "netlink-transport-degraded"), hooks);
        ok &= expect(application.start(),
                     "optional netlink transport failure killed application startup");
        ok &= expect(hasHealth(application.health(), "netlink_topology",
                               weaknet_dbus::RuntimeHealthState::Degraded,
                               "transport_or_worker_start_failed"),
                     "netlink transport failure used an inaccurate health reason");
        application.stop();
    }

    // Warm libdbus/glog process-global state before establishing the leak baseline.
    {
        weaknet_dbus::DaemonApplication warmup(makeConfig(root, "warmup"));
        ok &= expect(warmup.start(), "warmup application failed");
        warmup.stop();
    }
    const auto fd_baseline = countEntries("/proc/self/fd");
    const auto thread_baseline = countEntries("/proc/self/task");
    for (int iteration = 0; iteration < 3; ++iteration) {
        weaknet_dbus::DaemonApplication application(
            makeConfig(root, "repeat-" + std::to_string(iteration)));
        ok &= expect(application.start(), "repeated application start failed");
        const auto started = std::chrono::steady_clock::now();
        application.stop();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        std::cout << "repeat-stop-ms="
                  << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
                  << '\n';
        ok &= expect(elapsed < 5s, "repeated application stop exceeded five seconds");
    }
    ok &= expect(countEntries("/proc/self/task") == thread_baseline,
                 "thread count did not return to baseline");
    ok &= expect(countEntries("/proc/self/fd") == fd_baseline,
                 "FD count did not return to baseline");

    std::filesystem::remove_all(root);
    return ok ? 0 : 1;
}
