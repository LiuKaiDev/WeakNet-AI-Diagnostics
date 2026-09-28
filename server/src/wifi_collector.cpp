#include "wifi_collector.hpp"

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace weaknet_dbus::v2 {
namespace {

constexpr std::uint32_t kKernelSenderPid = 0;

const char* capabilityReason(WifiCapability capability) {
    switch (capability) {
        case WifiCapability::Available: return "available";
        case WifiCapability::NotWifi: return "selected interface is not a supported Wi-Fi client";
        case WifiCapability::NotAssociated: return "no station entry is present";
        case WifiCapability::Unsupported: return "nl80211 or driver field unsupported";
        case WifiCapability::PermissionDenied: return "permission denied by nl80211 transport";
        case WifiCapability::TransportUnavailable: return "Generic Netlink transport unavailable";
        case WifiCapability::Error: return "nl80211 query failed";
        case WifiCapability::NoTarget: return "authoritative Wi-Fi target unavailable";
    }
    return "unknown";
}

Validity validityFor(WifiCapability capability) {
    switch (capability) {
        case WifiCapability::Available:
        case WifiCapability::NotWifi:
        case WifiCapability::NotAssociated:
        case WifiCapability::Unsupported:
            return Validity::Partial;
        case WifiCapability::NoTarget:
        case WifiCapability::TransportUnavailable:
        case WifiCapability::PermissionDenied:
        case WifiCapability::Error:
            return Validity::Unavailable;
    }
    return Validity::Unavailable;
}

#if defined(__linux__)
void appendAttribute(std::vector<std::byte>& message, std::uint16_t type,
                     const void* data, std::size_t size) {
    const auto offset = message.size();
    const auto length = static_cast<std::uint16_t>(sizeof(nlattr) + size);
    message.resize(offset + NLA_ALIGN(length));
    auto* attribute = reinterpret_cast<nlattr*>(message.data() + offset);
    attribute->nla_len = length;
    attribute->nla_type = type;
    if (size != 0) std::memcpy(message.data() + offset + sizeof(nlattr), data, size);
    std::fill(message.begin() + static_cast<std::ptrdiff_t>(offset + length), message.end(),
              std::byte{});
}

template <typename T>
void appendScalar(std::vector<std::byte>& message, std::uint16_t type, T value) {
    appendAttribute(message, type, &value, sizeof(value));
}
#endif

}  // namespace

WifiCollector::WifiCollector(EventBus& bus, const Clock& clock, NetnsId netns,
                             WifiCollectorConfig config, WifiCollectorTestHooks hooks)
    : bus_(bus), clock_(clock), netns_(netns), config_(std::move(config)),
      hooks_(std::move(hooks)) {
    if (config_.interval < std::chrono::seconds(2))
        config_.interval = std::chrono::seconds(2);
    if (config_.timeout <= std::chrono::milliseconds::zero() ||
        config_.timeout > config_.interval)
        config_.timeout = std::min(std::chrono::milliseconds(1000),
                                   std::chrono::duration_cast<std::chrono::milliseconds>(config_.interval));
    if (config_.freshness <= std::chrono::seconds::zero()) config_.freshness = std::chrono::seconds(10);
}

WifiCollector::~WifiCollector() { stop(); }

bool WifiCollector::openSocket() {
#if defined(__linux__)
    if (hooks_.open_socket) {
        const int fd = hooks_.open_socket();
        if (fd < 0) return false;
        socket_fd_.store(fd);
        return true;
    }
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_GENERIC);
    if (fd < 0) return false;
    int receive_buffer = 256 * 1024;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) != 0) {
        ::close(fd);
        return false;
    }
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return false;
    }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd);
        return false;
    }
    socket_fd_.store(fd);
    return true;
#else
    return false;
#endif
}

void WifiCollector::closeSocket() noexcept {
#if defined(__linux__)
    const int fd = socket_fd_.exchange(-1);
    if (fd >= 0) ::close(fd);
#else
    socket_fd_.store(-1);
#endif
}

bool WifiCollector::start() {
    std::lock_guard lock(mutex_);
    if (running_) return true;
    if (!config_.enabled) {
        telemetry_.degraded = true;
        telemetry_.capability_reason = "Wi-Fi collector disabled";
    } else if (!hooks_.query && !openSocket()) {
        telemetry_.degraded = true;
        telemetry_.capability_reason = "Generic Netlink transport unavailable";
    }
    running_ = true;
    try {
        worker_ = std::jthread([this](std::stop_token token) { loop(token); });
    } catch (...) {
        running_ = false;
        closeSocket();
        return false;
    }
    return true;
}

void WifiCollector::stop() noexcept {
    std::jthread worker;
    {
        std::lock_guard lock(mutex_);
        if (!running_ && !worker_.joinable()) {
            closeSocket();
            return;
        }
        running_ = false;
        worker = std::move(worker_);
    }
    wake_.notify_all();
    if (worker.joinable()) {
        worker.request_stop();
        worker.join();
    }
    closeSocket();
}

void WifiCollector::updateTopology(const TopologySnapshot& snapshot,
                                   const UplinkSelection& uplink) {
    std::lock_guard lock(mutex_);
    std::optional<InterfaceId> next_target;
    std::uint64_t next_generation = snapshot.generation;
    if (snapshot.authoritative && uplink.interface &&
        uplink.validity != Validity::Unavailable) {
        next_target = uplink.interface;
    }
    if (target_ != next_target || target_generation_ != next_generation)
        latest_.reset();
    target_ = std::move(next_target);
    target_generation_ = next_generation;
    wake_.notify_all();
}

WifiCollectorTelemetry WifiCollector::telemetry() const {
    std::lock_guard lock(mutex_);
    return telemetry_;
}

std::optional<WifiObservation> WifiCollector::latest() const {
    std::lock_guard lock(mutex_);
    return latest_;
}

bool WifiCollector::latestFresh() const {
    std::lock_guard lock(mutex_);
    if (!latest_) return false;
    const auto now = clock_.monotonicNow();
    return latest_->monotonic_at <= now && now - latest_->monotonic_at <= config_.freshness;
}

bool WifiCollector::refreshForTests() { return refresh({}); }

void WifiCollector::loop(std::stop_token token) {
    while (!token.stop_requested()) {
        refresh(token);
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, token, config_.interval, [&] { return !running_; });
    }
}

bool WifiCollector::refresh(std::stop_token token) {
    std::optional<InterfaceId> target;
    std::uint64_t generation{};
    {
        std::lock_guard lock(mutex_);
        target = target_;
        generation = target_generation_;
    }
    if (!target) {
        WifiObservation observation;
        observation.netns = netns_;
        observation.capability = WifiCapability::NoTarget;
        observation.link_state = WifiLinkState::Unknown;
        observation.interface_generation = generation;
        observation.status = capabilityReason(observation.capability);
        publish(std::move(observation));
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        ++telemetry_.query_attempts;
    }
    QueryResult result;
    if (hooks_.query) {
        const auto queried = hooks_.query(*target, generation, token);
        if (queried) result.observation = *queried;
        else {
            result.observation.netns = netns_;
            result.observation.interface = *target;
            result.observation.interface_generation = generation;
            result.observation.capability = WifiCapability::Error;
            result.observation.link_state = WifiLinkState::Unknown;
            result.observation.status = "test query returned no observation";
        }
    } else {
        result = nativeQuery(*target, generation, token);
    }
    {
        std::lock_guard lock(mutex_);
        if (result.transport_status == WifiParseStatus::Timeout)
            ++telemetry_.timeouts;
        else if (result.transport_status == WifiParseStatus::Malformed ||
                 result.transport_status == WifiParseStatus::Truncated)
            ++telemetry_.malformed_replies;
        else if (result.transport_status == WifiParseStatus::WrongSequence ||
                 result.transport_status == WifiParseStatus::WrongSender ||
                 result.transport_status == WifiParseStatus::ErrorReply)
            ++telemetry_.transport_errors;
    }
    result.observation.netns = netns_;
    result.observation.interface = *target;
    result.observation.interface_generation = generation;
    result.observation.source = EventSource::WifiCollector;
    if (result.observation.status.empty())
        result.observation.status = capabilityReason(result.observation.capability);
    publish(std::move(result.observation));
    return true;
}

void WifiCollector::record(const WifiObservation& observation) {
    std::lock_guard lock(mutex_);
    latest_ = observation;
    switch (observation.capability) {
        case WifiCapability::Available:
            ++telemetry_.successful_observations;
            telemetry_.last_success_at = observation.observed_at;
            break;
        case WifiCapability::NotWifi: ++telemetry_.not_wifi_results; break;
        case WifiCapability::NotAssociated: ++telemetry_.not_associated_results; break;
        case WifiCapability::Unsupported: ++telemetry_.unsupported_results; break;
        case WifiCapability::PermissionDenied: ++telemetry_.permission_failures; break;
        case WifiCapability::TransportUnavailable: ++telemetry_.transport_errors; break;
        case WifiCapability::Error: ++telemetry_.transport_errors; break;
        case WifiCapability::NoTarget: ++telemetry_.no_target_results; break;
    }
    if (observation.capability != WifiCapability::Available)
        telemetry_.degraded = observation.capability != WifiCapability::NotWifi;
    telemetry_.capability_reason = observation.status;
}

void WifiCollector::publish(WifiObservation observation) {
    observation.observed_at = clock_.realtimeNow();
    observation.monotonic_at = clock_.monotonicNow();
    observation.source = EventSource::WifiCollector;
    observation.validity = validityFor(observation.capability);
    record(observation);
    NetworkEventHeader header{kNetworkEventSchemaVersion,
        EventId{next_event_id_.fetch_add(1)}, {}, EventKind::WifiObservation,
        EventSource::WifiCollector, observation.observed_at, observation.monotonic_at,
        netns_, observation.interface, std::nullopt, observation.validity, std::nullopt};
    (void)bus_.publish(NetworkEvent(std::move(header), std::move(observation)));
}

std::vector<std::byte> WifiCollector::makeRequest(
    std::uint16_t family, std::uint8_t command, std::uint16_t flags,
    std::uint32_t sequence, std::optional<std::uint32_t> ifindex,
    std::optional<std::string> family_name) const {
    std::vector<std::byte> message(NLMSG_LENGTH(sizeof(genlmsghdr)));
#if defined(__linux__)
    auto* header = reinterpret_cast<nlmsghdr*>(message.data());
    header->nlmsg_len = static_cast<std::uint16_t>(message.size());
    header->nlmsg_type = family;
    header->nlmsg_flags = flags;
    header->nlmsg_seq = sequence;
    header->nlmsg_pid = 0;
    auto* generic = static_cast<genlmsghdr*>(NLMSG_DATA(header));
    generic->cmd = command;
    generic->version = 0;
    generic->reserved = 0;
    if (ifindex) appendScalar(message, NL80211_ATTR_IFINDEX, *ifindex);
    if (family_name) appendAttribute(message, CTRL_ATTR_FAMILY_NAME,
                                      family_name->c_str(), family_name->size() + 1);
    header->nlmsg_len = static_cast<std::uint16_t>(message.size());
#else
    (void)family; (void)command; (void)flags; (void)sequence; (void)ifindex; (void)family_name;
#endif
    return message;
}

WifiParseStatus WifiCollector::requestAndReceive(const std::vector<std::byte>& request,
                                                 std::uint32_t sequence,
                                                 std::vector<std::byte>& response,
                                                 std::stop_token token) {
#if defined(__linux__)
    const int fd = socket_fd_.load();
    if (fd < 0) return WifiParseStatus::ErrorReply;
    sockaddr_nl destination{};
    destination.nl_family = AF_NETLINK;
    iovec iov{const_cast<std::byte*>(request.data()), request.size()};
    msghdr message{};
    message.msg_name = &destination;
    message.msg_namelen = sizeof(destination);
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    if (::sendmsg(fd, &message, 0) < 0) return WifiParseStatus::ErrorReply;
    const auto deadline = std::chrono::steady_clock::now() + config_.timeout;
    std::array<std::byte, 8192> buffer{};
    response.clear();
    while (!token.stop_requested()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero()) return WifiParseStatus::Timeout;
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1,
            static_cast<int>(std::min<std::int64_t>(remaining.count(), 50)));
        if (ready < 0) {
            if (errno == EINTR) continue;
            return WifiParseStatus::ErrorReply;
        }
        if (ready == 0) continue;
        sockaddr_nl sender{};
        iovec receive_iov{buffer.data(), buffer.size()};
        msghdr receive{};
        receive.msg_name = &sender;
        receive.msg_namelen = sizeof(sender);
        receive.msg_iov = &receive_iov;
        receive.msg_iovlen = 1;
        const auto received = ::recvmsg(fd, &receive, MSG_TRUNC);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            return WifiParseStatus::ErrorReply;
        }
        if ((receive.msg_flags & MSG_TRUNC) != 0 ||
            static_cast<std::size_t>(received) > buffer.size()) return WifiParseStatus::Truncated;
        if (sender.nl_pid != kKernelSenderPid) return WifiParseStatus::WrongSender;
        const auto datagram_size = static_cast<std::size_t>(received);
        std::size_t offset = 0;
        bool terminal = false;
        while (offset < datagram_size) {
            if (datagram_size - offset < sizeof(nlmsghdr)) return WifiParseStatus::Malformed;
            const auto* header = reinterpret_cast<const nlmsghdr*>(buffer.data() + offset);
            if (header->nlmsg_len < sizeof(nlmsghdr) ||
                header->nlmsg_len > datagram_size - offset ||
                header->nlmsg_seq != sequence) return WifiParseStatus::WrongSequence;
            if (header->nlmsg_type == NLMSG_DONE || header->nlmsg_type == NLMSG_ERROR)
                terminal = true;
            const auto aligned = NLMSG_ALIGN(header->nlmsg_len);
            if (aligned > datagram_size - offset) return WifiParseStatus::Malformed;
            offset += aligned;
        }
        response.insert(response.end(), buffer.begin(), buffer.begin() + received);
        if (terminal) return WifiParseStatus::Ok;
    }
    return WifiParseStatus::Timeout;
#else
    (void)request; (void)sequence; (void)response; (void)token;
    return WifiParseStatus::ErrorReply;
#endif
}

std::optional<std::uint16_t> WifiCollector::resolveFamily(
    std::stop_token token, int& error_code, WifiParseStatus& transport_status) {
#if defined(__linux__)
    const auto sequence = next_sequence_.fetch_add(1);
    const auto request = makeRequest(GENL_ID_CTRL, CTRL_CMD_GETFAMILY,
                                     NLM_F_REQUEST, sequence, std::nullopt,
                                     std::string("nl80211"));
    std::vector<std::byte> response;
    const auto transport = requestAndReceive(request, sequence, response, token);
    if (transport != WifiParseStatus::Ok) {
        transport_status = transport;
        return std::nullopt;
    }
    const auto parsed = WifiNl80211Parser::parseFamilyId(response.data(), response.size(),
                                                          kKernelSenderPid, sequence);
    error_code = parsed.error_code;
    transport_status = parsed.status;
    if (parsed.status != WifiParseStatus::Ok && parsed.status != WifiParseStatus::Done)
        return std::nullopt;
    return parsed.family_id;
#else
    (void)token; (void)error_code;
    return std::nullopt;
#endif
}

WifiCollector::QueryResult WifiCollector::nativeQuery(const InterfaceId& interface,
                                                      std::uint64_t generation,
                                                      std::stop_token token) {
    QueryResult result;
    result.observation.netns = netns_;
    result.observation.interface = interface;
    result.observation.interface_generation = generation;
    result.observation.capability = WifiCapability::TransportUnavailable;
#if defined(__linux__)
    int error_code = 0;
    WifiParseStatus family_status = WifiParseStatus::Ok;
    if (!family_id_) family_id_ = resolveFamily(token, error_code, family_status);
    if (!family_id_) {
        result.transport_status = family_status;
        result.observation.capability = error_code == EACCES || error_code == EPERM
            ? WifiCapability::PermissionDenied : WifiCapability::Unsupported;
        result.error_code = error_code;
        return result;
    }
    const auto interface_sequence = next_sequence_.fetch_add(1);
    const auto interface_request = makeRequest(*family_id_, NL80211_CMD_GET_INTERFACE,
                                               NLM_F_REQUEST, interface_sequence,
                                               interface.ifindex);
    std::vector<std::byte> response;
    const auto interface_transport = requestAndReceive(
        interface_request, interface_sequence, response, token);
    if (interface_transport != WifiParseStatus::Ok) {
        result.transport_status = interface_transport;
        result.observation.capability = WifiCapability::TransportUnavailable;
        return result;
    }
    const auto interface_reply = WifiNl80211Parser::parseInterface(
        response.data(), response.size(), kKernelSenderPid, interface_sequence,
        interface.ifindex);
    result.error_code = interface_reply.error_code;
    if (!interface_reply.interface) {
        result.transport_status = interface_reply.status;
        result.observation.capability = interface_reply.error_code == ENODEV ||
                interface_reply.error_code == ENOENT ? WifiCapability::NotWifi
            : interface_reply.error_code == EACCES || interface_reply.error_code == EPERM
                ? WifiCapability::PermissionDenied : WifiCapability::Error;
        return result;
    }
    result.observation.nl80211_iftype = interface_reply.interface->iftype;
    result.observation.ssid = interface_reply.interface->ssid;
    result.observation.frequency_mhz = interface_reply.interface->frequency_mhz;
    if (interface_reply.interface->iftype != NL80211_IFTYPE_STATION) {
        result.observation.capability = WifiCapability::NotWifi;
        result.observation.link_state = WifiLinkState::NotWifi;
        return result;
    }
    const auto station_sequence = next_sequence_.fetch_add(1);
    const auto station_request = makeRequest(*family_id_, NL80211_CMD_GET_STATION,
                                             NLM_F_REQUEST | NLM_F_DUMP, station_sequence,
                                             interface.ifindex);
    response.clear();
    const auto station_transport = requestAndReceive(
        station_request, station_sequence, response, token);
    if (station_transport != WifiParseStatus::Ok) {
        result.transport_status = station_transport;
        result.observation.capability = WifiCapability::TransportUnavailable;
        return result;
    }
    const auto station_reply = WifiNl80211Parser::parseStation(
        response.data(), response.size(), kKernelSenderPid, station_sequence);
    result.error_code = station_reply.error_code;
    if (station_reply.status == WifiParseStatus::NotFound || !station_reply.station) {
        if (station_reply.status != WifiParseStatus::Done &&
            station_reply.status != WifiParseStatus::NotFound)
            result.transport_status = station_reply.status;
        result.observation.capability = station_reply.status == WifiParseStatus::NotFound
            || station_reply.status == WifiParseStatus::Done
            ? WifiCapability::NotAssociated : WifiCapability::Error;
        result.observation.link_state = station_reply.status == WifiParseStatus::Done
            || station_reply.status == WifiParseStatus::NotFound
            ? WifiLinkState::NotAssociated : WifiLinkState::Unknown;
        return result;
    }
    const auto& station = *station_reply.station;
    result.observation.capability = WifiCapability::Available;
    result.observation.link_state = WifiLinkState::Associated;
    result.observation.bssid = station.bssid;
    result.observation.signal_dbm = station.signal_dbm;
    result.observation.signal_avg_dbm = station.signal_avg_dbm;
    result.observation.tx_bitrate_kbps = station.tx_bitrate_kbps;
    result.observation.rx_bitrate_kbps = station.rx_bitrate_kbps;
    result.observation.tx_retries = station.tx_retries;
    result.observation.tx_failed = station.tx_failed;
    result.observation.rx_packets = station.rx_packets;
    result.observation.tx_packets = station.tx_packets;
    result.observation.rx_bytes = station.rx_bytes;
    result.observation.tx_bytes = station.tx_bytes;
#else
    (void)interface; (void)generation; (void)token;
#endif
    return result;
}

}  // namespace weaknet_dbus::v2
