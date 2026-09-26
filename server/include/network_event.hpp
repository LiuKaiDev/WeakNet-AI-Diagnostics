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
};

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
    CollectorHealthObservation, SocketObservation>;

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
