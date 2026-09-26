#include "netlink_parser.hpp"
#include "netlink_topology.hpp"
#include "netlink_collector.hpp"
#include "weak_netmgr.hpp"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <net/if.h>

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
    ok &= expect(!collector.reconcileForTests({link_bytes}, seq) && collector.snapshot().links.size() == 1,
                 "incomplete dump replaced last-good state");
    auto malformed_after_candidate = link_bytes;
    reinterpret_cast<nlmsghdr*>(malformed_after_candidate.data())->nlmsg_len = 2;
    ok &= expect(!collector.reconcileForTests({link_bytes, malformed_after_candidate, done(seq)}, seq) &&
                 collector.snapshot().links.size() == 1,
                 "failed reconciliation published a partial candidate");

    auto notification = message(RTM_NEWLINK, link, 0, link_attrs);
    ok &= expect(collector.reconcileForTests({notification, link_bytes, route_bytes, done(seq)}, seq),
                 "dump/notification race fixture did not converge");

    weaknet_dbus::WeakNetMgr compatibility;
    compatibility.setTopologyCollector(&collector);
    const auto interfaces = compatibility.collectCurrentInterfaces();
    ok &= expect(interfaces.size() == 1 && interfaces.front().ifName() == "wan-test.0" &&
                 interfaces.front().isDefaultRoute(),
                 "V1 interface listing did not use reconciled topology");
    std::string using_name; std::uint32_t using_flags = 0;
    auto mutable_interfaces = interfaces;
    compatibility.updateCurrentUsing(mutable_interfaces, false, &using_name, &using_flags);
    ok &= expect(using_name == "wan-test.0" && !mutable_interfaces.empty() &&
                 mutable_interfaces.front().usingNow(),
                 "V1 current-uplink compatibility did not use policy result");
    subscription.unsubscribe();
    bus.stop();
    return ok ? 0 : 1;
}
