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
    TcpIntervalMetrics, SocketRouteContextObservation>;

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
