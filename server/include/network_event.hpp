#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <type_traits>

#include "clock.hpp"

namespace weaknet_dbus::v2 {

inline constexpr std::uint16_t kNetworkEventSchemaVersion = 1;

struct EventId {
    std::uint64_t value{};
    auto operator<=>(const EventId&) const = default;
};

struct EventSequence {
    std::uint64_t value{};
    auto operator<=>(const EventSequence&) const = default;
};

struct NetnsId {
    std::uint64_t device{};
    std::uint64_t inode{};
    auto operator<=>(const NetnsId&) const = default;
};

// Linux ifindex is stable within one NetnsId for the lifetime of an interface.
// The observed name is metadata and deliberately not part of identity.
struct InterfaceId {
    std::uint32_t ifindex{};
    std::string observed_name;
    bool operator==(const InterfaceId& other) const noexcept {
        return ifindex == other.ifindex;
    }
    std::strong_ordering operator<=>(const InterfaceId& other) const noexcept {
        return ifindex <=> other.ifindex;
    }
};

enum class SocketProtocol : std::uint8_t {
    Tcp = 6,
};

struct TcpSocketState {
    std::uint8_t raw{};
    bool known() const noexcept { return raw >= 1 && raw <= 12; }
    auto operator<=>(const TcpSocketState&) const = default;
};

struct SocketEndpoint {
    std::uint8_t family{};
    std::array<std::uint8_t, 16> address{};
    std::uint16_t port{};

    auto operator<=>(const SocketEndpoint&) const = default;
    std::string toString() const;
};

struct SocketTuple {
    NetnsId netns;
    SocketProtocol protocol{SocketProtocol::Tcp};
    std::uint8_t family{};
    SocketEndpoint local;
    SocketEndpoint remote;

    auto operator<=>(const SocketTuple&) const = default;
};

struct KernelSocketCookie {
    std::uint64_t value{};
    auto operator<=>(const KernelSocketCookie&) const = default;
};

struct SocketGeneration {
    std::uint64_t value{};
    auto operator<=>(const SocketGeneration&) const = default;
};

struct SocketId {
    NetnsId netns;
    std::optional<KernelSocketCookie> cookie;
    SocketGeneration generation;

    SocketId() = default;
    SocketId(NetnsId namespace_id, std::optional<KernelSocketCookie> socket_cookie,
             SocketGeneration lifecycle_generation)
        : netns(namespace_id), cookie(socket_cookie), generation(lifecycle_generation) {}

    auto operator<=>(const SocketId&) const = default;
};

enum class Validity : std::uint8_t { Valid, Unavailable, Stale, Partial, Reset };

enum class StatusCode : std::uint16_t {
    Unavailable,
    AmbiguousV1Scope,
    CollectorDegraded,
    CounterReset,
    InvalidSchema,
    InvalidPayload,
    PolicyEvidence,
};

struct Status {
    StatusCode code{StatusCode::Unavailable};
    std::string detail;
    auto operator<=>(const Status&) const = default;
};

enum class EventKind : std::uint8_t {
    LinkObservation,
    AddressObservation,
    RouteObservation,
    InterfaceObservation,
    UplinkObservation,
    ProbeRttMetric,
    TcpLossMetric,
    TrafficMetric,
    WifiRssiMetric,
    CollectorHealth,
    SocketObservation,
    TcpInfoObservation,
    TcpIntervalMetric,
    SocketRouteObservation,
    IncidentObservation,
    RootCauseHypothesisObservation,
    ProbeObservation,
};

enum class EventSource : std::uint8_t {
    NetlinkCollector,
    V1InterfaceSnapshot,
    V1UplinkMonitor,
    V1RttMonitor,
    V1TcpLossMonitor,
    V1TrafficMonitor,
    V1WifiMonitor,
    Runtime,
    SocketTracker,
    IncidentEngine,
    RootCauseEngine,
    ActiveProbe,
};

enum class SocketLifecycleState : std::uint8_t {
    Active,
    Closed,
};

struct TcpInfoRaw {
    // Gauges. Values retain Linux tcp_info units (microseconds/counts).
    std::optional<std::uint32_t> rtt_us;
    std::optional<std::uint32_t> rttvar_us;
    std::optional<std::uint32_t> snd_cwnd;
    std::optional<std::uint32_t> snd_ssthresh;
    std::optional<std::uint32_t> unacked;
    std::optional<std::uint32_t> reordering;
    std::optional<std::uint32_t> rcv_space;

    // Cumulative counters. They are never treated as gauges.
    std::optional<std::uint64_t> bytes_acked;
    std::optional<std::uint64_t> bytes_received;
    std::optional<std::uint32_t> segs_out;
    std::optional<std::uint32_t> segs_in;
    std::optional<std::uint32_t> data_segs_out;
    std::optional<std::uint32_t> data_segs_in;
    std::optional<std::uint32_t> total_retrans;
    std::optional<std::uint32_t> lost;
    std::optional<std::uint32_t> retrans;
    std::optional<std::uint32_t> delivered;
    std::optional<std::uint32_t> delivered_ce;
};

struct SocketObservation {
    SocketId id;
    NetnsId netns;
    SocketTuple tuple;
    RealtimeTime observed_at{};
    MonotonicTime monotonic_at{};
    EventSource source{EventSource::SocketTracker};
    Validity validity{Validity::Valid};
    SocketLifecycleState lifecycle{SocketLifecycleState::Active};
    bool present{true};
    TcpSocketState tcp_state{};
    std::optional<std::uint32_t> diag_ifindex;
    std::optional<TcpInfoRaw> tcp_info;
    bool tcp_info_malformed{false};
};

struct TcpInfoObservation {
    SocketId id;
    NetnsId netns;
    SocketTuple tuple;
    TcpSocketState tcp_state{};
    std::optional<TcpInfoRaw> raw;
    RealtimeTime observed_at{};
    MonotonicTime monotonic_at{};
    Validity validity{Validity::Unavailable};
    bool malformed{false};
};

enum class TcpMetricUnavailableReason : std::uint8_t {
    NoPreviousSample,
    IdentityChanged,
    GenerationChanged,
    FieldUnavailable,
    CounterReset,
    NonIncreasingTimestamp,
    ZeroDenominator,
    PartialObservation,
};

struct TcpIntervalMetrics {
    SocketId id;
    MonotonicTime interval_start{};
    MonotonicTime interval_end{};
    double elapsed_seconds{};
    std::optional<std::uint32_t> rtt_us;
    std::optional<std::uint32_t> rttvar_us;
    std::optional<std::uint32_t> snd_cwnd;
    std::optional<std::uint32_t> snd_ssthresh;
    std::optional<double> tx_acked_bytes_per_sec;
    std::optional<double> rx_bytes_per_sec;
    std::optional<double> segs_out_per_sec;
    std::optional<double> segs_in_per_sec;
    std::optional<double> data_segs_out_per_sec;
    std::optional<double> data_segs_in_per_sec;
    std::optional<std::uint64_t> delta_total_retrans;
    std::optional<std::uint64_t> delta_data_segs_out;
    std::optional<double> retransmission_segment_ratio;
    Validity validity{Validity::Valid};
    std::optional<TcpMetricUnavailableReason> unavailable_reason;
};

enum class RouteAttributionStatus : std::uint8_t {
    Available,
    Partial,
    Ambiguous,
    Unavailable,
};

enum class RouteAttributionReason : std::uint64_t {
    None = 0,
    ExactModeledMatch = 1ULL << 0,
    MultipathWithoutFlowHash = 1ULL << 1,
    DiagIfindexAgrees = 1ULL << 2,
    DiagIfindexConflicts = 1ULL << 3,
    DiagIfindexUnavailable = 1ULL << 4,
    TopologyNonAuthoritative = 1ULL << 5,
    TopologyPartial = 1ULL << 6,
    TopologyDegraded = 1ULL << 15,
    NoModeledRoute = 1ULL << 7,
    PolicyRoutingNotModeled = 1ULL << 8,
    AmbiguousCandidate = 1ULL << 9,
    GatewayEvidence = 1ULL << 10,
    OnLinkEvidence = 1ULL << 11,
    LocalRoute = 1ULL << 12,
    LocalAddressAgrees = 1ULL << 13,
    LocalAddressConflicts = 1ULL << 14,
    MissingOutputInterface = 1ULL << 16,
};

constexpr RouteAttributionReason operator|(RouteAttributionReason left,
                                            RouteAttributionReason right) noexcept {
    return static_cast<RouteAttributionReason>(static_cast<std::uint64_t>(left) |
                                                static_cast<std::uint64_t>(right));
}
constexpr RouteAttributionReason& operator|=(RouteAttributionReason& left,
                                              RouteAttributionReason right) noexcept {
    left = left | right;
    return left;
}

enum class SelectedUplinkRelationship : std::uint8_t {
    MatchesSelectedUplink,
    DifferentFromSelectedUplink,
    PossibleSelectedUplink,
    NonUplinkLocal,
    Unknown,
};

struct MatchedRouteSummary {
    std::string identity;
    std::uint8_t family{};
    std::uint8_t prefix_length{};
    std::uint32_t table{};
    std::uint32_t priority{};
    std::uint8_t type{};
    std::optional<std::array<std::uint8_t, 16>> gateway;
    std::optional<std::array<std::uint8_t, 16>> preferred_source;
    std::vector<std::uint32_t> nexthop_ifindices;
    std::vector<std::array<std::uint8_t, 16>> nexthop_gateways;
    bool on_link{false};
    bool multipath{false};
};

struct SocketRouteContextObservation {
    SocketId socket_id;
    NetnsId netns;
    SocketEndpoint destination;
    std::optional<MatchedRouteSummary> route;
    std::vector<InterfaceId> possible_interfaces;
    std::optional<InterfaceId> selected_interface;
    SelectedUplinkRelationship uplink_relationship{SelectedUplinkRelationship::Unknown};
    std::optional<std::uint32_t> diag_ifindex;
    RouteAttributionStatus attribution_status{RouteAttributionStatus::Unavailable};
    RouteAttributionReason reasons{RouteAttributionReason::None};
    std::uint64_t topology_generation{};
    bool topology_authoritative{false};
};

enum class IncidentType : std::uint8_t {
    HighTcpRtt,
    ElevatedTcpRetransmission,
    RouteUnavailable,
    UplinkUnavailable,
    SocketRouteConflict,
};

enum class IncidentState : std::uint8_t {
    Pending,
    Active,
    Resolved,
};

enum class IncidentSeverity : std::uint8_t {
    Info,
    Warning,
    Critical,
};

enum class IncidentEvidenceUnit : std::uint8_t {
    None,
    Microseconds,
    Ratio,
    Count,
};

enum class IncidentEvidenceCompleteness : std::uint8_t {
    Complete,
    Partial,
    Unavailable,
};

enum class IncidentConfidence : std::uint8_t {
    Unknown,
    Partial,
    High,
};

// Scope is typed identity, never a user-facing string.  A namespace scope is
// used for the modeled uplink rule; socket-scoped rules retain full
// generation identity so tuple reuse cannot transfer an incident.
using IncidentScope = std::variant<SocketId, InterfaceId, NetnsId>;

struct IncidentId {
    std::uint64_t value{};
    auto operator<=>(const IncidentId&) const = default;
};

struct IncidentEvidenceValue {
    std::variant<std::uint64_t, double> value{std::uint64_t{}};
    auto operator<=>(const IncidentEvidenceValue&) const = default;
};

struct IncidentEvidence {
    EventKind source_kind{EventKind::CollectorHealth};
    EventSource source{EventSource::Runtime};
    IncidentScope scope{NetnsId{}};
    RealtimeTime observed_at{};
    MonotonicTime monotonic_at{};
    std::optional<IncidentEvidenceValue> value;
    IncidentEvidenceUnit unit{IncidentEvidenceUnit::None};
    Validity validity{Validity::Unavailable};
    std::string condition;
};

struct IncidentObservation {
    IncidentId id;
    IncidentType type{IncidentType::HighTcpRtt};
    IncidentScope scope{NetnsId{}};
    IncidentState state{IncidentState::Active};
    IncidentSeverity severity{IncidentSeverity::Warning};
    IncidentConfidence confidence{IncidentConfidence::Unknown};
    RealtimeTime opened_at{};
    MonotonicTime opened_monotonic_at{};
    RealtimeTime last_updated_at{};
    MonotonicTime last_updated_monotonic_at{};
    std::optional<RealtimeTime> resolved_at;
    std::optional<MonotonicTime> resolved_monotonic_at;
    std::vector<IncidentEvidence> evidence;
    std::string condition;
    std::optional<std::string> resolution_condition;
    Validity validity{Validity::Valid};
    IncidentEvidenceCompleteness evidence_completeness{IncidentEvidenceCompleteness::Complete};
    EventSource source{EventSource::IncidentEngine};
};

enum class RootCauseType : std::uint8_t {
    UplinkAvailabilityProblem,
    LocalRoutingProblem,
    NetworkPathDegradation,
    RemoteOrUpstreamDegradation,
    LocalLinkSuspected,
    InsufficientEvidence,
};

using RootCauseScope = std::variant<SocketId, InterfaceId, NetnsId>;

struct RootCauseHypothesisId {
    RootCauseType type{RootCauseType::InsufficientEvidence};
    RootCauseScope scope{NetnsId{}};
    std::uint64_t occurrence{};
    auto operator<=>(const RootCauseHypothesisId&) const = default;
};

enum class HypothesisState : std::uint8_t { Active, Resolved };
enum class HypothesisConfidence : std::uint8_t { Low, Medium, High };
enum class RootCauseEvidenceRole : std::uint8_t { Supporting, Contradicting, Missing };

enum class RootCauseEvidenceKind : std::uint8_t {
    ActiveIncident,
    AuthoritativeNoUsableUplink,
    AuthoritativeRouteUnavailable,
    SocketRouteConflict,
    ModeledRouteAvailable,
    UsableUplinkAvailable,
    HighTcpRtt,
    ElevatedTcpRetransmission,
    GatewayProbeUnavailable,
    RemoteProbeUnavailable,
    GatewayLatencyUnavailable,
    WifiLinkQualityUnavailable,
    ActiveRemoteProbeUnavailable,
    PolicyRoutingStateUnavailable,
    IpRuleNotModeled,
    VrfStateUnavailable,
    TopologyUnavailable,
    AttributionAmbiguous,
};

enum class RootCauseEvidenceUnit : std::uint8_t {
    None,
    Microseconds,
    Ratio,
    Count,
};

enum class RootCauseEvidenceCapability : std::uint8_t {
    Available,
    Unavailable,
    NotImplemented,
};

struct RootCauseEvidence {
    std::optional<IncidentId> incident;
    EventKind source_kind{EventKind::CollectorHealth};
    EventSource source{EventSource::Runtime};
    RootCauseScope scope{NetnsId{}};
    RealtimeTime observed_at{};
    MonotonicTime monotonic_at{};
    RootCauseEvidenceRole role{RootCauseEvidenceRole::Supporting};
    RootCauseEvidenceKind kind{RootCauseEvidenceKind::ActiveIncident};
    std::optional<IncidentEvidenceValue> value;
    RootCauseEvidenceUnit unit{RootCauseEvidenceUnit::None};
    Validity validity{Validity::Unavailable};
    RootCauseEvidenceCapability capability{RootCauseEvidenceCapability::Available};
    std::string provenance;
    auto operator<=>(const RootCauseEvidence&) const = default;
};

struct RootCauseHypothesisObservation {
    RootCauseHypothesisId id;
    RootCauseType type{RootCauseType::InsufficientEvidence};
    RootCauseScope scope{NetnsId{}};
    HypothesisState state{HypothesisState::Active};
    HypothesisConfidence confidence{HypothesisConfidence::Low};
    RealtimeTime opened_at{};
    RealtimeTime last_updated_at{};
    std::optional<RealtimeTime> resolved_at;
    std::vector<RootCauseEvidence> supporting_evidence;
    std::vector<RootCauseEvidence> contradicting_evidence;
    std::vector<RootCauseEvidence> missing_evidence;
    std::string reason_code;
    EventSource provenance{EventSource::RootCauseEngine};
    auto operator<=>(const RootCauseHypothesisObservation&) const = default;
};

enum class ProbeTargetKind : std::uint8_t { Gateway, Remote };
enum class ProbeStatus : std::uint8_t {
    Success,
    Timeout,
    Unreachable,
    TransportUnavailable,
    NoTarget,
    InvalidReply,
    Error,
};

struct ProbeTarget {
    ProbeTargetKind kind{ProbeTargetKind::Remote};
    NetnsId netns;
    std::uint8_t family{};
    std::array<std::uint8_t, 16> address{};
    std::optional<InterfaceId> interface;
    std::uint64_t generation{};
    std::string provenance;
    auto operator<=>(const ProbeTarget&) const = default;
};

struct ProbeObservation {
    ProbeTarget target;
    NetnsId netns;
    RealtimeTime observed_at{};
    MonotonicTime monotonic_at{};
    std::uint64_t sequence{};
    ProbeStatus status{ProbeStatus::Error};
    std::optional<std::uint64_t> rtt_us;
    Validity validity{Validity::Unavailable};
    EventSource source{EventSource::ActiveProbe};
    std::string transport;
    std::string failure_reason;
    auto operator<=>(const ProbeObservation&) const = default;
};

using TcpObservation = TcpInfoObservation;
using TcpIntervalObservation = TcpIntervalMetrics;

struct InterfaceObservation {
    std::string interface_name;
    bool present{true};
    bool link_up{false};
};

struct LinkObservation {
    std::uint32_t ifindex{};
    std::string interface_name;
    bool present{true};
    bool link_up{false};
    bool carrier_up{false};
};

struct AddressObservation {
    std::uint32_t ifindex{};
    std::uint8_t family{};
    std::uint8_t prefix_length{};
    std::string address;
    bool present{true};
};

struct RouteObservation {
    std::uint8_t family{};
    std::uint8_t prefix_length{};
    std::uint32_t table{};
    std::uint32_t priority{};
    std::uint32_t output_ifindex{};
    bool present{true};
    bool multipath{false};
    bool on_link{false};
};

struct UplinkObservation {
    std::string interface_name;
    std::uint32_t method_flags{};
    bool selected{false};
    std::string evidence;
};

struct ProbeRttObservation { std::optional<double> milliseconds; };

struct TcpLossObservation {
    std::optional<double> rate_percent;
    std::uint64_t sent_delta{};
    std::uint64_t retransmit_delta{};
    std::string v1_level;
};

struct TrafficObservation {
    std::optional<std::uint64_t> bytes_per_second;
    std::optional<std::uint64_t> packets_per_second;
    std::optional<std::uint64_t> active_flows;
};

struct WifiRssiObservation { std::optional<std::int32_t> dbm; };

enum class CollectorState : std::uint8_t {
    Starting, Running, Disabled, Degraded, Failed, Stopped
};

struct CollectorHealthObservation {
    std::string component;
    CollectorState state{CollectorState::Starting};
};

using NetworkEventPayload = std::variant<
    LinkObservation, AddressObservation, RouteObservation, InterfaceObservation, UplinkObservation, ProbeRttObservation,
    TcpLossObservation, TrafficObservation, WifiRssiObservation,
    CollectorHealthObservation, SocketObservation, TcpInfoObservation,
    TcpIntervalMetrics, SocketRouteContextObservation, IncidentObservation,
    RootCauseHypothesisObservation, ProbeObservation>;

struct NetworkEventHeader {
    std::uint16_t schema_version{kNetworkEventSchemaVersion};
    EventId event_id{};
    EventSequence sequence{};
    EventKind kind{EventKind::InterfaceObservation};
    EventSource source{EventSource::Runtime};
    RealtimeTime observed_at{};
    MonotonicTime monotonic_at{};
    NetnsId netns{};
    std::optional<InterfaceId> interface;
    std::optional<SocketId> socket;
    Validity validity{Validity::Unavailable};
    std::optional<Status> status;
};

class NetworkEvent {
public:
    NetworkEvent(NetworkEventHeader header, NetworkEventPayload payload);

    const NetworkEventHeader& header() const noexcept { return header_; }
    const NetworkEventPayload& payload() const noexcept { return payload_; }

    bool replaceable() const noexcept;
    std::string coalescingKey() const;
    NetworkEvent withSequence(EventSequence sequence) const;

    static EventKind kindFor(const NetworkEventPayload& payload) noexcept;

private:
    NetworkEventHeader header_;
    NetworkEventPayload payload_;
};

std::optional<NetnsId> currentNetworkNamespace(std::string* error = nullptr) noexcept;

}  // namespace weaknet_dbus::v2

template <> struct std::hash<weaknet_dbus::v2::EventId> {
    std::size_t operator()(weaknet_dbus::v2::EventId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
template <> struct std::hash<weaknet_dbus::v2::NetnsId> {
    std::size_t operator()(const weaknet_dbus::v2::NetnsId& id) const noexcept {
        const auto first = std::hash<std::uint64_t>{}(id.device);
        return first ^ (std::hash<std::uint64_t>{}(id.inode) + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
};
template <> struct std::hash<weaknet_dbus::v2::InterfaceId> {
    std::size_t operator()(const weaknet_dbus::v2::InterfaceId& id) const noexcept {
        return std::hash<std::uint32_t>{}(id.ifindex);
    }
};
template <> struct std::hash<weaknet_dbus::v2::SocketId> {
    std::size_t operator()(const weaknet_dbus::v2::SocketId& id) const noexcept {
        const auto first = std::hash<std::uint64_t>{}(id.netns.device);
        const auto second = std::hash<std::uint64_t>{}(id.netns.inode);
        const auto cookie = std::hash<std::uint64_t>{}(id.cookie ? id.cookie->value : 0);
        const auto generation = std::hash<std::uint64_t>{}(id.generation.value);
        return first ^ (second + 0x9e3779b9U + (first << 6U) + (first >> 2U)) ^
            (cookie + 0x9e3779b9U + (second << 6U) + (second >> 2U)) ^ generation;
    }
};
template <> struct std::hash<weaknet_dbus::v2::IncidentId> {
    std::size_t operator()(weaknet_dbus::v2::IncidentId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
template <> struct std::hash<weaknet_dbus::v2::RootCauseHypothesisId> {
    std::size_t operator()(const weaknet_dbus::v2::RootCauseHypothesisId& id) const noexcept {
        std::size_t result = std::hash<std::uint8_t>{}(static_cast<std::uint8_t>(id.type));
        std::visit([&](const auto& scope) {
            using Scope = std::decay_t<decltype(scope)>;
            std::size_t value = 0;
            if constexpr (std::is_same_v<Scope, weaknet_dbus::v2::SocketId>)
                value = std::hash<weaknet_dbus::v2::SocketId>{}(scope);
            else if constexpr (std::is_same_v<Scope, weaknet_dbus::v2::InterfaceId>)
                value = std::hash<weaknet_dbus::v2::InterfaceId>{}(scope);
            else value = std::hash<weaknet_dbus::v2::NetnsId>{}(scope);
            result ^= value + 0x9e3779b9U + (result << 6U) + (result >> 2U);
        }, id.scope);
        return result ^ (std::hash<std::uint64_t>{}(id.occurrence) +
                         0x9e3779b9U + (result << 6U) + (result >> 2U));
    }
};
