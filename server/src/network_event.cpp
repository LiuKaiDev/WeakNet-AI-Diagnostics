#include "network_event.hpp"

#include <sys/stat.h>

#include <arpa/inet.h>

#include <sstream>
#include <type_traits>

namespace weaknet_dbus::v2 {

const char* rootCauseEvidenceKindName(RootCauseEvidenceKind kind) noexcept {
    switch (kind) {
        case RootCauseEvidenceKind::ActiveIncident: return "ActiveIncident";
        case RootCauseEvidenceKind::AuthoritativeNoUsableUplink: return "AuthoritativeNoUsableUplink";
        case RootCauseEvidenceKind::AuthoritativeRouteUnavailable: return "AuthoritativeRouteUnavailable";
        case RootCauseEvidenceKind::SocketRouteConflict: return "SocketRouteConflict";
        case RootCauseEvidenceKind::ModeledRouteAvailable: return "ModeledRouteAvailable";
        case RootCauseEvidenceKind::UsableUplinkAvailable: return "UsableUplinkAvailable";
        case RootCauseEvidenceKind::HighTcpRtt: return "HighTcpRtt";
        case RootCauseEvidenceKind::ElevatedTcpRetransmission: return "ElevatedTcpRetransmission";
        case RootCauseEvidenceKind::GatewayProbeUnavailable: return "GatewayProbeUnavailable";
        case RootCauseEvidenceKind::RemoteProbeUnavailable: return "RemoteProbeUnavailable";
        case RootCauseEvidenceKind::GatewayLatencyUnavailable: return "GatewayLatencyUnavailable";
        case RootCauseEvidenceKind::WifiLinkQualityUnavailable: return "WifiLinkQualityUnavailable";
        case RootCauseEvidenceKind::ActiveRemoteProbeUnavailable: return "ActiveRemoteProbeUnavailable";
        case RootCauseEvidenceKind::PolicyRoutingStateUnavailable: return "PolicyRoutingStateUnavailable";
        case RootCauseEvidenceKind::IpRuleNotModeled: return "IpRuleNotModeled";
        case RootCauseEvidenceKind::VrfStateUnavailable: return "VrfStateUnavailable";
        case RootCauseEvidenceKind::TopologyUnavailable: return "TopologyUnavailable";
        case RootCauseEvidenceKind::AttributionAmbiguous: return "AttributionAmbiguous";
        case RootCauseEvidenceKind::GatewayProbeReachable: return "GatewayProbeReachable";
        case RootCauseEvidenceKind::GatewayProbeHighRtt: return "GatewayProbeHighRtt";
        case RootCauseEvidenceKind::GatewayProbeTimeout: return "GatewayProbeTimeout";
        case RootCauseEvidenceKind::RemoteProbeReachable: return "RemoteProbeReachable";
        case RootCauseEvidenceKind::RemoteProbeHighRtt: return "RemoteProbeHighRtt";
        case RootCauseEvidenceKind::RemoteProbeTimeout: return "RemoteProbeTimeout";
        case RootCauseEvidenceKind::ProbeEvidenceStale: return "ProbeEvidenceStale";
        case RootCauseEvidenceKind::SocketEndpointSpecificProbeUnavailable:
            return "SocketEndpointSpecificProbeUnavailable";
        case RootCauseEvidenceKind::HopLevelEvidenceUnavailable: return "HopLevelEvidenceUnavailable";
        case RootCauseEvidenceKind::GatewayDeviceHealthUnavailable: return "GatewayDeviceHealthUnavailable";
    }
    return "Unknown";
}

std::string SocketEndpoint::toString() const {
    char buffer[INET6_ADDRSTRLEN]{};
    if (!inet_ntop(family, address.data(), buffer, sizeof(buffer))) return {};
    if (family == AF_INET6) return "[" + std::string(buffer) + "]:" + std::to_string(port);
    return std::string(buffer) + ":" + std::to_string(port);
}

EventKind NetworkEvent::kindFor(const NetworkEventPayload& payload) noexcept {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, LinkObservation>) return EventKind::LinkObservation;
        if constexpr (std::is_same_v<T, AddressObservation>) return EventKind::AddressObservation;
        if constexpr (std::is_same_v<T, RouteObservation>) return EventKind::RouteObservation;
        if constexpr (std::is_same_v<T, InterfaceObservation>) return EventKind::InterfaceObservation;
        if constexpr (std::is_same_v<T, UplinkObservation>) return EventKind::UplinkObservation;
        if constexpr (std::is_same_v<T, ProbeRttObservation>) return EventKind::ProbeRttMetric;
        if constexpr (std::is_same_v<T, TcpLossObservation>) return EventKind::TcpLossMetric;
        if constexpr (std::is_same_v<T, TrafficObservation>) return EventKind::TrafficMetric;
        if constexpr (std::is_same_v<T, WifiRssiObservation>) return EventKind::WifiRssiMetric;
        if constexpr (std::is_same_v<T, SocketObservation>) return EventKind::SocketObservation;
        if constexpr (std::is_same_v<T, TcpInfoObservation>) return EventKind::TcpInfoObservation;
        if constexpr (std::is_same_v<T, TcpIntervalMetrics>) return EventKind::TcpIntervalMetric;
        if constexpr (std::is_same_v<T, SocketRouteContextObservation>) return EventKind::SocketRouteObservation;
        if constexpr (std::is_same_v<T, IncidentObservation>) return EventKind::IncidentObservation;
        if constexpr (std::is_same_v<T, RootCauseHypothesisObservation>)
            return EventKind::RootCauseHypothesisObservation;
        if constexpr (std::is_same_v<T, ProbeObservation>) return EventKind::ProbeObservation;
        if constexpr (std::is_same_v<T, WifiObservation>) return EventKind::WifiObservation;
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
    if (header_.socket && header_.socket->netns != header_.netns) {
        throw std::invalid_argument("NetworkEvent socket identity namespace mismatch");
    }
    if (header_.interface && header_.interface->ifindex == 0) {
        throw std::invalid_argument("NetworkEvent interface ifindex must be nonzero");
    }
    if (const auto* socket = std::get_if<SocketObservation>(&payload_)) {
        if (socket->netns != header_.netns || socket->id.netns != header_.netns ||
            socket->tuple.netns != header_.netns ||
            (header_.socket && *header_.socket != socket->id)) {
            throw std::invalid_argument("SocketObservation namespace/identity mismatch");
        }
    }
    if (const auto* tcp = std::get_if<TcpInfoObservation>(&payload_)) {
        if (tcp->netns != header_.netns || tcp->id.netns != header_.netns ||
            tcp->tuple.netns != header_.netns ||
            (header_.socket && *header_.socket != tcp->id)) {
            throw std::invalid_argument("TcpInfoObservation namespace/identity mismatch");
        }
    }
    if (const auto* interval = std::get_if<TcpIntervalMetrics>(&payload_)) {
        if (interval->id.netns != header_.netns ||
            (header_.socket && *header_.socket != interval->id)) {
            throw std::invalid_argument("TcpIntervalMetrics namespace/identity mismatch");
        }
    }
    if (const auto* route = std::get_if<SocketRouteContextObservation>(&payload_)) {
        if (route->netns != header_.netns || route->socket_id.netns != header_.netns ||
            (header_.socket && *header_.socket != route->socket_id)) {
            throw std::invalid_argument("SocketRouteContextObservation namespace/identity mismatch");
        }
    }
    if (const auto* incident = std::get_if<IncidentObservation>(&payload_)) {
        const auto scope_matches = std::visit([&](const auto& scope) {
            using Scope = std::decay_t<decltype(scope)>;
            if constexpr (std::is_same_v<Scope, SocketId>) return scope.netns == header_.netns;
            if constexpr (std::is_same_v<Scope, NetnsId>) return scope == header_.netns;
            return true;  // InterfaceId is namespace-scoped by the header.
        }, incident->scope);
        if (!scope_matches) throw std::invalid_argument("IncidentObservation scope namespace mismatch");
    }
    if (const auto* hypothesis = std::get_if<RootCauseHypothesisObservation>(&payload_)) {
        const auto scope_matches = std::visit([&](const auto& scope) {
            using Scope = std::decay_t<decltype(scope)>;
            if constexpr (std::is_same_v<Scope, SocketId>) return scope.netns == header_.netns;
            if constexpr (std::is_same_v<Scope, NetnsId>) return scope == header_.netns;
            return true;
        }, hypothesis->scope);
        if (!scope_matches) throw std::invalid_argument("RootCauseHypothesis scope namespace mismatch");
    }
    if (const auto* probe = std::get_if<ProbeObservation>(&payload_)) {
        if (probe->netns != header_.netns || probe->target.netns != header_.netns ||
            (header_.interface && probe->target.interface &&
             *header_.interface != *probe->target.interface))
            throw std::invalid_argument("ProbeObservation namespace/identity mismatch");
    }
    if (const auto* wifi = std::get_if<WifiObservation>(&payload_)) {
        if (wifi->netns != header_.netns ||
            (wifi->interface && wifi->interface->ifindex == 0) ||
            header_.interface != wifi->interface ||
            header_.source != EventSource::WifiCollector ||
            wifi->source != EventSource::WifiCollector) {
            throw std::invalid_argument("WifiObservation namespace/identity mismatch");
        }
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
        case EventKind::LinkObservation:
        case EventKind::AddressObservation:
        case EventKind::RouteObservation:
        case EventKind::InterfaceObservation:
        case EventKind::UplinkObservation:
        case EventKind::CollectorHealth:
        case EventKind::SocketObservation:
        case EventKind::TcpInfoObservation:
        case EventKind::TcpIntervalMetric:
        case EventKind::SocketRouteObservation:
        case EventKind::IncidentObservation:
        case EventKind::RootCauseHypothesisObservation:
        case EventKind::ProbeObservation:
        case EventKind::WifiObservation:
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
    if (header_.socket) {
        key << 's' << header_.socket->netns.device << ':' << header_.socket->netns.inode << ':';
        if (header_.socket->cookie) key << header_.socket->cookie->value;
        else key << '-';
        key << ':' << header_.socket->generation.value;
    } else key << "s-";
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
