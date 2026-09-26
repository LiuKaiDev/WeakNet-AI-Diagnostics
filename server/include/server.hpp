#pragma once

#include <atomic>
#include <stop_token>

struct DBusConnection;

namespace weaknet_dbus {

class DbusService;
struct RuntimeConfig;
class RuntimeHealth;
class WeakNetMgr;
namespace v2 { class V1ObservationAdapter; }
namespace v2 { class NetlinkCollector; }
namespace v2 { class IncidentEngine; class RootCauseEngine; }
namespace v2 { class DiagnosticsQueryService; }

// Non-owning view passed to the retained V1 workers. DaemonApplication owns
// every referenced object and joins all workers before destroying them.
struct ServerContext {
    ::DBusConnection* connection = nullptr;
    std::atomic<bool> running{false};
    DbusService* service = nullptr;
    WeakNetMgr* weak_mgr = nullptr;
    RuntimeConfig* config = nullptr;
    RuntimeHealth* health = nullptr;
    v2::V1ObservationAdapter* v2_adapter = nullptr;
    v2::NetlinkCollector* topology = nullptr;
    v2::IncidentEngine* incidents = nullptr;
    v2::RootCauseEngine* root_causes = nullptr;
    v2::DiagnosticsQueryService* diagnostics = nullptr;
};

void run_iface_monitor(ServerContext* ctx, std::stop_token token);
void run_using_iface_monitor(ServerContext* ctx, std::stop_token token);
void run_traffic_analysis_monitor(ServerContext* ctx, std::stop_token token);
void run_network_quality_monitor(ServerContext* ctx, std::stop_token token);

int start_server();

}  // namespace weaknet_dbus
