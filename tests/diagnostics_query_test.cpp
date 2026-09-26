#include "diagnostics_query.hpp"

#include <iostream>

using namespace weaknet_dbus;
using namespace weaknet_dbus::v2;

int main() {
    EventBus bus;
    SystemClock clock;
    RuntimeHealth health;
    const NetnsId netns{1, 2};
    NetlinkCollector topology(bus, clock, netns);
    IncidentEngine incidents(bus, clock);
    RootCauseEngine root_causes(bus, clock);
    DiagnosticsQueryService query(health, incidents, root_causes, topology);

    const auto status = query.getStatusSnapshot();
    if (status.state != OverallDiagnosticState::Unknown ||
        status.active_incidents != 0 || status.active_hypotheses != 0 ||
        status.topology_authoritative) {
        std::cerr << "empty query snapshot did not remain unknown\n";
        return 1;
    }
    const auto diagnosis = query.getDiagnosisSnapshot();
    if (diagnosis.incidents.size() != 0 || diagnosis.hypotheses.size() != 0 ||
        diagnosis.limitations.empty()) {
        std::cerr << "diagnosis snapshot did not preserve empty/limited state\n";
        return 1;
    }
    return 0;
}

