// rtt_monitor.hpp
// 启动 RTT 监控线程，周期性调用 WeakNetMgr::updateRttAndState

#pragma once

#include <string>
#include <stop_token>

namespace weaknet_dbus {

struct ServerContext;

// 创建并启动 RTT 监控线程
// host: 目标主机（如 1.1.1.1 / 8.8.8.8 / 自定义域名）
void run_rtt_monitor(ServerContext* ctx, std::stop_token token,
                     const std::string& host, int intervalMs = 2000,
                     int timeoutMs = 800);

}  // namespace weaknet_dbus
