#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "network_event.hpp"

namespace weaknet_dbus::v2 {

enum class WifiParseStatus : std::uint8_t {
    Ok,
    Done,
    NotFound,
    ErrorReply,
    Malformed,
    WrongSequence,
    WrongSender,
    Truncated,
    Timeout,
};

struct WifiFamilyParseResult {
    WifiParseStatus status{WifiParseStatus::Malformed};
    std::optional<std::uint16_t> family_id;
    int error_code{};
};

struct WifiInterfaceReply {
    std::uint32_t ifindex{};
    std::uint32_t iftype{};
    std::optional<std::string> ssid;
    std::optional<std::uint32_t> frequency_mhz;
};

struct WifiStationReply {
    std::optional<WifiBssid> bssid;
    std::optional<std::int32_t> signal_dbm;
    std::optional<std::int32_t> signal_avg_dbm;
    std::optional<std::uint32_t> tx_bitrate_kbps;
    std::optional<std::uint32_t> rx_bitrate_kbps;
    std::optional<std::uint64_t> tx_retries;
    std::optional<std::uint64_t> tx_failed;
    std::optional<std::uint64_t> rx_packets;
    std::optional<std::uint64_t> tx_packets;
    std::optional<std::uint64_t> rx_bytes;
    std::optional<std::uint64_t> tx_bytes;
};

struct WifiMessageParseResult {
    WifiParseStatus status{WifiParseStatus::Malformed};
    std::optional<WifiInterfaceReply> interface;
    std::optional<WifiStationReply> station;
    int error_code{};
};

class WifiNl80211Parser {
public:
    static WifiFamilyParseResult parseFamilyId(const void* data, std::size_t size,
                                               std::uint32_t sender_pid,
                                               std::uint32_t expected_sequence);
    static WifiMessageParseResult parseInterface(const void* data, std::size_t size,
                                                 std::uint32_t sender_pid,
                                                 std::uint32_t expected_sequence,
                                                 std::uint32_t requested_ifindex);
    static WifiMessageParseResult parseStation(const void* data, std::size_t size,
                                               std::uint32_t sender_pid,
                                               std::uint32_t expected_sequence);
};

}  // namespace weaknet_dbus::v2
