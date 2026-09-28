#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <linux/netlink.h>
#else
struct sockaddr_nl;
#endif

#include "event_bus.hpp"
#include "netlink_parser.hpp"

namespace weaknet_dbus::v2 {

struct NetlinkCollectorTelemetry {
    std::uint64_t successful_reconciliations{};
    std::uint64_t failed_dumps{};
    std::uint64_t interrupted_dumps{};
    std::uint64_t parse_errors{};
    std::uint64_t truncations{};
    std::uint64_t overflow_events{};
    std::uint64_t resync_requests{};
    std::uint64_t forced_resync_requests{};
    std::uint64_t notifications_observed_during_reconciliation{};
    std::uint64_t reconciliation_races{};
    std::uint64_t ambiguous_route_deletes{};
    std::uint64_t notification_apply_failures{};
    std::uint64_t notifications_processed{};
    std::uint64_t state_changes_published{};
    std::size_t link_count{};
    std::size_t address_count{};
    std::size_t route_count{};
    bool degraded{false};
    std::string last_error;
};

struct NetlinkCollectorTestHooks {
    std::function<int()> open_socket;
    std::function<std::optional<TopologySnapshot>(std::stop_token)> reconcile;
    std::function<void(bool)> reconciliation_complete;
    std::function<bool()> inject_overflow;
    // Also used by the application as the bounded topology-change hook.  It
    // is invoked only after a committed authoritative/current model update.
    std::function<void(const TopologySnapshot&, const UplinkSelection&)> committed_callback;
    std::chrono::milliseconds recovery_retry_initial{std::chrono::milliseconds(250)};
    std::chrono::milliseconds recovery_retry_max{std::chrono::seconds(5)};
};

class TopologyState {
public:
    explicit TopologyState(NetnsId netns = {});
    bool apply(const ParsedMessage& message);
    TopologyApplyResult applyChecked(const ParsedMessage& message);
    void replace(const TopologySnapshot& snapshot);
    TopologySnapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    TopologySnapshot snapshot_;
};

class NetlinkCollector {
public:
    NetlinkCollector(EventBus& bus, const Clock& clock, NetnsId netns,
                     std::chrono::milliseconds reconciliation_interval = std::chrono::seconds(30),
                     NetlinkCollectorTestHooks test_hooks = {});
    ~NetlinkCollector();

    NetlinkCollector(const NetlinkCollector&) = delete;
    NetlinkCollector& operator=(const NetlinkCollector&) = delete;

    bool start();
    void stop() noexcept;
    bool running() const noexcept { return running_.load(); }
    void setCommittedCallback(std::function<void(const TopologySnapshot&, const UplinkSelection&)> callback);
    TopologySnapshot snapshot() const;
    UplinkSelection selectedUplink() const;
    NetlinkCollectorTelemetry telemetry() const;
    bool processNotificationForTests(const std::vector<std::byte>& datagram);
    bool reconcileForTests(const std::vector<std::vector<std::byte>>& datagrams,
                           std::uint32_t expected_sequence);

private:
    bool openSocket();
    bool fullReconcile(std::stop_token token);
    bool dump(std::uint16_t type, std::uint8_t family, TopologySnapshot& candidate,
              std::stop_token token, bool& notification_raced);
    void loop(std::stop_token token);
    void processDatagram(const void* data, std::size_t size, const sockaddr_nl& sender,
                         std::optional<std::uint32_t> expected_sequence, bool dump_response,
                         TopologyState& state, bool* complete = nullptr);
    void publishChanges(const TopologySnapshot& before, const TopologySnapshot& after);
    void publishMessage(const ParsedMessage& message);
    void updateTelemetrySnapshot(const TopologySnapshot& snapshot);
    void markReconciliationFailed();
    void markNotificationRace();
    void markOverflow();
    bool processNotificationBatch(const std::vector<ParsedMessage>& messages);
    void commitReconciliation(TopologySnapshot candidate);

    EventBus& bus_;
    const Clock& clock_;
    NetnsId netns_;
    std::chrono::milliseconds reconciliation_interval_;
    NetlinkCollectorTestHooks test_hooks_;
    mutable std::mutex mutex_;
    TopologyState state_;
    UplinkSelection selected_;
    NetlinkCollectorTelemetry telemetry_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    std::atomic<int> socket_fd_{-1};
    std::atomic<std::uint64_t> next_event_id_{1};
};

}  // namespace weaknet_dbus::v2
