#include "v1_observation_adapter.hpp"

#include <net/if.h>

#include <limits>
#include <stdexcept>
#include <utility>

namespace weaknet_dbus::v2 {
namespace {

std::optional<Status> ambiguousScopeStatus() {
    return Status{StatusCode::AmbiguousV1Scope,
                  "mirrored V1 scope/semantics; authoritative redesign is deferred"};
}

}  // namespace

V1ObservationAdapter::V1ObservationAdapter(EventBus& bus, MetricStore& metrics,
                                             const Clock& clock, NetnsId netns)
    : bus_(bus), metrics_(metrics), clock_(clock), netns_(netns) {
    if (netns_.inode == 0) throw std::invalid_argument("V1 adapter requires NetnsId");
}

std::optional<InterfaceId> V1ObservationAdapter::resolveInterface(const std::string& name) const {
    const auto index = if_nametoindex(name.c_str());
    if (index == 0) return std::nullopt;
    return InterfaceId{index, name};
}

EventId V1ObservationAdapter::nextEventId() {
    const auto value = next_event_id_.fetch_add(1);
    if (value == 0 || value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("V1 adapter event id exhausted");
    }
    return EventId{value};
}

SampleId V1ObservationAdapter::nextSampleId() {
    const auto value = next_sample_id_.fetch_add(1);
    if (value == 0 || value == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("V1 adapter sample id exhausted");
    }
    return SampleId{value};
}

NetworkEventHeader V1ObservationAdapter::header(
    EventKind kind, EventSource source, Validity validity,
    std::optional<InterfaceId> interface, std::optional<Status> status,
    RealtimeTime realtime, MonotonicTime monotonic) {
    return NetworkEventHeader{kNetworkEventSchemaVersion, nextEventId(), {}, kind, source,
                              realtime, monotonic, netns_, std::move(interface),
                              std::nullopt, validity, std::move(status)};
}

void V1ObservationAdapter::storeMetric(
    MetricKey key, std::optional<MetricValue> value, Validity validity,
    std::optional<Status> status, RealtimeTime realtime, MonotonicTime monotonic) {
    metrics_.insert(MetricSample{nextSampleId(), std::move(key), std::move(value), validity,
                                 realtime, monotonic, std::move(status)});
}

void V1ObservationAdapter::mirrorInterfaceSnapshot(const std::vector<NetInfo>& interfaces) {
    std::vector<NetworkEvent> pending;
    {
        std::lock_guard lock(interface_mutex_);
        std::map<std::string, InterfaceMirrorState> current;
        for (const auto& interface : interfaces) {
        const auto id = resolveInterface(interface.ifName());
        const bool link_up = interface.state() == NetState::Up;
        current.emplace(interface.ifName(), InterfaceMirrorState{id, link_up});
        const auto previous = mirrored_interfaces_.find(interface.ifName());
        if (previous != mirrored_interfaces_.end() && previous->second.id == id &&
            previous->second.link_up == link_up) {
            continue;
        }
        const auto realtime = clock_.realtimeNow();
        const auto monotonic = clock_.monotonicNow();
        const auto status = id ? ambiguousScopeStatus()
                               : std::optional<Status>(Status{StatusCode::Unavailable,
                                     "V1 interface name did not resolve to an ifindex"});
        pending.emplace_back(
            header(EventKind::InterfaceObservation, EventSource::V1InterfaceSnapshot,
                   Validity::Partial, id, status, realtime, monotonic),
            InterfaceObservation{interface.ifName(), true, link_up});
        }
        for (const auto& [name, previous] : mirrored_interfaces_) {
            if (current.contains(name)) continue;
            const auto realtime = clock_.realtimeNow();
            const auto monotonic = clock_.monotonicNow();
            const auto validity = previous.id ? Validity::Partial : Validity::Unavailable;
            const auto status = previous.id ? ambiguousScopeStatus()
                : std::optional<Status>(Status{StatusCode::Unavailable,
                                              "removed V1 interface lacked a stable ifindex"});
            pending.emplace_back(
                header(EventKind::InterfaceObservation, EventSource::V1InterfaceSnapshot,
                       validity, previous.id, status, realtime, monotonic),
                InterfaceObservation{name, false, false});
        }
        mirrored_interfaces_ = std::move(current);
    }
    // Do not invoke EventBus delivery while the adapter's mirror mutex is held.
    for (auto& event : pending) bus_.publish(std::move(event));
}

void V1ObservationAdapter::mirrorUplink(const std::string& interface_name,
                                         std::uint32_t method_flags) {
    const auto realtime = clock_.realtimeNow();
    const auto monotonic = clock_.monotonicNow();
    const auto id = interface_name.empty() ? std::nullopt : resolveInterface(interface_name);
    const auto validity = id ? Validity::Partial : Validity::Unavailable;
    auto status = id ? ambiguousScopeStatus()
                     : std::optional<Status>(Status{StatusCode::Unavailable,
                           "V1 reported no resolvable current uplink"});
    bus_.publish(NetworkEvent(
        header(EventKind::UplinkObservation, EventSource::V1UplinkMonitor,
               validity, id, std::move(status), realtime, monotonic),
        UplinkObservation{interface_name, method_flags, id.has_value()}));
}

void V1ObservationAdapter::mirrorRtt(const NetInfo& interface) {
    const auto realtime = clock_.realtimeNow();
    const auto monotonic = clock_.monotonicNow();
    const auto id = resolveInterface(interface.ifName());
    const auto available = interface.rttMs() >= 0;
    const auto validity = available && id ? Validity::Partial : Validity::Unavailable;
    auto status = available && id ? ambiguousScopeStatus()
                                  : std::optional<Status>(Status{StatusCode::Unavailable,
                                        "V1 RTT probe failed or interface scope is unavailable"});
    const std::optional<double> value = available && id
        ? std::optional<double>(static_cast<double>(interface.rttMs())) : std::nullopt;
    const MetricKey key{netns_, MetricName::Rtt, MetricUnit::Milliseconds,
                        EventSource::V1RttMonitor, id, std::nullopt, 0};
    storeMetric(key, value ? std::optional<MetricValue>(*value) : std::nullopt,
                validity, status, realtime, monotonic);
    bus_.publish(NetworkEvent(
        header(EventKind::ProbeRttMetric, EventSource::V1RttMonitor,
               validity, id, std::move(status), realtime, monotonic),
        ProbeRttObservation{value}));
}

void V1ObservationAdapter::mirrorTcpLoss(const std::string& interface_name,
                                          const TcpLossResult& result,
                                          bool counters_reset) {
    const auto realtime = clock_.realtimeNow();
    const auto monotonic = clock_.monotonicNow();
    const auto id = resolveInterface(interface_name);
    const bool has_rate = result.sentDelta >= 10 && result.level != "insufficient";
    const auto validity = counters_reset ? Validity::Reset
        : (has_rate && id ? Validity::Partial : Validity::Unavailable);
    std::optional<Status> status = counters_reset
        ? std::optional<Status>(Status{StatusCode::CounterReset, "V1 aggregate counters decreased"})
        : (has_rate && id ? ambiguousScopeStatus()
                          : std::optional<Status>(Status{StatusCode::Unavailable,
                                "V1 TCP-loss denominator/scope is insufficient"}));
    const std::optional<double> value = has_rate && id
        ? std::optional<double>(result.ratePercent) : std::nullopt;
    const MetricKey key{netns_, MetricName::TcpLossRate, MetricUnit::Percent,
                        EventSource::V1TcpLossMonitor, id, std::nullopt, 0};
    storeMetric(key, value ? std::optional<MetricValue>(*value) : std::nullopt,
                validity, status, realtime, monotonic);
    bus_.publish(NetworkEvent(
        header(EventKind::TcpLossMetric, EventSource::V1TcpLossMonitor,
               validity, id, std::move(status), realtime, monotonic),
        TcpLossObservation{value, result.sentDelta, result.retransDelta, result.level}));
}

void V1ObservationAdapter::mirrorTraffic(
    const std::string& interface_name, const NetTrafficAnalyzer::RealTimeStats& stats,
    bool collector_available) {
    const auto realtime = clock_.realtimeNow();
    const auto monotonic = clock_.monotonicNow();
    const auto id = resolveInterface(interface_name);
    const auto validity = collector_available && id ? Validity::Partial : Validity::Unavailable;
    auto status = collector_available && id ? ambiguousScopeStatus()
        : std::optional<Status>(Status{StatusCode::CollectorDegraded,
                                      "V1 eBPF traffic data is unavailable"});
    const std::optional<std::uint64_t> bps = collector_available && id
        ? std::optional<std::uint64_t>(stats.totalBps) : std::nullopt;
    const std::optional<std::uint64_t> pps = collector_available && id
        ? std::optional<std::uint64_t>(stats.totalPps) : std::nullopt;
    const std::optional<std::uint64_t> flows = collector_available && id
        ? std::optional<std::uint64_t>(stats.activeFlows) : std::nullopt;
    const auto store = [&](MetricName name, MetricUnit unit,
                           std::optional<std::uint64_t> value) {
        const MetricKey key{netns_, name, unit, EventSource::V1TrafficMonitor,
                            id, std::nullopt, 0};
        storeMetric(key, value ? std::optional<MetricValue>(*value) : std::nullopt,
                    validity, status, realtime, monotonic);
    };
    store(MetricName::TrafficBytesPerSecond, MetricUnit::BytesPerSecond, bps);
    store(MetricName::TrafficPacketsPerSecond, MetricUnit::PacketsPerSecond, pps);
    store(MetricName::TrafficActiveFlows, MetricUnit::Count, flows);
    bus_.publish(NetworkEvent(
        header(EventKind::TrafficMetric, EventSource::V1TrafficMonitor,
               validity, id, std::move(status), realtime, monotonic),
        TrafficObservation{bps, pps, flows}));
}

void V1ObservationAdapter::mirrorWifiRssi(const NetInfo& interface) {
    const auto realtime = clock_.realtimeNow();
    const auto monotonic = clock_.monotonicNow();
    const auto id = resolveInterface(interface.ifName());
    const bool available = interface.rssiDbm() != -1000;
    const auto validity = available && id ? Validity::Partial : Validity::Unavailable;
    auto status = available && id ? ambiguousScopeStatus()
        : std::optional<Status>(Status{StatusCode::Unavailable, "V1 Wi-Fi RSSI is unavailable"});
    const std::optional<std::int32_t> value = available && id
        ? std::optional<std::int32_t>(interface.rssiDbm()) : std::nullopt;
    const MetricKey key{netns_, MetricName::WifiRssi, MetricUnit::DecibelMilliwatts,
                        EventSource::V1WifiMonitor, id, std::nullopt, 0};
    storeMetric(key, value ? std::optional<MetricValue>(static_cast<std::int64_t>(*value))
                           : std::nullopt,
                validity, status, realtime, monotonic);
    bus_.publish(NetworkEvent(
        header(EventKind::WifiRssiMetric, EventSource::V1WifiMonitor,
               validity, id, std::move(status), realtime, monotonic),
        WifiRssiObservation{value}));
}

void V1ObservationAdapter::mirrorCollectorHealth(std::string component,
                                                  CollectorState state,
                                                  std::string reason) {
    const auto realtime = clock_.realtimeNow();
    const auto monotonic = clock_.monotonicNow();
    const auto validity = state == CollectorState::Running ? Validity::Valid
        : (state == CollectorState::Stopped ? Validity::Stale : Validity::Unavailable);
    std::optional<Status> status;
    if (validity != Validity::Valid) {
        status = Status{StatusCode::CollectorDegraded, std::move(reason)};
    }
    bus_.publish(NetworkEvent(
        header(EventKind::CollectorHealth, EventSource::Runtime, validity,
               std::nullopt, std::move(status), realtime, monotonic),
        CollectorHealthObservation{std::move(component), state}));
}

}  // namespace weaknet_dbus::v2
