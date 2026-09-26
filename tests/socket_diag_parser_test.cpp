#include "socket_diag_parser.hpp"

#include <arpa/inet.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>

#include <cstring>
#include <iostream>
#include <optional>
#include <vector>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

std::vector<std::byte> fixture(std::uint8_t family, std::uint32_t sequence,
                               bool done = true,
                               std::uint32_t cookie0 = 0x11223344,
                               std::uint32_t cookie1 = 0x55667788,
                               std::optional<std::vector<std::byte>> tcp_info = std::nullopt) {
    inet_diag_msg message{};
    message.idiag_family = family;
    message.id.idiag_sport = htons(12345);
    message.id.idiag_dport = htons(443);
    message.id.idiag_if = 7;
    message.id.idiag_cookie[0] = cookie0;
    message.id.idiag_cookie[1] = cookie1;
    message.idiag_state = 1;  // TCP_ESTABLISHED
    message.idiag_uid = 1000;
    if (family == AF_INET) {
        in_addr source{}, destination{};
        inet_pton(AF_INET, "192.0.2.10", &source);
        inet_pton(AF_INET, "198.51.100.20", &destination);
        std::memcpy(message.id.idiag_src, &source, sizeof(source));
        std::memcpy(message.id.idiag_dst, &destination, sizeof(destination));
    } else {
        inet_pton(AF_INET6, "2001:db8::10", message.id.idiag_src);
        inet_pton(AF_INET6, "2001:db8::20", message.id.idiag_dst);
    }
    const auto attribute_size = tcp_info
        ? static_cast<std::size_t>(NLA_ALIGN(sizeof(nlattr) + tcp_info->size())) : 0U;
    const auto payload_size = sizeof(message) + attribute_size;
    nlmsghdr socket_header{};
    socket_header.nlmsg_len = NLMSG_LENGTH(payload_size);
    socket_header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    socket_header.nlmsg_seq = sequence;
    std::vector<std::byte> result(NLMSG_SPACE(payload_size) + (done ? NLMSG_SPACE(0) : 0));
    std::memcpy(result.data(), &socket_header, sizeof(socket_header));
    std::memcpy(result.data() + NLMSG_HDRLEN, &message, sizeof(message));
    if (tcp_info) {
        auto* attribute = reinterpret_cast<nlattr*>(
            result.data() + NLMSG_HDRLEN + sizeof(message));
        attribute->nla_len = static_cast<std::uint16_t>(sizeof(nlattr) + tcp_info->size());
        attribute->nla_type = INET_DIAG_INFO;
        if (!tcp_info->empty()) {
            std::memcpy(reinterpret_cast<std::byte*>(attribute) + sizeof(nlattr),
                        tcp_info->data(), tcp_info->size());
        }
    }
    if (done) {
        nlmsghdr completion{};
        completion.nlmsg_len = NLMSG_LENGTH(0);
        completion.nlmsg_type = NLMSG_DONE;
        completion.nlmsg_seq = sequence;
        std::memcpy(result.data() + NLMSG_SPACE(payload_size), &completion, sizeof(completion));
    }
    return result;
}

std::vector<std::byte> tcpInfoPrefix(std::size_t size, std::uint32_t rtt,
                                     std::uint64_t bytes_acked,
                                     std::uint32_t total_retrans) {
    std::vector<std::byte> result(size);
    auto copy = [&](std::size_t offset, const auto& value) {
        if (offset + sizeof(value) <= result.size()) {
            std::memcpy(result.data() + offset, &value, sizeof(value));
        }
    };
    copy(offsetof(struct tcp_info, tcpi_rtt), rtt);
    copy(offsetof(struct tcp_info, tcpi_bytes_acked), bytes_acked);
    copy(offsetof(struct tcp_info, tcpi_total_retrans), total_retrans);
    return result;
}
}

int main() {
    bool ok = true;
    const NetnsId netns{1, 2};
    const auto v4 = fixture(AF_INET, 42);
    const auto parsed4 = SocketDiagParser::parse(v4.data(), v4.size(), 0, 42, netns);
    ok &= expect(!parsed4.malformed && parsed4.completed && parsed4.messages.size() == 2,
                 "IPv4 multipart fixture did not complete");
    if (!parsed4.messages.empty() && parsed4.messages[0].socket) {
        const auto& record = *parsed4.messages[0].socket;
        ok &= expect(record.tuple.local.port == 12345 && record.tuple.remote.port == 443,
                     "sock_diag ports were not converted from network order");
        ok &= expect(record.tuple.local.address[0] == 192 && record.tuple.remote.address[0] == 198,
                     "IPv4 addresses were not parsed");
        ok &= expect(record.cookie && record.cookie->value == 0x5566778811223344ULL,
                     "sock_diag cookie low/high words were reconstructed incorrectly");
        ok &= expect(record.diag_ifindex && *record.diag_ifindex == 7,
                     "idiag_if metadata was not retained");
    }
    const auto v6 = fixture(AF_INET6, 43);
    const auto parsed6 = SocketDiagParser::parse(v6.data(), v6.size(), 0, 43, netns);
    ok &= expect(!parsed6.malformed && parsed6.messages[0].socket &&
                     parsed6.messages[0].socket->tuple.local.address[0] == 0x20,
                 "IPv6 address fixture did not parse");
    ok &= expect(SocketDiagParser::parse(v4.data(), v4.size(), 9, 42, netns).malformed,
                 "non-kernel sender was accepted");
    ok &= expect(SocketDiagParser::parse(v4.data(), v4.size(), 0, 99, netns).malformed,
                 "wrong sequence was accepted");
    const auto incomplete = fixture(AF_INET, 44, false);
    ok &= expect(!SocketDiagParser::parse(incomplete.data(), incomplete.size(), 0, 44, netns).completed,
                 "dump without NLMSG_DONE completed");

    const auto unavailable = fixture(AF_INET, 45, true, INET_DIAG_NOCOOKIE,
                                     INET_DIAG_NOCOOKIE);
    const auto parsed_unavailable =
        SocketDiagParser::parse(unavailable.data(), unavailable.size(), 0, 45, netns);
    ok &= expect(parsed_unavailable.messages[0].socket &&
                     !parsed_unavailable.messages[0].socket->cookie,
                 "INET_DIAG_NOCOOKIE was treated as an available cookie");

    const auto zero = fixture(AF_INET, 46, true, 0, 0);
    const auto parsed_zero = SocketDiagParser::parse(zero.data(), zero.size(), 0, 46, netns);
    ok &= expect(parsed_zero.messages[0].socket && parsed_zero.messages[0].socket->cookie &&
                     parsed_zero.messages[0].socket->cookie->value == 0,
                 "valid zero cookie was treated as unavailable");

    const auto early_size = offsetof(struct tcp_info, tcpi_rtt) + sizeof(std::uint32_t);
    const auto early = fixture(AF_INET, 47, true, 1, 2,
                               tcpInfoPrefix(early_size, 1234, 0, 0));
    const auto parsed_early = SocketDiagParser::parse(early.data(), early.size(), 0, 47, netns);
    ok &= expect(parsed_early.messages[0].socket && parsed_early.messages[0].socket->tcp_info &&
                     parsed_early.messages[0].socket->tcp_info->rtt_us == 1234 &&
                     !parsed_early.messages[0].socket->tcp_info->bytes_acked,
                 "short TCP_INFO prefix did not preserve field availability");

    const auto counter_prefix_size = offsetof(struct tcp_info, tcpi_total_retrans) +
                                     sizeof(std::uint32_t);
    const auto counter_prefix = fixture(AF_INET, 51, true, 1, 2,
                                        tcpInfoPrefix(counter_prefix_size, 1234, 0, 7));
    const auto parsed_counter_prefix =
        SocketDiagParser::parse(counter_prefix.data(), counter_prefix.size(), 0, 51, netns);
    ok &= expect(parsed_counter_prefix.messages[0].socket &&
                     parsed_counter_prefix.messages[0].socket->tcp_info &&
                     parsed_counter_prefix.messages[0].socket->tcp_info->total_retrans == 7 &&
                     !parsed_counter_prefix.messages[0].socket->tcp_info->bytes_acked,
                 "TCP_INFO counter prefix exposed unavailable newer fields");

    const auto full = fixture(AF_INET, 48, true, 1, 2,
                              tcpInfoPrefix(sizeof(struct tcp_info), 4321, 987654, 4));
    const auto parsed_full = SocketDiagParser::parse(full.data(), full.size(), 0, 48, netns);
    ok &= expect(parsed_full.messages[0].socket && parsed_full.messages[0].socket->tcp_info &&
                     parsed_full.messages[0].socket->tcp_info->bytes_acked == 987654 &&
                     parsed_full.messages[0].socket->tcp_info->total_retrans == 4,
                 "full TCP_INFO payload did not parse known fields");

    auto extended_info = tcpInfoPrefix(sizeof(struct tcp_info) + 32, 11, 22, 33);
    const auto extended = fixture(AF_INET, 49, true, 1, 2, extended_info);
    const auto parsed_extended = SocketDiagParser::parse(extended.data(), extended.size(), 0, 49, netns);
    ok &= expect(!parsed_extended.malformed && parsed_extended.messages[0].socket &&
                     parsed_extended.messages[0].socket->tcp_info,
                 "newer TCP_INFO payload was rejected");

    const auto malformed_info = fixture(AF_INET, 50, true, 1, 2,
                                         std::vector<std::byte>{});
    const auto parsed_malformed =
        SocketDiagParser::parse(malformed_info.data(), malformed_info.size(), 0, 50, netns);
    ok &= expect(!parsed_malformed.malformed && parsed_malformed.messages[0].socket &&
                     parsed_malformed.messages[0].socket->tcp_info_malformed,
                 "malformed TCP_INFO attribute destroyed valid inventory");
    return ok ? 0 : 1;
}
