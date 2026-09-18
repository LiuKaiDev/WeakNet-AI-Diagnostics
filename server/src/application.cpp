#include "application.hpp"

#include <dbus/dbus.h>
#include <pthread.h>

#include <chrono>
#include <exception>
#include <iostream>
#include <string>
#include <stdexcept>
#include <utility>
#include <filesystem>

#include "common.hpp"
#include "dbus_service.hpp"
#include "event_manager.hpp"
#include "logger.hpp"
#include "looper.hpp"
#include "net_wifiriss.h"
#include "rssi_monitor.hpp"
#include "rtt_monitor.hpp"
#include "tcp_loss_monitor.hpp"
#include "using_iface.h"
#include "weak_netmgr.hpp"
#include "weaknet/build_config.hpp"
#include "v1_observation_adapter.hpp"

namespace weaknet_dbus {
namespace {

std::string resolvePingHelperPath() {
    std::error_code error;
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error) return {};
    const auto prefix = executable.parent_path().parent_path();
    return (prefix / WEAKNET_PING_HELPER_RELATIVE_PATH).string();
}

}  // namespace

DaemonApplication::DaemonApplication(RuntimeConfig config, ApplicationTestHooks hooks)
    : config_(std::move(config)), test_hooks_(std::move(hooks)) {
    context_.config = &config_;
    context_.health = &health_;
}

DaemonApplication::~DaemonApplication() {
    stop();
}

bool DaemonApplication::startSignalWaiter() {
    if (test_hooks_.fail_required_step == "signals") return false;
    sigemptyset(&signal_set_);
    sigaddset(&signal_set_, SIGINT);
    sigaddset(&signal_set_, SIGTERM);
    if (pthread_sigmask(SIG_BLOCK, &signal_set_, &previous_signal_mask_) != 0) {
        health_.set("signals", RuntimeHealthState::Failed, "pthread_sigmask_failed");
        return false;
    }
    signal_mask_installed_ = true;
    signal_thread_ = std::jthread([this](std::stop_token thread_token) {
        health_.set("signals", RuntimeHealthState::Running);
        while (!thread_token.stop_requested() && !stop_source_.stop_requested()) {
            timespec timeout{};
            timeout.tv_nsec = 100'000'000;
            const int signal = sigtimedwait(&signal_set_, nullptr, &timeout);
            if (signal == SIGINT || signal == SIGTERM) {
                requestStop();
                break;
            }
        }
        health_.set("signals", RuntimeHealthState::Stopped);
    });
    return true;
}

bool DaemonApplication::startDbus() {
    if (test_hooks_.fail_required_step == "dbus") return false;
    health_.set("dbus", RuntimeHealthState::Starting);
    dbus_threads_init_default();
    DBusError error;
    dbus_error_init(&error);
    context_.connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (dbus_error_is_set(&error)) {
        health_.set("dbus", RuntimeHealthState::Failed, error.message ? error.message : "connect_failed");
        dbus_error_free(&error);
    }
    if (!context_.connection) return false;
    dbus_connection_set_exit_on_disconnect(context_.connection, false);

    dbus_error_init(&error);
    const int result = dbus_bus_request_name(
        context_.connection, kBusName, DBUS_NAME_FLAG_REPLACE_EXISTING, &error);
    if (dbus_error_is_set(&error)) {
        health_.set("dbus", RuntimeHealthState::Failed, error.message ? error.message : "name_failed");
        dbus_error_free(&error);
        return false;
    }
    if (result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
        health_.set("dbus", RuntimeHealthState::Failed, "not_primary_owner");
        return false;
    }
    dbus_name_owned_ = true;

    service_ = std::make_unique<DbusService>(
        &context_, test_hooks_.ping_operation, resolvePingHelperPath());
    context_.service = service_.get();
    if (!service_->register_on_connection(context_.connection)) {
        health_.set("dbus", RuntimeHealthState::Failed, "object_registration_failed");
        return false;
    }
    dbus_path_registered_ = true;
    health_.set("dbus", RuntimeHealthState::Running);
    return true;
}

bool DaemonApplication::startWorkers() {
    if (test_hooks_.fail_required_step == "workers") return false;
    auto launch = [this](const char* name, auto entry) {
        if (test_hooks_.fail_optional_component == name) {
            health_.set(name, RuntimeHealthState::Degraded, "injected_failure");
            if (v2_adapter_) {
                v2_adapter_->mirrorCollectorHealth(
                    name, v2::CollectorState::Degraded, "injected_failure");
            }
            return;
        }
        health_.set(name, RuntimeHealthState::Starting);
        workers_.emplace_back([this, name, entry = std::move(entry)](std::stop_token) mutable {
            try {
                health_.set(name, RuntimeHealthState::Running);
                if (v2_adapter_) {
                    v2_adapter_->mirrorCollectorHealth(name, v2::CollectorState::Running);
                }
                entry(stop_source_.get_token());
                health_.set(name, RuntimeHealthState::Stopped);
                if (v2_adapter_) {
                    v2_adapter_->mirrorCollectorHealth(name, v2::CollectorState::Stopped);
                }
            } catch (const std::exception& error) {
                health_.set(name, RuntimeHealthState::Degraded, error.what());
                if (v2_adapter_) {
                    v2_adapter_->mirrorCollectorHealth(
                        name, v2::CollectorState::Degraded, error.what());
                }
                LOG_ERROR(LogModule::SERVER, name << " worker stopped: " << error.what());
            } catch (...) {
                health_.set(name, RuntimeHealthState::Degraded, "unknown_exception");
                if (v2_adapter_) {
                    v2_adapter_->mirrorCollectorHealth(
                        name, v2::CollectorState::Degraded, "unknown_exception");
                }
                LOG_ERROR(LogModule::SERVER, name << " worker stopped with unknown exception");
            }
        });
    };

    try {
        launch("interfaces", [this](std::stop_token token) {
            run_iface_monitor(&context_, token);
        });
        launch("uplink", [this](std::stop_token token) {
            run_using_iface_monitor(&context_, token);
        });
        launch("rtt", [this](std::stop_token token) {
            run_rtt_monitor(&context_, token, "223.5.5.5", 10000, 800);
        });
        launch("rssi", [this](std::stop_token token) {
            run_rssi_monitor(&context_, token);
        });
        launch("tcp_loss", [this](std::stop_token token) {
            run_tcp_loss_monitor(&context_, token);
        });
        launch("traffic", [this](std::stop_token token) {
            run_traffic_analysis_monitor(&context_, token);
        });
        launch("network_quality", [this](std::stop_token token) {
            run_network_quality_monitor(&context_, token);
        });
    } catch (const std::exception& error) {
        health_.set("workers", RuntimeHealthState::Failed, error.what());
        return false;
    }
    return true;
}

bool DaemonApplication::start() {
    if (started_.load()) return true;
    std::string error;
    if (!config_.validate(&error)) {
        std::cerr << "Invalid WeakNet runtime configuration: " << error << '\n';
        return false;
    }
    if (test_hooks_.fail_required_step == "logger") return false;
    if (!Logger::init("server", config_.log_dir.string(), LogLevel::INFO, true)) {
        return false;
    }
    health_.set("logger", RuntimeHealthState::Running);

    if (!startSignalWaiter()) {
        stop();
        return false;
    }

    if (!event_bus_.start()) {
        health_.set("v2_event_bus", RuntimeHealthState::Failed, "start_failed");
        stop();
        return false;
    }
    health_.set("v2_event_bus", RuntimeHealthState::Running);
    std::string netns_error;
    const auto netns = v2::currentNetworkNamespace(&netns_error);
    if (!netns) {
        health_.set("v2_data_plane", RuntimeHealthState::Failed, netns_error);
        stop();
        return false;
    }
    v2_adapter_ = std::make_unique<v2::V1ObservationAdapter>(
        event_bus_, metric_store_, clock_, *netns);
    context_.v2_adapter = v2_adapter_.get();
    health_.set("v2_data_plane", RuntimeHealthState::Running);

    if (test_hooks_.fail_required_step == "manager") {
        stop();
        return false;
    }
    weak_mgr_ = std::make_unique<WeakNetMgr>();
    WiFiRssiClient::getInstance()->setRuntimeDirectory(config_.runtime_dir.string());
    UsingInterfaceManager::getInstance()->start();
    context_.weak_mgr = weak_mgr_.get();
    context_.running.store(true);

    if (!startDbus()) {
        stop();
        return false;
    }

    getEventManager().startEventMonitoring(&context_);
    event_monitoring_started_ = true;
    if (test_hooks_.fail_required_step == "events") {
        stop();
        return false;
    }

    try {
        if (test_hooks_.fail_optional_component == "initial_interfaces") {
            throw std::runtime_error("injected_failure");
        }
        const auto interfaces = weak_mgr_->collectCurrentInterfaces();
        weak_mgr_->updateInterfaces(interfaces);
        v2_adapter_->mirrorInterfaceSnapshot(interfaces);
        std::string current_interface;
        for (const auto& interface : interfaces) {
            if (interface.usingNow()) {
                current_interface = interface.ifName();
                break;
            }
        }
        v2_adapter_->mirrorUplink(
            current_interface, UsingInterfaceManager::getInstance()->getMethodFlags());
        if (test_hooks_.seed_test_interface) {
            NetInfo test_interface("test0");
            test_interface.setUsingNow(true);
            test_interface.setState(NetState::Up);
            weak_mgr_->updateInterfaces({test_interface});
        }
        health_.set("initial_interfaces", RuntimeHealthState::Running);
    } catch (const std::exception& initial_error) {
        health_.set("initial_interfaces", RuntimeHealthState::Degraded, initial_error.what());
        LOG_ERROR(LogModule::INTERFACE, "initial interface collection failed: " << initial_error.what());
    }

    try {
        if (test_hooks_.fail_optional_component == "ebpf") {
            throw std::runtime_error("injected_failure");
        }
        weak_mgr_->startTrafficAnalysis("eth0", 10);
#if WEAKNET_ENABLE_EBPF
        const auto analyzer = weak_mgr_->getTrafficAnalyzer();
        health_.set("ebpf",
            analyzer && analyzer->hasEbpf() ? RuntimeHealthState::Running
                                           : RuntimeHealthState::Degraded,
            analyzer && analyzer->hasEbpf() ? "" : "load_or_attach_failed");
        v2_adapter_->mirrorCollectorHealth(
            "ebpf", analyzer && analyzer->hasEbpf() ? v2::CollectorState::Running
                                                     : v2::CollectorState::Degraded,
            analyzer && analyzer->hasEbpf() ? "" : "load_or_attach_failed");
#else
        health_.set("ebpf", RuntimeHealthState::Disabled, "not_built");
        v2_adapter_->mirrorCollectorHealth("ebpf", v2::CollectorState::Disabled,
                                           "not_built");
#endif
    } catch (const std::exception& traffic_error) {
        health_.set("ebpf", RuntimeHealthState::Degraded, traffic_error.what());
        v2_adapter_->mirrorCollectorHealth(
            "ebpf", v2::CollectorState::Degraded, traffic_error.what());
    }

    if (!startWorkers()) {
        stop();
        return false;
    }
    stopped_.store(false);
    started_.store(true);
    LOG_INFO(LogModule::SERVER, "daemon started"
        << (health_.degraded() ? " in degraded mode" : ""));
    return true;
}

int DaemonApplication::run() {
    if (!started_.load() || !context_.connection) return 1;
    Looper looper(context_.connection);
    looper.run(stop_source_.get_token());
    stop();
    return 0;
}

void DaemonApplication::requestStop() noexcept {
    context_.running.store(false);
    stop_source_.request_stop();
    if (weak_mgr_) weak_mgr_->requestTrafficAnalysisStop();
}

void DaemonApplication::stopWorkers() noexcept {
    for (auto iterator = workers_.rbegin(); iterator != workers_.rend(); ++iterator) {
        if (iterator->joinable()) iterator->join();
    }
    workers_.clear();

    try {
        if (weak_mgr_) weak_mgr_->stopTrafficAnalysis();
        UsingInterfaceManager::getInstance()->stop();
        WiFiRssiClient::getInstance()->disconnect();
    } catch (...) {
    }
}

void DaemonApplication::stopDbus() noexcept {
    if (dbus_path_registered_ && context_.connection) {
        dbus_connection_unregister_object_path(context_.connection, kObjectPath);
        dbus_path_registered_ = false;
    }
    context_.service = nullptr;
    service_.reset();

    if (dbus_name_owned_ && context_.connection) {
        DBusError error;
        dbus_error_init(&error);
        dbus_bus_release_name(context_.connection, kBusName, &error);
        if (dbus_error_is_set(&error)) dbus_error_free(&error);
        dbus_name_owned_ = false;
    }
    if (context_.connection) {
        dbus_connection_unref(context_.connection);
        context_.connection = nullptr;
    }
    health_.set("dbus", RuntimeHealthState::Stopped);
}

void DaemonApplication::restoreSignalMask() noexcept {
    if (signal_thread_.joinable()) {
        signal_thread_.request_stop();
        signal_thread_.join();
    }
    if (signal_mask_installed_) {
        pthread_sigmask(SIG_SETMASK, &previous_signal_mask_, nullptr);
        signal_mask_installed_ = false;
    }
}

void DaemonApplication::stop() noexcept {
    if (stopped_.exchange(true)) return;
    requestStop();
    stopWorkers();
    if (event_monitoring_started_) {
        getEventManager().stopEventMonitoring();
        event_monitoring_started_ = false;
    }
    event_bus_.stop();
    health_.set("v2_event_bus", RuntimeHealthState::Stopped);
    stopDbus();
    weak_mgr_.reset();
    context_.weak_mgr = nullptr;
    context_.v2_adapter = nullptr;
    v2_adapter_.reset();
    restoreSignalMask();
    health_.set("logger", RuntimeHealthState::Stopped);
    Logger::shutdown();
    started_.store(false);
}

}  // namespace weaknet_dbus
