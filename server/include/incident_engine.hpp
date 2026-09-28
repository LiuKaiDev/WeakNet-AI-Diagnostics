#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include "clock.hpp"
#include "event_bus.hpp"

namespace weaknet_dbus::v2 {

struct IncidentPolicy {
    std::uint32_t high_rtt_activation_us{200'000};
    std::uint32_t high_rtt_recovery_us{150'000};
    double retransmission_activation_ratio{0.10};
    double retransmission_recovery_ratio{0.05};
    std::uint64_t minimum_data_segments{10};
    std::size_t activation_consecutive{2};
    std::size_t recovery_consecutive{2};
    std::size_t max_recent_resolved{256};
    std::size_t max_evidence_per_incident{8};
    std::size_t max_tracked_records{4096};
};

class IncidentEngine {
public:
    IncidentEngine(EventBus& bus, const Clock& clock, IncidentPolicy policy = {});
    ~IncidentEngine();

    IncidentEngine(const IncidentEngine&) = delete;
    IncidentEngine& operator=(const IncidentEngine&) = delete;

    bool start();
    void stop() noexcept;
    bool running() const noexcept;

    // Public deterministic entry point for replay/unit tests.  EventBus uses
    // this same path; only committed typed NetworkEvents are accepted.
    bool process(const NetworkEvent& event);
    bool handle(const NetworkEvent& event) { return process(event); }

    std::vector<IncidentObservation> listActiveIncidents() const;
    std::vector<IncidentObservation> activeIncidents() const {
        return listActiveIncidents();
    }
    std::optional<IncidentObservation> getIncident(IncidentId id) const;
    std::vector<IncidentObservation> recentResolvedIncidents() const;
    const IncidentPolicy& policy() const noexcept { return policy_; }

private:
    struct IncidentKey {
        IncidentType type{IncidentType::HighTcpRtt};
        IncidentScope scope{NetnsId{}};
        auto operator<=>(const IncidentKey&) const = default;
    };

    struct Record {
        NetnsId netns;
        std::size_t bad_streak{};
        std::size_t good_streak{};
        bool active{false};
        IncidentObservation observation;
    };

    enum class Evaluation : std::uint8_t { Bad, Good, Neutral, Unavailable };

    static bool evidenceEquivalent(const IncidentEvidence& left,
                                   const IncidentEvidence& right);
    static bool hasReason(RouteAttributionReason reasons,
                          RouteAttributionReason wanted) noexcept;
    static IncidentScope socketScope(const SocketId& id) { return IncidentScope{id}; }
    void evaluateLocked(const IncidentKey& key, NetnsId netns,
                        Evaluation evaluation, const IncidentEvidence& evidence,
                        IncidentSeverity severity,
                        IncidentEvidenceCompleteness completeness,
                        std::vector<IncidentObservation>& emissions);
    void resolveRecordLocked(std::map<IncidentKey, Record>::iterator iterator,
                             RealtimeTime realtime, MonotonicTime monotonic,
                             const std::string& condition,
                             std::vector<IncidentObservation>& emissions);
    void resolveSocketLocked(const SocketId& id, RealtimeTime realtime,
                             MonotonicTime monotonic, const std::string& condition,
                             std::vector<IncidentObservation>& emissions);
    void resolveSupersededSocketGenerationsLocked(
        const SocketId& current, RealtimeTime realtime, MonotonicTime monotonic,
        std::vector<IncidentObservation>& emissions);
    void appendEvidenceLocked(IncidentObservation& incident,
                              const IncidentEvidence& evidence);
    IncidentEvidence makeEvidence(const NetworkEvent& event, IncidentScope scope,
                                  std::optional<IncidentEvidenceValue> value,
                                  IncidentEvidenceUnit unit, Validity validity,
                                  std::string condition) const;
    void publish(const IncidentObservation& incident);

    EventBus& bus_;
    IncidentPolicy policy_;
    mutable std::mutex mutex_;
    std::map<IncidentKey, Record> records_;
    std::map<SocketId, bool> authoritative_sockets_;
    std::deque<IncidentObservation> resolved_;
    EventBus::Subscription subscription_;
    bool running_{false};
    std::uint64_t next_incident_id_{1};
    std::atomic<std::uint64_t> next_event_id_{1};
};

}  // namespace weaknet_dbus::v2
