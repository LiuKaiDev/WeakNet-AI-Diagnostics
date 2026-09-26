#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "network_event.hpp"

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

}  // namespace weaknet_dbus::v2
