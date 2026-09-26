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
    bool isDefault() const noexcept { return destination_prefix == 0; }
    bool onLink() const noexcept { return !gateway.has_value() && multipath.empty(); }
    std::string identity() const;
    auto operator<=>(const RouteFact&) const = default;
};

struct TopologySnapshot {
    NetnsId netns;
    std::map<std::uint32_t, LinkFact> links;
    std::vector<AddressFact> addresses;
    std::vector<RouteFact> routes;
    bool authoritative{false};
    bool partial{false};
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
