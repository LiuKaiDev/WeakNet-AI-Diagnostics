#include "tcp_loss_monitor.hpp"

#include <chrono>

#include "dbus_service.hpp"
#include "logger.hpp"
#include "net_tcp.h"
#include "server.hpp"
#include "stop_utils.hpp"
#include "weak_netmgr.hpp"

using namespace std::chrono_literals;

namespace weaknet_dbus {

void run_tcp_loss_monitor(ServerContext* ctx, std::stop_token token) {
    LOG_INFO(LogModule::TCP_LOSS, "TCP loss monitor thread started");
    const auto monitor = TcpLossMonitor::getInstance();
    TcpStats previous;
    bool has_previous = false;
    while (!token.stop_requested()) {
        const auto interfaces = ctx->weak_mgr->getCurrentInterfaces();
        std::string current_interface;
        for (const auto& interface : interfaces) {
            if (interface.usingNow()) {
                current_interface = interface.ifName();
                break;
            }
        }
        if (current_interface.empty()) {
            if (waitForStop(token, 5s)) break;
            continue;
        }

        TcpStats current;
        if (!monitor->sampleForInterface(current_interface, current)) {
            if (waitForStop(token, 10s)) break;
            continue;
        }
        if (has_previous) {
            const auto result = monitor->compute(previous, current);
            if (result.sentDelta >= 10 &&
                ctx->weak_mgr->updateTcpLossRateSafe(
                    current_interface, result.ratePercent, result.level) &&
                ctx->service) {
                const std::string message = "TCP loss rate updated for " + current_interface +
                    ": " + std::to_string(result.ratePercent) + "% (" + result.level + ")";
                ctx->service->emitChanged(message, 0);
            }
        }
        previous = current;
        has_previous = true;
        if (waitForStop(token, 10s)) break;
    }
    LOG_INFO(LogModule::TCP_LOSS, "TCP loss monitor thread stopped");
}

}  // namespace weaknet_dbus
