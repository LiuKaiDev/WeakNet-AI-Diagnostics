#include "root_cause_engine.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <linux/rtnetlink.h>
#include <mutex>
#include <netinet/in.h>
#include <type_traits>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

const NetnsId kNetns{7, 8};
const NetnsId kOtherNetns{7, 80};
const SocketId kSocket{kNetns, KernelSocketCookie{9}, SocketGeneration{1}};
std::atomic<std::uint64_t> next_event_id{100};

NetnsId netnsOf(const IncidentScope& scope) {
    return std::visit([](const auto& value) -> NetnsId {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SocketId>) return value.netns;
        if constexpr (std::is_same_v<T, NetnsId>) return value;
        return kNetns;
    }, scope);
}

IncidentObservation incident(IncidentId id, IncidentType type, IncidentScope scope,
                             MonotonicTime time) {
    IncidentObservation value;
    value.id = id;
    value.type = type;
    value.scope = std::move(scope);
    value.state = IncidentState::Active;
    value.opened_at = RealtimeTime(time.time_since_epoch());
    value.opened_monotonic_at = time;
    value.last_updated_at = value.opened_at;
    value.last_updated_monotonic_at = time;
    value.validity = Validity::Valid;
    value.condition = "test evidence";
    IncidentEvidence evidence;
    evidence.source_kind = type == IncidentType::HighTcpRtt
        ? EventKind::TcpInfoObservation : EventKind::TcpIntervalMetric;
    evidence.source = EventSource::SocketTracker;
    evidence.scope = value.scope;
    evidence.observed_at = value.opened_at;
    evidence.monotonic_at = time;
    evidence.validity = Validity::Valid;
    evidence.condition = value.condition;
    if (type == IncidentType::HighTcpRtt) {
        evidence.value = IncidentEvidenceValue{std::uint64_t{250'000}};
        evidence.unit = IncidentEvidenceUnit::Microseconds;
    } else if (type == IncidentType::ElevatedTcpRetransmission) {
        evidence.value = IncidentEvidenceValue{0.4};
        evidence.unit = IncidentEvidenceUnit::Ratio;
    }
    value.evidence.push_back(std::move(evidence));
    return value;
}

NetworkEvent incidentEvent(const IncidentObservation& value) {
    const auto netns = netnsOf(value.scope);
    return NetworkEvent(NetworkEventHeader{
        kNetworkEventSchemaVersion, EventId{next_event_id++}, {},
        EventKind::IncidentObservation, EventSource::IncidentEngine,
        value.last_updated_at, value.last_updated_monotonic_at, netns,
        std::nullopt, std::holds_alternative<SocketId>(value.scope)
            ? std::optional<SocketId>(std::get<SocketId>(value.scope)) : std::nullopt,
        value.validity, std::nullopt}, value);
}

std::array<std::uint8_t, 16> address(const char* literal) {
    std::array<std::uint8_t, 16> result{};
    inet_pton(AF_INET, literal, result.data());
    return result;
}

NetworkEvent routeEvent(SocketId socket, const char* gateway, std::uint64_t generation,
                        MonotonicTime at) {
    SocketRouteContextObservation context;
    context.socket_id = socket;
    context.netns = socket.netns;
    context.destination = SocketEndpoint{AF_INET, address("198.51.100.10"), 443};
    MatchedRouteSummary route;
    route.family = AF_INET;
    route.table = RT_TABLE_MAIN;
    route.gateway = address(gateway);
    context.route = route;
    context.selected_interface = InterfaceId{3, "eth-test"};
    context.possible_interfaces.push_back(*context.selected_interface);
    context.uplink_relationship = SelectedUplinkRelationship::MatchesSelectedUplink;
    context.attribution_status = RouteAttributionStatus::Available;
    context.topology_authoritative = true;
    context.topology_generation = generation;
    return NetworkEvent(NetworkEventHeader{kNetworkEventSchemaVersion,
        EventId{next_event_id++}, {}, EventKind::SocketRouteObservation,
        EventSource::SocketTracker, RealtimeTime(at.time_since_epoch()), at,
        socket.netns, context.selected_interface, socket, Validity::Valid, std::nullopt},
        context);
}

NetworkEvent uplinkEvent(NetnsId netns, MonotonicTime at) {
    UplinkObservation uplink{"eth-test", 1, true, "test selected uplink"};
    return NetworkEvent(NetworkEventHeader{kNetworkEventSchemaVersion,
        EventId{next_event_id++}, {}, EventKind::UplinkObservation,
        EventSource::NetlinkCollector, RealtimeTime(at.time_since_epoch()), at,
        netns, InterfaceId{3, "eth-test"}, std::nullopt, Validity::Valid, std::nullopt},
        uplink);
}

NetworkEvent probeEvent(NetnsId netns, ProbeTargetKind kind, ProbeStatus status,
                        std::optional<std::uint64_t> rtt, const char* target,
                        std::uint64_t generation, MonotonicTime at) {
    ProbeObservation probe;
    probe.target.kind = kind;
    probe.target.netns = netns;
    probe.target.family = AF_INET;
    probe.target.address = address(target);
    probe.target.generation = generation;
    if (kind == ProbeTargetKind::Gateway)
        probe.target.interface = InterfaceId{3, "eth-test"};
    probe.netns = netns;
    probe.observed_at = RealtimeTime(at.time_since_epoch());
    probe.monotonic_at = at;
    probe.sequence = next_event_id.load();
    probe.status = status;
    probe.rtt_us = status == ProbeStatus::Success ? rtt : std::nullopt;
    probe.validity = status == ProbeStatus::Success ? Validity::Valid
        : status == ProbeStatus::Timeout ? Validity::Partial : Validity::Unavailable;
    probe.transport = "test";
    return NetworkEvent(NetworkEventHeader{kNetworkEventSchemaVersion,
        EventId{next_event_id++}, {}, EventKind::ProbeObservation,
        EventSource::ActiveProbe, probe.observed_at, at, netns,
        probe.target.interface, std::nullopt, probe.validity, std::nullopt}, probe);
}

const RootCauseHypothesisObservation* findHypothesis(
    const std::vector<RootCauseHypothesisObservation>& values, RootCauseType type) {
    const auto found = std::find_if(values.begin(), values.end(), [type](const auto& value) {
        return value.type == type;
    });
    return found == values.end() ? nullptr : &*found;
}

bool hasEvidence(const RootCauseHypothesisObservation& value,
                 RootCauseEvidenceRole role, RootCauseEvidenceKind kind) {
    const auto& evidence = role == RootCauseEvidenceRole::Supporting
        ? value.supporting_evidence
        : role == RootCauseEvidenceRole::Contradicting
            ? value.contradicting_evidence : value.missing_evidence;
    return std::any_of(evidence.begin(), evidence.end(), [kind](const auto& item) {
        return item.kind == kind;
    });
}
}

int main() {
    bool ok = true;
    ok &= expect(std::string(rootCauseEvidenceKindName(
                         RootCauseEvidenceKind::GatewayProbeReachable)) ==
                     "GatewayProbeReachable" &&
                 std::string(rootCauseEvidenceKindName(
                         RootCauseEvidenceKind::RemoteProbeTimeout)) ==
                     "RemoteProbeTimeout",
                 "probe evidence kind names are not serialization-safe");

    // Authoritative incidents retain authority and lifecycle/recurrence rules.
    {
        ManualClock clock;
        EventBus bus;
        RootCauseEngine engine(bus, clock);
        auto uplink = incident(IncidentId{1}, IncidentType::UplinkUnavailable,
                               IncidentScope{kNetns}, clock.monotonicNow());
        engine.process(incidentEvent(uplink));
        auto active = engine.listActiveHypotheses();
        ok &= expect(active.size() == 1 &&
                     active.front().type == RootCauseType::UplinkAvailabilityProblem &&
                     active.front().confidence == HypothesisConfidence::High,
                     "authoritative uplink incident did not retain high confidence");
        const auto first_id = active.front().id;
        uplink.state = IncidentState::Resolved;
        uplink.resolved_at = uplink.last_updated_at;
        engine.process(incidentEvent(uplink));
        ok &= expect(engine.listActiveHypotheses().empty(),
                     "incident resolution did not resolve dependent hypothesis");
        uplink.id = IncidentId{2};
        uplink.state = IncidentState::Active;
        uplink.resolved_at.reset();
        engine.process(incidentEvent(uplink));
        active = engine.listActiveHypotheses();
        ok &= expect(active.size() == 1 && active.front().id != first_id,
                     "recurrence reused a resolved hypothesis ID");
    }

    // Missing probes remain missing; target-matched fresh probes refine path location.
    {
        ManualClock clock;
        EventBus bus;
        RootCauseEngine engine(bus, clock);
        engine.process(routeEvent(kSocket, "192.0.2.1", 1, clock.monotonicNow()));
        engine.process(uplinkEvent(kNetns, clock.monotonicNow()));
        auto rtt = incident(IncidentId{10}, IncidentType::HighTcpRtt,
                            IncidentScope{kSocket}, clock.monotonicNow());
        auto retrans = incident(IncidentId{11}, IncidentType::ElevatedTcpRetransmission,
                                IncidentScope{kSocket}, clock.monotonicNow());
        engine.process(incidentEvent(rtt));
        auto active = engine.listActiveHypotheses();
        auto* path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && path->confidence == HypothesisConfidence::Low &&
                     hasEvidence(*path, RootCauseEvidenceRole::Missing,
                                 RootCauseEvidenceKind::GatewayProbeUnavailable) &&
                     findHypothesis(active, RootCauseType::InsufficientEvidence),
                     "missing probes were not represented as missing evidence");
        engine.process(incidentEvent(retrans));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && path->confidence == HypothesisConfidence::Medium,
                     "independent TCP incidents did not produce medium confidence");

        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Gateway, ProbeStatus::Success,
                                  2'000, "192.0.2.1", 1, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && hasEvidence(*path, RootCauseEvidenceRole::Supporting,
                                        RootCauseEvidenceKind::GatewayProbeReachable),
                     "fresh gateway success was not cached as reachability evidence");
        ok &= expect(!findHypothesis(active, RootCauseType::RemoteOrUpstreamDegradation),
                     "gateway success alone created an upstream hypothesis");

        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Remote, ProbeStatus::Timeout,
                                  std::nullopt, "198.51.100.1", 0, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        auto* remote = findHypothesis(active, RootCauseType::RemoteOrUpstreamDegradation);
        ok &= expect(remote && remote->confidence == HypothesisConfidence::Medium &&
                     hasEvidence(*remote, RootCauseEvidenceRole::Supporting,
                                 RootCauseEvidenceKind::RemoteProbeTimeout) &&
                     !findHypothesis(active, RootCauseType::InsufficientEvidence),
                     "gateway healthy plus remote timeout did not refine upstream location");

        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Remote, ProbeStatus::Success,
                                  5'000, "203.0.113.1", 0, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(!findHypothesis(active, RootCauseType::RemoteOrUpstreamDegradation) &&
                     path && hasEvidence(*path, RootCauseEvidenceRole::Contradicting,
                                         RootCauseEvidenceKind::RemoteProbeReachable),
                     "normal remote success did not weaken the upstream interpretation");

        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Remote, ProbeStatus::Success,
                                  200'000, "198.51.100.1", 0, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        remote = findHypothesis(active, RootCauseType::RemoteOrUpstreamDegradation);
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(remote && path && path->confidence == HypothesisConfidence::High &&
                     hasEvidence(*remote, RootCauseEvidenceRole::Supporting,
                                 RootCauseEvidenceKind::RemoteProbeHighRtt),
                     "remote high RTT did not provide independent path evidence");

        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Remote,
                                  ProbeStatus::TransportUnavailable, std::nullopt,
                                  "198.51.100.1", 0, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path &&
                     hasEvidence(*path, RootCauseEvidenceRole::Missing,
                                 RootCauseEvidenceKind::RemoteProbeUnavailable) &&
                     !hasEvidence(*path, RootCauseEvidenceRole::Supporting,
                                  RootCauseEvidenceKind::RemoteProbeTimeout),
                     "TransportUnavailable was interpreted as path failure");
        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Remote, ProbeStatus::NoTarget,
                                  std::nullopt, "0.0.0.0", 0, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && hasEvidence(*path, RootCauseEvidenceRole::Missing,
                                        RootCauseEvidenceKind::RemoteProbeUnavailable),
                     "NoTarget was not treated as missing context");
        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Remote,
                                  ProbeStatus::InvalidReply, std::nullopt,
                                  "203.0.113.1", 0, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path &&
                     hasEvidence(*path, RootCauseEvidenceRole::Missing,
                                 RootCauseEvidenceKind::RemoteProbeUnavailable) &&
                     !hasEvidence(*path, RootCauseEvidenceRole::Supporting,
                                  RootCauseEvidenceKind::RemoteProbeTimeout),
                     "InvalidReply was interpreted as path failure");

        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Gateway, ProbeStatus::Success,
                                  80'000, "192.0.2.1", 1, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && hasEvidence(*path, RootCauseEvidenceRole::Supporting,
                                        RootCauseEvidenceKind::GatewayProbeHighRtt) &&
                     !findHypothesis(active, RootCauseType::LocalLinkSuspected),
                     "gateway high RTT was over-interpreted");
        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Gateway, ProbeStatus::Timeout,
                                  std::nullopt, "192.0.2.1", 1, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && hasEvidence(*path, RootCauseEvidenceRole::Supporting,
                                        RootCauseEvidenceKind::GatewayProbeTimeout) &&
                     !findHypothesis(active, RootCauseType::LocalLinkSuspected),
                     "gateway timeout created a strong local-link claim");

        clock.advance(std::chrono::seconds(1));
        engine.process(probeEvent(kOtherNetns, ProbeTargetKind::Gateway,
                                  ProbeStatus::Success, 1'000, "192.0.2.1", 1,
                                  clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && hasEvidence(*path, RootCauseEvidenceRole::Supporting,
                                        RootCauseEvidenceKind::GatewayProbeTimeout),
                     "probe evidence crossed namespace identity");

        clock.advance(std::chrono::seconds(1));
        engine.process(routeEvent(kSocket, "192.0.2.2", 2, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && hasEvidence(*path, RootCauseEvidenceRole::Missing,
                                        RootCauseEvidenceKind::GatewayProbeUnavailable) &&
                     !hasEvidence(*path, RootCauseEvidenceRole::Supporting,
                                  RootCauseEvidenceKind::GatewayProbeTimeout),
                     "old gateway evidence affected a new gateway target");

        clock.advance(std::chrono::seconds(16));
        engine.process(uplinkEvent(kNetns, clock.monotonicNow()));
        active = engine.listActiveHypotheses();
        path = findHypothesis(active, RootCauseType::NetworkPathDegradation);
        ok &= expect(path && hasEvidence(*path, RootCauseEvidenceRole::Missing,
                                        RootCauseEvidenceKind::ProbeEvidenceStale),
                     "stale probe evidence was used as current proof");
    }

    // Probe observations alone do not create incidents or broad hypotheses.
    {
        ManualClock clock;
        EventBus bus;
        RootCauseEngine engine(bus, clock);
        engine.process(probeEvent(kNetns, ProbeTargetKind::Gateway, ProbeStatus::Timeout,
                                  std::nullopt, "192.0.2.1", 1, clock.monotonicNow()));
        ok &= expect(engine.listActiveHypotheses().empty(),
                     "gateway timeout alone created a root-cause hypothesis");
    }

    // EventBus delivery recomputes immediately, while timestamp/sequence-only
    // updates do not publish duplicate semantic hypothesis changes.
    {
        ManualClock clock;
        EventBus bus(128);
        RootCauseEngine engine(bus, clock);
        engine.process(routeEvent(kSocket, "192.0.2.1", 1, clock.monotonicNow()));
        engine.process(uplinkEvent(kNetns, clock.monotonicNow()));
        engine.process(incidentEvent(incident(IncidentId{30}, IncidentType::HighTcpRtt,
                                              IncidentScope{kSocket}, clock.monotonicNow())));
        engine.process(probeEvent(kNetns, ProbeTargetKind::Gateway, ProbeStatus::Success,
                                  2'000, "192.0.2.1", 1, clock.monotonicNow()));
        std::mutex mutex;
        std::condition_variable cv;
        unsigned remote_emissions = 0;
        auto subscription = bus.subscribe(
            EventFilter{EventKind::RootCauseHypothesisObservation,
                        EventSource::RootCauseEngine, kNetns},
            [&](const NetworkEvent& event) {
                const auto& hypothesis =
                    std::get<RootCauseHypothesisObservation>(event.payload());
                if (hypothesis.type == RootCauseType::RemoteOrUpstreamDegradation &&
                    hypothesis.state == HypothesisState::Active) {
                    std::lock_guard lock(mutex);
                    ++remote_emissions;
                    cv.notify_all();
                }
            });
        engine.start();
        bus.start();
        clock.advance(std::chrono::seconds(1));
        bus.publish(probeEvent(kNetns, ProbeTargetKind::Remote, ProbeStatus::Timeout,
                               std::nullopt, "198.51.100.1", 0, clock.monotonicNow()));
        clock.advance(std::chrono::seconds(1));
        bus.publish(probeEvent(kNetns, ProbeTargetKind::Remote, ProbeStatus::Timeout,
                               std::nullopt, "198.51.100.1", 0, clock.monotonicNow()));
        {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, std::chrono::seconds(2), [&] { return remote_emissions >= 1; });
        }
        bus.stop();
        engine.stop();
        ok &= expect(findHypothesis(engine.listActiveHypotheses(),
                                    RootCauseType::RemoteOrUpstreamDegradation),
                     "ProbeObservation did not trigger EventBus recomputation");
        ok &= expect(remote_emissions == 1,
                     "semantic no-op probe update emitted a duplicate hypothesis");
        subscription.unsubscribe();
    }

    // Self-output remains ignored.
    {
        ManualClock clock;
        EventBus bus;
        RootCauseEngine engine(bus, clock);
        RootCauseHypothesisObservation hypothesis;
        hypothesis.scope = kNetns;
        NetworkEvent self(NetworkEventHeader{kNetworkEventSchemaVersion,
            EventId{next_event_id++}, {}, EventKind::RootCauseHypothesisObservation,
            EventSource::RootCauseEngine, {}, {}, kNetns, std::nullopt, std::nullopt,
            Validity::Valid, std::nullopt}, hypothesis);
        ok &= expect(!engine.process(self), "engine consumed its own output");
    }

    return ok ? 0 : 1;
}
