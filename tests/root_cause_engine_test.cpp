#include "root_cause_engine.hpp"

#include <algorithm>
#include <iostream>
#include <netinet/in.h>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

const NetnsId kNetns{7, 8};
const SocketId kSocket{kNetns, KernelSocketCookie{9}, SocketGeneration{1}};

IncidentObservation incident(IncidentId id, IncidentType type, IncidentScope scope,
                             MonotonicTime time) {
    IncidentObservation value;
    value.id = id;
    value.type = type;
    value.scope = std::move(scope);
    value.state = IncidentState::Active;
    value.opened_at = RealtimeTime(time.time_since_epoch());
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
        evidence.value = IncidentEvidenceValue{std::uint64_t{250000}};
        evidence.unit = IncidentEvidenceUnit::Microseconds;
    } else if (type == IncidentType::ElevatedTcpRetransmission) {
        evidence.value = IncidentEvidenceValue{0.4};
        evidence.unit = IncidentEvidenceUnit::Ratio;
    }
    value.evidence.push_back(std::move(evidence));
    return value;
}

NetworkEvent event(const IncidentObservation& value) {
    return NetworkEvent(NetworkEventHeader{
        kNetworkEventSchemaVersion, EventId{value.id.value + 100}, {},
        EventKind::IncidentObservation, EventSource::IncidentEngine,
        value.last_updated_at, value.last_updated_monotonic_at, kNetns,
        std::nullopt, std::holds_alternative<SocketId>(value.scope)
            ? std::optional<SocketId>(std::get<SocketId>(value.scope)) : std::nullopt,
        value.validity, std::nullopt}, value);
}
}

int main() {
    bool ok = true;
    ManualClock clock;
    EventBus bus;
    RootCausePolicy policy;
    policy.max_recent_resolved = 2;
    RootCauseEngine engine(bus, clock, policy);

    auto uplink = incident(IncidentId{1}, IncidentType::UplinkUnavailable,
                           IncidentScope{kNetns}, MonotonicTime{});
    ok &= expect(engine.process(event(uplink)), "uplink incident was not consumed");
    auto active = engine.listActiveHypotheses();
    ok &= expect(active.size() == 1 && active.front().type == RootCauseType::UplinkAvailabilityProblem,
                 "uplink root cause did not open");
    ok &= expect(active.front().confidence == HypothesisConfidence::High,
                 "authoritative uplink evidence was not high confidence");
    const auto first_id = active.front().id;

    auto resolved = uplink;
    resolved.state = IncidentState::Resolved;
    resolved.resolved_at = resolved.last_updated_at;
    ok &= expect(engine.process(event(resolved)), "resolved uplink incident was not consumed");
    ok &= expect(engine.listActiveHypotheses().empty() && engine.getHypothesis(first_id),
                 "uplink root cause did not resolve into history");

    auto rtt = incident(IncidentId{2}, IncidentType::HighTcpRtt,
                        IncidentScope{kSocket}, MonotonicTime{std::chrono::seconds(1)});
    auto retrans = incident(IncidentId{3}, IncidentType::ElevatedTcpRetransmission,
                            IncidentScope{kSocket}, MonotonicTime{std::chrono::seconds(2)});
    engine.process(event(rtt));
    active = engine.listActiveHypotheses();
    const auto pathIt = std::find_if(active.begin(), active.end(), [](const auto& value) {
        return value.type == RootCauseType::NetworkPathDegradation;
    });
    ok &= expect(pathIt != active.end() && pathIt->confidence == HypothesisConfidence::Low &&
                 std::any_of(active.begin(), active.end(), [](const auto& value) {
                     return value.type == RootCauseType::InsufficientEvidence;
                 }),
                 "single TCP incident did not produce low-confidence path degradation");
    engine.process(event(retrans));
    active = engine.listActiveHypotheses();
    const auto pathIt2 = std::find_if(active.begin(), active.end(), [](const auto& value) {
        return value.type == RootCauseType::NetworkPathDegradation;
    });
    ok &= expect(pathIt2 != active.end() && pathIt2->confidence == HypothesisConfidence::Medium &&
                 pathIt2->supporting_evidence.size() == 2,
                 "independent TCP incidents did not upgrade the same socket hypothesis");

    auto other = incident(IncidentId{4}, IncidentType::ElevatedTcpRetransmission,
                          IncidentScope{SocketId{kNetns, KernelSocketCookie{10}, SocketGeneration{1}}},
                          MonotonicTime{std::chrono::seconds(3)});
    engine.process(event(other));
    active = engine.listActiveHypotheses();
    ok &= expect(std::count_if(active.begin(), active.end(), [](const auto& value) {
        return value.type == RootCauseType::NetworkPathDegradation;
    }) == 2, "different socket incidents were combined");

    NetworkEvent self(NetworkEventHeader{
        kNetworkEventSchemaVersion, EventId{999}, {},
        EventKind::RootCauseHypothesisObservation, EventSource::RootCauseEngine,
        {}, {}, kNetns, std::nullopt, std::nullopt, Validity::Valid, std::nullopt},
        active.front());
    ok &= expect(!engine.process(self), "engine consumed its own output");
    return ok ? 0 : 1;
}
