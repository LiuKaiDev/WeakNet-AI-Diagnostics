#include "socket_route_attributor.hpp"

#include <linux/rtnetlink.h>
#include <linux/if.h>
#include <netinet/in.h>

#include <iostream>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool value, const char* message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

RouteFact route(NetnsId ns, std::uint8_t family, std::uint8_t prefix,
                std::array<std::uint8_t, 16> destination, std::uint32_t ifindex,
                std::uint32_t table = RT_TABLE_MAIN, std::uint32_t metric = 100) {
    RouteFact value;
    value.netns = ns; value.family = family; value.destination_prefix = prefix;
    value.destination = destination; value.table = table; value.priority = metric;
    value.type = RTN_UNICAST; value.output_ifindex = ifindex;
    return value;
}

SocketObservation socket(NetnsId ns, std::array<std::uint8_t, 16> remote,
                          std::array<std::uint8_t, 16> local = {192, 0, 2, 10}) {
    SocketObservation value;
    value.id = SocketId{ns, KernelSocketCookie{9}, SocketGeneration{1}};
    value.netns = ns; value.tuple.netns = ns; value.tuple.family = AF_INET;
    value.tuple.local = SocketEndpoint{AF_INET, local, 40000};
    value.tuple.remote = SocketEndpoint{AF_INET, remote, 443};
    value.diag_ifindex = 3;
    return value;
}
}

int main() {
    bool ok = true;
    const NetnsId ns{1, 2};
    RouteFact non_byte = route(ns, AF_INET, 25, {198, 51, 100, 128}, 3);
    ok &= expect(SocketRouteAttributor::prefixMatches(
                     SocketEndpoint{AF_INET, {198, 51, 100, 200}, 1}, non_byte),
                 "IPv4 non-byte-aligned prefix did not match");
    ok &= expect(!SocketRouteAttributor::prefixMatches(
                     SocketEndpoint{AF_INET, {198, 51, 100, 1}, 1}, non_byte),
                 "IPv4 non-byte-aligned prefix matched incorrectly");
    RouteFact ipv6 = route(ns, AF_INET6, 65,
                           {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0x80}, 3);
    ok &= expect(SocketRouteAttributor::prefixMatches(
                     SocketEndpoint{AF_INET6, {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0x80, 1}, 1}, ipv6),
                 "IPv6 longest-prefix matching failed");
    TopologySnapshot topology; topology.netns = ns; topology.authoritative = true; topology.generation = 7;
    for (const auto& item : {std::pair{3U, "wan0"}, std::pair{4U, "wan1"}}) {
        LinkFact link; link.netns = ns; link.interface = InterfaceId{item.first, item.second};
        link.flags = IFF_UP | IFF_RUNNING; link.oper_state = 6; link.carrier = 1; link.carrier_known = true;
        topology.links.emplace(item.first, link);
    }
    AddressFact address; address.netns = ns; address.interface = topology.links.at(3).interface;
    address.family = AF_INET; address.prefix_length = 24; address.address = {192, 0, 2, 10};
    topology.addresses.push_back(address);
    topology.routes.push_back(route(ns, AF_INET, 0, {}, 4, RT_TABLE_MAIN, 1));
    topology.routes.push_back(route(ns, AF_INET, 24, {198, 51, 100, 0}, 3, RT_TABLE_MAIN, 500));
    auto observation = socket(ns, {198, 51, 100, 42});
    const auto selected = UplinkPolicy{}.select(topology);
    const auto context = SocketRouteAttributor{}.attribute(observation, topology, selected);
    ok &= expect(context.route && context.route->prefix_length == 24, "specific route did not beat default");
    ok &= expect(context.possible_interfaces.size() == 1 && context.possible_interfaces.front().ifindex == 3,
                 "specific route interface was not retained");
    ok &= expect(context.attribution_status == RouteAttributionStatus::Available,
                 "exact modeled route with supporting evidence was not available");
    ok &= expect(static_cast<std::uint64_t>(context.reasons) & static_cast<std::uint64_t>(RouteAttributionReason::DiagIfindexAgrees),
                 "diag_ifindex supporting evidence was not recorded");
    ok &= expect(static_cast<std::uint64_t>(context.reasons) & static_cast<std::uint64_t>(RouteAttributionReason::LocalAddressAgrees),
                 "local address supporting evidence was not recorded");

    observation.diag_ifindex = 4;
    const auto conflict = SocketRouteAttributor{}.attribute(observation, topology, selected);
    ok &= expect(conflict.attribution_status == RouteAttributionStatus::Partial &&
                 (static_cast<std::uint64_t>(conflict.reasons) & static_cast<std::uint64_t>(RouteAttributionReason::DiagIfindexConflicts)),
                 "conflicting diag_ifindex was silently accepted");

    auto multipath = route(ns, AF_INET, 24, {198, 51, 100, 0}, 0);
    multipath.output_ifindex.reset(); multipath.multipath = {{3, {}, 0, 0}, {4, {}, 0, 0}};
    topology.routes = {multipath}; observation.diag_ifindex = 4;
    const auto multi_context = SocketRouteAttributor{}.attribute(observation, topology, selected);
    ok &= expect(multi_context.attribution_status == RouteAttributionStatus::Partial &&
                 multi_context.possible_interfaces.size() == 2,
                 "multipath attribution did not retain every candidate");
    ok &= expect(static_cast<std::uint64_t>(multi_context.reasons) & static_cast<std::uint64_t>(RouteAttributionReason::MultipathWithoutFlowHash),
                 "multipath flow-hash limitation was not exposed");

    topology.routes.clear();
    const auto absent = SocketRouteAttributor{}.attribute(observation, topology, selected);
    ok &= expect(absent.attribution_status == RouteAttributionStatus::Unavailable &&
                 (static_cast<std::uint64_t>(absent.reasons) & static_cast<std::uint64_t>(RouteAttributionReason::NoModeledRoute)),
                 "missing route did not become unavailable");
    topology.authoritative = false;
    const auto non_authoritative = SocketRouteAttributor{}.attribute(observation, topology, selected);
    ok &= expect(non_authoritative.attribution_status == RouteAttributionStatus::Unavailable,
                 "non-authoritative topology produced a confident result");

    topology.authoritative = true;
    topology.routes = {route(ns, AF_INET, 24, {198, 51, 100, 0}, 3, RT_TABLE_MAIN, 10),
                       route(ns, AF_INET, 24, {198, 51, 100, 0}, 4, RT_TABLE_MAIN, 10)};
    const auto ambiguous = SocketRouteAttributor{}.attribute(observation, topology, selected);
    ok &= expect(ambiguous.attribution_status == RouteAttributionStatus::Ambiguous &&
                 ambiguous.possible_interfaces.size() == 2,
                 "equal modeled routes were arbitrarily selected");

    auto blackhole = route(ns, AF_INET, 24, {198, 51, 100, 0}, 0);
    blackhole.output_ifindex.reset(); blackhole.type = RTN_BLACKHOLE;
    topology.routes = {blackhole};
    const auto blocked = SocketRouteAttributor{}.attribute(observation, topology, selected);
    ok &= expect(blocked.possible_interfaces.empty() && blocked.attribution_status == RouteAttributionStatus::Partial,
                 "blackhole route invented a normal egress");
    SocketRouteContextObservation payload = blocked;
    NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{99}, {},
        EventKind::SocketRouteObservation, EventSource::SocketTracker,
        RealtimeTime{}, MonotonicTime{}, ns, std::nullopt, observation.id,
        Validity::Partial, std::nullopt};
    NetworkEvent event(header, payload);
    const auto copied = event;
    ok &= expect(std::get<SocketRouteContextObservation>(copied.payload()).socket_id == observation.id,
                 "route context event copy/round-trip failed");
    return ok ? 0 : 1;
}
