#include "incident_engine.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace weaknet_dbus::v2 {
namespace {

}  // namespace

IncidentEngine::IncidentEngine(EventBus& bus, const Clock& clock, IncidentPolicy policy)
    : bus_(bus), policy_(policy) {
    (void)clock;
    if (policy_.high_rtt_recovery_us >= policy_.high_rtt_activation_us ||
        policy_.retransmission_recovery_ratio >= policy_.retransmission_activation_ratio ||
        policy_.retransmission_recovery_ratio < 0.0 ||
        policy_.activation_consecutive == 0 || policy_.recovery_consecutive == 0 ||
        policy_.max_recent_resolved == 0 || policy_.max_evidence_per_incident == 0 ||
        policy_.max_tracked_records == 0) {
        throw std::invalid_argument("IncidentPolicy requires positive bounds and hysteresis");
    }
}

IncidentEngine::~IncidentEngine() { stop(); }

bool IncidentEngine::start() {
    std::lock_guard lock(mutex_);
    if (running_) return true;
    subscription_ = bus_.subscribe({}, [this](const NetworkEvent& event) {
        (void)process(event);
    });
    running_ = true;
    return true;
}

void IncidentEngine::stop() noexcept {
    EventBus::Subscription subscription;
    {
        std::lock_guard lock(mutex_);
        if (!running_) return;
        running_ = false;
        subscription = std::move(subscription_);
    }
    subscription.unsubscribe();
}

bool IncidentEngine::running() const noexcept {
    std::lock_guard lock(mutex_);
    return running_;
}

bool IncidentEngine::hasReason(RouteAttributionReason reasons,
                               RouteAttributionReason wanted) noexcept {
    return (static_cast<std::uint64_t>(reasons) &
            static_cast<std::uint64_t>(wanted)) != 0;
}

bool IncidentEngine::evidenceEquivalent(const IncidentEvidence& left,
                                        const IncidentEvidence& right) {
    return left.source_kind == right.source_kind && left.source == right.source &&
           left.scope == right.scope && left.value == right.value &&
           left.unit == right.unit && left.validity == right.validity &&
           left.condition == right.condition;
}

IncidentEvidence IncidentEngine::makeEvidence(
    const NetworkEvent& event, IncidentScope scope,
    std::optional<IncidentEvidenceValue> value, IncidentEvidenceUnit unit,
    Validity validity, std::string condition) const {
    return IncidentEvidence{event.header().kind, event.header().source, std::move(scope),
                            event.header().observed_at, event.header().monotonic_at,
                            std::move(value), unit, validity, std::move(condition)};
}

void IncidentEngine::appendEvidenceLocked(IncidentObservation& incident,
                                          const IncidentEvidence& evidence) {
    if (!incident.evidence.empty() && evidenceEquivalent(incident.evidence.back(), evidence)) return;
    if (incident.evidence.size() < policy_.max_evidence_per_incident) {
        incident.evidence.push_back(evidence);
        return;
    }
    if (policy_.max_evidence_per_incident == 1) {
        incident.evidence.front() = evidence;
        return;
    }
    // Preserve the opening evidence and evict the oldest intermediate sample.
    incident.evidence.erase(incident.evidence.begin() + 1);
    incident.evidence.push_back(evidence);
}

void IncidentEngine::resolveRecordLocked(
    std::map<IncidentKey, Record>::iterator iterator,
    RealtimeTime realtime, MonotonicTime monotonic, const std::string& condition,
    std::vector<IncidentObservation>& emissions) {
    auto resolved = iterator->second.observation;
    resolved.state = IncidentState::Resolved;
    resolved.last_updated_at = realtime;
    resolved.last_updated_monotonic_at = monotonic;
    resolved.resolved_at = realtime;
    resolved.resolved_monotonic_at = monotonic;
    resolved.resolution_condition = condition;
    emissions.push_back(resolved);
    resolved_.push_back(std::move(resolved));
    while (resolved_.size() > policy_.max_recent_resolved) resolved_.pop_front();
    records_.erase(iterator);
}

void IncidentEngine::resolveSocketLocked(
    const SocketId& id, RealtimeTime realtime, MonotonicTime monotonic,
    const std::string& condition, std::vector<IncidentObservation>& emissions) {
    for (auto iterator = records_.begin(); iterator != records_.end();) {
        const auto* socket = std::get_if<SocketId>(&iterator->first.scope);
        if (!socket || *socket != id) { ++iterator; continue; }
        if (!iterator->second.active) {
            iterator = records_.erase(iterator);
            continue;
        }
        auto current = iterator++;
        resolveRecordLocked(current, realtime, monotonic, condition, emissions);
    }
}

void IncidentEngine::resolveSupersededSocketGenerationsLocked(
    const SocketId& current, RealtimeTime realtime, MonotonicTime monotonic,
    std::vector<IncidentObservation>& emissions) {
    if (!current.cookie) return;
    std::vector<SocketId> superseded;
    for (const auto& [id, authoritative] : authoritative_sockets_) {
        if (authoritative && id != current && id.netns == current.netns &&
            id.cookie == current.cookie) superseded.push_back(id);
    }
    for (const auto& id : superseded) {
        authoritative_sockets_.erase(id);
        resolveSocketLocked(id, realtime, monotonic, "socket generation superseded",
                            emissions);
    }
}

void IncidentEngine::evaluateLocked(
    const IncidentKey& key, NetnsId netns, Evaluation evaluation,
    const IncidentEvidence& evidence, IncidentSeverity severity,
    IncidentEvidenceCompleteness completeness,
    std::vector<IncidentObservation>& emissions) {
    auto iterator = records_.find(key);
    if (evaluation == Evaluation::Unavailable) return;
    if (evaluation == Evaluation::Neutral) {
        if (iterator == records_.end()) return;
        if (iterator->second.active) iterator->second.good_streak = 0;
        else records_.erase(iterator);
        return;
    }
    if (evaluation == Evaluation::Good) {
        if (iterator == records_.end()) return;
        auto& record = iterator->second;
        if (!record.active) {
            records_.erase(iterator);
            return;
        }
        record.bad_streak = 0;
        ++record.good_streak;
        if (record.good_streak >= policy_.recovery_consecutive) {
            appendEvidenceLocked(record.observation, evidence);
            resolveRecordLocked(iterator, evidence.observed_at, evidence.monotonic_at,
                                evidence.condition, emissions);
        }
        return;
    }

    if (iterator == records_.end()) {
        if (records_.size() >= policy_.max_tracked_records) return;
        Record record;
        record.netns = netns;
        iterator = records_.emplace(key, std::move(record)).first;
    }
    auto& record = iterator->second;
    record.netns = netns;
    record.good_streak = 0;
    if (!record.active) {
        ++record.bad_streak;
        if (record.bad_streak < policy_.activation_consecutive) return;
        if (next_incident_id_ == 0) throw std::overflow_error("incident id exhausted");
        IncidentObservation incident;
        incident.id = IncidentId{next_incident_id_++};
        incident.type = key.type;
        incident.scope = key.scope;
        incident.state = IncidentState::Active;
        incident.severity = severity;
        incident.confidence = completeness == IncidentEvidenceCompleteness::Complete
            ? IncidentConfidence::High
            : completeness == IncidentEvidenceCompleteness::Partial
                ? IncidentConfidence::Partial : IncidentConfidence::Unknown;
        incident.opened_at = evidence.observed_at;
        incident.opened_monotonic_at = evidence.monotonic_at;
        incident.last_updated_at = evidence.observed_at;
        incident.last_updated_monotonic_at = evidence.monotonic_at;
        incident.condition = evidence.condition;
        incident.validity = completeness == IncidentEvidenceCompleteness::Complete
            ? Validity::Valid : Validity::Partial;
        incident.evidence_completeness = completeness;
        appendEvidenceLocked(incident, evidence);
        record.observation = incident;
        record.active = true;
        emissions.push_back(std::move(incident));
        return;
    }

    record.bad_streak = std::min(record.bad_streak + 1,
                                 policy_.activation_consecutive);
    if (!record.observation.evidence.empty() &&
        evidenceEquivalent(record.observation.evidence.back(), evidence)) return;
    appendEvidenceLocked(record.observation, evidence);
    record.observation.last_updated_at = evidence.observed_at;
    record.observation.last_updated_monotonic_at = evidence.monotonic_at;
    record.observation.evidence_completeness = completeness;
    record.observation.confidence = completeness == IncidentEvidenceCompleteness::Complete
        ? IncidentConfidence::High
        : completeness == IncidentEvidenceCompleteness::Partial
            ? IncidentConfidence::Partial : IncidentConfidence::Unknown;
    record.observation.validity = completeness == IncidentEvidenceCompleteness::Complete
        ? Validity::Valid : Validity::Partial;
    emissions.push_back(record.observation);
}

bool IncidentEngine::process(const NetworkEvent& event) {
    std::vector<IncidentObservation> emissions;
    bool consumed = true;
    {
        std::lock_guard lock(mutex_);
        const auto& header = event.header();
        switch (header.kind) {
            case EventKind::SocketObservation: {
                if (header.source != EventSource::SocketTracker) { consumed = false; break; }
                const auto& socket = std::get<SocketObservation>(event.payload());
                if (socket.present && socket.lifecycle == SocketLifecycleState::Active &&
                    header.validity == Validity::Valid && socket.validity == Validity::Valid) {
                    resolveSupersededSocketGenerationsLocked(
                        socket.id, header.observed_at, header.monotonic_at, emissions);
                    authoritative_sockets_[socket.id] = true;
                } else if (!socket.present || socket.lifecycle == SocketLifecycleState::Closed) {
                    authoritative_sockets_.erase(socket.id);
                    resolveSocketLocked(socket.id, header.observed_at, header.monotonic_at,
                                        "socket lifecycle closed", emissions);
                }
                break;
            }
            case EventKind::TcpInfoObservation: {
                if (header.source != EventSource::SocketTracker) { consumed = false; break; }
                const auto& tcp = std::get<TcpInfoObservation>(event.payload());
                resolveSupersededSocketGenerationsLocked(
                    tcp.id, header.observed_at, header.monotonic_at, emissions);
                const IncidentScope scope{tcp.id};
                const bool available = header.validity == Validity::Valid &&
                    tcp.validity == Validity::Valid && tcp.raw && tcp.raw->rtt_us;
                const auto value = available
                    ? std::optional<IncidentEvidenceValue>(IncidentEvidenceValue{
                          static_cast<std::uint64_t>(*tcp.raw->rtt_us)})
                    : std::nullopt;
                auto evaluation = Evaluation::Unavailable;
                std::string condition = "TCP-estimated RTT unavailable";
                if (available) {
                    if (*tcp.raw->rtt_us >= policy_.high_rtt_activation_us) {
                        evaluation = Evaluation::Bad;
                        condition = "rtt_us >= high_rtt_activation_us";
                    } else if (*tcp.raw->rtt_us <= policy_.high_rtt_recovery_us) {
                        evaluation = Evaluation::Good;
                        condition = "rtt_us <= high_rtt_recovery_us";
                    } else {
                        evaluation = Evaluation::Neutral;
                        condition = "RTT inside hysteresis band";
                    }
                }
                const auto evidence = makeEvidence(event, scope, value,
                    IncidentEvidenceUnit::Microseconds,
                    available ? Validity::Valid : header.validity, condition);
                evaluateLocked({IncidentType::HighTcpRtt, scope}, tcp.netns,
                               evaluation, evidence, IncidentSeverity::Warning,
                               available ? IncidentEvidenceCompleteness::Complete
                                         : IncidentEvidenceCompleteness::Unavailable,
                               emissions);
                break;
            }
            case EventKind::TcpIntervalMetric: {
                if (header.source != EventSource::SocketTracker) { consumed = false; break; }
                const auto& metric = std::get<TcpIntervalMetrics>(event.payload());
                resolveSupersededSocketGenerationsLocked(
                    metric.id, header.observed_at, header.monotonic_at, emissions);
                const IncidentScope scope{metric.id};
                const bool volume_ok = policy_.minimum_data_segments == 0 ||
                    (metric.delta_data_segs_out &&
                     *metric.delta_data_segs_out >= policy_.minimum_data_segments);
                const bool available = header.validity == Validity::Valid &&
                    metric.validity == Validity::Valid &&
                    metric.retransmission_segment_ratio && volume_ok &&
                    (!metric.unavailable_reason ||
                     *metric.unavailable_reason != TcpMetricUnavailableReason::CounterReset);
                const auto value = metric.retransmission_segment_ratio
                    ? std::optional<IncidentEvidenceValue>(IncidentEvidenceValue{
                          *metric.retransmission_segment_ratio}) : std::nullopt;
                auto evaluation = Evaluation::Unavailable;
                std::string condition = volume_ok
                    ? "retransmission-segment ratio unavailable"
                    : "insufficient data segments for retransmission rule";
                if (available) {
                    if (*metric.retransmission_segment_ratio >=
                        policy_.retransmission_activation_ratio) {
                        evaluation = Evaluation::Bad;
                        condition = "retransmission_segment_ratio >= activation threshold";
                    } else if (*metric.retransmission_segment_ratio <=
                               policy_.retransmission_recovery_ratio) {
                        evaluation = Evaluation::Good;
                        condition = "retransmission_segment_ratio <= recovery threshold";
                    } else {
                        evaluation = Evaluation::Neutral;
                        condition = "retransmission ratio inside hysteresis band";
                    }
                }
                const auto evidence = makeEvidence(event, scope, value,
                    IncidentEvidenceUnit::Ratio,
                    available ? Validity::Valid : header.validity, condition);
                evaluateLocked({IncidentType::ElevatedTcpRetransmission, scope},
                               metric.id.netns, evaluation, evidence,
                               IncidentSeverity::Warning,
                               available ? IncidentEvidenceCompleteness::Complete
                                         : IncidentEvidenceCompleteness::Unavailable,
                               emissions);
                break;
            }
            case EventKind::SocketRouteObservation: {
                if (header.source != EventSource::SocketTracker) { consumed = false; break; }
                const auto& route = std::get<SocketRouteContextObservation>(event.payload());
                const IncidentScope scope{route.socket_id};
                const bool socket_authoritative = authoritative_sockets_.contains(route.socket_id) &&
                    authoritative_sockets_.at(route.socket_id);
                const bool explicitly_missing = socket_authoritative &&
                    route.attribution_status == RouteAttributionStatus::Unavailable &&
                    hasReason(route.reasons, RouteAttributionReason::NoModeledRoute) &&
                    !hasReason(route.reasons, RouteAttributionReason::TopologyNonAuthoritative) &&
                    !hasReason(route.reasons, RouteAttributionReason::TopologyPartial) &&
                    !hasReason(route.reasons, RouteAttributionReason::TopologyDegraded);
                Evaluation route_evaluation = Evaluation::Unavailable;
                std::string route_condition = "route attribution insufficient";
                if (explicitly_missing) {
                    route_evaluation = Evaluation::Bad;
                    route_condition = "authoritative model has no matching route";
                } else if (socket_authoritative &&
                           route.attribution_status == RouteAttributionStatus::Available) {
                    route_evaluation = Evaluation::Good;
                    route_condition = "modeled route available";
                }
                const auto route_evidence = makeEvidence(event, scope, std::nullopt,
                    IncidentEvidenceUnit::None, header.validity, route_condition);
                evaluateLocked({IncidentType::RouteUnavailable, scope}, route.netns,
                               route_evaluation, route_evidence,
                               IncidentSeverity::Warning,
                               explicitly_missing ? IncidentEvidenceCompleteness::Complete
                                                  : IncidentEvidenceCompleteness::Unavailable,
                               emissions);

                const bool conflict = socket_authoritative &&
                    hasReason(route.reasons, RouteAttributionReason::DiagIfindexConflicts) &&
                    (route.attribution_status == RouteAttributionStatus::Partial ||
                     route.attribution_status == RouteAttributionStatus::Ambiguous);
                Evaluation conflict_evaluation = Evaluation::Unavailable;
                std::string conflict_condition = "routing evidence insufficient";
                if (conflict) {
                    conflict_evaluation = Evaluation::Bad;
                    conflict_condition = "diag_ifindex conflicts with modeled route interfaces";
                } else if (socket_authoritative &&
                           route.attribution_status == RouteAttributionStatus::Available) {
                    conflict_evaluation = Evaluation::Good;
                    conflict_condition = "modeled route and available interface evidence agree";
                }
                const auto diag_value = route.diag_ifindex && *route.diag_ifindex != 0
                    ? std::optional<IncidentEvidenceValue>(IncidentEvidenceValue{
                          static_cast<std::uint64_t>(*route.diag_ifindex)})
                    : std::nullopt;
                const auto conflict_evidence = makeEvidence(event, scope, diag_value,
                    IncidentEvidenceUnit::Count, header.validity, conflict_condition);
                evaluateLocked({IncidentType::SocketRouteConflict, scope}, route.netns,
                               conflict_evaluation, conflict_evidence,
                               IncidentSeverity::Warning,
                               conflict ? IncidentEvidenceCompleteness::Partial
                                        : IncidentEvidenceCompleteness::Unavailable,
                               emissions);
                break;
            }
            case EventKind::UplinkObservation: {
                if (header.source != EventSource::NetlinkCollector) { consumed = false; break; }
                const auto& uplink = std::get<UplinkObservation>(event.payload());
                const IncidentScope scope{header.netns};
                Evaluation evaluation = Evaluation::Unavailable;
                std::string condition = "uplink evidence unavailable";
                if (uplink.selected &&
                    (header.validity == Validity::Valid || header.validity == Validity::Partial)) {
                    evaluation = Evaluation::Good;
                    condition = "authoritative model selected a usable uplink";
                } else if (!uplink.selected &&
                           (header.validity == Validity::Unavailable ||
                            header.validity == Validity::Valid)) {
                    evaluation = Evaluation::Bad;
                    condition = "authoritative model has no usable selected uplink";
                }
                const auto evidence = makeEvidence(event, scope, std::nullopt,
                    IncidentEvidenceUnit::None, header.validity, condition);
                evaluateLocked({IncidentType::UplinkUnavailable, scope}, header.netns,
                               evaluation, evidence, IncidentSeverity::Warning,
                               evaluation == Evaluation::Unavailable
                                   ? IncidentEvidenceCompleteness::Unavailable
                                   : IncidentEvidenceCompleteness::Complete,
                               emissions);
                break;
            }
            default:
                consumed = false;
                break;
        }
    }
    for (const auto& incident : emissions) publish(incident);
    return consumed;
}

void IncidentEngine::publish(const IncidentObservation& incident) {
    NetnsId netns{};
    std::optional<InterfaceId> interface;
    std::optional<SocketId> socket;
    std::visit([&](const auto& scope) {
        using Scope = std::decay_t<decltype(scope)>;
        if constexpr (std::is_same_v<Scope, SocketId>) {
            netns = scope.netns;
            socket = scope;
        } else if constexpr (std::is_same_v<Scope, InterfaceId>) {
            interface = scope;
        } else {
            netns = scope;
        }
    }, incident.scope);
    if (netns.inode == 0) {
        if (!incident.evidence.empty()) {
            std::visit([&](const auto& scope) {
                using Scope = std::decay_t<decltype(scope)>;
                if constexpr (std::is_same_v<Scope, SocketId>) netns = scope.netns;
                else if constexpr (std::is_same_v<Scope, NetnsId>) netns = scope;
            }, incident.evidence.back().scope);
        }
    }
    NetworkEventHeader header{kNetworkEventSchemaVersion,
        EventId{next_event_id_.fetch_add(1)}, {}, EventKind::IncidentObservation,
        EventSource::IncidentEngine, incident.last_updated_at,
        incident.last_updated_monotonic_at, netns, interface, socket,
        incident.validity, std::nullopt};
    (void)bus_.publish(NetworkEvent(std::move(header), incident));
}

std::vector<IncidentObservation> IncidentEngine::listActiveIncidents() const {
    std::vector<IncidentObservation> result;
    std::lock_guard lock(mutex_);
    for (const auto& [key, record] : records_) {
        (void)key;
        if (record.active) result.push_back(record.observation);
    }
    return result;
}

std::optional<IncidentObservation> IncidentEngine::getIncident(IncidentId id) const {
    std::lock_guard lock(mutex_);
    for (const auto& [key, record] : records_) {
        (void)key;
        if (record.active && record.observation.id == id) return record.observation;
    }
    const auto resolved = std::find_if(resolved_.begin(), resolved_.end(),
        [&](const auto& incident) { return incident.id == id; });
    if (resolved != resolved_.end()) return *resolved;
    return std::nullopt;
}

std::vector<IncidentObservation> IncidentEngine::recentResolvedIncidents() const {
    std::lock_guard lock(mutex_);
    return {resolved_.begin(), resolved_.end()};
}

}  // namespace weaknet_dbus::v2
