#include "rssi_monitor.hpp"

#include <chrono>

#include "dbus_service.hpp"
#include "logger.hpp"
#include "server.hpp"
#include "stop_utils.hpp"
#include "weak_netmgr.hpp"
#include "v1_observation_adapter.hpp"

using namespace std::chrono_literals;

namespace weaknet_dbus {

void run_rssi_monitor(ServerContext* ctx, std::stop_token token,
                      const std::string& ctrlDir) {
    LOG_INFO(LogModule::RSSI, "RSSI monitor thread started");
    while (!token.stop_requested()) {
        const bool changed = ctx->weak_mgr->updateWifiRssiSafe(ctrlDir);
        if (ctx->v2_adapter) {
            for (const auto& interface : ctx->weak_mgr->getCurrentInterfaces()) {
                if (interface.type() == NetType::WiFi) {
                    ctx->v2_adapter->mirrorWifiRssi(interface);
                }
            }
        }
        if (changed && ctx->service) {
            ctx->service->emitChanged("WiFi RSSI updated", 0);
        }
        if (waitForStop(token, 10s)) break;
    }
    LOG_INFO(LogModule::RSSI, "RSSI monitor thread stopped");
}

}  // namespace weaknet_dbus
