#include "diagnostics_query.hpp"

#include <algorithm>
#include <chrono>

namespace weaknet_dbus::v2 {

const char* overallDiagnosticStateName(OverallDiagnosticState state) noexcept {
    switch (state) {
        case OverallDiagnosticState::Healthy: return "Healthy";
        case OverallDiagnosticState::Degraded: return "Degraded";
        case OverallDiagnosticState::Unknown: return "Unknown";
    }
    return "Unknown";
}

DiagnosticsQueryService::DiagnosticsQueryService(const RuntimeHealth& health,
                                                 const IncidentEngine& incidents,
                                                 const RootCauseEngine& root_causes,
                                                 const NetlinkCollector& topology)
    : health_(health), incidents_(incidents), root_causes_(root_causes), topology_(topology) {}

StatusSnapshot DiagnosticsQueryService::getStatusSnapshot() const {
    StatusSnapshot result;
    const auto topology = topology_.snapshot();
    const auto uplink = topology_.selectedUplink();
    result.topology_authoritative = topology.authoritative;
    result.topology_degraded = topology.degraded || topology.partial;
    for (const auto& entry : health_.snapshot()) {
        if (entry.component == "socket_tracker")
            result.socket_tracker_degraded = entry.state == RuntimeHealthState::Degraded ||
                                             entry.state == RuntimeHealthState::Failed;
    }
    result.uplink = uplink;
    result.active_incidents = incidents_.listActiveIncidents().size();
    result.active_hypotheses = root_causes_.listActiveHypotheses().size();
    result.timestamp_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    bool runtime_unknown = !incidents_.running() || !root_causes_.running() ||
                           !topology.authoritative;
    result.runtime_degraded = health_.degraded();
    bool runtime_degraded = result.runtime_degraded || result.topology_degraded ||
                            result.active_incidents != 0 || result.active_hypotheses != 0;
    result.state = runtime_unknown ? OverallDiagnosticState::Unknown
                                   : runtime_degraded ? OverallDiagnosticState::Degraded
                                                      : OverallDiagnosticState::Healthy;
    return result;
}

std::vector<IncidentObservation> DiagnosticsQueryService::listActiveIncidents() const {
    auto result = incidents_.listActiveIncidents();
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.id.value < right.id.value;
    });
    return result;
}

std::vector<RootCauseHypothesisObservation> DiagnosticsQueryService::listRootCauses() const {
    auto result = root_causes_.listActiveHypotheses();
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        if (left.id.type != right.id.type)
            return static_cast<unsigned>(left.id.type) < static_cast<unsigned>(right.id.type);
        return left.id.occurrence < right.id.occurrence;
    });
    return result;
}

TopologySnapshot DiagnosticsQueryService::getTopologySnapshot() const {
    return topology_.snapshot();
}

DiagnosisSnapshot DiagnosticsQueryService::getDiagnosisSnapshot() const {
    DiagnosisSnapshot result;
    result.status = getStatusSnapshot();
    result.incidents = listActiveIncidents();
    result.hypotheses = listRootCauses();
    result.topology = topology_.snapshot();
    if (!result.topology.authoritative)
        result.limitations.emplace_back("authoritative topology unavailable");
    if (result.topology.degraded || result.topology.partial)
        result.limitations.emplace_back("topology collector degraded or partial");
    result.limitations.emplace_back(
        "active-probe evidence is freshness-bounded and target-specific");
    return result;
}

}  // namespace weaknet_dbus::v2
