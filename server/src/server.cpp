#include "server.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <string>
#include <vector>

#include "application.hpp"
#include "dbus_service.hpp"
#include "event_manager.hpp"
#include "logger.hpp"
#include "net_info.hpp"
#include "network_quality_assessor.hpp"
#include "stop_utils.hpp"
#include "weak_netmgr.hpp"
#include "v1_observation_adapter.hpp"
#include "using_iface.h"

using namespace std::chrono_literals;

namespace weaknet_dbus {
namespace {

bool diffInterfaces(const std::vector<std::string>& old_list,
                    const std::vector<std::string>& new_list,
                    std::vector<std::string>& added,
                    std::vector<std::string>& removed) {
    added.clear();
    removed.clear();
    for (const auto& item : new_list) {
        if (std::find(old_list.begin(), old_list.end(), item) == old_list.end()) {
            added.push_back(item);
        }
    }
    for (const auto& item : old_list) {
        if (std::find(new_list.begin(), new_list.end(), item) == new_list.end()) {
            removed.push_back(item);
        }
    }
    return !added.empty() || !removed.empty();
}

}  // namespace

void run_iface_monitor(ServerContext* ctx, std::stop_token token) {
    LOG_INFO(LogModule::INTERFACE, "monitor thread started");
    std::vector<NetInfo> current;
    while (!token.stop_requested()) {
        const auto latest = ctx->weak_mgr->collectCurrentInterfaces();
        auto old_names = WeakNetMgr::namesOf(current);
        auto new_names = WeakNetMgr::namesOf(latest);
        std::vector<std::string> added;
        std::vector<std::string> removed;
        if (diffInterfaces(old_names, new_names, added, removed)) {
            current = latest;
            ctx->weak_mgr->updateInterfaces(current);
            if (ctx->v2_adapter) ctx->v2_adapter->mirrorInterfaceSnapshot(current);
            std::string message = "Interfaces changed (using flags in log): +";
            for (std::size_t index = 0; index < added.size(); ++index) {
                message += (index == 0 ? "" : ",") + added[index];
            }
            message += " -";
            for (std::size_t index = 0; index < removed.size(); ++index) {
                message += (index == 0 ? "" : ",") + removed[index];
            }
            LOG_INFO(LogModule::INTERFACE, message);
            if (ctx->service) {
                ctx->service->emitChanged(message, 0);
                getEventManager().emitInterfaceChanged(message, "network_manager");
            }
        }
        if (waitForStop(token, 10s)) break;
    }
    LOG_INFO(LogModule::INTERFACE, "monitor thread stopped");
}

void run_using_iface_monitor(ServerContext* ctx, std::stop_token token) {
    LOG_INFO(LogModule::WEAK_MGR, "using interface monitor started");
    while (!token.stop_requested()) {
        const bool changed = ctx->weak_mgr->updateCurrentUsingSafe();
        const auto interfaces = ctx->weak_mgr->getCurrentInterfaces();
        if (changed && ctx->service) {
            std::string current_interface;
            for (const auto& interface : interfaces) {
                if (interface.usingNow()) {
                    current_interface = interface.ifName();
                    break;
                }
            }
            const std::string message = "Using iface updated: " +
                (current_interface.empty() ? std::string("(none)") : current_interface);
            ctx->service->emitChanged(message, 0);
            getEventManager().emitConnectionModeChanged(
                message, current_interface.empty() ? "none" : current_interface);
        }
        if (changed && ctx->v2_adapter) {
            std::string current_interface;
            for (const auto& interface : interfaces) {
                if (interface.usingNow()) {
                    current_interface = interface.ifName();
                    break;
                }
            }
            ctx->v2_adapter->mirrorUplink(
                current_interface, UsingInterfaceManager::getInstance()->getMethodFlags());
        }
        if (waitForStop(token, 10s)) break;
    }
    LOG_INFO(LogModule::WEAK_MGR, "using interface monitor stopped");
}

void run_traffic_analysis_monitor(ServerContext* ctx, std::stop_token token) {
    LOG_INFO(LogModule::WEAK_MGR, "traffic analysis monitor started");
    while (!token.stop_requested()) {
        const bool changed = ctx->weak_mgr->updateTrafficAnalysisSafe(token);
        if (changed && ctx->v2_adapter) {
            const auto analyzer = ctx->weak_mgr->getTrafficAnalyzer();
            std::string current_interface;
            for (const auto& interface : ctx->weak_mgr->getCurrentInterfaces()) {
                if (interface.usingNow()) {
                    current_interface = interface.ifName();
                    break;
                }
            }
            if (!current_interface.empty() && analyzer) {
                ctx->v2_adapter->mirrorTraffic(current_interface,
                    analyzer->getCurrentStats(), analyzer->hasEbpf());
            }
        }
        if (changed && ctx->service) {
            ctx->service->emitChanged("Traffic analysis updated", 0);
        }
        if (waitForStop(token, 10s)) break;
    }
    LOG_INFO(LogModule::WEAK_MGR, "traffic analysis monitor stopped");
}

void run_network_quality_monitor(ServerContext* ctx, std::stop_token token) {
    LOG_INFO(LogModule::WEAK_MGR, "network quality monitor started");
    NetworkQualityAssessor assessor;
    NetworkQualityResult previous;
    previous.level = NetworkQualityLevel::UNKNOWN;
    while (!token.stop_requested()) {
        const auto interfaces = ctx->weak_mgr->getCurrentInterfaces();
        const auto current = assessor.assessQuality(interfaces);
        if (current.level != previous.level ||
            std::abs(current.score - previous.score) > 15.0) {
            LOG_INFO(LogModule::WEAK_MGR, "network quality changed: "
                << current.levelName << " (score: " << std::fixed
                << std::setprecision(1) << current.score << ")");
            getEventManager().emitNetworkQualityChanged(
                current.levelName, current.details, "network_quality_assessor");
            previous = current;
        }
        if (waitForStop(token, 15s)) break;
    }
    LOG_INFO(LogModule::WEAK_MGR, "network quality monitor stopped");
}

int start_server() {
    DaemonApplication application;
    if (!application.start()) return 1;
    return application.run();
}

}  // namespace weaknet_dbus
