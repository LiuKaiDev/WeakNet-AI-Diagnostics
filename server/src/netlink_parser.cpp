#include "netlink_parser.hpp"

#if defined(__linux__)
#include <arpa/inet.h>
#include <linux/if.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <netinet/in.h>

#include <algorithm>
#include <cstring>

namespace weaknet_dbus::v2 {
namespace {

template <typename T>
bool copyAttribute(const rtattr* attribute, T& result) {
    if (!attribute || RTA_PAYLOAD(attribute) < static_cast<int>(sizeof(T))) return false;
    std::memcpy(&result, RTA_DATA(attribute), sizeof(T));
    return true;
}

std::array<std::uint8_t, 16> attributeAddress(const rtattr* attribute, std::uint8_t family) {
    std::array<std::uint8_t, 16> result{};
    if (!attribute) return result;
    const auto size = family == AF_INET ? sizeof(in_addr) : sizeof(in6_addr);
    if (RTA_PAYLOAD(attribute) >= static_cast<int>(size)) std::memcpy(result.data(), RTA_DATA(attribute), size);
    return result;
}

bool parseAttributes(const rtattr* first, int length, std::vector<const rtattr*>& attributes) {
    if (length < 0) return false;
    while (length > 0) {
        if (!RTA_OK(first, length)) return false;
        attributes.push_back(first);
        first = RTA_NEXT(first, length);
    }
    return true;
}

LinkFact parseLink(const ifinfomsg* message, const std::vector<const rtattr*>& attributes, NetnsId netns) {
    LinkFact result;
    result.netns = netns;
    result.interface.ifindex = static_cast<std::uint32_t>(message->ifi_index);
    result.flags = message->ifi_flags;
    result.link_type = message->ifi_type;
    for (const auto* attribute : attributes) {
        const auto type = attribute->rta_type & NLA_TYPE_MASK;
        if (type == IFLA_IFNAME && RTA_PAYLOAD(attribute) > 0) {
            const auto* name = static_cast<const char*>(RTA_DATA(attribute));
            result.interface.observed_name.assign(name, strnlen(name, RTA_PAYLOAD(attribute)));
        } else if (type == IFLA_OPERSTATE) {
            copyAttribute(attribute, result.oper_state);
        } else if (type == IFLA_CARRIER) {
            copyAttribute(attribute, result.carrier);
            result.carrier_known = true;
        }
    }
    return result;
}

AddressFact parseAddress(const ifaddrmsg* message, const std::vector<const rtattr*>& attributes,
                         NetnsId netns) {
    AddressFact result;
    result.netns = netns;
    result.interface.ifindex = message->ifa_index;
    result.family = message->ifa_family;
    result.prefix_length = message->ifa_prefixlen;
    result.scope = message->ifa_scope;
    result.flags = message->ifa_flags;
    const rtattr* address = nullptr;
    for (const auto* attribute : attributes) {
        const auto type = attribute->rta_type & NLA_TYPE_MASK;
        if (type == IFA_LOCAL) address = attribute;
        else if (type == IFA_ADDRESS && !address) address = attribute;
        else if (type == IFA_FLAGS) copyAttribute(attribute, result.flags);
    }
    result.address = attributeAddress(address, result.family);
    return result;
}

bool validAddressAttribute(const rtattr* attribute, std::uint8_t family) {
    if (!attribute) return true;
    const auto expected = family == AF_INET ? sizeof(in_addr) :
                          family == AF_INET6 ? sizeof(in6_addr) : 0U;
    return expected == 0U || RTA_PAYLOAD(attribute) == expected;
}

bool validMultipath(const rtattr* attribute, std::uint8_t family) {
    if (!attribute) return true;
    int length = RTA_PAYLOAD(attribute);
    auto* nexthop = static_cast<const rtnexthop*>(RTA_DATA(attribute));
    while (length > 0) {
        if (length < static_cast<int>(sizeof(rtnexthop)) ||
            nexthop->rtnh_len < sizeof(rtnexthop) ||
            nexthop->rtnh_len > static_cast<std::size_t>(length)) return false;
        int nested_length = static_cast<int>(nexthop->rtnh_len) -
                            static_cast<int>(sizeof(rtnexthop));
        auto* nested = reinterpret_cast<const rtattr*>(
            reinterpret_cast<const std::byte*>(nexthop) + sizeof(rtnexthop));
        while (nested_length > 0) {
            if (!RTA_OK(nested, nested_length)) return false;
            if ((nested->rta_type & NLA_TYPE_MASK) == RTA_GATEWAY &&
                !validAddressAttribute(nested, family)) return false;
            nested = RTA_NEXT(nested, nested_length);
        }
        const auto aligned = RTNH_ALIGN(nexthop->rtnh_len);
        if (aligned > static_cast<std::size_t>(length)) return false;
        length -= static_cast<int>(aligned);
        nexthop = RTNH_NEXT(nexthop);
    }
    return true;
}

RouteFact parseRoute(const rtmsg* message, const std::vector<const rtattr*>& attributes,
                     NetnsId netns, bool& valid) {
    RouteFact result;
    result.netns = netns;
    result.family = message->rtm_family;
    result.destination_prefix = message->rtm_dst_len;
    result.table = message->rtm_table;
    result.protocol = message->rtm_protocol;
    result.scope = message->rtm_scope;
    result.type = message->rtm_type;
    const rtattr* multipath = nullptr;
    for (const auto* attribute : attributes) {
        const auto type = attribute->rta_type & NLA_TYPE_MASK;
        switch (type) {
            case RTA_DST: result.destination = attributeAddress(attribute, result.family); break;
            case RTA_GATEWAY: result.gateway = attributeAddress(attribute, result.family); break;
            case RTA_PREFSRC: result.preferred_source = attributeAddress(attribute, result.family); break;
            case RTA_OIF: { std::uint32_t value{}; if (copyAttribute(attribute, value)) result.output_ifindex = value; break; }
            case RTA_PRIORITY: copyAttribute(attribute, result.priority); break;
            case RTA_TABLE: copyAttribute(attribute, result.table); break;
            case RTA_MULTIPATH: multipath = attribute; break;
            default: break;
        }
    }
    for (const auto* attribute : attributes) {
        const auto type = attribute->rta_type & NLA_TYPE_MASK;
        if ((type == RTA_DST || type == RTA_GATEWAY || type == RTA_PREFSRC) &&
            !validAddressAttribute(attribute, result.family)) valid = false;
        if ((type == RTA_OIF || type == RTA_PRIORITY || type == RTA_TABLE) &&
            RTA_PAYLOAD(attribute) != sizeof(std::uint32_t)) valid = false;
        if (type == RTA_MULTIPATH && !validMultipath(attribute, result.family)) valid = false;
    }
    if (!valid) return result;
    if (multipath) {
        int length = RTA_PAYLOAD(multipath);
        auto* nexthop = static_cast<const rtnexthop*>(RTA_DATA(multipath));
        while (length > 0) {
            if (length < static_cast<int>(sizeof(rtnexthop)) || nexthop->rtnh_len < sizeof(rtnexthop) ||
                nexthop->rtnh_len > length) break;
            RouteNexthop item;
            item.ifindex = nexthop->rtnh_ifindex;
            item.hops = nexthop->rtnh_hops;
            item.flags = nexthop->rtnh_flags;
            int nested_length = static_cast<int>(nexthop->rtnh_len) - static_cast<int>(sizeof(rtnexthop));
            auto* nested = reinterpret_cast<rtattr*>(reinterpret_cast<char*>(const_cast<rtnexthop*>(nexthop)) + sizeof(rtnexthop));
            while (nested_length > 0 && RTA_OK(nested, nested_length)) {
                if ((nested->rta_type & NLA_TYPE_MASK) == RTA_GATEWAY) {
                    item.gateway = attributeAddress(nested, result.family);
                }
                nested = RTA_NEXT(nested, nested_length);
            }
            result.multipath.push_back(item);
            length -= RTNH_ALIGN(nexthop->rtnh_len);
            nexthop = RTNH_NEXT(nexthop);
        }
    }
    return result;
}

}  // namespace

ParseResult RtnetlinkParser::parse(const void* data, std::size_t size,
                                   std::uint32_t sender_pid,
                                   std::optional<std::uint32_t> expected_sequence,
                                   NetnsId netns, bool dump_response) {
    ParseResult result;
    if (sender_pid != 0) { result.malformed = true; result.error = "unexpected netlink sender"; return result; }
    if (!data || size == 0) { result.malformed = true; result.error = "empty netlink datagram"; return result; }
    int remaining = static_cast<int>(size);
    auto* header = static_cast<const nlmsghdr*>(data);
    while (remaining > 0) {
        if (remaining < static_cast<int>(sizeof(nlmsghdr)) || header->nlmsg_len < sizeof(nlmsghdr) ||
            header->nlmsg_len > static_cast<std::uint32_t>(remaining)) {
            result.malformed = true; result.error = "malformed nlmsg_len"; break;
        }
        const auto sequence = header->nlmsg_seq;
        if (expected_sequence && sequence != *expected_sequence && sequence != 0) {
            result.malformed = true; result.error = "unexpected netlink sequence"; break;
        }
        if (!expected_sequence && sequence != 0) {
            result.malformed = true; result.error = "unexpected notification sequence"; break;
        }
        if (header->nlmsg_pid != 0 && header->nlmsg_pid != sender_pid) {
            result.malformed = true; result.error = "unexpected netlink sender"; break;
        }
        ParsedMessage parsed;
        parsed.sequence = sequence;
        parsed.sender_pid = header->nlmsg_pid;
        if (header->nlmsg_type == NLMSG_DONE) {
            if (expected_sequence && sequence != *expected_sequence) {
                result.malformed = true;
                result.error = "unexpected completion sequence";
                break;
            }
            parsed.kind = ParsedMessageKind::Done;
            parsed.dump_interrupted = (header->nlmsg_flags & NLM_F_DUMP_INTR) != 0;
            result.completed = !parsed.dump_interrupted;
            result.messages.push_back(parsed);
        } else if (header->nlmsg_type == NLMSG_ERROR) {
            if (expected_sequence && sequence != *expected_sequence) {
                result.malformed = true;
                result.error = "unexpected error sequence";
                break;
            }
            parsed.kind = ParsedMessageKind::Error;
            if (NLMSG_PAYLOAD(header, 0) < sizeof(nlmsgerr)) {
                result.malformed = true; result.error = "short NLMSG_ERROR"; break;
            }
            const auto* error = static_cast<const nlmsgerr*>(NLMSG_DATA(header));
            parsed.error_code = static_cast<std::uint16_t>(error->error < 0 ? -error->error : error->error);
            result.messages.push_back(parsed);
            if (error->error != 0) { result.error = "netlink request error"; }
        } else {
            const auto type = header->nlmsg_type;
            if (type == RTM_NEWLINK || type == RTM_DELLINK) {
                if (NLMSG_PAYLOAD(header, 0) < sizeof(ifinfomsg)) { result.malformed = true; result.error = "short link message"; break; }
                const auto* message = static_cast<const ifinfomsg*>(NLMSG_DATA(header));
                const int length = IFLA_PAYLOAD(header);
                if (length < 0) { result.malformed = true; result.error = "invalid link attributes"; break; }
                std::vector<const rtattr*> attributes;
                if (!parseAttributes(IFLA_RTA(message), length, attributes)) { result.malformed = true; result.error = "malformed link attributes"; break; }
                bool valid = true;
                for (const auto* attribute : attributes) {
                    const auto attribute_type = attribute->rta_type & NLA_TYPE_MASK;
                    if (attribute_type == IFLA_IFNAME && RTA_PAYLOAD(attribute) == 0) valid = false;
                    if ((attribute_type == IFLA_OPERSTATE || attribute_type == IFLA_CARRIER) &&
                        RTA_PAYLOAD(attribute) != sizeof(std::uint8_t)) valid = false;
                }
                if (!valid) { result.malformed = true; result.error = "malformed link attributes"; break; }
                parsed.kind = ParsedMessageKind::Link;
                parsed.link = parseLink(message, attributes, netns);
                parsed.link->present = type == RTM_NEWLINK;
            } else if (type == RTM_NEWADDR || type == RTM_DELADDR) {
                if (NLMSG_PAYLOAD(header, 0) < sizeof(ifaddrmsg)) { result.malformed = true; result.error = "short address message"; break; }
                const auto* message = static_cast<const ifaddrmsg*>(NLMSG_DATA(header));
                std::vector<const rtattr*> attributes;
                if (!parseAttributes(IFA_RTA(message), IFA_PAYLOAD(header), attributes)) { result.malformed = true; result.error = "malformed address attributes"; break; }
                bool valid = true;
                for (const auto* attribute : attributes) {
                    const auto attribute_type = attribute->rta_type & NLA_TYPE_MASK;
                    if ((attribute_type == IFA_LOCAL || attribute_type == IFA_ADDRESS) &&
                        !validAddressAttribute(attribute, message->ifa_family)) valid = false;
                    if (attribute_type == IFA_FLAGS &&
                        RTA_PAYLOAD(attribute) != sizeof(std::uint32_t)) valid = false;
                }
                if (!valid) { result.malformed = true; result.error = "malformed address attributes"; break; }
                parsed.kind = ParsedMessageKind::Address;
                parsed.address = parseAddress(message, attributes, netns);
                parsed.address->present = type == RTM_NEWADDR;
            } else if (type == RTM_NEWROUTE || type == RTM_DELROUTE) {
                if (NLMSG_PAYLOAD(header, 0) < sizeof(rtmsg)) { result.malformed = true; result.error = "short route message"; break; }
                const auto* message = static_cast<const rtmsg*>(NLMSG_DATA(header));
                std::vector<const rtattr*> attributes;
                if (!parseAttributes(RTM_RTA(message), RTM_PAYLOAD(header), attributes)) { result.malformed = true; result.error = "malformed route attributes"; break; }
                parsed.kind = ParsedMessageKind::Route;
                bool valid = true;
                parsed.route = parseRoute(message, attributes, netns, valid);
                if (!valid) { result.malformed = true; result.error = "malformed route attributes"; break; }
                parsed.route->present = type == RTM_NEWROUTE;
            } else {
                parsed.kind = ParsedMessageKind::Notification;
            }
            result.messages.push_back(std::move(parsed));
        }
        const auto aligned = NLMSG_ALIGN(header->nlmsg_len);
        if (aligned > static_cast<std::size_t>(remaining)) { result.truncated = true; result.error = "truncated netlink message"; break; }
        remaining -= static_cast<int>(aligned);
        header = reinterpret_cast<const nlmsghdr*>(reinterpret_cast<const char*>(header) + aligned);
    }
    // A multipart dump commonly spans several datagrams.  Completion is a
    // property of the receive loop, not of an individual datagram.
    (void)dump_response;
    return result;
}

}  // namespace weaknet_dbus::v2
#else
namespace weaknet_dbus::v2 { ParseResult RtnetlinkParser::parse(const void*, std::size_t, std::uint32_t, std::optional<std::uint32_t>, NetnsId, bool) { return {}; } }
#endif
