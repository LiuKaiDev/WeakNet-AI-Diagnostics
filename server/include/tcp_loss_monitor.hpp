// tcp_loss_monitor.hpp
// TCP丢包率监控线程声明

#pragma once

#include "server.hpp"

namespace weaknet_dbus {

// 启动TCP丢包率监控线程
// 监控当前上网网卡的TCP丢包率，并更新到ServerContext中的NetInfo列表
void run_tcp_loss_monitor(ServerContext* ctx, std::stop_token token);

} // namespace weaknet_dbus
