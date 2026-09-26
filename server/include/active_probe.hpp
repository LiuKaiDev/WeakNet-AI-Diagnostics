#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include "clock.hpp"
#include "event_bus.hpp"
#include "netlink_topology.hpp"

namespace weaknet_dbus::v2 {

struct ProbeConfig {
    bool enabled{true};
    std::chrono::seconds interval{5};
    std::chrono::milliseconds timeout{1000};
    std::string remote_ipv4{"198.51.100.1"};
};

struct ProbeAttemptResult {
    ProbeStatus status{ProbeStatus::Error};
    std::optional<std::uint64_t> rtt_us;
    std::string reason;
};

struct ProbeTelemetry {
    std::uint64_t attempts{};
    std::uint64_t successes{};
    std::uint64_t timeouts{};
    std::uint64_t unreachable{};
    std::uint64_t invalid_replies{};
    std::uint64_t transport_errors{};
    std::uint64_t unavailable_capability{};
    std::uint64_t no_targets{};
    std::optional<RealtimeTime> last_success_at;
    bool degraded{false};
    std::string capability_reason;
};

struct ActiveProbeTestHooks {
    std::function<ProbeAttemptResult(const ProbeTarget&, std::uint64_t,
                                     std::chrono::milliseconds, std::stop_token)> probe;
};

class ActiveProbe {
public:
    ActiveProbe(EventBus& bus, const Clock& clock, NetnsId netns,
                ProbeConfig config = {}, ActiveProbeTestHooks hooks = {});
    ~ActiveProbe();

    ActiveProbe(const ActiveProbe&) = delete;
    ActiveProbe& operator=(const ActiveProbe&) = delete;

    bool start();
    void stop() noexcept;
    bool running() const noexcept { return running_.load(); }

    void updateTopology(const TopologySnapshot& snapshot,
                        const UplinkSelection& uplink);
    bool probeOnceForTests();

    std::optional<ProbeTarget> gatewayTarget() const;
    std::optional<ProbeTarget> remoteTarget() const;
    ProbeTelemetry telemetry() const;
    const ProbeConfig& config() const noexcept { return config_; }

private:
    void loop(std::stop_token token);
    bool probeTarget(const std::optional<ProbeTarget>& target, ProbeTargetKind kind,
                     std::stop_token token);
    ProbeAttemptResult nativeProbe(const ProbeTarget& target, std::uint64_t sequence,
                                   std::chrono::milliseconds timeout,
                                   std::stop_token token) const;
    void publish(const ProbeTarget& target, std::uint64_t sequence,
                 ProbeAttemptResult result);
    void record(const ProbeObservation& observation);
    static std::optional<ProbeTarget> selectGateway(const TopologySnapshot& snapshot,
                                                    const UplinkSelection& uplink,
                                                    NetnsId netns);
    static std::optional<ProbeTarget> parseRemote(std::string_view literal,
                                                  NetnsId netns);

    EventBus& bus_;
    const Clock& clock_;
    const NetnsId netns_;
    ProbeConfig config_;
    ActiveProbeTestHooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<ProbeTarget> gateway_;
    std::optional<ProbeTarget> remote_;
    TopologySnapshot topology_;
    UplinkSelection uplink_;
    ProbeTelemetry telemetry_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<std::uint64_t> next_sequence_{1};
    std::atomic<std::uint64_t> next_event_id_{1};
};

}  // namespace weaknet_dbus::v2
