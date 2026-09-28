#pragma once

#include <optional>

#include "netlink_topology.hpp"
#include "network_event.hpp"

namespace weaknet_dbus::v2 {

// Pure, deterministic attribution over a committed Phase 4 model.  This is
// intentionally not a Linux RPDB/FIB implementation.
class SocketRouteAttributor {
public:
    SocketRouteContextObservation attribute(
        const SocketObservation& socket,
        const TopologySnapshot& topology,
        const UplinkSelection& selected_uplink = {}) const;

    SocketRouteContextObservation attribute(
        const SocketId& id, const SocketTuple& tuple,
        std::optional<std::uint32_t> diag_ifindex,
        const TopologySnapshot& topology,
        const UplinkSelection& selected_uplink = {}) const;

    static bool prefixMatches(const SocketEndpoint& destination,
                              const RouteFact& route) noexcept;

private:
    static InterfaceId interfaceFor(const TopologySnapshot& topology,
                                     std::uint32_t ifindex);
};

}  // namespace weaknet_dbus::v2
