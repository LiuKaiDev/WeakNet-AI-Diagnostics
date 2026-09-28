// rssi_monitor.hpp
// 启动 Wi-Fi RSSI 监控线程

#pragma once

#include <string>
#include <stop_token>

namespace weaknet_dbus {

struct ServerContext;

void run_rssi_monitor(ServerContext* ctx, std::stop_token token,
                      const std::string& ctrlDir = "");

}  // namespace weaknet_dbus
