#pragma once

#include <atomic>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>
#include <string>
#include <functional>

#include <signal.h>

#include "runtime_config.hpp"
#include "runtime_health.hpp"
#include "server.hpp"

namespace weaknet_dbus {

struct ApplicationTestHooks {
    std::string fail_required_step;
    std::string fail_optional_component;
    bool seed_test_interface = false;
    std::function<int(std::stop_token, const std::string&, const std::string&, int)>
        ping_operation;
};

class DaemonApplication {
public:
    explicit DaemonApplication(RuntimeConfig config = RuntimeConfig::fromEnvironment(),
                               ApplicationTestHooks hooks = {});
    ~DaemonApplication();

    DaemonApplication(const DaemonApplication&) = delete;
    DaemonApplication& operator=(const DaemonApplication&) = delete;

    bool start();
    int run();
    void requestStop() noexcept;
    void stop() noexcept;

    bool running() const noexcept { return started_.load() && !stop_source_.stop_requested(); }
    RuntimeHealth& health() noexcept { return health_; }

private:
    bool startSignalWaiter();
    bool startDbus();
    bool startWorkers();
    void stopWorkers() noexcept;
    void stopDbus() noexcept;
    void restoreSignalMask() noexcept;

    RuntimeConfig config_;
    RuntimeHealth health_;
    std::stop_source stop_source_;
    std::atomic<bool> started_{false};
    std::atomic<bool> stopped_{false};

    ServerContext context_;
    std::unique_ptr<WeakNetMgr> weak_mgr_;
    std::unique_ptr<DbusService> service_;
    std::vector<std::jthread> workers_;
    std::jthread signal_thread_;

    sigset_t signal_set_{};
    sigset_t previous_signal_mask_{};
    bool signal_mask_installed_ = false;
    bool dbus_name_owned_ = false;
    bool dbus_path_registered_ = false;
    bool event_monitoring_started_ = false;
    ApplicationTestHooks test_hooks_;
};

}  // namespace weaknet_dbus
