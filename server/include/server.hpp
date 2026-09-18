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
};

void run_iface_monitor(ServerContext* ctx, std::stop_token token);
void run_using_iface_monitor(ServerContext* ctx, std::stop_token token);
void run_traffic_analysis_monitor(ServerContext* ctx, std::stop_token token);
void run_network_quality_monitor(ServerContext* ctx, std::stop_token token);

int start_server();

}  // namespace weaknet_dbus
