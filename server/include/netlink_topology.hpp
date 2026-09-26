#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "network_event.hpp"

namespace weaknet_dbus::v2 {

struct LinkFact {
    NetnsId netns;
    InterfaceId interface;
    std::uint32_t flags{};
    std::uint8_t oper_state{};
    std::uint8_t carrier{};
    bool carrier_known{false};
    std::uint16_t link_type{};
    bool present{true};
    bool up() const noexcept;
    bool usable() const noexcept;
};

struct AddressFact {
    NetnsId netns;
    InterfaceId interface;
    std::uint8_t family{};
    std::uint8_t prefix_length{};
    std::array<std::uint8_t, 16> address{};
    std::uint8_t scope{};
    std::uint32_t flags{};
    bool present{true};
    auto operator<=>(const AddressFact&) const = default;
};

struct RouteNexthop {
    std::uint32_t ifindex{};
    std::array<std::uint8_t, 16> gateway{};
    std::uint8_t hops{};
    std::uint8_t flags{};
    auto operator<=>(const RouteNexthop&) const = default;
};

struct RouteFact {
    NetnsId netns;
    std::uint8_t family{};
    std::uint8_t destination_prefix{};
    std::array<std::uint8_t, 16> destination{};
    std::uint32_t table{};
    std::uint32_t priority{};
    std::uint8_t protocol{};
    std::uint8_t scope{};
    std::uint8_t type{};
    std::optional<std::uint32_t> output_ifindex;
    std::optional<std::array<std::uint8_t, 16>> gateway;
    std::optional<std::array<std::uint8_t, 16>> preferred_source;
    std::vector<RouteNexthop> multipath;
    bool present{true};
    // Bits identify attributes present on a delete notification.  Dump/new
    // facts normally carry every modeled attribute; deletes may omit some,
    // in which case removal is only allowed when the match is unambiguous.
    std::uint32_t attribute_mask{};
    bool isDefault() const noexcept { return destination_prefix == 0; }
    bool onLink() const noexcept { return !gateway.has_value() && multipath.empty(); }
    std::string identity() const;
    auto operator<=>(const RouteFact&) const = default;
};

enum class TopologyApplyResult : std::uint8_t {
    NoChange,
    Changed,
    Invalid,
    AmbiguousDelete,
};

enum RouteAttributeMask : std::uint32_t {
    RouteDestinationAttribute = 1U << 0,
    RouteGatewayAttribute = 1U << 1,
    RoutePreferredSourceAttribute = 1U << 2,
    RouteOutputInterfaceAttribute = 1U << 3,
    RoutePriorityAttribute = 1U << 4,
    RouteTableAttribute = 1U << 5,
    RouteMultipathAttribute = 1U << 6,
};

struct TopologySnapshot {
    NetnsId netns;
    std::map<std::uint32_t, LinkFact> links;
    std::vector<AddressFact> addresses;
    std::vector<RouteFact> routes;
    bool authoritative{false};
    bool partial{false};
    bool degraded{false};
    std::uint64_t generation{};
};

struct UplinkSelection {
    std::optional<InterfaceId> interface;
    std::uint32_t method_flags{};
    Validity validity{Validity::Unavailable};
    std::string evidence;
};

class UplinkPolicy {
public:
    UplinkSelection select(const TopologySnapshot& snapshot) const;
};

}  // namespace weaknet_dbus::v2
