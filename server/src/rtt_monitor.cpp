#include "rtt_monitor.hpp"

#include <chrono>

#include "dbus_service.hpp"
#include "logger.hpp"
#include "server.hpp"
#include "stop_utils.hpp"
#include "weak_netmgr.hpp"

namespace weaknet_dbus {

void run_rtt_monitor(ServerContext* ctx, std::stop_token token,
                     const std::string& host, int intervalMs, int timeoutMs) {
    LOG_INFO(LogModule::RTT, "RTT monitor thread started");
    while (!token.stop_requested()) {
        try {
            const bool changed = ctx->weak_mgr->updateRttAndStateSafe(host, timeoutMs);
            if (changed && ctx->service) {
                ctx->service->emitChanged("RTT/Quality updated", 0);
            }
        } catch (const std::exception& error) {
            LOG_ERROR(LogModule::RTT, "RTT monitor error: " << error.what());
            throw;
        }
        if (waitForStop(token, std::chrono::milliseconds(intervalMs))) break;
    }
    LOG_INFO(LogModule::RTT, "RTT monitor thread stopped");
}

}  // namespace weaknet_dbus
