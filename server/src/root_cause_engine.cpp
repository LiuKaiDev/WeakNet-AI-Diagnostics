#include "root_cause_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace weaknet_dbus::v2 {
namespace {

bool usableValidity(Validity validity) noexcept {
    return validity == Validity::Valid || validity == Validity::Partial;
}

std::optional<SocketId> socketFromScope(const RootCauseScope& scope) {
    if (const auto* socket = std::get_if<SocketId>(&scope)) return *socket;
    return std::nullopt;
}

}  // namespace

RootCauseEngine::RootCauseEngine(EventBus& bus, const Clock& clock,
                                 RootCausePolicy policy)
    : bus_(bus), clock_(clock), policy_(policy) {
    if (policy_.max_recent_resolved == 0 || policy_.max_evidence_per_hypothesis == 0 ||
        policy_.max_active_hypotheses == 0) {
        throw std::invalid_argument("RootCausePolicy requires positive bounds");
    }
}

RootCauseEngine::~RootCauseEngine() { stop(); }

bool RootCauseEngine::start() {
    std::lock_guard lock(mutex_);
    if (running_) return true;
    subscription_ = bus_.subscribe({}, [this](const NetworkEvent& event) {
        (void)process(event);
    });
    running_ = true;
    return true;
}

void RootCauseEngine::stop() noexcept {
    EventBus::Subscription subscription;
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
        subscription = std::move(subscription_);
    }
    subscription.unsubscribe();
}

bool RootCauseEngine::running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_;
}

RootCauseScope RootCauseEngine::incidentScope(const IncidentObservation& incident) {
    return std::visit([](const auto& scope) -> RootCauseScope { return scope; }, incident.scope);
}

bool RootCauseEngine::scopeMatches(const IncidentScope& left,
                                   const RootCauseScope& right) {
    return std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, SocketId>) {
            const auto* socket = std::get_if<SocketId>(&right);
            return socket && *socket == value;
        } else if constexpr (std::is_same_v<T, NetnsId>) {
            const auto* netns = std::get_if<NetnsId>(&right);
            return netns && *netns == value;
        } else {
            const auto* interface = std::get_if<InterfaceId>(&right);
            return interface && *interface == value;
        }
    }, left);
}

bool RootCauseEngine::hasIncident(const IncidentMap& incidents, IncidentType type,
                                  const RootCauseScope& scope) {
    return std::any_of(incidents.begin(), incidents.end(), [&](const auto& entry) {
        return entry.second.type == type && scopeMatches(entry.second.scope, scope);
    });
}

RootCauseEvidenceKind RootCauseEngine::evidenceKind(IncidentType type) {
    switch (type) {
        case IncidentType::UplinkUnavailable:
            return RootCauseEvidenceKind::AuthoritativeNoUsableUplink;
        case IncidentType::RouteUnavailable:
            return RootCauseEvidenceKind::AuthoritativeRouteUnavailable;
        case IncidentType::SocketRouteConflict:
            return RootCauseEvidenceKind::SocketRouteConflict;
        case IncidentType::HighTcpRtt:
            return RootCauseEvidenceKind::HighTcpRtt;
        case IncidentType::ElevatedTcpRetransmission:
            return RootCauseEvidenceKind::ElevatedTcpRetransmission;
    }
    return RootCauseEvidenceKind::ActiveIncident;
}

void RootCauseEngine::addEvidence(std::vector<RootCauseEvidence>& destination,
                                  RootCauseEvidence evidence) const {
    if (std::find(destination.begin(), destination.end(), evidence) != destination.end()) return;
    if (destination.size() < policy_.max_evidence_per_hypothesis) {
        destination.push_back(std::move(evidence));
        return;
    }
    if (policy_.max_evidence_per_hypothesis == 1) {
        destination.front() = std::move(evidence);
        return;
    }
    destination.erase(destination.begin() + 1);
    destination.push_back(std::move(evidence));
}

RootCauseEvidence RootCauseEngine::incidentEvidence(
    const IncidentObservation& incident, RootCauseEvidenceRole role,
    RootCauseEvidenceKind kind) const {
    RootCauseEvidence evidence;
    evidence.incident = incident.id;
    evidence.source_kind = EventKind::IncidentObservation;
    evidence.source = EventSource::IncidentEngine;
    evidence.scope = incidentScope(incident);
    evidence.observed_at = incident.last_updated_at;
    evidence.monotonic_at = incident.last_updated_monotonic_at;
    evidence.role = role;
    evidence.kind = kind;
    evidence.validity = incident.validity;
    evidence.provenance = incident.condition;
    if (!incident.evidence.empty()) {
        const auto& source = incident.evidence.back();
        evidence.value = source.value;
        evidence.unit = source.unit == IncidentEvidenceUnit::Microseconds
            ? RootCauseEvidenceUnit::Microseconds
            : source.unit == IncidentEvidenceUnit::Ratio
                ? RootCauseEvidenceUnit::Ratio
                : source.unit == IncidentEvidenceUnit::Count
                    ? RootCauseEvidenceUnit::Count : RootCauseEvidenceUnit::None;
    }
    return evidence;
}

RootCauseEvidence RootCauseEngine::observationEvidence(
    const NetworkEvent& event, RootCauseScope scope, RootCauseEvidenceRole role,
    RootCauseEvidenceKind kind, RootCauseEvidenceCapability capability,
    std::string provenance) const {
    RootCauseEvidence evidence;
    evidence.source_kind = event.header().kind;
    evidence.source = event.header().source;
    evidence.scope = std::move(scope);
    evidence.observed_at = event.header().observed_at;
    evidence.monotonic_at = event.header().monotonic_at;
    evidence.role = role;
    evidence.kind = kind;
    evidence.validity = event.header().validity;
    evidence.capability = capability;
    evidence.provenance = std::move(provenance);
    return evidence;
}

std::map<RootCauseEngine::CandidateKey, RootCauseEngine::Candidate>
RootCauseEngine::buildCandidatesLocked() const {
    std::map<CandidateKey, Candidate> candidates;

    auto make = [&](RootCauseType type, RootCauseScope scope, std::string reason,
                    RealtimeTime timestamp) -> RootCauseHypothesisObservation& {
        CandidateKey key{type, scope};
        auto [iterator, inserted] = candidates.try_emplace(key);
        if (inserted) {
            iterator->second.observation.type = type;
            iterator->second.observation.scope = key.scope;
            iterator->second.observation.opened_at = timestamp;
            iterator->second.observation.last_updated_at = timestamp;
            iterator->second.observation.reason_code = std::move(reason);
        }
        return iterator->second.observation;
    };
    auto syntheticRouteEvent = [](SocketId socket, Validity validity) {
        SocketRouteContextObservation context;
        context.socket_id = socket;
        context.netns = socket.netns;
        return NetworkEvent(NetworkEventHeader{
                kNetworkEventSchemaVersion, EventId{1}, {}, EventKind::SocketRouteObservation,
                EventSource::SocketTracker, {}, {}, socket.netns, std::nullopt, socket,
                validity, std::nullopt}, context);
    };

    for (const auto& [id, incident] : active_incidents_) {
        (void)id;
        const auto scope = incidentScope(incident);
        auto add = [&](RootCauseType type, std::string reason, HypothesisConfidence confidence) {
            auto& hypothesis = make(type, scope, std::move(reason), incident.opened_at);
            hypothesis.confidence = std::max(hypothesis.confidence, confidence);
            addEvidence(hypothesis.supporting_evidence,
                        incidentEvidence(incident, RootCauseEvidenceRole::Supporting,
                                         evidenceKind(incident.type)));
            hypothesis.last_updated_at = std::max(hypothesis.last_updated_at,
                                                  incident.last_updated_at);
        };
        switch (incident.type) {
            case IncidentType::UplinkUnavailable:
                add(RootCauseType::UplinkAvailabilityProblem,
                    "active_uplink_unavailable", HypothesisConfidence::High);
                break;
            case IncidentType::RouteUnavailable:
                add(RootCauseType::LocalRoutingProblem,
                    "authoritative_no_modeled_route", HypothesisConfidence::High);
                break;
            case IncidentType::SocketRouteConflict:
                add(RootCauseType::LocalRoutingProblem,
                    "socket_route_evidence_conflict", HypothesisConfidence::Medium);
                break;
            case IncidentType::HighTcpRtt:
            case IncidentType::ElevatedTcpRetransmission: {
                auto& hypothesis = make(RootCauseType::NetworkPathDegradation, scope,
                                         "observed_tcp_path_degradation", incident.opened_at);
                hypothesis.confidence = incident.type == IncidentType::HighTcpRtt
                    ? std::max(hypothesis.confidence, HypothesisConfidence::Low)
                    : std::max(hypothesis.confidence, HypothesisConfidence::Low);
                addEvidence(hypothesis.supporting_evidence,
                            incidentEvidence(incident, RootCauseEvidenceRole::Supporting,
                                             evidenceKind(incident.type)));
                hypothesis.last_updated_at = std::max(hypothesis.last_updated_at,
                                                      incident.last_updated_at);
                break;
            }
        }
    }

    // A second semantically independent TCP incident upgrades the path rule.
    for (auto& [key, candidate] : candidates) {
        if (key.type != RootCauseType::NetworkPathDegradation) continue;
        const bool rtt = std::any_of(candidate.observation.supporting_evidence.begin(),
                                     candidate.observation.supporting_evidence.end(),
                                     [](const auto& evidence) {
                                         return evidence.kind == RootCauseEvidenceKind::HighTcpRtt;
                                     });
        const bool retrans = std::any_of(candidate.observation.supporting_evidence.begin(),
                                          candidate.observation.supporting_evidence.end(),
                                          [](const auto& evidence) {
                                              return evidence.kind == RootCauseEvidenceKind::ElevatedTcpRetransmission;
                                          });
        if (rtt && retrans) candidate.observation.confidence = HypothesisConfidence::Medium;
    }

    // Route and uplink facts are only positive when an actual authoritative
    // observation says so.  Unknown topology is represented as missing.
    std::vector<Candidate> insufficient;
    for (auto& [key, candidate] : candidates) {
        if (key.type == RootCauseType::LocalRoutingProblem) {
            const auto socket = std::get<SocketId>(key.scope);
            addEvidence(candidate.observation.missing_evidence,
                        observationEvidence(syntheticRouteEvent(socket, Validity::Unavailable), key.scope,
                            RootCauseEvidenceRole::Missing,
                            RootCauseEvidenceKind::PolicyRoutingStateUnavailable,
                            RootCauseEvidenceCapability::NotImplemented,
                            "policy routing rules are not modeled"));
            const auto route = route_events_.find(socket);
            if (route != route_events_.end()) {
                const auto& context = std::get<SocketRouteContextObservation>(route->second.payload());
                if (context.attribution_status == RouteAttributionStatus::Available &&
                    context.topology_authoritative && usableValidity(route->second.header().validity)) {
                    addEvidence(candidate.observation.contradicting_evidence,
                                observationEvidence(route->second, key.scope,
                                    RootCauseEvidenceRole::Contradicting,
                                    RootCauseEvidenceKind::ModeledRouteAvailable,
                                    RootCauseEvidenceCapability::Available,
                                    "authoritative modeled route is available"));
                    if (candidate.observation.confidence == HypothesisConfidence::High)
                        candidate.observation.confidence = HypothesisConfidence::Medium;
                    else if (candidate.observation.confidence == HypothesisConfidence::Medium)
                        candidate.observation.confidence = HypothesisConfidence::Low;
                }
            }
        }
        if (key.type != RootCauseType::NetworkPathDegradation) continue;
        const auto socket = socketFromScope(key.scope);
        if (!socket) continue;
        const auto route = route_events_.find(*socket);
        const auto uplink = uplink_events_.find(socket->netns);
        const auto* route_context = route == route_events_.end()
            ? nullptr : &std::get<SocketRouteContextObservation>(route->second.payload());
        const auto* uplink_observation = uplink == uplink_events_.end()
            ? nullptr : &std::get<UplinkObservation>(uplink->second.payload());
        const bool route_available = route_context &&
            route_context->attribution_status == RouteAttributionStatus::Available &&
            route_context->topology_authoritative &&
            usableValidity(route->second.header().validity);
        const bool uplink_available = uplink_observation && uplink_observation->selected &&
            uplink_observation->method_flags != 0 &&
            usableValidity(uplink->second.header().validity);
        if (route_available) {
            addEvidence(candidate.observation.supporting_evidence,
                        observationEvidence(route->second, key.scope,
                            RootCauseEvidenceRole::Supporting,
                            RootCauseEvidenceKind::ModeledRouteAvailable,
                            RootCauseEvidenceCapability::Available,
                            "authoritative modeled route available"));
        } else {
            addEvidence(candidate.observation.missing_evidence,
                        observationEvidence(syntheticRouteEvent(*socket, Validity::Unavailable), key.scope,
                            RootCauseEvidenceRole::Missing,
                            RootCauseEvidenceKind::TopologyUnavailable,
                            RootCauseEvidenceCapability::Unavailable,
                            "authoritative route context unavailable"));
            Candidate insufficient_candidate;
            auto& insufficient_observation = insufficient_candidate.observation;
            insufficient_observation.type = RootCauseType::InsufficientEvidence;
            insufficient_observation.scope = key.scope;
            insufficient_observation.opened_at = candidate.observation.opened_at;
            insufficient_observation.last_updated_at = candidate.observation.last_updated_at;
            insufficient_observation.confidence = HypothesisConfidence::Low;
            insufficient_observation.reason_code = "tcp_path_context_unavailable";
            for (const auto& evidence : candidate.observation.supporting_evidence)
                addEvidence(insufficient_observation.supporting_evidence, evidence);
            for (const auto& evidence : candidate.observation.missing_evidence)
                if (evidence.kind == RootCauseEvidenceKind::TopologyUnavailable)
                    addEvidence(insufficient_observation.missing_evidence, evidence);
            insufficient.push_back(std::move(insufficient_candidate));
        }
        if (uplink_available) {
            addEvidence(candidate.observation.supporting_evidence,
                        observationEvidence(uplink->second, key.scope,
                            RootCauseEvidenceRole::Supporting,
                            RootCauseEvidenceKind::UsableUplinkAvailable,
                            RootCauseEvidenceCapability::Available,
                            "authoritative selected uplink available"));
        } else {
            addEvidence(candidate.observation.missing_evidence,
                        observationEvidence(NetworkEvent(
                            NetworkEventHeader{kNetworkEventSchemaVersion, EventId{1}, {},
                                EventKind::UplinkObservation, EventSource::NetlinkCollector,
                                {}, {}, socket->netns, std::nullopt, std::nullopt,
                                Validity::Unavailable, std::nullopt},
                            UplinkObservation{}), key.scope, RootCauseEvidenceRole::Missing,
                            RootCauseEvidenceKind::TopologyUnavailable,
                            RootCauseEvidenceCapability::Unavailable,
                            "authoritative uplink state unavailable"));
        }
        addEvidence(candidate.observation.missing_evidence,
                    observationEvidence(NetworkEvent(
                        NetworkEventHeader{kNetworkEventSchemaVersion, EventId{1}, {},
                            EventKind::CollectorHealth, EventSource::Runtime, {}, {},
                            socket->netns, std::nullopt, *socket, Validity::Unavailable,
                            std::nullopt}, CollectorHealthObservation{"gateway_probe", CollectorState::Disabled}),
                        key.scope, RootCauseEvidenceRole::Missing,
                        RootCauseEvidenceKind::GatewayProbeUnavailable,
                        RootCauseEvidenceCapability::NotImplemented,
                        "gateway probe is not implemented in this stage"));
        addEvidence(candidate.observation.missing_evidence,
                    observationEvidence(NetworkEvent(
                        NetworkEventHeader{kNetworkEventSchemaVersion, EventId{1}, {},
                            EventKind::CollectorHealth, EventSource::Runtime, {}, {},
                            socket->netns, std::nullopt, *socket, Validity::Unavailable,
                            std::nullopt}, CollectorHealthObservation{"remote_probe", CollectorState::Disabled}),
                        key.scope, RootCauseEvidenceRole::Missing,
                        RootCauseEvidenceKind::RemoteProbeUnavailable,
                        RootCauseEvidenceCapability::NotImplemented,
                        "remote active probe is not implemented in this stage"));
    }
    for (auto& candidate : insufficient) {
        candidates.emplace(CandidateKey{candidate.observation.type, candidate.observation.scope},
                           std::move(candidate));
    }

    for (auto& [key, candidate] : candidates) {
        if (key.type != RootCauseType::UplinkAvailabilityProblem) continue;
        const auto uplink = uplink_events_.find(std::get<NetnsId>(key.scope));
        if (uplink == uplink_events_.end()) continue;
        const auto& observation = std::get<UplinkObservation>(uplink->second.payload());
        if (observation.selected && usableValidity(uplink->second.header().validity)) {
            addEvidence(candidate.observation.contradicting_evidence,
                        observationEvidence(uplink->second, key.scope,
                            RootCauseEvidenceRole::Contradicting,
                            RootCauseEvidenceKind::UsableUplinkAvailable,
                            RootCauseEvidenceCapability::Available,
                            "authoritative selected uplink restored"));
            if (candidate.observation.confidence == HypothesisConfidence::High)
                candidate.observation.confidence = HypothesisConfidence::Medium;
            else if (candidate.observation.confidence == HypothesisConfidence::Medium)
                candidate.observation.confidence = HypothesisConfidence::Low;
        }
    }

    // Remote/upstream is intentionally conservative: it is an alternative
    // only when both local modeled route and uplink facts are positive.
    std::vector<Candidate> remote;
    for (const auto& [key, candidate] : candidates) {
        if (key.type != RootCauseType::NetworkPathDegradation) continue;
        const auto socket = socketFromScope(key.scope);
        if (!socket) continue;
        const auto route = route_events_.find(*socket);
        const auto uplink = uplink_events_.find(socket->netns);
        if (route == route_events_.end() || uplink == uplink_events_.end()) continue;
        const auto& route_context = std::get<SocketRouteContextObservation>(route->second.payload());
        const auto& uplink_observation = std::get<UplinkObservation>(uplink->second.payload());
        if (route_context.attribution_status != RouteAttributionStatus::Available ||
            !route_context.topology_authoritative ||
            !usableValidity(route->second.header().validity) ||
            !uplink_observation.selected ||
            !usableValidity(uplink->second.header().validity)) continue;
        CandidateKey remote_key{RootCauseType::RemoteOrUpstreamDegradation, key.scope};
        Candidate remote_candidate;
        auto& hypothesis = remote_candidate.observation;
        hypothesis.type = remote_key.type;
        hypothesis.scope = remote_key.scope;
        hypothesis.opened_at = candidate.observation.opened_at;
        hypothesis.last_updated_at = candidate.observation.last_updated_at;
        hypothesis.confidence = HypothesisConfidence::Low;
        hypothesis.reason_code = "local_route_and_uplink_healthy_path_degraded";
        for (const auto& evidence : candidate.observation.supporting_evidence)
            addEvidence(hypothesis.supporting_evidence, evidence);
        addEvidence(hypothesis.missing_evidence, observationEvidence(
            NetworkEvent(NetworkEventHeader{kNetworkEventSchemaVersion, EventId{1}, {},
                EventKind::CollectorHealth, EventSource::Runtime, {}, {}, socket->netns,
                std::nullopt, *socket, Validity::Unavailable, std::nullopt},
                CollectorHealthObservation{"gateway_probe", CollectorState::Disabled}),
            key.scope, RootCauseEvidenceRole::Missing,
            RootCauseEvidenceKind::GatewayLatencyUnavailable,
            RootCauseEvidenceCapability::NotImplemented,
            "gateway latency probe is unavailable"));
        addEvidence(hypothesis.missing_evidence, observationEvidence(
            NetworkEvent(NetworkEventHeader{kNetworkEventSchemaVersion, EventId{1}, {},
                EventKind::CollectorHealth, EventSource::Runtime, {}, {}, socket->netns,
                std::nullopt, *socket, Validity::Unavailable, std::nullopt},
                CollectorHealthObservation{"remote_probe", CollectorState::Disabled}),
            key.scope, RootCauseEvidenceRole::Missing,
            RootCauseEvidenceKind::ActiveRemoteProbeUnavailable,
            RootCauseEvidenceCapability::NotImplemented,
            "active remote probe is unavailable"));
        remote.push_back(std::move(remote_candidate));
    }
    for (auto& candidate : remote) {
        CandidateKey key{candidate.observation.type, candidate.observation.scope};
        candidates.emplace(std::move(key), std::move(candidate));
    }
    return candidates;
}

bool RootCauseEngine::sameContent(const RootCauseHypothesisObservation& left,
                                  const RootCauseHypothesisObservation& right) {
    return left.type == right.type && left.scope == right.scope &&
           left.confidence == right.confidence &&
           left.supporting_evidence == right.supporting_evidence &&
           left.contradicting_evidence == right.contradicting_evidence &&
           left.missing_evidence == right.missing_evidence &&
           left.reason_code == right.reason_code;
}

void RootCauseEngine::reconcileLocked(
    RealtimeTime now, std::vector<RootCauseHypothesisObservation>& emissions) {
    auto desired = buildCandidatesLocked();
    for (auto& [key, candidate] : desired) {
        if (active_.size() >= policy_.max_active_hypotheses && !active_.contains(key)) continue;
        auto observation = candidate.observation;
        auto current = active_.find(key);
        if (current == active_.end()) {
            if (next_occurrence_ == 0) throw std::overflow_error("root-cause occurrence exhausted");
            observation.id = RootCauseHypothesisId{key.type, key.scope, next_occurrence_++};
            observation.state = HypothesisState::Active;
            active_.emplace(key, observation);
            emissions.push_back(std::move(observation));
        } else {
            observation.id = current->second.id;
            observation.opened_at = current->second.opened_at;
            observation.state = HypothesisState::Active;
            observation.last_updated_at = std::max(observation.last_updated_at,
                                                   current->second.last_updated_at);
            if (!sameContent(current->second, observation)) {
                observation.last_updated_at = now;
                current->second = observation;
                emissions.push_back(observation);
            }
        }
    }
    for (auto iterator = active_.begin(); iterator != active_.end();) {
        if (desired.contains(iterator->first)) {
            ++iterator;
            continue;
        }
        auto resolved = iterator->second;
        resolved.state = HypothesisState::Resolved;
        resolved.resolved_at = now;
        resolved.last_updated_at = now;
        resolved_.push_back(resolved);
        while (resolved_.size() > policy_.max_recent_resolved) resolved_.pop_front();
        emissions.push_back(std::move(resolved));
        iterator = active_.erase(iterator);
    }
}

bool RootCauseEngine::process(const NetworkEvent& event) {
    if (event.header().source == EventSource::RootCauseEngine ||
        event.header().kind == EventKind::RootCauseHypothesisObservation) return false;
    std::vector<RootCauseHypothesisObservation> emissions;
    bool consumed = false;
    {
        std::lock_guard lock(mutex_);
        switch (event.header().kind) {
            case EventKind::IncidentObservation: {
                if (event.header().source != EventSource::IncidentEngine) break;
                const auto& incident = std::get<IncidentObservation>(event.payload());
                if (incident.state == IncidentState::Active)
                    active_incidents_[incident.id] = incident;
                else if (incident.state == IncidentState::Resolved)
                    active_incidents_.erase(incident.id);
                else break;
                while (active_incidents_.size() > policy_.max_active_hypotheses * 2)
                    active_incidents_.erase(active_incidents_.begin());
                consumed = true;
                reconcileLocked(event.header().observed_at, emissions);
                break;
            }
            case EventKind::SocketRouteObservation: {
                if (event.header().source != EventSource::SocketTracker) break;
                const auto& context = std::get<SocketRouteContextObservation>(event.payload());
                route_events_.insert_or_assign(context.socket_id, event);
                while (route_events_.size() > policy_.max_active_hypotheses * 2)
                    route_events_.erase(route_events_.begin());
                consumed = true;
                reconcileLocked(event.header().observed_at, emissions);
                break;
            }
            case EventKind::UplinkObservation:
                if (event.header().source != EventSource::NetlinkCollector) break;
                uplink_events_.insert_or_assign(event.header().netns, event);
                while (uplink_events_.size() > policy_.max_active_hypotheses)
                    uplink_events_.erase(uplink_events_.begin());
                consumed = true;
                reconcileLocked(event.header().observed_at, emissions);
                break;
            default:
                break;
        }
    }
    for (const auto& hypothesis : emissions) publish(hypothesis);
    return consumed;
}

void RootCauseEngine::publish(const RootCauseHypothesisObservation& hypothesis) {
    NetnsId netns{};
    std::optional<SocketId> socket;
    std::optional<InterfaceId> interface;
    std::visit([&](const auto& scope) {
        using T = std::decay_t<decltype(scope)>;
        if constexpr (std::is_same_v<T, SocketId>) { netns = scope.netns; socket = scope; }
        else if constexpr (std::is_same_v<T, NetnsId>) netns = scope;
        else interface = scope;
    }, hypothesis.scope);
    if (netns.inode == 0 && !hypothesis.supporting_evidence.empty()) {
        std::visit([&](const auto& scope) {
            using T = std::decay_t<decltype(scope)>;
            if constexpr (std::is_same_v<T, SocketId>) netns = scope.netns;
            else if constexpr (std::is_same_v<T, NetnsId>) netns = scope;
        }, hypothesis.supporting_evidence.front().scope);
    }
    if (netns.inode == 0) return;
    NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{next_event_id_++}, {},
        EventKind::RootCauseHypothesisObservation, EventSource::RootCauseEngine,
        hypothesis.last_updated_at, clock_.monotonicNow(), netns, interface, socket,
        Validity::Valid, std::nullopt};
    (void)bus_.publish(NetworkEvent(std::move(header), hypothesis));
}

std::vector<RootCauseHypothesisObservation> RootCauseEngine::listActiveHypotheses() const {
    std::vector<RootCauseHypothesisObservation> result;
    std::lock_guard lock(mutex_);
    for (const auto& [key, observation] : active_) {
        (void)key;
        result.push_back(observation);
    }
    return result;
}

std::optional<RootCauseHypothesisObservation> RootCauseEngine::getHypothesis(
    RootCauseHypothesisId id) const {
    std::lock_guard lock(mutex_);
    for (const auto& [key, observation] : active_)
        if (observation.id == id) return observation;
    for (const auto& observation : resolved_)
        if (observation.id == id) return observation;
    return std::nullopt;
}

std::vector<RootCauseHypothesisObservation> RootCauseEngine::recentResolvedHypotheses() const {
    std::lock_guard lock(mutex_);
    return {resolved_.begin(), resolved_.end()};
}

}  // namespace weaknet_dbus::v2
