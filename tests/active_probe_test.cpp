#include "active_probe.hpp"

#include <arpa/inet.h>
#include <linux/rtnetlink.h>

#include <atomic>
#include <iostream>
#include <vector>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

TopologySnapshot topology(NetnsId netns, std::uint32_t ifindex,
                          const char* gateway, bool authoritative = true) {
    TopologySnapshot snapshot;
    snapshot.netns = netns;
    snapshot.authoritative = authoritative;
    snapshot.generation = 1;
    RouteFact route;
    route.netns = netns;
    route.family = AF_INET;
    route.destination_prefix = 0;
    route.table = RT_TABLE_MAIN;
    route.output_ifindex = ifindex;
    if (gateway) inet_pton(AF_INET, gateway, route.gateway.emplace().data());
    snapshot.routes.push_back(route);
    return snapshot;
}

UplinkSelection uplink(NetnsId netns, std::uint32_t ifindex) {
    UplinkSelection selection;
    selection.interface = InterfaceId{ifindex, "eth-test"};
    selection.validity = Validity::Valid;
    selection.method_flags = 1;
    (void)netns;
    return selection;
}
}

int main() {
    bool ok = true;
    const NetnsId netns{3, 4};
    ManualClock clock;

    {
        EventBus bus;
        ActiveProbe probe(bus, clock, netns, ProbeConfig{}, {});
        probe.updateTopology(topology(netns, 3, "192.0.2.1"), uplink(netns, 3));
        const auto target = probe.gatewayTarget();
        ok &= expect(target && target->kind == ProbeTargetKind::Gateway,
                     "authoritative gateway target was not selected");
        ok &= expect(target && target->generation == 1,
                     "gateway target generation was not retained");
        auto changed = topology(netns, 3, "192.0.2.2");
        changed.generation = 2;
        probe.updateTopology(changed, uplink(netns, 3));
        ok &= expect(probe.gatewayTarget() && probe.gatewayTarget()->generation == 2 &&
                         probe.gatewayTarget()->address[3] == 2,
                     "gateway target identity did not change with topology");
        probe.updateTopology(topology(netns, 3, "192.0.2.2", false), uplink(netns, 3));
        ok &= expect(probe.gatewayTarget() &&
                         probe.gatewayTarget()->provenance.find("stale") != std::string::npos,
                     "last-known-good gateway was not marked stale");
        probe.updateTopology(topology(netns, 3, nullptr), uplink(netns, 3));
        ok &= expect(!probe.gatewayTarget(), "on-link route invented a gateway target");
        probe.updateTopology(topology(netns, 3, "192.0.2.1", false), uplink(netns, 3));
        ok &= expect(!probe.gatewayTarget(), "non-authoritative topology selected gateway");
    }

    {
        EventBus bus;
        std::atomic<unsigned> events{0};
        auto subscription = bus.subscribe(
            EventFilter{EventKind::ProbeObservation, EventSource::ActiveProbe, netns},
            [&](const NetworkEvent& event) {
                const auto& observation = std::get<ProbeObservation>(event.payload());
                ++events;
                if (observation.target.kind == ProbeTargetKind::Gateway)
                    ok &= expect(observation.status == ProbeStatus::Success && observation.rtt_us &&
                                     *observation.rtt_us == 1234,
                                 "successful gateway probe observation was malformed");
                if (observation.target.kind == ProbeTargetKind::Remote)
                    ok &= expect(observation.status == ProbeStatus::Timeout && !observation.rtt_us,
                                 "timeout retained a fake RTT");
            });
        ok &= expect(bus.start(), "probe EventBus did not start");
        ActiveProbeTestHooks hooks;
        hooks.probe = [](const ProbeTarget& target, std::uint64_t,
                         std::chrono::milliseconds, std::stop_token) {
            return target.kind == ProbeTargetKind::Gateway
                ? ProbeAttemptResult{ProbeStatus::Success, 1234, {}}
                : ProbeAttemptResult{ProbeStatus::Timeout, std::nullopt, "test timeout"};
        };
        ActiveProbe probe(bus, clock, netns, ProbeConfig{}, std::move(hooks));
        probe.updateTopology(topology(netns, 3, "192.0.2.1"), uplink(netns, 3));
        ok &= expect(probe.probeOnceForTests(), "probe cycle did not attempt targets");
        bus.stop();
        const auto telemetry = probe.telemetry();
        ok &= expect(events.load() == 2, "probe observations were not published exactly once");
        ok &= expect(telemetry.successes == 1 && telemetry.timeouts == 1,
                     "probe telemetry counters were incorrect");
        subscription.unsubscribe();
    }

    {
        EventBus bus;
        ActiveProbeTestHooks hooks;
        hooks.probe = [](const ProbeTarget&, std::uint64_t, std::chrono::milliseconds,
                         std::stop_token) {
            return ProbeAttemptResult{ProbeStatus::TransportUnavailable, std::nullopt,
                                      "permission denied"};
        };
        ActiveProbe probe(bus, clock, netns, ProbeConfig{}, std::move(hooks));
        probe.updateTopology(topology(netns, 3, "192.0.2.1"), uplink(netns, 3));
        probe.probeOnceForTests();
        ok &= expect(probe.telemetry().unavailable_capability == 2 &&
                         probe.telemetry().degraded,
                     "transport capability failure was not reported separately");
    }

    {
        ProbeConfig config;
        config.remote_ipv4 = "not-an-ip";
        EventBus bus;
        ActiveProbe probe(bus, clock, netns, config);
        ok &= expect(!probe.remoteTarget() && probe.telemetry().degraded,
                     "invalid remote target was not rejected");
    }
    return ok ? 0 : 1;
}
