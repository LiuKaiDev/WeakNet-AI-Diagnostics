#include "network_event.hpp"

#include <iostream>
#include <string>
#include <unordered_set>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
}

int main() {
    bool ok = true;
    const NetnsId ns{11, 22};
    const InterfaceId first{7, "eth-old"};
    const InterfaceId renamed{7, "eth-new"};
    ok &= expect(first == renamed, "interface identity must be ifindex, not name");
    ok &= expect(NetnsId{11, 22} != NetnsId{11, 23}, "netns identity comparison failed");
    ok &= expect(SocketId{4, 1} < SocketId{4, 2}, "socket generation ordering failed");
    std::unordered_set<EventId> ids{EventId{1}, EventId{1}, EventId{2}};
    ok &= expect(ids.size() == 2, "typed ID hashing failed");

    const auto realtime = RealtimeTime(std::chrono::seconds(123));
    const auto monotonic = MonotonicTime(std::chrono::seconds(45));
    for (const auto validity : {Validity::Valid, Validity::Unavailable, Validity::Stale,
                                Validity::Partial, Validity::Reset}) {
        NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{9}, EventSequence{},
            EventKind::ProbeRttMetric, EventSource::V1RttMonitor, realtime, monotonic,
            ns, first, std::nullopt, validity, std::nullopt};
        NetworkEvent event(header, ProbeRttObservation{12.5});
        ok &= expect(event.header().schema_version == 1, "schema version changed");
        ok &= expect(event.header().validity == validity, "validity was not preserved");
        ok &= expect(event.header().observed_at == realtime &&
                     event.header().monotonic_at == monotonic, "timestamps changed");
        auto copied = event;
        auto moved = std::move(copied);
        ok &= expect(std::get<ProbeRttObservation>(moved.payload()).milliseconds == 12.5,
                     "NetworkEvent value semantics failed");
    }

    bool rejected_kind = false;
    try {
        NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{1}, {},
            EventKind::TrafficMetric, EventSource::Runtime, realtime, monotonic,
            ns, std::nullopt, std::nullopt, Validity::Valid, std::nullopt};
        (void)NetworkEvent(header, ProbeRttObservation{1.0});
    } catch (const std::invalid_argument&) { rejected_kind = true; }
    ok &= expect(rejected_kind, "payload/header mismatch was accepted");

    bool rejected_schema = false;
    try {
        NetworkEventHeader header{99, EventId{1}, {}, EventKind::ProbeRttMetric,
            EventSource::Runtime, realtime, monotonic, ns, std::nullopt,
            std::nullopt, Validity::Valid, std::nullopt};
        (void)NetworkEvent(header, ProbeRttObservation{1.0});
    } catch (const std::invalid_argument&) { rejected_schema = true; }
    ok &= expect(rejected_schema, "unsupported schema was accepted");

    std::string error;
    const auto current = currentNetworkNamespace(&error);
    ok &= expect(current && current->inode != 0, "kernel network namespace identity unavailable: " + error);
    return ok ? 0 : 1;
}
