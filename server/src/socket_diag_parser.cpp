#include "socket_diag_parser.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>

#include <cstring>

namespace weaknet_dbus::v2 {
namespace {

bool knownAttribute(std::uint16_t type) {
    switch (type & NLA_TYPE_MASK) {
        case INET_DIAG_MEMINFO:
        case INET_DIAG_INFO:
        case INET_DIAG_VEGASINFO:
        case INET_DIAG_CONG:
        case INET_DIAG_TOS:
        case INET_DIAG_TCLASS:
        case INET_DIAG_SKMEMINFO:
        case INET_DIAG_SHUTDOWN:
        case INET_DIAG_DCTCPINFO:
        case INET_DIAG_PROTOCOL:
        case INET_DIAG_SKV6ONLY:
        case INET_DIAG_LOCALS:
        case INET_DIAG_PEERS:
            return true;
        default:
            return false;
    }
}

std::optional<TcpInfoRaw> parseTcpInfo(const std::byte* payload, std::size_t length,
                                       bool& malformed) {
    // tcpi_unacked is the first field represented by TcpInfoRaw. A shorter
    // payload is still safe to receive, but cannot contain a usable prefix.
    if (length < offsetof(struct tcp_info, tcpi_unacked)) {
        malformed = true;
        return std::nullopt;
    }
    TcpInfoRaw result;
    const auto read = [&](auto& destination, std::size_t offset) {
        using Value = typename std::decay_t<decltype(destination)>::value_type;
        if (length < offset + sizeof(Value)) return;
        Value value{};
        std::memcpy(&value, payload + offset, sizeof(value));
        destination = value;
    };
    read(result.unacked, offsetof(struct tcp_info, tcpi_unacked));
    read(result.lost, offsetof(struct tcp_info, tcpi_lost));
    read(result.retrans, offsetof(struct tcp_info, tcpi_retrans));
    read(result.rtt_us, offsetof(struct tcp_info, tcpi_rtt));
    read(result.rttvar_us, offsetof(struct tcp_info, tcpi_rttvar));
    read(result.snd_ssthresh, offsetof(struct tcp_info, tcpi_snd_ssthresh));
    read(result.snd_cwnd, offsetof(struct tcp_info, tcpi_snd_cwnd));
    read(result.reordering, offsetof(struct tcp_info, tcpi_reordering));
    read(result.rcv_space, offsetof(struct tcp_info, tcpi_rcv_space));
    read(result.total_retrans, offsetof(struct tcp_info, tcpi_total_retrans));
    read(result.bytes_acked, offsetof(struct tcp_info, tcpi_bytes_acked));
    read(result.bytes_received, offsetof(struct tcp_info, tcpi_bytes_received));
    read(result.segs_out, offsetof(struct tcp_info, tcpi_segs_out));
    read(result.segs_in, offsetof(struct tcp_info, tcpi_segs_in));
    read(result.data_segs_in, offsetof(struct tcp_info, tcpi_data_segs_in));
    read(result.data_segs_out, offsetof(struct tcp_info, tcpi_data_segs_out));
    read(result.delivered, offsetof(struct tcp_info, tcpi_delivered));
    read(result.delivered_ce, offsetof(struct tcp_info, tcpi_delivered_ce));
    return result;
}

bool parseAttributes(const std::byte* first, std::size_t length, std::string& error,
                     std::optional<TcpInfoRaw>& tcp_info, bool& tcp_info_malformed) {
    while (length != 0) {
        if (length < sizeof(nlattr)) { error = "short sock_diag attribute"; return false; }
        const auto* attribute = reinterpret_cast<const nlattr*>(first);
        if (attribute->nla_len < sizeof(nlattr) || attribute->nla_len > length) {
            error = "malformed sock_diag attribute length"; return false;
        }
        const auto aligned = static_cast<std::size_t>(NLA_ALIGN(attribute->nla_len));
        if (aligned > length) { error = "truncated sock_diag attribute"; return false; }
        const auto type = attribute->nla_type & NLA_TYPE_MASK;
        if (type == INET_DIAG_INFO) {
            bool malformed = false;
            tcp_info = parseTcpInfo(reinterpret_cast<const std::byte*>(attribute) + sizeof(nlattr),
                                    attribute->nla_len - sizeof(nlattr), malformed);
            tcp_info_malformed = malformed;
        } else if (knownAttribute(attribute->nla_type) && attribute->nla_len == sizeof(nlattr)) {
            error = "empty known sock_diag attribute"; return false;
        }
        first += aligned;
        length -= aligned;
    }
    return true;
}

SocketDiagRecord parseSocket(const inet_diag_msg& message, NetnsId netns) {
    SocketDiagRecord result;
    result.tuple.netns = netns;
    result.tuple.protocol = SocketProtocol::Tcp;
    result.tuple.family = message.idiag_family;
    result.tuple.local.family = message.idiag_family;
    result.tuple.remote.family = message.idiag_family;
    result.tuple.local.port = ntohs(message.id.idiag_sport);
    result.tuple.remote.port = ntohs(message.id.idiag_dport);
    const auto address_size = message.idiag_family == AF_INET ? 4U : 16U;
    std::memcpy(result.tuple.local.address.data(), message.id.idiag_src, address_size);
    std::memcpy(result.tuple.remote.address.data(), message.id.idiag_dst, address_size);
    result.tcp_state.raw = message.idiag_state;
    result.diag_ifindex = message.id.idiag_if;
    result.uid = message.idiag_uid;
    result.inode = message.idiag_inode;
    // Linux exports the native u64 cookie as low word first, then high word.
    // These are not network-order fields. INET_DIAG_NOCOOKIE in both words
    // means that no cookie is available; zero is a valid cookie value.
    if (message.id.idiag_cookie[0] != INET_DIAG_NOCOOKIE ||
        message.id.idiag_cookie[1] != INET_DIAG_NOCOOKIE) {
        const auto value = static_cast<std::uint64_t>(message.id.idiag_cookie[0]) |
                           (static_cast<std::uint64_t>(message.id.idiag_cookie[1]) << 32U);
        result.cookie = KernelSocketCookie{value};
    }
    return result;
}

}  // namespace

SocketDiagParseResult SocketDiagParser::parse(const void* data, std::size_t size,
                                               std::uint32_t sender_pid,
                                               std::optional<std::uint32_t> expected_sequence,
                                               NetnsId netns) {
    SocketDiagParseResult result;
    if (sender_pid != 0) { result.malformed = true; result.error = "unexpected netlink sender"; return result; }
    if (!data || size == 0) { result.malformed = true; result.error = "empty netlink datagram"; return result; }
    std::size_t remaining = size;
    const auto* cursor = static_cast<const std::byte*>(data);
    while (remaining != 0) {
        if (remaining < sizeof(nlmsghdr)) { result.malformed = true; result.error = "short netlink header"; break; }
        const auto* header = reinterpret_cast<const nlmsghdr*>(cursor);
        if (header->nlmsg_len < sizeof(nlmsghdr) || header->nlmsg_len > remaining) {
            result.malformed = true; result.error = "malformed nlmsg_len"; break;
        }
        if (expected_sequence && header->nlmsg_seq != *expected_sequence) {
            result.malformed = true; result.error = "unexpected netlink sequence"; break;
        }
        if (header->nlmsg_pid != 0) {
            result.malformed = true; result.error = "non-kernel netlink sender"; break;
        }
        if ((header->nlmsg_flags & NLM_F_DUMP_INTR) != 0) result.dump_interrupted = true;
        SocketDiagMessage parsed;
        parsed.sequence = header->nlmsg_seq;
        parsed.sender_pid = header->nlmsg_pid;
        if (header->nlmsg_type == NLMSG_DONE) {
            parsed.kind = SocketDiagMessageKind::Done;
            parsed.dump_interrupted = (header->nlmsg_flags & NLM_F_DUMP_INTR) != 0;
            result.completed = !parsed.dump_interrupted;
            result.messages.push_back(parsed);
        } else if (header->nlmsg_type == NLMSG_ERROR) {
            if (NLMSG_PAYLOAD(header, 0) < sizeof(nlmsgerr)) {
                result.malformed = true; result.error = "short NLMSG_ERROR"; break;
            }
            parsed.kind = SocketDiagMessageKind::Error;
            const auto* error = static_cast<const nlmsgerr*>(NLMSG_DATA(header));
            parsed.error_code = static_cast<std::uint16_t>(error->error < 0 ? -error->error : error->error);
            result.messages.push_back(parsed);
            if (error->error != 0) result.error = "netlink request error";
        } else if (header->nlmsg_type == SOCK_DIAG_BY_FAMILY) {
            if (NLMSG_PAYLOAD(header, 0) < sizeof(inet_diag_msg)) {
                result.malformed = true; result.error = "short inet_diag_msg"; break;
            }
            const auto* message = static_cast<const inet_diag_msg*>(NLMSG_DATA(header));
            if (message->idiag_family != AF_INET && message->idiag_family != AF_INET6) {
                result.malformed = true; result.error = "unsupported inet_diag family"; break;
            }
            bool tcp_info_malformed = false;
            std::optional<TcpInfoRaw> tcp_info;
            if (!parseAttributes(reinterpret_cast<const std::byte*>(message) + sizeof(inet_diag_msg),
                                 NLMSG_PAYLOAD(header, 0) - sizeof(inet_diag_msg), result.error,
                                 tcp_info, tcp_info_malformed)) {
                result.malformed = true; break;
            }
            parsed.socket = parseSocket(*message, netns);
            parsed.socket->tcp_info = std::move(tcp_info);
            parsed.socket->tcp_info_malformed = tcp_info_malformed;
            result.messages.push_back(std::move(parsed));
        } else {
            result.malformed = true;
            result.error = "unexpected sock_diag message type";
            break;
        }
        const auto aligned = NLMSG_ALIGN(header->nlmsg_len);
        if (aligned > remaining) { result.malformed = true; result.error = "truncated netlink message"; break; }
        cursor += aligned;
        remaining -= aligned;
    }
    return result;
}

}  // namespace weaknet_dbus::v2
#else
namespace weaknet_dbus::v2 {
SocketDiagParseResult SocketDiagParser::parse(const void*, std::size_t, std::uint32_t,
                                               std::optional<std::uint32_t>, NetnsId) { return {}; }
}
#endif
