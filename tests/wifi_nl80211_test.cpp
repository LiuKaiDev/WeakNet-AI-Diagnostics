#include "wifi_collector.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <linux/nl80211.h>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool value, const char* message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

void appendAttr(std::vector<std::byte>& message, std::uint16_t type,
                const void* data, std::size_t size) {
    const auto offset = message.size();
    const auto length = static_cast<std::uint16_t>(sizeof(nlattr) + size);
    message.resize(offset + NLA_ALIGN(length));
    auto* attr = reinterpret_cast<nlattr*>(message.data() + offset);
    attr->nla_len = length;
    attr->nla_type = type;
    std::memcpy(message.data() + offset + sizeof(nlattr), data, size);
    std::fill(message.begin() + static_cast<std::ptrdiff_t>(offset + length), message.end(),
              std::byte{});
}

template <typename T>
void scalarAttr(std::vector<std::byte>& message, std::uint16_t type, T value) {
    appendAttr(message, type, &value, sizeof(value));
}

std::vector<std::byte> genericMessage(std::uint16_t type, std::uint8_t command,
                                      std::uint32_t sequence,
                                      const std::vector<std::byte>& attrs,
                                      std::uint16_t flags = NLM_F_REQUEST) {
    std::vector<std::byte> message(NLMSG_LENGTH(sizeof(genlmsghdr)));
    auto* header = reinterpret_cast<nlmsghdr*>(message.data());
    header->nlmsg_type = type;
    header->nlmsg_flags = flags;
    header->nlmsg_seq = sequence;
    header->nlmsg_pid = 0;
    auto* generic = static_cast<genlmsghdr*>(NLMSG_DATA(header));
    generic->cmd = command;
    generic->version = 0;
    generic->reserved = 0;
    message.insert(message.end(), attrs.begin(), attrs.end());
    header = reinterpret_cast<nlmsghdr*>(message.data());
    header->nlmsg_len = static_cast<std::uint16_t>(message.size());
    return message;
}

void appendDone(std::vector<std::byte>& message, std::uint32_t sequence) {
    const auto offset = message.size();
    message.resize(offset + NLMSG_LENGTH(0));
    auto* header = reinterpret_cast<nlmsghdr*>(message.data() + offset);
    header->nlmsg_len = NLMSG_LENGTH(0);
    header->nlmsg_type = NLMSG_DONE;
    header->nlmsg_seq = sequence;
    header->nlmsg_pid = 0;
}

std::vector<std::byte> stationMessage(std::uint32_t sequence, bool malformed_rate = false) {
    std::vector<std::byte> station;
    const std::array<std::uint8_t, 6> bssid{0, 1, 2, 3, 4, 5};
    appendAttr(station, NL80211_ATTR_MAC, bssid.data(), bssid.size());
    std::vector<std::byte> info;
    const std::uint8_t signal = 195; // signed -61 dBm
    const std::uint8_t signal_avg = 190;
    scalarAttr(info, NL80211_STA_INFO_SIGNAL, signal);
    scalarAttr(info, NL80211_STA_INFO_SIGNAL_AVG, signal_avg);
    std::vector<std::byte> tx_rate;
    const std::uint32_t tx_kbps_100 = 4333;
    scalarAttr(tx_rate, NL80211_RATE_INFO_BITRATE32, tx_kbps_100);
    if (malformed_rate) {
        const auto offset = tx_rate.size();
        tx_rate.resize(offset + NLA_ALIGN(sizeof(nlattr)));
        auto* attr = reinterpret_cast<nlattr*>(tx_rate.data() + offset);
        attr->nla_len = sizeof(nlattr) + 8;
        attr->nla_type = NL80211_RATE_INFO_BITRATE;
    }
    appendAttr(info, NL80211_STA_INFO_TX_BITRATE, tx_rate.data(), tx_rate.size());
    const std::uint32_t rx_kbps_100 = 1200;
    std::vector<std::byte> rx_rate;
    scalarAttr(rx_rate, NL80211_RATE_INFO_BITRATE, static_cast<std::uint16_t>(rx_kbps_100));
    appendAttr(info, NL80211_STA_INFO_RX_BITRATE, rx_rate.data(), rx_rate.size());
    scalarAttr(info, NL80211_STA_INFO_TX_RETRIES, std::uint32_t{7});
    scalarAttr(info, NL80211_STA_INFO_TX_FAILED, std::uint32_t{2});
    scalarAttr(info, NL80211_STA_INFO_RX_PACKETS, std::uint32_t{100});
    scalarAttr(info, NL80211_STA_INFO_TX_PACKETS, std::uint32_t{80});
    scalarAttr(info, NL80211_STA_INFO_RX_BYTES64, std::uint64_t{9'000});
    scalarAttr(info, NL80211_STA_INFO_TX_BYTES, std::uint32_t{8'000});
    appendAttr(station, NL80211_ATTR_STA_INFO, info.data(), info.size());
    auto result = genericMessage(42, NL80211_CMD_NEW_STATION, sequence, station,
                                 NLM_F_MULTI);
    appendDone(result, sequence);
    return result;
}
}

int main() {
    bool ok = true;
    constexpr std::uint32_t sequence = 11;

    std::vector<std::byte> family_attrs;
    scalarAttr(family_attrs, CTRL_ATTR_FAMILY_ID, std::uint16_t{37});
    auto family = genericMessage(GENL_ID_CTRL, CTRL_CMD_NEWFAMILY, sequence, family_attrs);
    appendDone(family, sequence);
    auto family_result = WifiNl80211Parser::parseFamilyId(
        family.data(), family.size(), 0, sequence);
    ok &= expect(family_result.family_id && *family_result.family_id == 37,
                 "nl80211 family ID did not resolve");
    ok &= expect(WifiNl80211Parser::parseFamilyId(
                     family.data(), family.size(), 0, sequence + 1).status ==
                     WifiParseStatus::WrongSequence,
                 "wrong Generic Netlink sequence was accepted");
    ok &= expect(WifiNl80211Parser::parseFamilyId(
                     family.data(), family.size(), 99, sequence).status ==
                     WifiParseStatus::WrongSender,
                 "non-kernel Generic Netlink sender was accepted");

    std::vector<std::byte> interface_attrs;
    scalarAttr(interface_attrs, NL80211_ATTR_IFINDEX, std::uint32_t{3});
    scalarAttr(interface_attrs, NL80211_ATTR_IFTYPE,
               static_cast<std::uint32_t>(NL80211_IFTYPE_STATION));
    const char ssid[] = {'t', 'e', 's', 't', '\0'};
    appendAttr(interface_attrs, NL80211_ATTR_SSID, ssid, sizeof(ssid));
    scalarAttr(interface_attrs, NL80211_ATTR_WIPHY_FREQ, std::uint32_t{5180});
    auto interface = genericMessage(37, NL80211_CMD_NEW_INTERFACE, sequence,
                                    interface_attrs);
    appendDone(interface, sequence);
    const auto interface_result = WifiNl80211Parser::parseInterface(
        interface.data(), interface.size(), 0, sequence, 3);
    ok &= expect(interface_result.interface &&
                     interface_result.interface->ifindex == 3 &&
                     interface_result.interface->iftype == NL80211_IFTYPE_STATION &&
                     interface_result.interface->ssid &&
                     interface_result.interface->ssid->size() == sizeof(ssid),
                 "station interface or length-delimited SSID did not parse");

    const auto station = stationMessage(sequence);
    const auto station_result = WifiNl80211Parser::parseStation(
        station.data(), station.size(), 0, sequence);
    ok &= expect(station_result.station && station_result.station->bssid &&
                     station_result.station->signal_dbm == -61 &&
                     station_result.station->signal_avg_dbm == -66 &&
                     station_result.station->tx_bitrate_kbps == 433'300 &&
                     station_result.station->rx_bitrate_kbps == 120'000 &&
                     station_result.station->rx_bytes == 9'000 &&
                     station_result.station->tx_bytes == 8'000,
                 "station signal, bitrate, or counters were not decoded semantically");
    ok &= expect(station_result.station->tx_retries == 7 &&
                     station_result.station->tx_failed == 2,
                 "station retry counters were not preserved");
    const auto malformed = stationMessage(sequence, true);
    ok &= expect(WifiNl80211Parser::parseStation(
                     malformed.data(), malformed.size(), 0, sequence).status ==
                     WifiParseStatus::Malformed,
                 "malformed nested bitrate attribute was accepted");

    std::vector<std::byte> error_message(NLMSG_LENGTH(sizeof(nlmsgerr)));
    auto* error_header = reinterpret_cast<nlmsghdr*>(error_message.data());
    error_header->nlmsg_len = error_message.size();
    error_header->nlmsg_type = NLMSG_ERROR;
    error_header->nlmsg_seq = sequence;
    error_header->nlmsg_pid = 0;
    auto* error = static_cast<nlmsgerr*>(NLMSG_DATA(error_header));
    error->error = -EOPNOTSUPP;
    const auto error_result = WifiNl80211Parser::parseFamilyId(
        error_message.data(), error_message.size(), 0, sequence);
    ok &= expect(error_result.status == WifiParseStatus::ErrorReply &&
                     error_result.error_code == EOPNOTSUPP,
                 "NLMSG_ERROR was not surfaced");

    std::vector<std::byte> no_station(NLMSG_LENGTH(0));
    auto* no_station_header = reinterpret_cast<nlmsghdr*>(no_station.data());
    no_station_header->nlmsg_len = NLMSG_LENGTH(0);
    no_station_header->nlmsg_type = NLMSG_DONE;
    no_station_header->nlmsg_seq = sequence;
    no_station_header->nlmsg_pid = 0;
    const auto no_station_result = WifiNl80211Parser::parseStation(
        no_station.data(), no_station.size(), 0, sequence);
    ok &= expect(no_station_result.status == WifiParseStatus::Done &&
                     !no_station_result.station,
                 "empty station dump was not distinguishable from a station");

    ManualClock clock;
    EventBus bus;
    std::atomic<unsigned> published{0};
    auto subscription = bus.subscribe(
        EventFilter{EventKind::WifiObservation, EventSource::WifiCollector, NetnsId{1, 2}},
        [&](const NetworkEvent& event) {
            const auto& observation = std::get<WifiObservation>(event.payload());
            if (observation.signal_dbm == -61) ++published;
        });
    bus.start();
    WifiCollectorTestHooks hooks;
    hooks.query = [](const InterfaceId& interface, std::uint64_t generation,
                     std::stop_token) -> std::optional<WifiObservation> {
        WifiObservation observation;
        observation.interface = interface;
        observation.interface_generation = generation;
        observation.link_state = WifiLinkState::Associated;
        observation.capability = WifiCapability::Available;
        observation.signal_dbm = -61;
        return observation;
    };
    WifiCollector collector(bus, clock, NetnsId{1, 2}, {}, std::move(hooks));
    TopologySnapshot topology;
    topology.netns = NetnsId{1, 2};
    topology.authoritative = true;
    topology.generation = 1;
    UplinkSelection uplink;
    uplink.interface = InterfaceId{3, "wifi0"};
    uplink.validity = Validity::Valid;
    collector.updateTopology(topology, uplink);
    collector.refreshForTests();
    bus.stop();
    ok &= expect(published.load() == 1,
                 "WifiObservation did not round-trip through EventBus");
    auto latest = collector.latest();
    ok &= expect(latest && latest->interface && latest->interface->ifindex == 3 &&
                     latest->signal_dbm == -61 && latest->validity == Validity::Partial,
                 "collector did not preserve typed Wi-Fi observation identity");
    ok &= expect(collector.latestFresh(), "fresh Wi-Fi observation was not recognized");
    clock.advance(std::chrono::seconds(11));
    ok &= expect(!collector.latestFresh(), "expired Wi-Fi observation remained fresh");
    uplink.interface = InterfaceId{4, "wifi1"};
    topology.generation = 2;
    collector.updateTopology(topology, uplink);
    ok &= expect(!collector.latest(),
                 "Wi-Fi observation remained current after interface generation change");
    collector.refreshForTests();
    latest = collector.latest();
    ok &= expect(latest && latest->interface && latest->interface->ifindex == 4 &&
                     latest->interface_generation == 2,
                 "Wi-Fi observation transferred across interface generation");
    topology.authoritative = false;
    collector.updateTopology(topology, uplink);
    collector.refreshForTests();
    latest = collector.latest();
    ok &= expect(latest && latest->capability == WifiCapability::NoTarget &&
                     !latest->interface,
                 "non-authoritative topology fabricated a Wi-Fi target");

    return ok ? 0 : 1;
}
