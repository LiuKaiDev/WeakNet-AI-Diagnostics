#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "network_event.hpp"

namespace weaknet_dbus::v2 {

struct SocketDiagRecord {
    SocketTuple tuple;
    TcpSocketState tcp_state{};
    std::optional<KernelSocketCookie> cookie;
    std::optional<std::uint32_t> diag_ifindex;
    std::optional<std::uint32_t> uid;
    std::optional<std::uint32_t> inode;
};

enum class SocketDiagMessageKind : std::uint8_t { Socket, Done, Error };

struct SocketDiagMessage {
    SocketDiagMessageKind kind{SocketDiagMessageKind::Socket};
    std::optional<SocketDiagRecord> socket;
    std::uint16_t error_code{};
    std::uint32_t sequence{};
    std::uint32_t sender_pid{};
    bool dump_interrupted{false};
};

struct SocketDiagParseResult {
    std::vector<SocketDiagMessage> messages;
    bool malformed{false};
    bool completed{false};
    bool dump_interrupted{false};
    std::string error;
};

class SocketDiagParser {
public:
    static SocketDiagParseResult parse(const void* data, std::size_t size,
                                       std::uint32_t sender_pid,
                                       std::optional<std::uint32_t> expected_sequence,
                                       NetnsId netns);
};

}  // namespace weaknet_dbus::v2
