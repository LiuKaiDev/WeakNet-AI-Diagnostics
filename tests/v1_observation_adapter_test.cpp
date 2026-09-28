#include "v1_observation_adapter.hpp"

#include <net/if.h>

#include <algorithm>
#include <iostream>
#include <mutex>
#include <vector>

using namespace weaknet_dbus;
using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
}

int main() {
    bool ok = true;
    ManualClock clock(RealtimeTime(std::chrono::seconds(100)),
                      MonotonicTime(std::chrono::seconds(10)));
    EventBus bus(64);
    MetricStore store(clock);
    const NetnsId ns{7, 9};
    V1ObservationAdapter adapter(bus, store, clock, ns);
    std::mutex mutex;
    std::vector<NetworkEvent> events;
    bus.start();
    auto token = bus.subscribe({}, [&](const NetworkEvent& event) {
        std::lock_guard lock(mutex);
        events.push_back(event);
    });

    NetInfo loopback("lo");
    loopback.setState(NetState::Up);
    loopback.setUsingNow(true);
    loopback.setRttMs(42);
    const auto before = loopback;
    adapter.mirrorInterfaceSnapshot({loopback});
    adapter.mirrorUplink("lo", 3);
    adapter.mirrorRtt(loopback);
    TcpLossResult loss;
    loss.ratePercent = 2.5;
    loss.sentDelta = 100;
    loss.retransDelta = 2;
    loss.level = "degraded";
    adapter.mirrorTcpLoss("lo", loss);
    NetTrafficAnalyzer::RealTimeStats traffic;
    traffic.totalBps = 1000;
    traffic.totalPps = 20;
    traffic.activeFlows = 3;
    adapter.mirrorTraffic("lo", traffic, true);
    loopback.setType(NetType::WiFi);
    loopback.setRssiDbm(-55);
    adapter.mirrorWifiRssi(loopback);
    adapter.mirrorCollectorHealth("test", CollectorState::Degraded, "fixture");
    bus.stop();

    ok &= expect(before.rttMs() == 42 && before.state() == NetState::Up,
                 "adapter mutated V1 state");
    ok &= expect(events.size() == 7, "representative mirrors did not publish one event each");
    for (const auto& event : events) {
        ok &= expect(event.header().netns == ns, "adapter lost namespace identity");
        ok &= expect(event.header().observed_at == clock.realtimeNow() &&
                     event.header().monotonic_at == clock.monotonicNow(),
                     "adapter did not use injected clock");
    }
    const auto has_kind = [&](EventKind kind) {
        return std::any_of(events.begin(), events.end(),
                           [kind](const NetworkEvent& event) { return event.header().kind == kind; });
    };
    ok &= expect(has_kind(EventKind::InterfaceObservation) &&
                 has_kind(EventKind::UplinkObservation) &&
                 has_kind(EventKind::ProbeRttMetric) &&
                 has_kind(EventKind::TcpLossMetric) &&
                 has_kind(EventKind::TrafficMetric) &&
                 has_kind(EventKind::WifiRssiMetric) &&
                 has_kind(EventKind::CollectorHealth), "adapter event kind missing");

    const InterfaceId lo{static_cast<std::uint32_t>(if_nametoindex("lo")), "ignored"};
    const MetricKey rtt_key{ns, MetricName::Rtt, MetricUnit::Milliseconds,
                            EventSource::V1RttMonitor, lo, std::nullopt, 0};
    const auto rtt = store.latest(rtt_key);
    ok &= expect(rtt && rtt->validity == Validity::Partial && rtt->value &&
                 std::get<double>(*rtt->value) == 42.0,
                 "RTT mirror metric semantics mismatch");
    const MetricKey traffic_key{ns, MetricName::TrafficBytesPerSecond,
        MetricUnit::BytesPerSecond, EventSource::V1TrafficMonitor, lo, std::nullopt, 0};
    const auto traffic_sample = store.latest(traffic_key);
    ok &= expect(traffic_sample && traffic_sample->value &&
                 std::get<std::uint64_t>(*traffic_sample->value) == 1000,
                 "traffic mirror metric mismatch");

    EventBus unavailable_bus(8);
    MetricStore unavailable_store(clock);
    V1ObservationAdapter unavailable_adapter(unavailable_bus, unavailable_store, clock, ns);
    unavailable_bus.start();
    unavailable_adapter.mirrorTraffic("lo", traffic, false);
    unavailable_bus.stop();
    const auto unavailable = unavailable_store.latest(traffic_key);
    ok &= expect(unavailable && unavailable->validity == Validity::Unavailable &&
                 !unavailable->value, "degraded V1 traffic silently became zero");
    return ok ? 0 : 1;
}
