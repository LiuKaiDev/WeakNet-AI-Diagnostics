#include "incident_engine.hpp"

#include <iostream>
#include <algorithm>
#include <atomic>
#include <utility>
#include <netinet/in.h>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool value, const char* message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

const NetnsId kNetns{10, 20};
const SocketId kSocket{kNetns, KernelSocketCookie{42}, SocketGeneration{1}};

NetworkEvent makeEvent(EventKind kind, EventSource source, Validity validity,
                       NetworkEventPayload payload, MonotonicTime monotonic,
                       std::optional<SocketId> socket = std::nullopt) {
    static std::uint64_t id = 1;
    return NetworkEvent(NetworkEventHeader{kNetworkEventSchemaVersion, EventId{id++}, {},
        kind, source, RealtimeTime(monotonic.time_since_epoch()), monotonic,
        kNetns, std::nullopt, socket, validity, std::nullopt}, std::move(payload));
}

TcpInfoObservation tcpInfo(SocketId id, std::uint32_t rtt, MonotonicTime at,
                            Validity validity = Validity::Valid) {
    TcpInfoRaw raw;
    raw.rtt_us = rtt;
    SocketTuple tuple;
    tuple.netns = id.netns; tuple.family = AF_INET;
    tuple.local = SocketEndpoint{AF_INET, {192, 0, 2, 10}, 40000};
    tuple.remote = SocketEndpoint{AF_INET, {198, 51, 100, 10}, 443};
    return TcpInfoObservation{id, id.netns, tuple, TcpSocketState{1}, raw,
                              RealtimeTime(at.time_since_epoch()), at, validity, false};
}

SocketObservation socketObservation(SocketId id, MonotonicTime at,
                                    bool present = true) {
    SocketTuple tuple;
    tuple.netns = id.netns; tuple.family = AF_INET;
    tuple.local = SocketEndpoint{AF_INET, {192, 0, 2, 10}, 40000};
    tuple.remote = SocketEndpoint{AF_INET, {198, 51, 100, 10}, 443};
    return SocketObservation{id, id.netns, tuple,
        RealtimeTime(at.time_since_epoch()), at, EventSource::SocketTracker,
        present ? Validity::Valid : Validity::Stale,
        present ? SocketLifecycleState::Active : SocketLifecycleState::Closed,
        present, TcpSocketState{1}, std::uint32_t{3}, std::nullopt, false};
}

TcpIntervalMetrics retrans(SocketId id, double ratio, std::uint64_t segments,
                           MonotonicTime at, Validity validity = Validity::Valid) {
    TcpIntervalMetrics metric;
    metric.id = id; metric.interval_start = at - std::chrono::seconds(1);
    metric.interval_end = at; metric.elapsed_seconds = 1.0;
    metric.delta_data_segs_out = segments;
    metric.delta_total_retrans = static_cast<std::uint64_t>(ratio * segments);
    metric.retransmission_segment_ratio = ratio;
    metric.validity = validity;
    return metric;
}

SocketRouteContextObservation routeContext(SocketId id, RouteAttributionStatus status,
                                           RouteAttributionReason reasons) {
    SocketRouteContextObservation context;
    context.socket_id = id; context.netns = id.netns;
    context.destination = SocketEndpoint{AF_INET, {198, 51, 100, 10}, 443};
    context.attribution_status = status; context.reasons = reasons;
    context.topology_authoritative = true;
    return context;
}
}

int main() {
    bool ok = true;
    ManualClock clock;
    EventBus bus(128);
    IncidentPolicy policy;
    policy.activation_consecutive = 2;
    policy.recovery_consecutive = 2;
    policy.minimum_data_segments = 5;
    IncidentEngine engine(bus, clock, policy);

    const auto socket_event = makeEvent(EventKind::SocketObservation,
        EventSource::SocketTracker, Validity::Valid,
        socketObservation(kSocket, MonotonicTime{}), MonotonicTime{}, kSocket);
    engine.process(socket_event);

    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 250'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    ok &= expect(engine.listActiveIncidents().empty(), "one RTT spike opened an incident");
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 260'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    auto active = engine.listActiveIncidents();
    ok &= expect(active.size() == 1 && active.front().type == IncidentType::HighTcpRtt,
                 "persistent high RTT did not open an incident");
    ok &= expect(!active.front().evidence.empty() && active.front().evidence.front().value &&
                 std::get<std::uint64_t>(active.front().evidence.front().value->value) == 260'000,
                 "high RTT evidence did not retain the actual value");
    const auto first_id = active.front().id;

    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 175'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    ok &= expect(engine.listActiveIncidents().size() == 1,
                 "RTT hysteresis-band sample flapped an active incident");
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Unavailable, tcpInfo(kSocket, 100'000, clock.monotonicNow(), Validity::Unavailable),
        clock.monotonicNow(), kSocket));
    ok &= expect(engine.listActiveIncidents().size() == 1,
                 "missing RTT evidence resolved an active incident");
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 100'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    ok &= expect(engine.listActiveIncidents().size() == 1, "one good RTT sample resolved incident");
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 100'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    ok &= expect(engine.listActiveIncidents().empty() && engine.getIncident(first_id) &&
                 engine.getIncident(first_id)->state == IncidentState::Resolved,
                 "persistent RTT recovery did not resolve incident");

    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 250'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 250'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    const auto recurrence = engine.listActiveIncidents();
    ok &= expect(recurrence.size() == 1 && recurrence.front().id != first_id,
                 "reopened incident reused its historical IncidentId");

    // A new high RTT occurrence receives a new ID; generation changes do not
    // transfer the old occurrence.
    const SocketId new_generation{kNetns, KernelSocketCookie{42}, SocketGeneration{2}};
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::SocketObservation, EventSource::SocketTracker,
        Validity::Valid, socketObservation(new_generation, clock.monotonicNow()), clock.monotonicNow(), new_generation));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(new_generation, 250'000, clock.monotonicNow()), clock.monotonicNow(), new_generation));
    ok &= expect(engine.listActiveIncidents().empty(), "socket generation transferred RTT streak");

    // Retransmission requires a valid ratio and configured minimum volume.
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpIntervalMetric, EventSource::SocketTracker,
        Validity::Valid, retrans(new_generation, 0.5, 1, clock.monotonicNow()), clock.monotonicNow(), new_generation));
    ok &= expect(engine.listActiveIncidents().empty(), "low-volume retransmission opened incident");
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpIntervalMetric, EventSource::SocketTracker,
        Validity::Valid, retrans(new_generation, 0.5, 10, clock.monotonicNow()), clock.monotonicNow(), new_generation));
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::TcpIntervalMetric, EventSource::SocketTracker,
        Validity::Valid, retrans(new_generation, 0.5, 10, clock.monotonicNow()), clock.monotonicNow(), new_generation));
    ok &= expect(engine.listActiveIncidents().size() == 1 &&
                 engine.listActiveIncidents().front().type == IncidentType::ElevatedTcpRetransmission,
                 "persistent retransmission ratio did not open incident");

    // Explicit no-route is actionable; non-authoritative and multipath partial
    // attribution are only insufficient evidence.
    const auto no_route = makeEvent(EventKind::SocketRouteObservation,
        EventSource::SocketTracker, Validity::Unavailable,
        routeContext(new_generation, RouteAttributionStatus::Unavailable,
                     RouteAttributionReason::NoModeledRoute), clock.monotonicNow(), new_generation);
    engine.process(no_route); engine.process(no_route);
    ok &= expect(engine.listActiveIncidents().size() == 2,
                 "explicit no-modeled-route did not open its incident");
    const auto partial_route = makeEvent(EventKind::SocketRouteObservation,
        EventSource::SocketTracker, Validity::Partial,
        routeContext(new_generation, RouteAttributionStatus::Partial,
                     RouteAttributionReason::MultipathWithoutFlowHash), clock.monotonicNow(), new_generation);
    engine.process(partial_route);
    ok &= expect(engine.listActiveIncidents().size() == 2,
                 "multipath partial attribution opened route incident");

    const auto conflict_context = makeEvent(EventKind::SocketRouteObservation,
        EventSource::SocketTracker, Validity::Partial,
        routeContext(new_generation, RouteAttributionStatus::Partial,
                     RouteAttributionReason::DiagIfindexConflicts), clock.monotonicNow(), new_generation);
    engine.process(conflict_context); engine.process(conflict_context);
    const auto conflict_active = engine.listActiveIncidents();
    ok &= expect(std::any_of(conflict_active.begin(), conflict_active.end(),
                 [](const auto& incident) { return incident.type == IncidentType::SocketRouteConflict; }),
                 "persistent route evidence conflict did not open incident");

    // Uplink absence is explicit only when the authoritative netlink policy
    // emits unavailable; stale/non-authoritative evidence is ignored.
    const auto uplink_bad = makeEvent(EventKind::UplinkObservation,
        EventSource::NetlinkCollector, Validity::Unavailable,
        UplinkObservation{"", 0, false, "no usable unicast default route"}, clock.monotonicNow());
    engine.process(uplink_bad); engine.process(uplink_bad);
    const auto active_after_uplink = engine.listActiveIncidents();
    ok &= expect(std::any_of(active_after_uplink.begin(), active_after_uplink.end(),
                 [](const auto& incident) { return incident.type == IncidentType::UplinkUnavailable; }),
                 "authoritative no-uplink state did not open incident");

    const auto close = makeEvent(EventKind::SocketObservation, EventSource::SocketTracker,
        Validity::Stale, socketObservation(new_generation, clock.monotonicNow(), false),
        clock.monotonicNow(), new_generation);
    engine.process(close);
    const auto after_close = engine.listActiveIncidents();
    ok &= expect(std::none_of(after_close.begin(), after_close.end(),
                 [](const auto& incident) {
                     return std::holds_alternative<SocketId>(incident.scope);
                 }), "socket close did not terminate socket-scoped incidents");
    ok &= expect(engine.recentResolvedIncidents().size() <= policy.max_recent_resolved,
                 "resolved history exceeded configured bound");

    if (!engine.recentResolvedIncidents().empty()) {
        const auto resolved = engine.recentResolvedIncidents().front();
        NetworkEvent round_trip(
            NetworkEventHeader{kNetworkEventSchemaVersion, EventId{900}, {},
                EventKind::IncidentObservation, EventSource::IncidentEngine,
                resolved.last_updated_at, resolved.last_updated_monotonic_at,
                kNetns, std::nullopt, std::nullopt, resolved.validity, std::nullopt},
            resolved);
        ok &= expect(std::get<IncidentObservation>(round_trip.payload()).id == resolved.id,
                     "IncidentObservation copy/round-trip failed");
    }

    std::atomic<unsigned> published{0};
    auto subscription = bus.subscribe(
        EventFilter{EventKind::IncidentObservation, EventSource::IncidentEngine, kNetns},
        [&](const NetworkEvent&) { published.fetch_add(1); });
    ok &= expect(bus.start() && engine.start(), "IncidentEngine/EventBus did not start");
    // Replaying a fresh occurrence exercises typed EventBus publication and
    // avoids asserting on asynchronous delivery before the dispatcher runs.
    clock.advance(std::chrono::seconds(1));
    engine.process(makeEvent(EventKind::SocketObservation, EventSource::SocketTracker,
        Validity::Valid, socketObservation(kSocket, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 250'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    engine.process(makeEvent(EventKind::TcpInfoObservation, EventSource::SocketTracker,
        Validity::Valid, tcpInfo(kSocket, 250'000, clock.monotonicNow()), clock.monotonicNow(), kSocket));
    engine.stop();
    bus.stop();
    ok &= expect(published.load() == 1, "unchanged incident samples emitted duplicate events");
    subscription.unsubscribe();
    return ok ? 0 : 1;
}
