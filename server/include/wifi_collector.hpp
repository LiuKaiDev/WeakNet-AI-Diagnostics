#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

#include "clock.hpp"
#include "event_bus.hpp"
#include "netlink_topology.hpp"
#include "wifi_nl80211_parser.hpp"

namespace weaknet_dbus::v2 {

struct WifiCollectorConfig {
    bool enabled{true};
    std::chrono::seconds interval{5};
    std::chrono::milliseconds timeout{1000};
    std::chrono::seconds freshness{10};
};

struct WifiCollectorTelemetry {
    std::uint64_t query_attempts{};
    std::uint64_t successful_observations{};
    std::uint64_t not_wifi_results{};
    std::uint64_t not_associated_results{};
    std::uint64_t unsupported_results{};
    std::uint64_t permission_failures{};
    std::uint64_t malformed_replies{};
    std::uint64_t timeouts{};
    std::uint64_t transport_errors{};
    std::uint64_t no_target_results{};
    std::optional<RealtimeTime> last_success_at;
    bool degraded{false};
    std::string capability_reason;
};

struct WifiCollectorTestHooks {
    std::function<int()> open_socket;
    std::function<std::optional<WifiObservation>(const InterfaceId&, std::uint64_t,
                                                 std::stop_token)> query;
};

class WifiCollector {
public:
    WifiCollector(EventBus& bus, const Clock& clock, NetnsId netns,
                  WifiCollectorConfig config = {}, WifiCollectorTestHooks hooks = {});
    ~WifiCollector();

    WifiCollector(const WifiCollector&) = delete;
    WifiCollector& operator=(const WifiCollector&) = delete;

    bool start();
    void stop() noexcept;
    bool running() const noexcept { return running_.load(); }

    void updateTopology(const TopologySnapshot& snapshot, const UplinkSelection& uplink);
    bool refreshForTests();
    WifiCollectorTelemetry telemetry() const;
    std::optional<WifiObservation> latest() const;
    bool latestFresh() const;
    const WifiCollectorConfig& config() const noexcept { return config_; }

private:
    struct QueryResult {
        WifiObservation observation;
        int error_code{};
        WifiParseStatus transport_status{WifiParseStatus::Ok};
    };

    void loop(std::stop_token token);
    bool refresh(std::stop_token token);
    QueryResult nativeQuery(const InterfaceId& interface, std::uint64_t generation,
                            std::stop_token token);
    bool openSocket();
    void closeSocket() noexcept;
    std::optional<std::uint16_t> resolveFamily(std::stop_token token, int& error_code,
                                               WifiParseStatus& transport_status);
    WifiParseStatus requestAndReceive(const std::vector<std::byte>& request,
                                      std::uint32_t sequence,
                                      std::vector<std::byte>& response,
                                      std::stop_token token);
    std::vector<std::byte> makeRequest(std::uint16_t family, std::uint8_t command,
                                       std::uint16_t flags, std::uint32_t sequence,
                                       std::optional<std::uint32_t> ifindex = std::nullopt,
                                       std::optional<std::string> family_name = std::nullopt) const;
    void publish(WifiObservation observation);
    void record(const WifiObservation& observation);

    EventBus& bus_;
    const Clock& clock_;
    const NetnsId netns_;
    WifiCollectorConfig config_;
    WifiCollectorTestHooks hooks_;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::optional<InterfaceId> target_;
    std::uint64_t target_generation_{};
    std::optional<WifiObservation> latest_;
    WifiCollectorTelemetry telemetry_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<int> socket_fd_{-1};
    std::atomic<std::uint32_t> next_sequence_{1};
    std::atomic<std::uint64_t> next_event_id_{1};
    std::optional<std::uint16_t> family_id_;
};

}  // namespace weaknet_dbus::v2
