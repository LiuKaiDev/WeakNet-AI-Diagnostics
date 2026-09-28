#include "netlink_parser.hpp"
#include "netlink_topology.hpp"
#include "netlink_collector.hpp"
#include "weak_netmgr.hpp"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <sys/eventfd.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <condition_variable>
#include <mutex>
#include <vector>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

template <typename T>
void attribute(std::vector<std::byte>& bytes, std::uint16_t type, const T& value) {
    const auto length = static_cast<std::uint16_t>(RTA_LENGTH(sizeof(T)));
    const auto offset = bytes.size();
    bytes.resize(offset + RTA_ALIGN(length));
    auto* item = reinterpret_cast<rtattr*>(bytes.data() + offset);
    item->rta_type = type;
    item->rta_len = length;
    std::memcpy(RTA_DATA(item), &value, sizeof(T));
}

void stringAttribute(std::vector<std::byte>& bytes, std::uint16_t type, const char* value) {
    const auto length = static_cast<std::uint16_t>(RTA_LENGTH(std::strlen(value) + 1));
    const auto offset = bytes.size();
    bytes.resize(offset + RTA_ALIGN(length));
    auto* item = reinterpret_cast<rtattr*>(bytes.data() + offset);
    item->rta_type = type;
    item->rta_len = length;
    std::memcpy(RTA_DATA(item), value, std::strlen(value) + 1);
}

void rawAttribute(std::vector<std::byte>& bytes, std::uint16_t type,
                  const void* data, std::size_t size) {
    const auto length = static_cast<std::uint16_t>(RTA_LENGTH(size));
    const auto offset = bytes.size();
    bytes.resize(offset + RTA_ALIGN(length));
    auto* item = reinterpret_cast<rtattr*>(bytes.data() + offset);
    item->rta_type = type; item->rta_len = length;
    if (size != 0) std::memcpy(RTA_DATA(item), data, size);
}

template <typename T>
std::vector<std::byte> message(std::uint16_t type, const T& payload, std::uint32_t sequence,
                               const std::vector<std::byte>& attrs = {}) {
    const auto length = NLMSG_LENGTH(sizeof(T)) + attrs.size();
    std::vector<std::byte> bytes(NLMSG_ALIGN(length));
    auto* header = reinterpret_cast<nlmsghdr*>(bytes.data());
    header->nlmsg_len = length;
    header->nlmsg_type = type;
    header->nlmsg_seq = sequence;
    std::memcpy(NLMSG_DATA(header), &payload, sizeof(T));
    if (!attrs.empty()) std::memcpy(reinterpret_cast<std::byte*>(NLMSG_DATA(header)) + sizeof(T), attrs.data(), attrs.size());
    return bytes;
}

std::vector<std::byte> done(std::uint32_t sequence, bool interrupted = false) {
    std::vector<std::byte> bytes(NLMSG_ALIGN(NLMSG_LENGTH(0)));
    auto* header = reinterpret_cast<nlmsghdr*>(bytes.data());
    header->nlmsg_len = NLMSG_LENGTH(0);
    header->nlmsg_type = NLMSG_DONE;
    header->nlmsg_seq = sequence;
    if (interrupted) header->nlmsg_flags = NLM_F_DUMP_INTR;
    return bytes;
}

std::vector<std::byte> errorMessage(std::uint32_t sequence, int error) {
    nlmsgerr payload{}; payload.error = error;
    return message(NLMSG_ERROR, payload, sequence);
}

std::vector<std::byte> join(const std::vector<std::vector<std::byte>>& parts) {
    std::vector<std::byte> result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

RouteFact route(NetnsId ns, std::uint8_t family, std::uint32_t ifindex, std::uint32_t metric,
                std::uint32_t table = RT_TABLE_MAIN, bool gateway = true) {
    RouteFact item;
    item.netns = ns; item.family = family; item.table = table; item.priority = metric;
    item.type = RTN_UNICAST; item.scope = RT_SCOPE_UNIVERSE; item.output_ifindex = ifindex;
    if (gateway) item.gateway = std::array<std::uint8_t, 16>{1, 2, 3, 4};
    return item;
}

int fakePollableSocket() {
    return ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
}
}

int main() {
    bool ok = true;
    const NetnsId ns{1, 99};
    const std::uint32_t seq = 44;

    ifinfomsg link{}; link.ifi_index = 3; link.ifi_flags = IFF_UP | IFF_RUNNING; link.ifi_change = 0;
    std::vector<std::byte> link_attrs; stringAttribute(link_attrs, IFLA_IFNAME, "wan-test.0");
    std::uint8_t carrier = 1; attribute(link_attrs, IFLA_CARRIER, carrier);
    const auto link_bytes = message(RTM_NEWLINK, link, seq, link_attrs);
    const auto link_result = RtnetlinkParser::parse(link_bytes.data(), link_bytes.size(), 0, seq, ns, false);
    ok &= expect(!link_result.malformed && link_result.messages.size() == 1 &&
                 link_result.messages[0].link && link_result.messages[0].link->interface.observed_name == "wan-test.0",
                 "RTM_NEWLINK/name parse failed");

    ifaddrmsg address{}; address.ifa_family = AF_INET6; address.ifa_prefixlen = 64; address.ifa_index = 3;
    std::array<std::uint8_t, 16> ipv6{}; ipv6[0] = 0x20; ipv6[1] = 0x01; ipv6[15] = 1;
    std::vector<std::byte> address_attrs; attribute(address_attrs, IFA_ADDRESS, ipv6);
    const auto address_bytes = message(RTM_NEWADDR, address, seq, address_attrs);
    const auto address_result = RtnetlinkParser::parse(address_bytes.data(), address_bytes.size(), 0, seq, ns, false);
    ok &= expect(address_result.messages[0].address && address_result.messages[0].address->family == AF_INET6,
                 "IPv6 address parse failed");
    address.ifa_family = AF_INET; address.ifa_prefixlen = 24;
    std::array<std::uint8_t, 4> ipv4{192, 0, 2, 1}; address_attrs.clear();
    rawAttribute(address_attrs, IFA_LOCAL, ipv4.data(), ipv4.size());
    const auto address_delete_bytes = message(RTM_DELADDR, address, seq, address_attrs);
    const auto address_delete = RtnetlinkParser::parse(address_delete_bytes.data(), address_delete_bytes.size(), 0, seq, ns, false);
    ok &= expect(address_delete.messages[0].address && !address_delete.messages[0].address->present,
                 "IPv4 address delete parse failed");

    rtmsg route_header{}; route_header.rtm_family = AF_INET; route_header.rtm_dst_len = 0;
    route_header.rtm_table = RT_TABLE_MAIN; route_header.rtm_protocol = RTPROT_STATIC;
    route_header.rtm_scope = RT_SCOPE_UNIVERSE; route_header.rtm_type = RTN_UNICAST;
    std::vector<std::byte> route_attrs; std::uint32_t ifindex = 3; std::uint32_t metric = 20;
    attribute(route_attrs, RTA_OIF, ifindex); attribute(route_attrs, RTA_PRIORITY, metric);
    const auto route_bytes = message(RTM_NEWROUTE, route_header, seq, route_attrs);
    const auto route_result = RtnetlinkParser::parse(route_bytes.data(), route_bytes.size(), 0, seq, ns, false);
    ok &= expect(route_result.messages[0].route && route_result.messages[0].route->isDefault() &&
                 route_result.messages[0].route->priority == 20,
                 "default route/priority parse failed");
    std::uint32_t effective_table = 100; attribute(route_attrs, RTA_TABLE, effective_table);
    const auto table_bytes = message(RTM_NEWROUTE, route_header, seq, route_attrs);
    const auto table_result = RtnetlinkParser::parse(table_bytes.data(), table_bytes.size(), 0, seq, ns, false);
    ok &= expect(table_result.messages[0].route && table_result.messages[0].route->table == 100,
                 "RTA_TABLE did not override rtmsg table");
    rtmsg route6_header = route_header; route6_header.rtm_family = AF_INET6;
    std::vector<std::byte> route6_attrs; attribute(route6_attrs, RTA_OIF, ifindex);
    std::array<std::uint8_t, 16> gateway6{}; gateway6[0] = 0x20; gateway6[1] = 0x01; gateway6[15] = 1;
    rawAttribute(route6_attrs, RTA_GATEWAY, gateway6.data(), gateway6.size());
    const auto route6_bytes = message(RTM_NEWROUTE, route6_header, seq, route6_attrs);
    const auto route6_result = RtnetlinkParser::parse(route6_bytes.data(), route6_bytes.size(), 0, seq, ns, false);
    ok &= expect(route6_result.messages[0].route && route6_result.messages[0].route->family == AF_INET6 &&
                 route6_result.messages[0].route->gateway.has_value(),
                 "IPv6 route/gateway parse failed");

    std::vector<std::byte> multipath_payload(RTNH_ALIGN(sizeof(rtnexthop)));
    auto* hop = reinterpret_cast<rtnexthop*>(multipath_payload.data());
    hop->rtnh_len = sizeof(rtnexthop); hop->rtnh_ifindex = 3; hop->rtnh_hops = 2;
    std::vector<std::byte> multipath_attrs;
    rawAttribute(multipath_attrs, RTA_MULTIPATH, multipath_payload.data(), multipath_payload.size());
    const auto multipath_bytes = message(RTM_NEWROUTE, route_header, seq, multipath_attrs);
    const auto multipath_result = RtnetlinkParser::parse(multipath_bytes.data(), multipath_bytes.size(), 0, seq, ns, false);
    ok &= expect(multipath_result.messages[0].route && multipath_result.messages[0].route->multipath.size() == 1 &&
                 multipath_result.messages[0].route->multipath[0].hops == 2,
                 "RTA_MULTIPATH parse failed");

    const auto complete = join({link_bytes, done(seq)});
    const auto complete_result = RtnetlinkParser::parse(complete.data(), complete.size(), 0, seq, ns, true);
    ok &= expect(complete_result.completed, "NLMSG_DONE completion failed");
    const auto first_part = RtnetlinkParser::parse(link_bytes.data(), link_bytes.size(), 0, seq, ns, true);
    ok &= expect(!first_part.malformed && !first_part.completed && first_part.error.empty(),
                 "multipart first datagram was treated as a failed dump");
    const auto interrupted = done(seq, true);
    const auto interrupted_result = RtnetlinkParser::parse(interrupted.data(), interrupted.size(), 0, seq, ns, true);
    ok &= expect(!interrupted_result.completed && interrupted_result.messages[0].dump_interrupted,
                 "NLM_F_DUMP_INTR was ignored");
    const auto wrong_completion = done(0);
    ok &= expect(RtnetlinkParser::parse(wrong_completion.data(), wrong_completion.size(), 0, seq, ns, true).malformed,
                 "unsolicited NLMSG_DONE completed a dump");
    const auto wrong_sequence = RtnetlinkParser::parse(link_bytes.data(), link_bytes.size(), 0, 45, ns, false);
    ok &= expect(wrong_sequence.malformed, "unexpected sequence was accepted");
    const auto wrong_sender = RtnetlinkParser::parse(link_bytes.data(), link_bytes.size(), 7, seq, ns, false);
    ok &= expect(wrong_sender.malformed, "unexpected sender was accepted");
    const auto error_bytes = errorMessage(seq, -EPERM);
    const auto error_result = RtnetlinkParser::parse(error_bytes.data(), error_bytes.size(), 0, seq, ns, false);
    ok &= expect(!error_result.messages.empty() && error_result.messages[0].kind == ParsedMessageKind::Error &&
                 error_result.messages[0].error_code == EPERM, "NLMSG_ERROR parse failed");
    auto malformed = link_bytes; reinterpret_cast<nlmsghdr*>(malformed.data())->nlmsg_len = 2;
    const auto malformed_result = RtnetlinkParser::parse(malformed.data(), malformed.size(), 0, seq, ns, false);
    ok &= expect(malformed_result.malformed, "malformed nlmsg_len was accepted");
    auto malformed_attr = link_bytes;
    auto* bad_attr = reinterpret_cast<rtattr*>(reinterpret_cast<std::byte*>(NLMSG_DATA(reinterpret_cast<nlmsghdr*>(malformed_attr.data()))) + sizeof(ifinfomsg));
    bad_attr->rta_len = 1;
    ok &= expect(RtnetlinkParser::parse(malformed_attr.data(), malformed_attr.size(), 0, seq, ns, false).malformed,
                 "malformed attribute length was accepted");
    std::vector<std::byte> short_gateway; const std::uint8_t bad_gateway = 1;
    rawAttribute(short_gateway, RTA_GATEWAY, &bad_gateway, sizeof(bad_gateway));
    const auto short_gateway_bytes = message(RTM_NEWROUTE, route_header, seq, short_gateway);
    ok &= expect(RtnetlinkParser::parse(short_gateway_bytes.data(), short_gateway_bytes.size(), 0, seq, ns, false).malformed,
                 "short known route attribute was accepted");

    TopologyState reconciled(ns);
    ParsedMessage link_add; link_add.kind = ParsedMessageKind::Link; link_add.link = *link_result.messages[0].link;
    ok &= expect(reconciled.apply(link_add), "link add did not change state");
    auto renamed = link_add; renamed.link->interface.observed_name = "wan_test-1.2";
    ok &= expect(reconciled.apply(renamed) && reconciled.snapshot().links.at(3).interface.observed_name == "wan_test-1.2",
                 "link rename/punctuation reconciliation failed");
    ParsedMessage address_add; address_add.kind = ParsedMessageKind::Address; address_add.address = *address_result.messages[0].address;
    ParsedMessage address_remove; address_remove.kind = ParsedMessageKind::Address; address_remove.address = *address_delete.messages[0].address;
    auto address_v4 = address_remove; address_v4.address->present = true;
    reconciled.apply(address_add); reconciled.apply(address_v4); reconciled.apply(address_remove);
    ok &= expect(reconciled.snapshot().addresses.size() == 1,
                 "address delete erased an unrelated address");
    ParsedMessage route_add_low; route_add_low.kind = ParsedMessageKind::Route; route_add_low.route = *route_result.messages[0].route;
    auto route_add_high = route_add_low; route_add_high.route->priority = 30;
    reconciled.apply(route_add_low); reconciled.apply(route_add_high);
    auto route_remove = route_add_low; route_remove.route->present = false;
    reconciled.apply(route_remove);
    ok &= expect(reconciled.snapshot().routes.size() == 1 && reconciled.snapshot().routes[0].priority == 30,
                 "route deletion did not preserve route multiplicity");
    auto link_delete = link_add;
    link_delete.link->present = false;
    ok &= expect(reconciled.apply(link_delete) && reconciled.snapshot().links.empty() &&
                 reconciled.snapshot().addresses.empty() && reconciled.snapshot().routes.empty(),
                 "link deletion did not clear dependent address/route state");
    auto reused_link = link_add;
    reused_link.link->interface.observed_name = "reused.if_3";
    ok &= expect(reconciled.apply(reused_link) && reconciled.snapshot().links.at(3).interface.observed_name == "reused.if_3" &&
                 reconciled.snapshot().addresses.empty() && reconciled.snapshot().routes.empty(),
                 "ifindex reuse retained stale dependent state");
    auto route_with_source = route_add_low;
    route_with_source.route->preferred_source = std::array<std::uint8_t, 16>{192, 0, 2, 10};
    ok &= expect(route_with_source.route->identity() != route_add_low.route->identity(),
                 "preferred source was omitted from route identity");

    TopologySnapshot snapshot; snapshot.netns = ns; snapshot.authoritative = true;
    LinkFact link_fact; link_fact.netns = ns; link_fact.interface = InterfaceId{3, "wan-test.0"};
    link_fact.flags = IFF_UP | IFF_RUNNING; link_fact.oper_state = 6; link_fact.carrier = 1; link_fact.carrier_known = true;
    snapshot.links.emplace(3, link_fact);
    auto low = route(ns, AF_INET, 3, 200); auto high = route(ns, AF_INET, 3, 10);
    snapshot.routes = {low, high};
    const auto selected = UplinkPolicy{}.select(snapshot);
    ok &= expect(selected.interface && selected.interface->ifindex == 3 && selected.validity == Validity::Valid &&
                 selected.evidence.find("metric=10") != std::string::npos,
                 "uplink metric policy failed");
    auto on_link = route(ns, AF_INET6, 3, 1, RT_TABLE_MAIN, false); snapshot.routes = {on_link};
    const auto on_link_selected = UplinkPolicy{}.select(snapshot);
    ok &= expect(on_link_selected.interface && on_link_selected.evidence.find("on-link") != std::string::npos,
                 "on-link default policy failed");
    auto other = route(ns, AF_INET, 3, 1, 100); snapshot.routes = {other};
    ok &= expect(!UplinkPolicy{}.select(snapshot).interface, "unsupported non-main table selected");
    auto multipath = route(ns, AF_INET, 3, 1); multipath.output_ifindex.reset(); multipath.multipath = {{3, {}, 0, 0}, {3, {}, 1, 0}}; snapshot.routes = {multipath};
    const auto multipath_selected = UplinkPolicy{}.select(snapshot);
    ok &= expect(multipath_selected.interface && multipath_selected.validity == Validity::Partial &&
                 multipath_selected.evidence.find("multipath=yes") != std::string::npos,
                 "multipath policy did not remain partial");
    snapshot.netns = NetnsId{2, 99};
    multipath.netns = snapshot.netns; snapshot.links[3].netns = snapshot.netns; snapshot.routes = {multipath};
    ok &= expect(static_cast<bool>(UplinkPolicy{}.select(snapshot).interface), "namespace identity was lost in policy input");

    LinkFact second_link = link_fact; second_link.netns = snapshot.netns;
    second_link.interface = InterfaceId{4, "wan_other-1"}; snapshot.links.emplace(4, second_link);
    snapshot.links[3].netns = snapshot.netns;
    auto equal_left = route(snapshot.netns, AF_INET, 4, 5);
    auto equal_right = route(snapshot.netns, AF_INET, 3, 5);
    snapshot.routes = {equal_left, equal_right};
    ok &= expect(UplinkPolicy{}.select(snapshot).interface->ifindex == 3,
                 "equal-metric deterministic ifindex tie-break failed");
    snapshot.links[3].flags = 0;
    ok &= expect(UplinkPolicy{}.select(snapshot).interface->ifindex == 4,
                 "link-down route was selected");

    EventBus bus(64); ManualClock clock; NetlinkCollector collector(bus, clock, ns);
    ok &= expect(bus.start(), "fake transport EventBus did not start");
    std::mutex event_mutex; std::condition_variable event_cv; std::vector<NetworkEvent> events;
    auto subscription = bus.subscribe({}, [&](const NetworkEvent& event) {
        std::lock_guard lock(event_mutex); events.push_back(event); event_cv.notify_all();
    });
    ok &= expect(collector.reconcileForTests({link_bytes, route_bytes, done(seq)}, seq),
                 "multipart fake reconciliation failed");
    ok &= expect(collector.snapshot().links.size() == 1, "reconciliation did not commit link state");
    {
        std::unique_lock lock(event_mutex);
        event_cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return !events.empty(); });
    }
    ok &= expect(!events.empty() && events.front().header().sequence.value != 0 &&
                 events.front().header().netns == ns &&
                 events.front().header().kind == EventKind::LinkObservation,
                 "topology event sequence/scope assertion failed");
    ok &= expect(!collector.reconcileForTests({}, seq) && collector.snapshot().links.size() == 1,
                 "immediate no-data/EAGAIN equivalent replaced last-good state");
    ok &= expect(!collector.reconcileForTests({link_bytes}, seq) &&
                     collector.snapshot().authoritative && collector.snapshot().links.size() == 1,
                 "incomplete dump replaced last-good state");
    std::vector<std::byte> partial_attrs;
    stringAttribute(partial_attrs, IFLA_IFNAME, "partial-only.0");
    attribute(partial_attrs, IFLA_CARRIER, carrier);
    const auto partial_candidate = message(RTM_NEWLINK, link, seq, partial_attrs);
    auto malformed_after_candidate = partial_candidate;
    reinterpret_cast<nlmsghdr*>(malformed_after_candidate.data())->nlmsg_len = 2;
    ok &= expect(!collector.reconcileForTests(
                     {partial_candidate, malformed_after_candidate, done(seq)}, seq) &&
                     collector.snapshot().authoritative && collector.snapshot().links.size() == 1 &&
                     collector.snapshot().links.at(3).interface.observed_name == "wan-test.0",
                 "failed reconciliation published a partial candidate");

    auto notification = message(RTM_NEWLINK, link, 0, link_attrs);
    const auto published_before_race = collector.telemetry().state_changes_published;
    ok &= expect(!collector.reconcileForTests({notification, link_bytes, route_bytes, done(seq)}, seq) &&
                     collector.snapshot().authoritative && collector.snapshot().links.size() == 1 &&
                     collector.telemetry().notifications_observed_during_reconciliation == 1 &&
                     collector.telemetry().reconciliation_races == 1 &&
                     collector.telemetry().state_changes_published == published_before_race,
                 "dump/notification race committed a stale candidate or published events");
    ok &= expect(collector.reconcileForTests({link_bytes, route_bytes, done(seq)}, seq) &&
                     !collector.telemetry().degraded,
                 "clean reconciliation did not recover after a rejected raced dump");

    {
        TopologyState routes(ns);
        auto first = route(ns, AF_INET, 3, 10);
        auto second = route(ns, AF_INET, 3, 20);
        ParsedMessage add_first; add_first.kind = ParsedMessageKind::Route; add_first.route = first;
        ParsedMessage add_second; add_second.kind = ParsedMessageKind::Route; add_second.route = second;
        ok &= expect(routes.applyChecked(add_first) == TopologyApplyResult::Changed &&
                         routes.applyChecked(add_second) == TopologyApplyResult::Changed,
                     "route identity fixture did not add both similar routes");
        auto exact_delete = first; exact_delete.present = false;
        exact_delete.attribute_mask = RoutePriorityAttribute | RouteOutputInterfaceAttribute |
                                      RouteGatewayAttribute;
        ParsedMessage delete_first; delete_first.kind = ParsedMessageKind::Route;
        delete_first.route = exact_delete;
        ok &= expect(routes.applyChecked(delete_first) == TopologyApplyResult::Changed &&
                         routes.snapshot().routes.size() == 1 &&
                         routes.snapshot().routes.front().priority == 20,
                     "exact route delete removed the wrong similar route");

        auto ambiguous_left = route(ns, AF_INET6, 3, 30);
        auto ambiguous_right = route(ns, AF_INET6, 4, 30);
        ParsedMessage add_left; add_left.kind = ParsedMessageKind::Route; add_left.route = ambiguous_left;
        ParsedMessage add_right; add_right.kind = ParsedMessageKind::Route; add_right.route = ambiguous_right;
        routes.applyChecked(add_left); routes.applyChecked(add_right);
        auto ambiguous_delete = ambiguous_left; ambiguous_delete.present = false;
        ambiguous_delete.output_ifindex.reset();
        ambiguous_delete.gateway.reset();
        ambiguous_delete.attribute_mask = 0;
        ParsedMessage delete_ambiguous; delete_ambiguous.kind = ParsedMessageKind::Route;
        delete_ambiguous.route = ambiguous_delete;
        ok &= expect(routes.applyChecked(delete_ambiguous) == TopologyApplyResult::AmbiguousDelete &&
                         routes.snapshot().routes.size() == 3,
                     "ambiguous route delete guessed instead of requesting reconciliation");

        auto multipath_left = route(ns, AF_INET, 3, 40);
        multipath_left.output_ifindex.reset();
        multipath_left.multipath = {{3, {}, 0, 0}, {4, {}, 1, 0}};
        auto multipath_reordered = multipath_left;
        std::reverse(multipath_reordered.multipath.begin(), multipath_reordered.multipath.end());
        ParsedMessage add_multipath; add_multipath.kind = ParsedMessageKind::Route;
        add_multipath.route = multipath_left;
        ParsedMessage add_reordered; add_reordered.kind = ParsedMessageKind::Route;
        add_reordered.route = multipath_reordered;
        ok &= expect(routes.applyChecked(add_multipath) == TopologyApplyResult::Changed &&
                         routes.applyChecked(add_reordered) == TopologyApplyResult::NoChange,
                     "multipath nexthop ordering changed route identity");
        auto unrelated_multipath = multipath_left;
        unrelated_multipath.priority = 41;
        unrelated_multipath.multipath = {{3, {}, 0, 0}, {5, {}, 1, 0}};
        ParsedMessage add_unrelated; add_unrelated.kind = ParsedMessageKind::Route;
        add_unrelated.route = unrelated_multipath;
        routes.applyChecked(add_unrelated);
        auto delete_multipath = multipath_left;
        delete_multipath.present = false;
        delete_multipath.attribute_mask = RoutePriorityAttribute | RouteMultipathAttribute;
        ParsedMessage delete_multipath_message;
        delete_multipath_message.kind = ParsedMessageKind::Route;
        delete_multipath_message.route = delete_multipath;
        const auto delete_result = routes.applyChecked(delete_multipath_message);
        const auto after_multipath_delete = routes.snapshot();
        const bool left_remaining = std::any_of(after_multipath_delete.routes.begin(), after_multipath_delete.routes.end(),
            [&](const RouteFact& item) { return item.identity() == multipath_left.identity(); });
        const bool unrelated_remaining = std::any_of(after_multipath_delete.routes.begin(), after_multipath_delete.routes.end(),
            [&](const RouteFact& item) { return item.identity() == unrelated_multipath.identity(); });
        ok &= expect(delete_result == TopologyApplyResult::Changed && !left_remaining && unrelated_remaining,
                     "multipath delete removed an unrelated route");
    }

    {
        const auto before = collector.snapshot();
        auto malformed_notification = link_bytes;
        reinterpret_cast<nlmsghdr*>(malformed_notification.data())->nlmsg_len = 2;
        ok &= expect(!collector.processNotificationForTests(malformed_notification) &&
                         collector.snapshot().links.size() == before.links.size() &&
                         collector.telemetry().notification_apply_failures != 0 &&
                         collector.telemetry().forced_resync_requests != 0,
                     "malformed notification partially corrupted authoritative topology");
    }

    {
        EventBus delete_bus(16);
        ManualClock delete_clock;
        NetlinkCollector delete_collector(delete_bus, delete_clock, ns);
        std::vector<std::byte> first_route_attrs;
        std::vector<std::byte> second_route_attrs;
        std::uint32_t first_ifindex = 3;
        std::uint32_t second_ifindex = 4;
        attribute(first_route_attrs, RTA_OIF, first_ifindex);
        attribute(second_route_attrs, RTA_OIF, second_ifindex);
        auto first_route_bytes = message(RTM_NEWROUTE, route_header, seq, first_route_attrs);
        auto second_route_bytes = message(RTM_NEWROUTE, route_header, seq, second_route_attrs);
        ok &= expect(delete_collector.reconcileForTests(
                         {first_route_bytes, second_route_bytes, done(seq)}, seq),
                     "ambiguous-delete fixture reconciliation failed");
        auto ambiguous_delete_bytes = message(RTM_DELROUTE, route_header, 0, {});
        ok &= expect(!delete_collector.processNotificationForTests(ambiguous_delete_bytes) &&
                         delete_collector.snapshot().routes.size() == 2 &&
                         delete_collector.telemetry().ambiguous_route_deletes == 1 &&
                         delete_collector.telemetry().forced_resync_requests != 0,
                     "ambiguous route notification guessed or failed to request resync");

        NetlinkCollector batch_collector(delete_bus, delete_clock, ns);
        ok &= expect(batch_collector.reconcileForTests(
                         {first_route_bytes, second_route_bytes, done(seq)}, seq),
                     "notification batch fixture reconciliation failed");
        const auto mixed_notifications = join({
            message(RTM_NEWLINK, link, 0, link_attrs), ambiguous_delete_bytes});
        ok &= expect(!batch_collector.processNotificationForTests(mixed_notifications) &&
                         batch_collector.snapshot().links.empty() &&
                         batch_collector.snapshot().routes.size() == 2,
                     "failed notification batch partially mutated authoritative state");
    }

    {
        EventBus overflow_bus(16);
        SystemClock overflow_clock;
        std::atomic<bool> inject_overflow{true};
        std::atomic<int> reconciliations{0};
        std::mutex overflow_mutex;
        std::condition_variable overflow_cv;
        NetlinkCollectorTestHooks hooks;
        hooks.open_socket = fakePollableSocket;
        hooks.recovery_retry_initial = std::chrono::milliseconds(25);
        hooks.recovery_retry_max = std::chrono::milliseconds(50);
        hooks.inject_overflow = [&] { return inject_overflow.exchange(false); };
        hooks.reconcile = [&](std::stop_token) -> std::optional<TopologySnapshot> {
            const auto attempt = ++reconciliations;
            TopologySnapshot recovered;
            recovered.netns = ns;
            recovered.links.emplace(3, link_fact);
            auto recovered_route = route(ns, AF_INET, 3, attempt == 1 ? 50 : 60);
            recovered.routes.push_back(recovered_route);
            return recovered;
        };
        hooks.reconciliation_complete = [&](bool) { overflow_cv.notify_all(); };
        NetlinkCollector overflow_collector(
            overflow_bus, overflow_clock, ns, std::chrono::seconds(30), std::move(hooks));
        ok &= expect(overflow_collector.start(), "ENOBUFS seam collector failed to start");
        {
            std::unique_lock lock(overflow_mutex);
            overflow_cv.wait_for(lock, std::chrono::seconds(1), [&] {
                return reconciliations.load() >= 2;
            });
        }
        const auto overflow_telemetry = overflow_collector.telemetry();
        ok &= expect(overflow_telemetry.overflow_events != 0 &&
                         overflow_telemetry.forced_resync_requests != 0 &&
                         overflow_collector.snapshot().authoritative &&
                         !overflow_collector.snapshot().links.empty(),
                     "ENOBUFS did not preserve topology and request prompt reconciliation");
        overflow_collector.stop();
    }

    {
        EventBus lifecycle_bus(64);
        SystemClock lifecycle_clock;
        std::mutex reconciliation_mutex;
        std::condition_variable reconciliation_cv;
        std::atomic<int> reconciliation_attempts{0};
        std::atomic<int> completed_reconciliations{0};
        NetlinkCollectorTestHooks hooks;
        hooks.open_socket = fakePollableSocket;
        hooks.recovery_retry_initial = std::chrono::milliseconds(25);
        hooks.recovery_retry_max = std::chrono::milliseconds(50);
        hooks.reconcile = [&](std::stop_token) -> std::optional<TopologySnapshot> {
            const auto attempt = reconciliation_attempts.fetch_add(1) + 1;
            if (attempt == 1) return std::nullopt;
            TopologySnapshot recovered;
            recovered.netns = ns;
            recovered.links.emplace(3, link_fact);
            recovered.routes.push_back(high);
            return recovered;
        };
        hooks.reconciliation_complete = [&](bool) {
            completed_reconciliations.fetch_add(1);
            reconciliation_cv.notify_all();
        };
        NetlinkCollector lifecycle_collector(
            lifecycle_bus, lifecycle_clock, ns, std::chrono::seconds(30), std::move(hooks));
        ok &= expect(lifecycle_collector.start(),
                     "initial reconciliation failure was treated as transport startup failure");
        ok &= expect(lifecycle_collector.running(),
                     "collector did not remain running after initial reconciliation failure");
        ok &= expect(lifecycle_collector.telemetry().degraded,
                     "initial reconciliation failure did not degrade telemetry");
        ok &= expect(!lifecycle_collector.snapshot().authoritative,
                     "initial reconciliation failure produced an authoritative empty snapshot");
        {
            std::unique_lock lock(reconciliation_mutex);
            reconciliation_cv.wait_for(lock, std::chrono::seconds(1), [&] {
                return completed_reconciliations.load() >= 2;
            });
        }
        ok &= expect(reconciliation_attempts.load() >= 2 &&
                         completed_reconciliations.load() >= 2,
                     "degraded collector did not retry reconciliation early");
        ok &= expect(lifecycle_collector.snapshot().authoritative &&
                         lifecycle_collector.snapshot().links.size() == 1,
                     "successful retry did not commit an authoritative snapshot");
        ok &= expect(!lifecycle_collector.telemetry().degraded,
                     "successful retry did not clear degraded telemetry");
        lifecycle_collector.stop();
    }

    {
        EventBus transport_bus(8);
        SystemClock transport_clock;
        NetlinkCollectorTestHooks hooks;
        hooks.open_socket = [] { return -1; };
        NetlinkCollector transport_failure(
            transport_bus, transport_clock, ns, std::chrono::seconds(30), std::move(hooks));
        ok &= expect(!transport_failure.start() && !transport_failure.running(),
                     "transport initialization failure was treated as a running collector");
    }

    {
        EventBus retry_bus(8);
        SystemClock retry_clock;
        NetlinkCollectorTestHooks hooks;
        hooks.open_socket = fakePollableSocket;
        hooks.reconcile = [](std::stop_token) -> std::optional<TopologySnapshot> {
            return std::nullopt;
        };
        hooks.recovery_retry_initial = std::chrono::seconds(10);
        hooks.recovery_retry_max = std::chrono::seconds(10);
        NetlinkCollector retry_collector(
            retry_bus, retry_clock, ns, std::chrono::seconds(30), std::move(hooks));
        ok &= expect(retry_collector.start() && retry_collector.telemetry().degraded,
                     "degraded retry collector did not start");
        const auto stop_started = std::chrono::steady_clock::now();
        retry_collector.stop();
        const auto stop_elapsed = std::chrono::steady_clock::now() - stop_started;
        ok &= expect(stop_elapsed < std::chrono::seconds(1) && !retry_collector.running(),
                     "stop was not bounded while waiting for degraded reconciliation retry");
    }

    weaknet_dbus::WeakNetMgr compatibility;
    compatibility.setTopologyCollector(&collector);
    const auto interfaces = compatibility.collectCurrentInterfaces();
    ok &= expect(interfaces.size() == 1 && interfaces.front().ifName() == "wan-test.0" &&
                 interfaces.front().isDefaultRoute(),
                 "compatibility interface listing did not use reconciled topology");
    std::string using_name; std::uint32_t using_flags = 0;
    auto mutable_interfaces = interfaces;
    compatibility.updateCurrentUsing(mutable_interfaces, false, &using_name, &using_flags);
    ok &= expect(using_name == "wan-test.0" && !mutable_interfaces.empty() &&
                 mutable_interfaces.front().usingNow(),
                 "compatibility current-uplink query did not use policy result");
    subscription.unsubscribe();
    bus.stop();
    return ok ? 0 : 1;
}
