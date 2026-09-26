#include "netlink_collector.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace weaknet_dbus::v2 {
namespace {

std::string addressString(const std::array<std::uint8_t, 16>& address, std::uint8_t family) {
    char buffer[INET6_ADDRSTRLEN]{};
    const void* source = address.data();
    if (!inet_ntop(family, source, buffer, sizeof(buffer))) return {};
    return buffer;
}

}  // namespace

TopologyState::TopologyState(NetnsId netns) {
    snapshot_.netns = netns;
}

bool TopologyState::apply(const ParsedMessage& message) {
    return applyChecked(message) == TopologyApplyResult::Changed;
}

TopologyApplyResult TopologyState::applyChecked(const ParsedMessage& message) {
    std::lock_guard lock(mutex_);
    if (message.kind == ParsedMessageKind::Link && message.link) {
        const auto& fact = *message.link;
        if (fact.netns != snapshot_.netns) return TopologyApplyResult::Invalid;
        if (!fact.present) {
            const auto erased = snapshot_.links.erase(fact.interface.ifindex);
            const auto old_addresses = snapshot_.addresses.size();
            snapshot_.addresses.erase(std::remove_if(snapshot_.addresses.begin(), snapshot_.addresses.end(),
                [&](const AddressFact& item) { return item.interface.ifindex == fact.interface.ifindex; }), snapshot_.addresses.end());
            const auto old_routes = snapshot_.routes.size();
            snapshot_.routes.erase(std::remove_if(snapshot_.routes.begin(), snapshot_.routes.end(),
                [&](const RouteFact& item) {
                    return (item.output_ifindex && *item.output_ifindex == fact.interface.ifindex) ||
                        std::any_of(item.multipath.begin(), item.multipath.end(), [&](const RouteNexthop& hop) { return hop.ifindex == fact.interface.ifindex; });
                }), snapshot_.routes.end());
            return erased != 0 || old_addresses != snapshot_.addresses.size() ||
                       old_routes != snapshot_.routes.size()
                ? TopologyApplyResult::Changed : TopologyApplyResult::NoChange;
        }
        const auto iterator = snapshot_.links.find(fact.interface.ifindex);
        const bool changed = iterator == snapshot_.links.end() || iterator->second.interface.observed_name != fact.interface.observed_name ||
                             iterator->second.flags != fact.flags || iterator->second.oper_state != fact.oper_state || iterator->second.carrier != fact.carrier || iterator->second.carrier_known != fact.carrier_known;
        snapshot_.links[fact.interface.ifindex] = fact;
        return changed ? TopologyApplyResult::Changed : TopologyApplyResult::NoChange;
    }
    if (message.kind == ParsedMessageKind::Address && message.address) {
        const auto& fact = *message.address;
        if (fact.netns != snapshot_.netns) return TopologyApplyResult::Invalid;
        const auto match = [&](const AddressFact& item) {
            return item.netns == fact.netns && item.interface == fact.interface && item.family == fact.family &&
                   item.prefix_length == fact.prefix_length && item.address == fact.address;
        };
        const auto iterator = std::find_if(snapshot_.addresses.begin(), snapshot_.addresses.end(), match);
        if (!fact.present) {
            if (iterator == snapshot_.addresses.end()) return TopologyApplyResult::NoChange;
            snapshot_.addresses.erase(iterator);
            return TopologyApplyResult::Changed;
        }
        if (iterator == snapshot_.addresses.end()) { snapshot_.addresses.push_back(fact); return TopologyApplyResult::Changed; }
        if (*iterator == fact) return TopologyApplyResult::NoChange;
        *iterator = fact;
        return TopologyApplyResult::Changed;
    }
    if (message.kind == ParsedMessageKind::Route && message.route) {
        auto fact = *message.route;
        if (fact.netns != snapshot_.netns) return TopologyApplyResult::Invalid;
        std::sort(fact.multipath.begin(), fact.multipath.end());
        if (!fact.present) {
            if (fact.destination_prefix != 0 &&
                !(fact.attribute_mask & RouteDestinationAttribute)) {
                return TopologyApplyResult::AmbiguousDelete;
            }
            const auto matches = [&](const RouteFact& item) {
                if (item.netns != fact.netns || item.family != fact.family ||
                    item.destination_prefix != fact.destination_prefix ||
                    item.table != fact.table ||
                    item.protocol != fact.protocol || item.scope != fact.scope ||
                    item.type != fact.type) return false;
                if ((fact.attribute_mask & RouteDestinationAttribute) &&
                    item.destination != fact.destination) return false;
                if ((fact.attribute_mask & RoutePriorityAttribute) && item.priority != fact.priority) return false;
                if ((fact.attribute_mask & RouteOutputInterfaceAttribute) && item.output_ifindex != fact.output_ifindex) return false;
                if ((fact.attribute_mask & RouteGatewayAttribute) && item.gateway != fact.gateway) return false;
                if ((fact.attribute_mask & RoutePreferredSourceAttribute) && item.preferred_source != fact.preferred_source) return false;
                if ((fact.attribute_mask & RouteMultipathAttribute) && item.multipath != fact.multipath) return false;
                return true;
            };
            std::vector<std::size_t> matches_found;
            for (std::size_t index = 0; index < snapshot_.routes.size(); ++index) {
                if (matches(snapshot_.routes[index])) matches_found.push_back(index);
            }
            if (matches_found.empty()) return TopologyApplyResult::NoChange;
            if (matches_found.size() != 1) return TopologyApplyResult::AmbiguousDelete;
            snapshot_.routes.erase(snapshot_.routes.begin() + static_cast<std::ptrdiff_t>(matches_found.front()));
            return TopologyApplyResult::Changed;
        }
        const auto key = fact.identity();
        const auto iterator = std::find_if(snapshot_.routes.begin(), snapshot_.routes.end(),
            [&](const RouteFact& item) { return item.identity() == key; });
        if (iterator == snapshot_.routes.end()) {
            snapshot_.routes.push_back(std::move(fact));
            return TopologyApplyResult::Changed;
        }
        // identity() canonicalizes multipath nexthop ordering, so an update
        // that only reorders semantically equivalent nexthops is a no-op.
        return TopologyApplyResult::NoChange;
    }
    return TopologyApplyResult::Invalid;
}

void TopologyState::replace(const TopologySnapshot& snapshot) {
    std::lock_guard lock(mutex_);
    snapshot_ = snapshot;
}

TopologySnapshot TopologyState::snapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}

NetlinkCollector::NetlinkCollector(EventBus& bus, const Clock& clock, NetnsId netns,
                                   std::chrono::milliseconds reconciliation_interval,
                                   NetlinkCollectorTestHooks test_hooks)
    : bus_(bus), clock_(clock), netns_(netns),
      reconciliation_interval_(reconciliation_interval),
      test_hooks_(std::move(test_hooks)), state_(netns) {
    if (netns_.inode == 0) throw std::invalid_argument("NetlinkCollector requires namespace identity");
    if (reconciliation_interval_ <= std::chrono::milliseconds::zero() ||
        test_hooks_.recovery_retry_initial <= std::chrono::milliseconds::zero() ||
        test_hooks_.recovery_retry_max < test_hooks_.recovery_retry_initial) {
        throw std::invalid_argument("NetlinkCollector requires positive bounded retry intervals");
    }
}

NetlinkCollector::~NetlinkCollector() { stop(); }

bool NetlinkCollector::openSocket() {
#if defined(__linux__)
    if (test_hooks_.open_socket) {
        const int fd = test_hooks_.open_socket();
        if (fd < 0) return false;
        socket_fd_.store(fd);
        return true;
    }
    const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return false;
    int receive_buffer = 1024 * 1024;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) != 0) {
        ::close(fd);
        return false;
    }
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    address.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR |
                        RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) { ::close(fd); return false; }
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) { ::close(fd); return false; }
    socket_fd_.store(fd);
    return true;
#else
    return false;
#endif
}

bool NetlinkCollector::start() {
    if (running_.exchange(true)) return true;
    if (!openSocket()) { running_.store(false); return false; }
    (void)fullReconcile({});
    try {
        worker_ = std::jthread([this](std::stop_token token) { loop(token); });
    } catch (...) {
        running_.store(false);
#if defined(__linux__)
        const int fd = socket_fd_.exchange(-1); if (fd >= 0) ::close(fd);
#endif
        return false;
    }
    return true;
}

void NetlinkCollector::stop() noexcept {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
#if defined(__linux__)
    const int fd = socket_fd_.exchange(-1);
    if (fd >= 0) ::close(fd);
#endif
}

TopologySnapshot NetlinkCollector::snapshot() const { return state_.snapshot(); }
UplinkSelection NetlinkCollector::selectedUplink() const { std::lock_guard lock(mutex_); return selected_; }

void NetlinkCollector::setCommittedCallback(
    std::function<void(const TopologySnapshot&, const UplinkSelection&)> callback) {
    std::lock_guard lock(mutex_);
    test_hooks_.committed_callback = std::move(callback);
}
NetlinkCollectorTelemetry NetlinkCollector::telemetry() const {
    std::lock_guard lock(mutex_);
    auto result = telemetry_;
    const auto snapshot = state_.snapshot();
    result.link_count = snapshot.links.size(); result.address_count = snapshot.addresses.size(); result.route_count = snapshot.routes.size();
    return result;
}

bool NetlinkCollector::dump(std::uint16_t type, std::uint8_t family, TopologySnapshot& candidate,
                            std::stop_token token, bool& notification_raced) {
#if defined(__linux__)
    const int fd = socket_fd_.load();
    if (fd < 0) return false;
    static std::atomic<std::uint32_t> sequence{100};
    const auto request_sequence = sequence.fetch_add(1);
    struct { nlmsghdr header; rtgenmsg generation; } request{};
    request.header.nlmsg_len = sizeof(request);
    request.header.nlmsg_type = type;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = request_sequence;
    request.generation.rtgen_family = family;
    sockaddr_nl destination{}; destination.nl_family = AF_NETLINK;
    iovec vector{&request, sizeof(request)};
    msghdr message{}; message.msg_name = &destination; message.msg_namelen = sizeof(destination);
    message.msg_iov = &vector; message.msg_iovlen = 1;
    if (::sendmsg(fd, &message, 0) < 0) return false;
    std::array<std::byte, 256 * 1024> buffer{};
    const auto deadline = clock_.monotonicNow() + std::chrono::seconds(5);
    bool complete = false;
    while (!complete && !token.stop_requested()) {
        const auto now = clock_.monotonicNow();
        if (now >= deadline) return false;
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; return false; }
        if (ready == 0) continue;
        sockaddr_nl sender{}; iovec receive_vector{buffer.data(), buffer.size()};
        msghdr receive{}; receive.msg_name = &sender; receive.msg_namelen = sizeof(sender);
        receive.msg_iov = &receive_vector; receive.msg_iovlen = 1;
        const auto length = ::recvmsg(fd, &receive, MSG_DONTWAIT);
        if (length < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == ENOBUFS) {
                markOverflow();
            }
            return false;
        }
        if ((receive.msg_flags & MSG_TRUNC) != 0) {
            std::lock_guard lock(mutex_);
            ++telemetry_.truncations;
            ++telemetry_.forced_resync_requests;
            ++telemetry_.resync_requests;
            telemetry_.degraded = true;
            telemetry_.last_error = "netlink_datagram_truncated";
            return false;
        }
        const auto parsed = RtnetlinkParser::parse(buffer.data(), static_cast<std::size_t>(length), sender.nl_pid,
                                                   request_sequence, netns_, true);
        if (parsed.malformed || parsed.truncated || !parsed.error.empty()) {
            std::lock_guard lock(mutex_); ++telemetry_.parse_errors; telemetry_.last_error = parsed.error; return false;
        }
        for (const auto& item : parsed.messages) {
            if (item.sequence == 0 &&
                (item.kind == ParsedMessageKind::Link || item.kind == ParsedMessageKind::Address ||
                 item.kind == ParsedMessageKind::Route)) {
                notification_raced = true;
                std::lock_guard lock(mutex_);
                ++telemetry_.notifications_observed_during_reconciliation;
                continue;
            }
            if (item.kind == ParsedMessageKind::Error && item.error_code != 0) return false;
            if (item.kind == ParsedMessageKind::Done) {
                if (item.dump_interrupted) {
                    std::lock_guard lock(mutex_);
                    ++telemetry_.interrupted_dumps;
                    return false;
                }
                complete = true;
                continue;
            }
            if (item.kind == ParsedMessageKind::Link || item.kind == ParsedMessageKind::Address || item.kind == ParsedMessageKind::Route) {
                TopologyState temporary(candidate.netns);
                temporary.replace(candidate);
                const auto result = temporary.applyChecked(item);
                if (result != TopologyApplyResult::Changed && result != TopologyApplyResult::NoChange) {
                    return false;
                }
                candidate = temporary.snapshot();
            }
        }
    }
    return complete;
#else
    (void)type; (void)family; (void)candidate; (void)token; return false;
#endif
}

bool NetlinkCollector::fullReconcile(std::stop_token token) {
    TopologySnapshot candidate;
    candidate.netns = netns_;
    bool notification_raced = false;
    if (test_hooks_.reconcile) {
        auto result = test_hooks_.reconcile(token);
        if (!result) {
            markReconciliationFailed();
            if (test_hooks_.reconciliation_complete) {
                test_hooks_.reconciliation_complete(false);
            }
            return false;
        }
        candidate = std::move(*result);
        candidate.netns = netns_;
    } else if (!dump(RTM_GETLINK, AF_PACKET, candidate, token, notification_raced) ||
               !dump(RTM_GETADDR, AF_UNSPEC, candidate, token, notification_raced) ||
               !dump(RTM_GETROUTE, AF_INET, candidate, token, notification_raced) ||
               !dump(RTM_GETROUTE, AF_INET6, candidate, token, notification_raced)) {
        markReconciliationFailed();
        if (notification_raced) markNotificationRace();
        if (test_hooks_.reconciliation_complete) {
            test_hooks_.reconciliation_complete(false);
        }
        return false;
    }
    if (notification_raced) {
        markNotificationRace();
        if (test_hooks_.reconciliation_complete) test_hooks_.reconciliation_complete(false);
        return false;
    }
    commitReconciliation(std::move(candidate));
    if (test_hooks_.reconciliation_complete) {
        test_hooks_.reconciliation_complete(true);
    }
    return true;
}

void NetlinkCollector::markReconciliationFailed() {
    std::lock_guard lock(mutex_);
    ++telemetry_.failed_dumps;
    telemetry_.degraded = true;
    if (telemetry_.last_error.empty()) telemetry_.last_error = "reconciliation_failed";
    auto snapshot = state_.snapshot();
    if (snapshot.authoritative) { snapshot.degraded = true; state_.replace(snapshot); }
}

void NetlinkCollector::markNotificationRace() {
    std::lock_guard lock(mutex_);
    ++telemetry_.reconciliation_races;
    ++telemetry_.forced_resync_requests;
    ++telemetry_.resync_requests;
    telemetry_.degraded = true;
    telemetry_.last_error = "notification_raced_reconciliation";
    auto snapshot = state_.snapshot();
    if (snapshot.authoritative) { snapshot.degraded = true; state_.replace(snapshot); }
}

void NetlinkCollector::markOverflow() {
    std::lock_guard lock(mutex_);
    ++telemetry_.overflow_events;
    ++telemetry_.forced_resync_requests;
    ++telemetry_.resync_requests;
    telemetry_.degraded = true;
    telemetry_.last_error = "netlink_receive_overflow";
    auto snapshot = state_.snapshot();
    if (snapshot.authoritative) { snapshot.degraded = true; state_.replace(snapshot); }
}

bool NetlinkCollector::processNotificationBatch(const std::vector<ParsedMessage>& messages) {
    if (messages.empty()) return true;
    for (const auto& message : messages) {
        if (message.kind == ParsedMessageKind::Link || message.kind == ParsedMessageKind::Address ||
            message.kind == ParsedMessageKind::Route) continue;
        std::lock_guard lock(mutex_);
        ++telemetry_.notification_apply_failures;
        ++telemetry_.forced_resync_requests;
        ++telemetry_.resync_requests;
        telemetry_.degraded = true;
        telemetry_.last_error = "unsupported_netlink_notification";
        return false;
    }
    const auto before = state_.snapshot();
    TopologyState candidate(before.netns);
    candidate.replace(before);
    bool changed = false;
    for (const auto& message : messages) {
        const auto result = candidate.applyChecked(message);
        if (result == TopologyApplyResult::Invalid || result == TopologyApplyResult::AmbiguousDelete) {
            std::lock_guard lock(mutex_);
            if (result == TopologyApplyResult::AmbiguousDelete) ++telemetry_.ambiguous_route_deletes;
            ++telemetry_.notification_apply_failures;
            ++telemetry_.forced_resync_requests;
            ++telemetry_.resync_requests;
            telemetry_.degraded = true;
            telemetry_.last_error = result == TopologyApplyResult::AmbiguousDelete
                ? "ambiguous_route_delete" : "notification_apply_failed";
            return false;
        }
        changed = changed || result == TopologyApplyResult::Changed;
    }
    if (changed) {
        auto after = candidate.snapshot();
        after.degraded = false;
        after.generation = before.generation + 1;
        state_.replace(after);
        {
            std::lock_guard lock(mutex_);
            selected_ = UplinkPolicy{}.select(after);
        }
        publishChanges(before, after);
        std::function<void(const TopologySnapshot&, const UplinkSelection&)> callback;
        { std::lock_guard lock(mutex_); callback = test_hooks_.committed_callback; }
        if (callback) callback(after, UplinkPolicy{}.select(after));
    }
    std::lock_guard lock(mutex_);
    telemetry_.notifications_processed += messages.size();
    return true;
}

void NetlinkCollector::commitReconciliation(TopologySnapshot candidate) {
    candidate.authoritative = true;
    candidate.partial = false;
    candidate.degraded = false;
    const auto before = state_.snapshot();
    candidate.generation = before.generation + 1;
    state_.replace(candidate);
    {
        std::lock_guard lock(mutex_);
        selected_ = UplinkPolicy{}.select(candidate);
    }
    publishChanges(before, candidate);
    std::function<void(const TopologySnapshot&, const UplinkSelection&)> callback;
    { std::lock_guard lock(mutex_); callback = test_hooks_.committed_callback; }
    if (callback) callback(candidate, UplinkPolicy{}.select(candidate));
    {
        std::lock_guard lock(mutex_);
        ++telemetry_.successful_reconciliations;
        telemetry_.degraded = false;
        telemetry_.last_error.clear();
    }
}

void NetlinkCollector::publishChanges(const TopologySnapshot& before, const TopologySnapshot& after) {
    for (const auto& [index, fact] : after.links) {
        const auto old = before.links.find(index);
        if (old == before.links.end() || old->second.interface.observed_name != fact.interface.observed_name || old->second.flags != fact.flags || old->second.oper_state != fact.oper_state || old->second.carrier != fact.carrier || old->second.carrier_known != fact.carrier_known) {
            ParsedMessage message; message.kind = ParsedMessageKind::Link; message.link = fact; publishMessage(message);
        }
    }
    for (const auto& [index, fact] : before.links) if (!after.links.contains(index)) { ParsedMessage message; message.kind = ParsedMessageKind::Link; message.link = fact; message.link->present = false; publishMessage(message); }
    for (const auto& fact : after.addresses) if (std::find(before.addresses.begin(), before.addresses.end(), fact) == before.addresses.end()) { ParsedMessage message; message.kind = ParsedMessageKind::Address; message.address = fact; publishMessage(message); }
    for (const auto& fact : before.addresses) if (std::find(after.addresses.begin(), after.addresses.end(), fact) == after.addresses.end()) { ParsedMessage message; message.kind = ParsedMessageKind::Address; message.address = fact; message.address->present = false; publishMessage(message); }
    for (const auto& fact : after.routes) if (std::none_of(before.routes.begin(), before.routes.end(), [&](const RouteFact& old) { return old.identity() == fact.identity(); })) { ParsedMessage message; message.kind = ParsedMessageKind::Route; message.route = fact; publishMessage(message); }
    for (const auto& fact : before.routes) if (std::none_of(after.routes.begin(), after.routes.end(), [&](const RouteFact& current) { return current.identity() == fact.identity(); })) { ParsedMessage message; message.kind = ParsedMessageKind::Route; message.route = fact; message.route->present = false; publishMessage(message); }
    const auto old_uplink = UplinkPolicy{}.select(before);
    const auto new_uplink = UplinkPolicy{}.select(after);
    const bool changed = old_uplink.method_flags != new_uplink.method_flags ||
        old_uplink.validity != new_uplink.validity ||
        old_uplink.interface.has_value() != new_uplink.interface.has_value() ||
        (old_uplink.interface && new_uplink.interface && *old_uplink.interface != *new_uplink.interface);
    if (changed) {
        NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{next_event_id_.fetch_add(1)}, {},
            EventKind::UplinkObservation, EventSource::NetlinkCollector,
            clock_.realtimeNow(), clock_.monotonicNow(), netns_, new_uplink.interface,
            std::nullopt, new_uplink.validity,
            Status{StatusCode::PolicyEvidence, new_uplink.evidence}};
        bus_.publish(NetworkEvent(std::move(header), UplinkObservation{
            new_uplink.interface ? new_uplink.interface->observed_name : std::string{},
            new_uplink.method_flags, new_uplink.interface.has_value(), new_uplink.evidence}));
    }
}

void NetlinkCollector::publishMessage(const ParsedMessage& message) {
    const auto realtime = clock_.realtimeNow(); const auto monotonic = clock_.monotonicNow();
    NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{next_event_id_.fetch_add(1)}, {}, EventKind::CollectorHealth, EventSource::NetlinkCollector, realtime, monotonic, netns_, std::nullopt, std::nullopt, Validity::Valid, std::nullopt};
    if (message.kind == ParsedMessageKind::Link && message.link) {
        const auto& fact = *message.link; header.kind = EventKind::LinkObservation; header.interface = fact.interface; header.validity = fact.present ? Validity::Valid : Validity::Partial;
        bus_.publish(NetworkEvent(header, LinkObservation{fact.interface.ifindex, fact.interface.observed_name, fact.present, fact.up(), fact.carrier != 0}));
    } else if (message.kind == ParsedMessageKind::Address && message.address) {
        const auto& fact = *message.address; header.kind = EventKind::AddressObservation; header.interface = fact.interface; header.validity = fact.present ? Validity::Valid : Validity::Partial;
        bus_.publish(NetworkEvent(header, AddressObservation{fact.interface.ifindex, fact.family, fact.prefix_length, addressString(fact.address, fact.family), fact.present}));
    } else if (message.kind == ParsedMessageKind::Route && message.route) {
        const auto& fact = *message.route; header.kind = EventKind::RouteObservation; if (fact.output_ifindex) header.interface = InterfaceId{*fact.output_ifindex, {}}; header.validity = fact.present ? Validity::Valid : Validity::Partial;
        bus_.publish(NetworkEvent(header, RouteObservation{fact.family, fact.destination_prefix, fact.table, fact.priority, fact.output_ifindex.value_or(0), fact.present, !fact.multipath.empty(), fact.onLink()}));
    }
    std::lock_guard lock(mutex_); ++telemetry_.state_changes_published;
}

void NetlinkCollector::updateTelemetrySnapshot(const TopologySnapshot&) {}

void NetlinkCollector::loop(std::stop_token token) {
#if defined(__linux__)
    auto retry_delay = test_hooks_.recovery_retry_initial;
    bool degraded = false;
    {
        std::lock_guard lock(mutex_);
        degraded = telemetry_.degraded;
    }
    auto next_reconcile = clock_.monotonicNow() +
        (degraded ? retry_delay : reconciliation_interval_);
    std::array<std::byte, 256 * 1024> buffer{};
    while (!token.stop_requested()) {
        const auto now = clock_.monotonicNow();
        if (now >= next_reconcile) {
            { std::lock_guard lock(mutex_); ++telemetry_.resync_requests; }
            if (fullReconcile(token)) {
                retry_delay = test_hooks_.recovery_retry_initial;
                next_reconcile = clock_.monotonicNow() + reconciliation_interval_;
            } else {
                retry_delay = std::min(retry_delay * 2, test_hooks_.recovery_retry_max);
                next_reconcile = clock_.monotonicNow() + retry_delay;
            }
            continue;
        }
        if (test_hooks_.inject_overflow && test_hooks_.inject_overflow()) {
            markOverflow();
            next_reconcile = std::min(next_reconcile,
                                      clock_.monotonicNow() + retry_delay);
            continue;
        }
        const int fd = socket_fd_.load(); if (fd < 0) break;
        pollfd descriptor{fd, POLLIN, 0}; const int ready = ::poll(&descriptor, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (ready == 0) continue;
        sockaddr_nl sender{}; iovec vector{buffer.data(), buffer.size()}; msghdr message{};
        message.msg_name = &sender; message.msg_namelen = sizeof(sender); message.msg_iov = &vector; message.msg_iovlen = 1;
        const auto length = ::recvmsg(fd, &message, MSG_DONTWAIT);
        if (length < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == ENOBUFS) {
                markOverflow();
                next_reconcile = std::min(next_reconcile, clock_.monotonicNow() + retry_delay);
                continue;
            }
            break;
        }
        if ((message.msg_flags & MSG_TRUNC) != 0) {
            {
                std::lock_guard lock(mutex_);
                ++telemetry_.truncations;
                ++telemetry_.forced_resync_requests;
                ++telemetry_.resync_requests;
                ++telemetry_.notification_apply_failures;
                telemetry_.degraded = true;
                telemetry_.last_error = "netlink_datagram_truncated";
            }
            next_reconcile = std::min(next_reconcile, clock_.monotonicNow() + retry_delay);
            continue;
        }
        const auto parsed = RtnetlinkParser::parse(buffer.data(), static_cast<std::size_t>(length), sender.nl_pid, std::nullopt, netns_, false);
        if (parsed.malformed || parsed.truncated || !parsed.error.empty()) {
            {
                std::lock_guard lock(mutex_);
                ++telemetry_.parse_errors;
                ++telemetry_.notification_apply_failures;
                ++telemetry_.forced_resync_requests;
                ++telemetry_.resync_requests;
                telemetry_.degraded = true;
                telemetry_.last_error = parsed.error.empty() ? "netlink_parse_failed" : parsed.error;
            }
            next_reconcile = std::min(next_reconcile, clock_.monotonicNow() + retry_delay);
            continue;
        }
        std::vector<ParsedMessage> notifications;
        for (const auto& item : parsed.messages) {
            if (item.kind == ParsedMessageKind::Notification || item.kind == ParsedMessageKind::Link ||
                item.kind == ParsedMessageKind::Address || item.kind == ParsedMessageKind::Route) {
                notifications.push_back(item);
            }
        }
        if (!notifications.empty() && !processNotificationBatch(notifications)) {
            next_reconcile = std::min(next_reconcile,
                                      clock_.monotonicNow() + retry_delay);
        }
    }
#else
    (void)token;
#endif
}

void NetlinkCollector::processDatagram(const void*, std::size_t, const sockaddr_nl&, std::optional<std::uint32_t>, bool, TopologyState&, bool*) {}

bool NetlinkCollector::reconcileForTests(const std::vector<std::vector<std::byte>>& datagrams, std::uint32_t expected_sequence) {
    if (datagrams.empty()) return false;
    TopologyState candidate(netns_);
    bool complete = false;
    bool notification_raced = false;
    for (const auto& datagram : datagrams) {
        const auto result = RtnetlinkParser::parse(datagram.data(), datagram.size(), 0, expected_sequence, netns_, false);
        if (result.malformed || result.truncated || !result.error.empty()) return false;
        for (const auto& item : result.messages) {
            if (item.sequence == 0 &&
                (item.kind == ParsedMessageKind::Link || item.kind == ParsedMessageKind::Address ||
                 item.kind == ParsedMessageKind::Route)) {
                notification_raced = true;
                std::lock_guard lock(mutex_);
                ++telemetry_.notifications_observed_during_reconciliation;
                continue;
            }
            if (item.kind == ParsedMessageKind::Done) {
                if (item.dump_interrupted) return false;
                complete = true;
            } else {
                const auto result = candidate.applyChecked(item);
                if (result != TopologyApplyResult::Changed && result != TopologyApplyResult::NoChange) {
                    return false;
                }
            }
        }
    }
    if (!complete) return false;
    if (notification_raced) {
        markNotificationRace();
        return false;
    }
    commitReconciliation(candidate.snapshot());
    return true;
}

bool NetlinkCollector::processNotificationForTests(const std::vector<std::byte>& datagram) {
    const auto result = RtnetlinkParser::parse(datagram.data(), datagram.size(), 0,
                                                std::nullopt, netns_, false);
    if (result.malformed || result.truncated || !result.error.empty()) {
        std::lock_guard lock(mutex_);
        ++telemetry_.parse_errors;
        ++telemetry_.notification_apply_failures;
        ++telemetry_.forced_resync_requests;
        ++telemetry_.resync_requests;
        telemetry_.degraded = true;
        telemetry_.last_error = result.error.empty() ? "notification_parse_failed" : result.error;
        return false;
    }
    return processNotificationBatch(result.messages);
}

}  // namespace weaknet_dbus::v2
