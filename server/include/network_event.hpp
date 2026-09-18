#pragma once

#include <chrono>
#include <compare>
#include <cstdint>
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

struct SocketId {
    std::uint64_t cookie{};
    std::uint64_t generation{};
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
};

struct Status {
    StatusCode code{StatusCode::Unavailable};
    std::string detail;
    auto operator<=>(const Status&) const = default;
};

enum class EventKind : std::uint8_t {
    InterfaceObservation,
    UplinkObservation,
    ProbeRttMetric,
    TcpLossMetric,
    TrafficMetric,
    WifiRssiMetric,
    CollectorHealth,
};

enum class EventSource : std::uint8_t {
    V1InterfaceSnapshot,
    V1UplinkMonitor,
    V1RttMonitor,
    V1TcpLossMonitor,
    V1TrafficMonitor,
    V1WifiMonitor,
    Runtime,
};

struct InterfaceObservation {
    std::string interface_name;
    bool present{true};
    bool link_up{false};
};

struct UplinkObservation {
    std::string interface_name;
    std::uint32_t method_flags{};
    bool selected{false};
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
    InterfaceObservation, UplinkObservation, ProbeRttObservation,
    TcpLossObservation, TrafficObservation, WifiRssiObservation,
    CollectorHealthObservation>;

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
        const auto first = std::hash<std::uint64_t>{}(id.cookie);
        return first ^ (std::hash<std::uint64_t>{}(id.generation) + 0x9e3779b9U + (first << 6U) + (first >> 2U));
    }
};
