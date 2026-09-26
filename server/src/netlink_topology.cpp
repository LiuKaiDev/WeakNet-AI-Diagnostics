#include "netlink_topology.hpp"

#include <linux/if.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>
#include <netinet/in.h>

#include <algorithm>
#include <iomanip>
#include <sstream>
#include <tuple>

namespace weaknet_dbus::v2 {

bool LinkFact::up() const noexcept {
    return present && (flags & IFF_UP) != 0;
}

bool LinkFact::usable() const noexcept {
    if (!up() || (flags & IFF_LOOPBACK) != 0) return false;
    return oper_state != IF_OPER_DOWN && oper_state != IF_OPER_NOTPRESENT && (!carrier_known || carrier != 0);
}

std::string RouteFact::identity() const {
    std::ostringstream output;
    output << static_cast<unsigned>(family) << ':' << static_cast<unsigned>(destination_prefix)
           << ':' << table << ':' << priority << ':' << static_cast<unsigned>(protocol)
           << ':' << static_cast<unsigned>(scope) << ':' << static_cast<unsigned>(type) << ':';
    for (const auto byte : destination) output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    output << ':' << (output_ifindex ? std::to_string(*output_ifindex) : "-") << ':';
    if (gateway) for (const auto byte : *gateway) output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
    output << ":src=";
    if (preferred_source) {
        for (const auto byte : *preferred_source) {
            output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        }
    } else {
        output << '-';
    }
    output << ':';
    for (const auto& nexthop : multipath) {
        output << nexthop.ifindex << ',' << static_cast<unsigned>(nexthop.hops) << ','
               << static_cast<unsigned>(nexthop.flags) << ';';
        for (const auto byte : nexthop.gateway) output << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(byte);
        output << '|';
    }
    return output.str();
}

UplinkSelection UplinkPolicy::select(const TopologySnapshot& snapshot) const {
    struct Candidate {
        const RouteFact* route{};
        std::uint32_t ifindex{};
        bool multipath{};
    };
    std::vector<Candidate> candidates;
    UplinkSelection result;
    bool has_v4 = false;
    bool has_v6 = false;
    for (const auto& route : snapshot.routes) {
        if (route.netns != snapshot.netns || !route.present || !route.isDefault() || route.type != RTN_UNICAST ||
            (route.table != RT_TABLE_MAIN && route.table != RT_TABLE_DEFAULT)) continue;
        const auto add = [&](std::uint32_t ifindex, bool multipath) {
            const auto link = snapshot.links.find(ifindex);
            if (link == snapshot.links.end() || link->second.netns != snapshot.netns || !link->second.usable()) return;
            candidates.push_back(Candidate{&route, ifindex, multipath});
            has_v4 = has_v4 || route.family == AF_INET;
            has_v6 = has_v6 || route.family == AF_INET6;
        };
        if (!route.multipath.empty()) {
            for (const auto& nexthop : route.multipath) add(nexthop.ifindex, true);
        } else if (route.output_ifindex) {
            add(*route.output_ifindex, false);
        }
    }
    result.method_flags = (has_v4 ? 1U : 0U) | (has_v6 ? 2U : 0U);
    if (candidates.empty()) {
        result.validity = snapshot.authoritative ? Validity::Unavailable : Validity::Stale;
        result.evidence = "no usable unicast default route in table main/default";
        return result;
    }
    const auto rank = [](const Candidate& candidate) {
        const auto& route = *candidate.route;
        const int table_rank = route.table == RT_TABLE_MAIN ? 0 : 1;
        const int family_rank = route.family == AF_INET ? 0 : 1;
        return std::tuple{table_rank, route.priority, family_rank,
                          candidate.multipath ? 1 : 0, candidate.ifindex,
                          route.identity()};
    };
    const auto chosen = *std::min_element(candidates.begin(), candidates.end(),
        [&](const Candidate& left, const Candidate& right) { return rank(left) < rank(right); });
    const auto& link = snapshot.links.at(chosen.ifindex);
    result.interface = link.interface;
    result.validity = chosen.multipath ? Validity::Partial : Validity::Valid;
    std::ostringstream evidence;
    evidence << "table=" << chosen.route->table << ",metric=" << chosen.route->priority
             << ",family=" << (chosen.route->family == AF_INET ? "ipv4" : "ipv6")
             << ",ifindex=" << chosen.ifindex
             << ",gateway=" << (chosen.route->gateway ? "yes" : "on-link")
             << ",multipath=" << (chosen.multipath ? "yes; representative=min-ranked-nexthop" : "no")
             << ",tie-break=table-main,metric,ipv4,non-multipath,ifindex,route-identity";
    result.evidence = evidence.str();
    return result;
}

}  // namespace weaknet_dbus::v2
