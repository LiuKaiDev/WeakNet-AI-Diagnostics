#pragma once

#include <cstddef>
#include <cstdint>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "clock.hpp"
#include "event_bus.hpp"

namespace weaknet_dbus::v2 {

struct RootCausePolicy {
    std::size_t max_recent_resolved{256};
    std::size_t max_evidence_per_hypothesis{8};
    std::size_t max_active_hypotheses{1024};
    std::chrono::seconds probe_freshness{15};
    // WifiCollector's documented polling/freshness contract is ten seconds.
    std::chrono::seconds wifi_freshness{10};
    std::uint64_t gateway_rtt_high_us{75'000};
    std::uint64_t remote_rtt_high_us{175'000};
    // RSSI values are engineering heuristics, not universal radio limits.
    std::int32_t wifi_signal_weak_dbm{-70};
    std::int32_t wifi_signal_very_weak_dbm{-80};
    // Hysteresis prevents a one-dBm oscillation from changing evidence.
    std::int32_t wifi_signal_weak_recover_dbm{-67};
    std::int32_t wifi_signal_very_weak_recover_dbm{-77};
    std::uint32_t wifi_tx_bitrate_low_kbps{6'000};
};

class RootCauseEngine {
public:
    RootCauseEngine(EventBus& bus, const Clock& clock, RootCausePolicy policy = {});
    ~RootCauseEngine();

    RootCauseEngine(const RootCauseEngine&) = delete;
    RootCauseEngine& operator=(const RootCauseEngine&) = delete;

    bool start();
    void stop() noexcept;
    bool running() const noexcept;

    // Public deterministic entry point used by replay and unit tests.  The
    // EventBus subscription calls this same function.
    bool process(const NetworkEvent& event);
    bool handle(const NetworkEvent& event) { return process(event); }

    std::vector<RootCauseHypothesisObservation> listActiveHypotheses() const;
    std::vector<RootCauseHypothesisObservation> activeHypotheses() const {
        return listActiveHypotheses();
    }
    std::optional<RootCauseHypothesisObservation> getHypothesis(
        RootCauseHypothesisId id) const;
    std::vector<RootCauseHypothesisObservation> recentResolvedHypotheses() const;
    const RootCausePolicy& policy() const noexcept { return policy_; }

private:
    struct CandidateKey {
        RootCauseType type{RootCauseType::InsufficientEvidence};
        RootCauseScope scope{NetnsId{}};
        auto operator<=>(const CandidateKey&) const = default;
    };

    struct Candidate {
        RootCauseHypothesisObservation observation;
    };

    struct ProbeCache {
        std::optional<NetworkEvent> gateway;
        std::optional<NetworkEvent> remote;
    };

    enum class WifiSignalCategory : std::uint8_t { None, Normal, Weak, VeryWeak };

    struct WifiCacheKey {
        NetnsId netns;
        InterfaceId interface;
        auto operator<=>(const WifiCacheKey&) const = default;
    };

    struct WifiCacheEntry {
        NetworkEvent event;
        WifiSignalCategory signal_category{WifiSignalCategory::None};
    };

    using IncidentMap = std::map<IncidentId, IncidentObservation>;

    static bool sameContent(const RootCauseHypothesisObservation& left,
                            const RootCauseHypothesisObservation& right);
    static bool hasIncident(const IncidentMap& incidents, IncidentType type,
                            const RootCauseScope& scope);
    static RootCauseScope incidentScope(const IncidentObservation& incident);
    static bool scopeMatches(const IncidentScope& left, const RootCauseScope& right);
    static RootCauseEvidenceKind evidenceKind(IncidentType type);
    static bool evidenceEquivalent(const RootCauseEvidence& left,
                                   const RootCauseEvidence& right);

    void addEvidence(std::vector<RootCauseEvidence>& destination,
                     RootCauseEvidence evidence) const;
    RootCauseEvidence incidentEvidence(const IncidentObservation& incident,
                                       RootCauseEvidenceRole role,
                                       RootCauseEvidenceKind kind) const;
    RootCauseEvidence observationEvidence(const NetworkEvent& event,
                                          RootCauseScope scope,
                                          RootCauseEvidenceRole role,
                                          RootCauseEvidenceKind kind,
                                          RootCauseEvidenceCapability capability,
                                          std::string provenance) const;
    RootCauseEvidence probeEvidence(const NetworkEvent& event,
                                    RootCauseScope scope,
                                    RootCauseEvidenceRole role,
                                    RootCauseEvidenceKind kind,
                                    RootCauseEvidenceCapability capability,
                                    std::string provenance) const;
    std::map<CandidateKey, Candidate> buildCandidatesLocked() const;
    void reconcileLocked(RealtimeTime now,
                         std::vector<RootCauseHypothesisObservation>& emissions);
    void publish(const RootCauseHypothesisObservation& hypothesis);

    EventBus& bus_;
    const Clock& clock_;
    RootCausePolicy policy_;
    mutable std::mutex mutex_;
    IncidentMap active_incidents_;
    std::map<SocketId, NetworkEvent> route_events_;
    std::map<NetnsId, NetworkEvent> uplink_events_;
    std::map<NetnsId, ProbeCache> probe_events_;
    std::map<WifiCacheKey, WifiCacheEntry> wifi_events_;
    std::map<CandidateKey, RootCauseHypothesisObservation> active_;
    std::deque<RootCauseHypothesisObservation> resolved_;
    std::uint64_t next_occurrence_{1};
    EventBus::Subscription subscription_;
    bool running_{false};
    std::atomic<std::uint64_t> next_event_id_{1};
};

}  // namespace weaknet_dbus::v2
