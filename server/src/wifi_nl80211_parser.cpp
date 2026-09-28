#include "wifi_nl80211_parser.hpp"

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>
#endif

#include <algorithm>
#include <limits>
#include <vector>

namespace weaknet_dbus::v2 {
namespace {

#if defined(__linux__)
struct AttributeList {
    std::vector<const nlattr*> values;
    bool valid{true};
};

AttributeList attributes(const void* data, std::size_t length) {
    AttributeList result;
    const auto* current = static_cast<const nlattr*>(data);
    std::size_t remaining = length;
    while (remaining != 0) {
        if (remaining < sizeof(nlattr) || current->nla_len < sizeof(nlattr) ||
            current->nla_len > remaining) {
            result.valid = false;
            return result;
        }
        result.values.push_back(current);
        const auto aligned = NLA_ALIGN(current->nla_len);
        if (aligned > remaining) {
            result.valid = false;
            return result;
        }
        remaining -= aligned;
        current = reinterpret_cast<const nlattr*>(
            reinterpret_cast<const std::byte*>(current) + aligned);
    }
    return result;
}

std::optional<const nlattr*> findAttribute(const AttributeList& list, int type) {
    for (const auto* attribute : list.values)
        if ((attribute->nla_type & NLA_TYPE_MASK) == type) return attribute;
    return std::nullopt;
}

template <typename T>
std::optional<T> scalar(const nlattr* attribute) {
    if (!attribute || attribute->nla_len - sizeof(nlattr) != sizeof(T)) return std::nullopt;
    T value{};
    std::memcpy(&value, reinterpret_cast<const std::byte*>(attribute) + sizeof(nlattr), sizeof(T));
    return value;
}

std::optional<std::string> bytes(const nlattr* attribute) {
    if (!attribute || attribute->nla_len < sizeof(nlattr)) return std::nullopt;
    const auto size = attribute->nla_len - sizeof(nlattr);
    return std::string(reinterpret_cast<const char*>(attribute) + sizeof(nlattr), size);
}

WifiParseStatus validateHeader(const nlmsghdr* header, std::size_t remaining,
                               std::uint32_t sender_pid, std::uint32_t expected_sequence) {
    if (remaining < sizeof(nlmsghdr) || header->nlmsg_len < sizeof(nlmsghdr) ||
        header->nlmsg_len > remaining) return WifiParseStatus::Malformed;
    if (header->nlmsg_pid != sender_pid) return WifiParseStatus::WrongSender;
    if (header->nlmsg_seq != expected_sequence) return WifiParseStatus::WrongSequence;
    return WifiParseStatus::Ok;
}

template <typename Callback>
WifiParseStatus walkMessages(const void* data, std::size_t size, std::uint32_t sender_pid,
                             std::uint32_t expected_sequence, Callback callback,
                             int* error_code) {
    const auto* bytes_data = static_cast<const std::byte*>(data);
    std::size_t remaining = size;
    while (remaining != 0) {
        if (remaining < sizeof(nlmsghdr)) return WifiParseStatus::Malformed;
        const auto* header = reinterpret_cast<const nlmsghdr*>(bytes_data);
        const auto validation = validateHeader(header, remaining, sender_pid, expected_sequence);
        if (validation != WifiParseStatus::Ok) return validation;
        if (header->nlmsg_type == NLMSG_DONE) return WifiParseStatus::Done;
        if (header->nlmsg_type == NLMSG_ERROR) {
            if (header->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr))) return WifiParseStatus::Malformed;
            nlmsgerr error{};
            std::memcpy(&error, NLMSG_DATA(header), sizeof(error));
            if (error.error == 0) return WifiParseStatus::Done;
            if (error_code) *error_code = -error.error;
            return error.error == -ENOENT ? WifiParseStatus::NotFound
                                          : WifiParseStatus::ErrorReply;
        }
        if (header->nlmsg_type < NLMSG_MIN_TYPE) return WifiParseStatus::Malformed;
        if (header->nlmsg_len < NLMSG_LENGTH(sizeof(genlmsghdr))) return WifiParseStatus::Malformed;
        const auto* generic = static_cast<const genlmsghdr*>(NLMSG_DATA(header));
        const auto payload_length = header->nlmsg_len - NLMSG_LENGTH(sizeof(genlmsghdr));
        const auto attr_result = attributes(
            reinterpret_cast<const std::byte*>(generic) + sizeof(genlmsghdr), payload_length);
        if (!attr_result.valid) return WifiParseStatus::Malformed;
        const auto result = callback(generic, attr_result);
        if (result != WifiParseStatus::Ok) return result;
        const auto aligned = NLMSG_ALIGN(header->nlmsg_len);
        if (aligned > remaining) return WifiParseStatus::Malformed;
        bytes_data += aligned;
        remaining -= aligned;
    }
    return WifiParseStatus::Malformed;
}

std::optional<WifiStationReply> parseStationAttributes(const AttributeList& top_level) {
    WifiStationReply result;
    if (const auto mac = findAttribute(top_level, NL80211_ATTR_MAC)) {
        if ((*mac)->nla_len - sizeof(nlattr) != 6) return std::nullopt;
        result.bssid = WifiBssid{};
        std::memcpy(result.bssid->address.data(),
                    reinterpret_cast<const std::byte*>(*mac) + sizeof(nlattr), 6);
    }
    const auto info = findAttribute(top_level, NL80211_ATTR_STA_INFO);
    if (!info) return result;
    const auto nested = attributes(reinterpret_cast<const std::byte*>(*info) + sizeof(nlattr),
                                   (*info)->nla_len - sizeof(nlattr));
    if (!nested.valid) return std::nullopt;
    auto signal = findAttribute(nested, NL80211_STA_INFO_SIGNAL);
    if (signal) {
        const auto value = scalar<std::uint8_t>(*signal);
        if (!value) return std::nullopt;
        result.signal_dbm = static_cast<std::int8_t>(*value);
    }
    auto signal_avg = findAttribute(nested, NL80211_STA_INFO_SIGNAL_AVG);
    if (signal_avg) {
        const auto value = scalar<std::uint8_t>(*signal_avg);
        if (!value) return std::nullopt;
        result.signal_avg_dbm = static_cast<std::int8_t>(*value);
    }
    bool malformed_rate = false;
    auto parseRate = [&malformed_rate](const nlattr* attribute) -> std::optional<std::uint32_t> {
        if (!attribute) return std::nullopt;
        const auto rate = attributes(reinterpret_cast<const std::byte*>(attribute) + sizeof(nlattr),
                                     attribute->nla_len - sizeof(nlattr));
        if (!rate.valid) {
            malformed_rate = true;
            return std::nullopt;
        }
        for (const auto* item : rate.values) {
            const auto type = item->nla_type & NLA_TYPE_MASK;
            if (type == NL80211_RATE_INFO_BITRATE32 &&
                item->nla_len - sizeof(nlattr) != sizeof(std::uint32_t)) {
                malformed_rate = true;
                return std::nullopt;
            }
            if (type == NL80211_RATE_INFO_BITRATE &&
                item->nla_len - sizeof(nlattr) != sizeof(std::uint16_t)) {
                malformed_rate = true;
                return std::nullopt;
            }
        }
        if (const auto wide = findAttribute(rate, NL80211_RATE_INFO_BITRATE32)) {
            const auto value = scalar<std::uint32_t>(*wide);
            if (!value || *value > std::numeric_limits<std::uint32_t>::max() / 100U)
                return std::nullopt;
            return *value * 100U;
        }
        if (const auto narrow = findAttribute(rate, NL80211_RATE_INFO_BITRATE)) {
            const auto value = scalar<std::uint16_t>(*narrow);
            if (!value) return std::nullopt;
            return static_cast<std::uint32_t>(*value) * 100U;
        }
        return std::nullopt;
    };
    if (const auto rate = findAttribute(nested, NL80211_STA_INFO_TX_BITRATE))
        result.tx_bitrate_kbps = parseRate(*rate);
    if (const auto rate = findAttribute(nested, NL80211_STA_INFO_RX_BITRATE))
        result.rx_bitrate_kbps = parseRate(*rate);
    if (malformed_rate) return std::nullopt;
    auto counter = [&](int type, int fallback) -> std::optional<std::uint64_t> {
        if (const auto wide = findAttribute(nested, type)) {
            const auto value = scalar<std::uint64_t>(*wide);
            if (!value) return std::nullopt;
            return *value;
        }
        if (const auto narrow = findAttribute(nested, fallback)) {
            const auto value = scalar<std::uint32_t>(*narrow);
            if (!value) return std::nullopt;
            return *value;
        }
        return std::nullopt;
    };
    if (const auto value = findAttribute(nested, NL80211_STA_INFO_TX_RETRIES)) {
        result.tx_retries = scalar<std::uint32_t>(*value);
    }
    if (const auto value = findAttribute(nested, NL80211_STA_INFO_TX_FAILED)) {
        result.tx_failed = scalar<std::uint32_t>(*value);
    }
    if (const auto value = findAttribute(nested, NL80211_STA_INFO_RX_PACKETS))
        result.rx_packets = scalar<std::uint32_t>(*value);
    if (const auto value = findAttribute(nested, NL80211_STA_INFO_TX_PACKETS))
        result.tx_packets = scalar<std::uint32_t>(*value);
    result.rx_bytes = counter(NL80211_STA_INFO_RX_BYTES64, NL80211_STA_INFO_RX_BYTES);
    result.tx_bytes = counter(NL80211_STA_INFO_TX_BYTES64, NL80211_STA_INFO_TX_BYTES);
    return result;
}
#endif

}  // namespace

WifiFamilyParseResult WifiNl80211Parser::parseFamilyId(const void* data, std::size_t size,
                                                       std::uint32_t sender_pid,
                                                       std::uint32_t expected_sequence) {
    WifiFamilyParseResult result;
#if defined(__linux__)
    result.status = walkMessages(data, size, sender_pid, expected_sequence,
        [&](const genlmsghdr*, const AttributeList& attrs) {
            const auto family = findAttribute(attrs, CTRL_ATTR_FAMILY_ID);
            if (!family) return WifiParseStatus::Malformed;
            const auto id = scalar<std::uint16_t>(*family);
            if (!id || *id == 0) return WifiParseStatus::Malformed;
            result.family_id = *id;
            return WifiParseStatus::Ok;
        }, &result.error_code);
#else
    (void)data; (void)size; (void)sender_pid; (void)expected_sequence;
    result.status = WifiParseStatus::ErrorReply;
#endif
    return result;
}

WifiMessageParseResult WifiNl80211Parser::parseInterface(const void* data, std::size_t size,
                                                         std::uint32_t sender_pid,
                                                         std::uint32_t expected_sequence,
                                                         std::uint32_t requested_ifindex) {
    WifiMessageParseResult result;
#if defined(__linux__)
    result.status = walkMessages(data, size, sender_pid, expected_sequence,
        [&](const genlmsghdr*, const AttributeList& attrs) {
            WifiInterfaceReply reply;
            const auto ifindex = findAttribute(attrs, NL80211_ATTR_IFINDEX);
            const auto iftype = findAttribute(attrs, NL80211_ATTR_IFTYPE);
            if (!ifindex || !iftype) return WifiParseStatus::Malformed;
            const auto parsed_ifindex = scalar<std::uint32_t>(*ifindex);
            const auto parsed_iftype = scalar<std::uint32_t>(*iftype);
            if (!parsed_ifindex || !parsed_iftype || *parsed_ifindex != requested_ifindex)
                return WifiParseStatus::Malformed;
            reply.ifindex = *parsed_ifindex;
            reply.iftype = *parsed_iftype;
            if (const auto ssid = findAttribute(attrs, NL80211_ATTR_SSID)) {
                reply.ssid = bytes(*ssid);
                if (!reply.ssid) return WifiParseStatus::Malformed;
            }
            if (const auto frequency = findAttribute(attrs, NL80211_ATTR_WIPHY_FREQ)) {
                reply.frequency_mhz = scalar<std::uint32_t>(*frequency);
                if (!reply.frequency_mhz) return WifiParseStatus::Malformed;
            }
            result.interface = std::move(reply);
            return WifiParseStatus::Ok;
        }, &result.error_code);
#else
    (void)data; (void)size; (void)sender_pid; (void)expected_sequence; (void)requested_ifindex;
    result.status = WifiParseStatus::ErrorReply;
#endif
    return result;
}

WifiMessageParseResult WifiNl80211Parser::parseStation(const void* data, std::size_t size,
                                                       std::uint32_t sender_pid,
                                                       std::uint32_t expected_sequence) {
    WifiMessageParseResult result;
#if defined(__linux__)
    result.status = walkMessages(data, size, sender_pid, expected_sequence,
        [&](const genlmsghdr*, const AttributeList& attrs) {
            result.station = parseStationAttributes(attrs);
            return result.station ? WifiParseStatus::Ok : WifiParseStatus::Malformed;
        }, &result.error_code);
#else
    (void)data; (void)size; (void)sender_pid; (void)expected_sequence;
    result.status = WifiParseStatus::ErrorReply;
#endif
    return result;
}

}  // namespace weaknet_dbus::v2
