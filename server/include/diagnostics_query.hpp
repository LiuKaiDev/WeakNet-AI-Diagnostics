#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "incident_engine.hpp"
#include "netlink_collector.hpp"
#include "root_cause_engine.hpp"
#include "runtime_health.hpp"

namespace weaknet_dbus::v2 {

enum class OverallDiagnosticState : std::uint8_t { Healthy, Degraded, Unknown };

struct StatusSnapshot {
    OverallDiagnosticState state{OverallDiagnosticState::Unknown};
    bool topology_authoritative{false};
    bool topology_degraded{false};
    bool socket_tracker_degraded{false};
    bool runtime_degraded{false};
    std::size_t active_incidents{};
    std::size_t active_hypotheses{};
    std::optional<UplinkSelection> uplink;
    std::uint64_t timestamp_ms{};
};

struct DiagnosisSnapshot {
    StatusSnapshot status;
    std::vector<IncidentObservation> incidents;
    std::vector<RootCauseHypothesisObservation> hypotheses;
    TopologySnapshot topology;
    std::vector<std::string> limitations;
};

class DiagnosticsQueryService {
public:
    DiagnosticsQueryService(const RuntimeHealth& health,
                            const IncidentEngine& incidents,
                            const RootCauseEngine& root_causes,
                            const NetlinkCollector& topology);

    StatusSnapshot getStatusSnapshot() const;
    std::vector<IncidentObservation> listActiveIncidents() const;
    std::vector<RootCauseHypothesisObservation> listRootCauses() const;
    DiagnosisSnapshot getDiagnosisSnapshot() const;
    TopologySnapshot getTopologySnapshot() const;

private:
    const RuntimeHealth& health_;
    const IncidentEngine& incidents_;
    const RootCauseEngine& root_causes_;
    const NetlinkCollector& topology_;
};

const char* overallDiagnosticStateName(OverallDiagnosticState state) noexcept;

}  // namespace weaknet_dbus::v2
