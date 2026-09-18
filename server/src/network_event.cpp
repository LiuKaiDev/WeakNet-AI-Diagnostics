#include "network_event.hpp"

#include <sys/stat.h>

#include <sstream>
#include <type_traits>

namespace weaknet_dbus::v2 {

EventKind NetworkEvent::kindFor(const NetworkEventPayload& payload) noexcept {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, InterfaceObservation>) return EventKind::InterfaceObservation;
        if constexpr (std::is_same_v<T, UplinkObservation>) return EventKind::UplinkObservation;
        if constexpr (std::is_same_v<T, ProbeRttObservation>) return EventKind::ProbeRttMetric;
        if constexpr (std::is_same_v<T, TcpLossObservation>) return EventKind::TcpLossMetric;
        if constexpr (std::is_same_v<T, TrafficObservation>) return EventKind::TrafficMetric;
        if constexpr (std::is_same_v<T, WifiRssiObservation>) return EventKind::WifiRssiMetric;
        return EventKind::CollectorHealth;
    }, payload);
}

NetworkEvent::NetworkEvent(NetworkEventHeader header, NetworkEventPayload payload)
    : header_(std::move(header)), payload_(std::move(payload)) {
    if (header_.schema_version != kNetworkEventSchemaVersion) {
        throw std::invalid_argument("unsupported NetworkEvent schema version");
    }
    if (header_.event_id.value == 0) {
        throw std::invalid_argument("NetworkEvent event_id must be nonzero");
    }
    if (header_.netns.inode == 0) {
        throw std::invalid_argument("NetworkEvent requires a network namespace identity");
    }
    if (header_.interface && header_.interface->ifindex == 0) {
        throw std::invalid_argument("NetworkEvent interface ifindex must be nonzero");
    }
    if (header_.kind != kindFor(payload_)) {
        throw std::invalid_argument("NetworkEvent header kind does not match payload");
    }
}

bool NetworkEvent::replaceable() const noexcept {
    switch (header_.kind) {
        case EventKind::ProbeRttMetric:
        case EventKind::TcpLossMetric:
        case EventKind::TrafficMetric:
        case EventKind::WifiRssiMetric:
            return true;
        case EventKind::InterfaceObservation:
        case EventKind::UplinkObservation:
        case EventKind::CollectorHealth:
            return false;
    }
    return false;
}

std::string NetworkEvent::coalescingKey() const {
    if (!replaceable()) return {};
    std::ostringstream key;
    key << static_cast<unsigned>(header_.kind) << ':'
        << static_cast<unsigned>(header_.source) << ':'
        << header_.netns.device << ':' << header_.netns.inode << ':';
    if (header_.interface) key << 'i' << header_.interface->ifindex;
    else key << "i-";
    key << ':';
    if (header_.socket) key << 's' << header_.socket->cookie << ':' << header_.socket->generation;
    else key << "s-";
    return key.str();
}

NetworkEvent NetworkEvent::withSequence(EventSequence sequence) const {
    auto header = header_;
    header.sequence = sequence;
    return NetworkEvent(std::move(header), payload_);
}

std::optional<NetnsId> currentNetworkNamespace(std::string* error) noexcept {
    struct stat metadata {};
    if (::stat("/proc/self/ns/net", &metadata) != 0) {
        if (error) *error = "stat(/proc/self/ns/net) failed";
        return std::nullopt;
    }
    return NetnsId{static_cast<std::uint64_t>(metadata.st_dev),
                   static_cast<std::uint64_t>(metadata.st_ino)};
}

}  // namespace weaknet_dbus::v2
