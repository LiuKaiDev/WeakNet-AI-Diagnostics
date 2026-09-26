#include "netlink_collector.hpp"
#include "network_event.hpp"

#if defined(__linux__)
#include <sched.h>
#include <cerrno>
#include <cstring>
#include <iostream>
#endif

using namespace weaknet_dbus::v2;

int main() {
#if !defined(__linux__)
    std::cout << "SKIP: Linux network namespaces are required\n";
    return 77;
#else
    if (::unshare(CLONE_NEWNET) != 0) {
        std::cout << "SKIP: disposable network namespace prerequisite unavailable: unshare(CLONE_NEWNET): "
                  << std::strerror(errno) << "\n";
        return 77;
    }
    std::string error;
    const auto netns = currentNetworkNamespace(&error);
    if (!netns) {
        std::cerr << "network namespace identity failed: " << error << '\n';
        return 1;
    }
    ManualClock clock;
    EventBus bus(32);
    if (!bus.start()) return 1;
    NetlinkCollector collector(bus, clock, *netns, std::chrono::milliseconds(100));
    const bool started = collector.start();
    if (started) collector.stop();
    bus.stop();
    if (!started) {
        std::cerr << "collector failed in disposable namespace\n";
        return 1;
    }
    return collector.snapshot().authoritative ? 0 : 1;
#endif
}
