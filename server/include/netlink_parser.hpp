#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "netlink_topology.hpp"

namespace weaknet_dbus::v2 {

enum class ParsedMessageKind : std::uint8_t { Link, Address, Route, Done, Error, Notification };

struct ParsedMessage {
    ParsedMessageKind kind{ParsedMessageKind::Notification};
    std::optional<LinkFact> link;
    std::optional<AddressFact> address;
    std::optional<RouteFact> route;
    std::uint16_t error_code{};
    std::uint32_t sequence{};
    std::uint32_t sender_pid{};
    bool dump_interrupted{false};
};

struct ParseResult {
    std::vector<ParsedMessage> messages;
    bool malformed{false};
    bool truncated{false};
    bool completed{false};
    std::string error;
};

class RtnetlinkParser {
public:
    static ParseResult parse(const void* data, std::size_t size,
                             std::uint32_t sender_pid,
                             std::optional<std::uint32_t> expected_sequence,
                             NetnsId netns,
                             bool dump_response);
};

}  // namespace weaknet_dbus::v2
