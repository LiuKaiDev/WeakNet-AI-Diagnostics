#include "socket_route_attributor.hpp"

#include <linux/rtnetlink.h>
#include <netinet/in.h>

#include <algorithm>
#include <set>
#include <tuple>

namespace weaknet_dbus::v2 {
namespace {

bool supportedFamily(std::uint8_t family) noexcept {
    return family == AF_INET || family == AF_INET6;
}

bool normalForwardRoute(const RouteFact& route) noexcept {
    return route.type == RTN_UNICAST;
}

bool localRoute(const RouteFact& route) noexcept {
    return route.type == RTN_LOCAL;
}

int tableRank(std::uint32_t table) noexcept {
    if (table == RT_TABLE_MAIN) return 0;
    if (table == RT_TABLE_DEFAULT) return 1;
    return 2;
}

bool sameOutput(const RouteFact& left, const RouteFact& right) {
    std::set<std::uint32_t> a;
    std::set<std::uint32_t> b;
    if (left.output_ifindex && *left.output_ifindex != 0) a.insert(*left.output_ifindex);
    for (const auto& hop : left.multipath) if (hop.ifindex != 0) a.insert(hop.ifindex);
    if (right.output_ifindex && *right.output_ifindex != 0) b.insert(*right.output_ifindex);
    for (const auto& hop : right.multipath) if (hop.ifindex != 0) b.insert(hop.ifindex);
    return a == b && left.type == right.type;
}

bool hasReason(RouteAttributionReason reasons, RouteAttributionReason wanted) noexcept {
    return (static_cast<std::uint64_t>(reasons) & static_cast<std::uint64_t>(wanted)) != 0;
}

}  // namespace

bool SocketRouteAttributor::prefixMatches(const SocketEndpoint& destination,
                                          const RouteFact& route) noexcept {
    if (!supportedFamily(destination.family) || destination.family != route.family ||
        route.destination_prefix > (destination.family == AF_INET ? 32 : 128)) return false;
    const auto bytes = destination.family == AF_INET ? 4U : 16U;
    const auto full = static_cast<unsigned>(route.destination_prefix / 8U);
    const auto remainder = static_cast<unsigned>(route.destination_prefix % 8U);
    for (unsigned index = 0; index < full; ++index) {
        if (destination.address[index] != route.destination[index]) return false;
    }
    if (remainder != 0U) {
        const auto mask = static_cast<std::uint8_t>(0xffU << (8U - remainder));
        if ((destination.address[full] & mask) != (route.destination[full] & mask)) return false;
    }
    (void)bytes;
    return true;
}

InterfaceId SocketRouteAttributor::interfaceFor(const TopologySnapshot& topology,
                                                std::uint32_t ifindex) {
    const auto found = topology.links.find(ifindex);
    if (found != topology.links.end() && found->second.netns == topology.netns)
        return found->second.interface;
    return InterfaceId{ifindex, {}};
}

SocketRouteContextObservation SocketRouteAttributor::attribute(
    const SocketObservation& socket, const TopologySnapshot& topology,
    const UplinkSelection& selected_uplink) const {
    return attribute(socket.id, socket.tuple, socket.diag_ifindex, topology, selected_uplink);
}

SocketRouteContextObservation SocketRouteAttributor::attribute(
    const SocketId& id, const SocketTuple& tuple,
    std::optional<std::uint32_t> diag_ifindex,
    const TopologySnapshot& topology,
    const UplinkSelection& selected_uplink) const {
    SocketRouteContextObservation result;
    result.socket_id = id;
    result.netns = tuple.netns;
    result.destination = tuple.remote;
    result.diag_ifindex = diag_ifindex;
    result.topology_generation = topology.generation;
    result.topology_authoritative = topology.authoritative;

    if (tuple.netns != topology.netns || !topology.authoritative) {
        result.attribution_status = RouteAttributionStatus::Unavailable;
        result.reasons |= RouteAttributionReason::TopologyNonAuthoritative;
        if (diag_ifindex.value_or(0) == 0) result.reasons |= RouteAttributionReason::DiagIfindexUnavailable;
        return result;
    }
    if (topology.partial || topology.degraded) {
        result.attribution_status = RouteAttributionStatus::Partial;
        result.reasons |= topology.partial ? RouteAttributionReason::TopologyPartial : RouteAttributionReason::None;
        result.reasons |= topology.degraded ? RouteAttributionReason::TopologyDegraded : RouteAttributionReason::None;
    }
    if (diag_ifindex.value_or(0) == 0) result.reasons |= RouteAttributionReason::DiagIfindexUnavailable;

    std::vector<const RouteFact*> candidates;
    for (const auto& route : topology.routes) {
        if (!route.present || route.netns != topology.netns ||
            !supportedFamily(route.family) || !prefixMatches(tuple.remote, route)) continue;
        // The topology model supports these route types. Other types are not silently
        // converted into a normal egress claim.
        if (route.type != RTN_UNICAST && route.type != RTN_LOCAL &&
            route.type != RTN_BLACKHOLE && route.type != RTN_UNREACHABLE &&
            route.type != RTN_PROHIBIT) continue;
        candidates.push_back(&route);
    }
    if (candidates.empty()) {
        result.attribution_status = RouteAttributionStatus::Unavailable;
        result.reasons |= RouteAttributionReason::NoModeledRoute;
        return result;
    }

    const auto prefix = (*std::max_element(candidates.begin(), candidates.end(),
        [](const auto* left, const auto* right) {
            return left->destination_prefix < right->destination_prefix;
        }))->destination_prefix;
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](const auto* route) { return route->destination_prefix != prefix; }), candidates.end());
    const auto best_rank = std::min_element(candidates.begin(), candidates.end(),
        [](const auto* left, const auto* right) {
            return std::tuple{tableRank(left->table), left->priority} <
                   std::tuple{tableRank(right->table), right->priority};
        });
    const auto rank = std::tuple{tableRank((*best_rank)->table), (*best_rank)->priority};
    candidates.erase(std::remove_if(candidates.begin(), candidates.end(),
        [&](const auto* route) {
            return std::tuple{tableRank(route->table), route->priority} != rank;
        }), candidates.end());

    bool ambiguous = false;
    for (std::size_t index = 1; index < candidates.size(); ++index) {
        if (!sameOutput(*candidates.front(), *candidates[index])) {
            ambiguous = true;
            break;
        }
    }
    if (ambiguous) result.reasons |= RouteAttributionReason::AmbiguousCandidate;

    const auto* route = *std::min_element(candidates.begin(), candidates.end(),
        [](const auto* left, const auto* right) { return left->identity() < right->identity(); });
    MatchedRouteSummary summary{route->identity(), route->family,
        route->destination_prefix, route->table, route->priority, route->type,
        route->gateway, route->preferred_source, {}, {},
        normalForwardRoute(*route) && route->onLink(), !route->multipath.empty()};
    for (const auto& hop : route->multipath) {
        summary.nexthop_ifindices.push_back(hop.ifindex);
        summary.nexthop_gateways.push_back(hop.gateway);
    }
    result.route = std::move(summary);

    std::set<std::uint32_t> ifindices;
    const auto addOutputs = [&](const RouteFact& candidate) {
        if (!normalForwardRoute(candidate)) return;
        if (candidate.output_ifindex && *candidate.output_ifindex != 0) ifindices.insert(*candidate.output_ifindex);
        for (const auto& hop : candidate.multipath) if (hop.ifindex != 0) ifindices.insert(hop.ifindex);
    };
    // An ambiguity is not resolved by the final identity sort.  Expose the
    // union of possible interfaces from every tied candidate.
    if (ambiguous) for (const auto* candidate : candidates) addOutputs(*candidate);
    else addOutputs(*route);
    for (const auto ifindex : ifindices) result.possible_interfaces.push_back(interfaceFor(topology, ifindex));
    if (normalForwardRoute(*route) && ifindices.empty())
        result.reasons |= RouteAttributionReason::MissingOutputInterface;
    if (route->gateway || std::any_of(route->multipath.begin(), route->multipath.end(),
                                      [](const auto& hop) { return hop.gateway != std::array<std::uint8_t, 16>{}; }))
        result.reasons |= RouteAttributionReason::GatewayEvidence;
    if (route->onLink() && normalForwardRoute(*route)) result.reasons |= RouteAttributionReason::OnLinkEvidence;
    if (!normalForwardRoute(*route)) result.reasons |= localRoute(*route) ? RouteAttributionReason::LocalRoute : RouteAttributionReason::PolicyRoutingNotModeled;
    if (tableRank(route->table) == 2) result.reasons |= RouteAttributionReason::PolicyRoutingNotModeled;

    if (diag_ifindex && *diag_ifindex != 0) {
        const auto found = ifindices.contains(*diag_ifindex);
        result.reasons |= found ? RouteAttributionReason::DiagIfindexAgrees : RouteAttributionReason::DiagIfindexConflicts;
    }

    bool local_agrees = false;
    for (const auto& address : topology.addresses) {
        if (address.netns == topology.netns && address.family == tuple.local.family &&
            address.address == tuple.local.address && ifindices.contains(address.interface.ifindex)) {
            local_agrees = true;
            break;
        }
    }
    if (local_agrees || (route->preferred_source && *route->preferred_source == tuple.local.address))
        result.reasons |= RouteAttributionReason::LocalAddressAgrees;
    else if (!ifindices.empty() && tuple.local.family == tuple.family)
        result.reasons |= RouteAttributionReason::LocalAddressConflicts;
    if (route->preferred_source && *route->preferred_source != tuple.local.address)
        result.reasons |= RouteAttributionReason::LocalAddressConflicts;

    const bool local = localRoute(*route);
    if (!local && result.possible_interfaces.size() == 1)
        result.selected_interface = result.possible_interfaces.front();
    if (local) {
        result.uplink_relationship = SelectedUplinkRelationship::NonUplinkLocal;
    } else if (selected_uplink.interface && !result.possible_interfaces.empty()) {
        const auto matches = std::any_of(result.possible_interfaces.begin(), result.possible_interfaces.end(),
            [&](const auto& item) { return item == *selected_uplink.interface; });
        if (matches && result.possible_interfaces.size() == 1) {
            result.uplink_relationship = SelectedUplinkRelationship::MatchesSelectedUplink;
        } else if (matches) {
            result.uplink_relationship = SelectedUplinkRelationship::PossibleSelectedUplink;
        } else {
            result.uplink_relationship = SelectedUplinkRelationship::DifferentFromSelectedUplink;
        }
    } else {
        result.uplink_relationship = SelectedUplinkRelationship::Unknown;
    }
    if (!ambiguous && normalForwardRoute(*route) && route->multipath.empty() &&
        !hasReason(result.reasons, RouteAttributionReason::DiagIfindexConflicts) &&
        !hasReason(result.reasons, RouteAttributionReason::LocalAddressConflicts) &&
        !hasReason(result.reasons, RouteAttributionReason::MissingOutputInterface) &&
        !hasReason(result.reasons, RouteAttributionReason::PolicyRoutingNotModeled) &&
        !topology.partial && !topology.degraded) {
        result.attribution_status = RouteAttributionStatus::Available;
        result.reasons |= RouteAttributionReason::ExactModeledMatch;
    } else if (ambiguous) {
        result.attribution_status = RouteAttributionStatus::Ambiguous;
    } else {
        result.attribution_status = RouteAttributionStatus::Partial;
        if (!route->multipath.empty()) result.reasons |= RouteAttributionReason::MultipathWithoutFlowHash;
    }
    return result;
}

}  // namespace weaknet_dbus::v2
