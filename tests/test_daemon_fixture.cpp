#include "application.hpp"
#include "stop_utils.hpp"

#include <chrono>

using namespace std::chrono_literals;

int main() {
    weaknet_dbus::ApplicationTestHooks hooks;
    hooks.fail_optional_component = "interfaces";
    hooks.seed_test_interface = true;
    hooks.ping_operation = [](std::stop_token token, const std::string&,
                              const std::string&, int) {
        weaknet_dbus::waitForStop(token, 30s);
        return -5;
    };
    weaknet_dbus::DaemonApplication application(
        weaknet_dbus::RuntimeConfig::fromEnvironment(), std::move(hooks));
    if (!application.start()) return 1;
    return application.run();
}
