#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "network_event.hpp"
#include "socket_diag_parser.hpp"
#include "event_bus.hpp"
#include "clock.hpp"
#include "scoped_fd.hpp"

namespace weaknet_dbus::v2 {

enum class SocketSnapshotDisposition : std::uint8_t {
    Authoritative,
    Partial,
    Failed,
};

struct SocketObservationInput {
    SocketTuple tuple;
    std::optional<KernelSocketCookie> cookie;
    RealtimeTime observed_at{};
    MonotonicTime monotonic_at{};
    EventSource source{EventSource::SocketTracker};
    Validity validity{Validity::Valid};
    bool present{true};
    TcpSocketState tcp_state{};
    std::optional<std::uint32_t> diag_ifindex;
};

struct SocketResolution {
    SocketObservation observation;
    bool created_generation{false};
    bool inconsistent_metadata{false};
};

struct SocketLifecycleConfig {
    std::size_t max_active_entries{4096};
    std::size_t max_closed_entries{1024};
};

// In-process identity/lifecycle model for future socket collectors. It has no
// kernel I/O and is deliberately driven by explicit snapshot boundaries.
class SocketLifecycleTable {
public:
    explicit SocketLifecycleTable(SocketLifecycleConfig config = {});

    SocketLifecycleTable(const SocketLifecycleTable&) = delete;
    SocketLifecycleTable& operator=(const SocketLifecycleTable&) = delete;

    bool beginSnapshot(SocketSnapshotDisposition disposition);
    std::optional<SocketResolution> observe(const SocketObservationInput& input);
    bool commitSnapshot();
    void abortSnapshot() noexcept;

    // Explicit authoritative close indication. Returns false for an unknown
    // or already-closed identity, and never guesses from tuple metadata.
    bool close(const SocketId& id);

    std::vector<SocketObservation> active() const;
    std::vector<SocketObservation> recentlyClosed() const;
    std::size_t activeCount() const noexcept { return active_.size(); }
    std::size_t closedCount() const noexcept { return closed_.size(); }
    bool snapshotInProgress() const noexcept { return snapshot_in_progress_; }

private:
    struct CookieKey {
        NetnsId netns;
        KernelSocketCookie cookie;
        auto operator<=>(const CookieKey&) const = default;
    };

    static bool validInput(const SocketObservationInput& input) noexcept;
    SocketGeneration nextGeneration();
    SocketId newIdentity(const SocketTuple& tuple,
                         std::optional<KernelSocketCookie> cookie);
    SocketObservation makeObservation(const SocketId& id,
                                      const SocketObservationInput& input) const;
    std::optional<SocketId> pendingWeak(const SocketTuple& tuple) const;
    std::optional<SocketId> pendingCookie(const CookieKey& key) const;
    void erasePending(const SocketId& id);
    void supersede(const SocketId& id);
    void rememberClosed(SocketObservation observation);
    void clearCandidate() noexcept;

    SocketLifecycleConfig config_;
    bool snapshot_in_progress_{false};
    SocketSnapshotDisposition disposition_{SocketSnapshotDisposition::Failed};
    std::uint64_t next_generation_{1};

    std::map<SocketId, SocketObservation> active_;
    std::map<SocketTuple, SocketId> weak_active_;
    std::map<CookieKey, SocketId> cookie_active_;
    std::deque<SocketObservation> closed_;

    std::map<SocketId, SocketObservation> pending_;
    std::map<SocketTuple, SocketId> pending_weak_;
    std::map<CookieKey, SocketId> pending_cookie_;
    std::set<SocketId> pending_seen_;
    std::set<SocketId> pending_superseded_;
};

struct SocketTrackerTelemetry {
    std::uint64_t reconciliation_attempts{};
    std::uint64_t reconciliation_successes{};
    std::uint64_t reconciliation_failures{};
    std::uint64_t timeouts{};
    std::uint64_t truncations{};
    std::uint64_t interrupted_dumps{};
    std::uint64_t netlink_errors{};
    std::uint64_t overflow_events{};
    std::uint64_t malformed_messages{};
    std::uint64_t parsed_socket_count{};
    std::uint64_t authoritative_commits{};
    std::uint64_t capacity_exhaustions{};
    std::size_t active_socket_count{};
    bool degraded{false};
    std::string last_error;
    std::optional<RealtimeTime> last_successful_reconciliation;
};

struct SocketTrackerTestHooks {
    std::function<int()> open_socket;
    std::function<std::optional<std::vector<SocketDiagRecord>>(std::uint8_t, std::stop_token)> dump;
    std::chrono::milliseconds recovery_retry_initial{std::chrono::milliseconds(250)};
    std::chrono::milliseconds recovery_retry_max{std::chrono::seconds(5)};
};

class SocketTracker {
public:
    static constexpr auto kInventoryInterval = std::chrono::seconds(5);
    SocketTracker(EventBus& bus, const Clock& clock, NetnsId netns,
                  SocketLifecycleConfig lifecycle_config = {},
                  std::chrono::milliseconds interval = kInventoryInterval,
                  SocketTrackerTestHooks hooks = {});
    ~SocketTracker();
    SocketTracker(const SocketTracker&) = delete;
    SocketTracker& operator=(const SocketTracker&) = delete;
    bool start();
    void stop() noexcept;
    bool running() const noexcept { return running_.load(); }
    std::vector<SocketObservation> active() const;
    SocketTrackerTelemetry telemetry() const;
    bool reconcileForTests();

private:
    bool openSocket();
    bool reconcile(std::stop_token token);
    bool dumpFamily(std::uint8_t family, std::vector<SocketDiagRecord>& output,
                    std::stop_token token);
    void loop(std::stop_token token);
    void markFailure(const std::string& reason);
    void publishCommitted(const std::vector<SocketObservation>& before,
                          const std::vector<SocketObservation>& after,
                          const std::vector<SocketObservation>& closed);

    EventBus& bus_;
    const Clock& clock_;
    NetnsId netns_;
    SocketLifecycleTable lifecycle_;
    std::chrono::milliseconds interval_;
    SocketTrackerTestHooks hooks_;
    mutable std::mutex mutex_;
    mutable std::mutex lifecycle_mutex_;
    SocketTrackerTelemetry telemetry_;
    std::jthread worker_;
    std::atomic<bool> running_{false};
    ::weaknet_dbus::ScopedFd socket_;
    std::atomic<std::uint64_t> next_event_id_{1};
};

}  // namespace weaknet_dbus::v2
