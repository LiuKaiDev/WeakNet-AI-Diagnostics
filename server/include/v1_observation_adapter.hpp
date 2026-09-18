#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "event_bus.hpp"
#include "metric_store.hpp"
#include "net_info.hpp"
#include "net_tcp.h"
#include "net_traffic.h"

namespace weaknet_dbus::v2 {

// Additive Phase 3 bridge. It observes already-normalized V1 values, but never
// writes back to V1 or changes what the V1 D-Bus API reads.
class V1ObservationAdapter {
public:
    V1ObservationAdapter(EventBus& bus, MetricStore& metrics,
                         const Clock& clock, NetnsId netns);

    void mirrorInterfaceSnapshot(const std::vector<NetInfo>& interfaces);
    void mirrorUplink(const std::string& interface_name, std::uint32_t method_flags);
    void mirrorRtt(const NetInfo& interface);
    void mirrorTcpLoss(const std::string& interface_name,
                       const TcpLossResult& result,
                       bool counters_reset = false);
    void mirrorTraffic(const std::string& interface_name,
                       const NetTrafficAnalyzer::RealTimeStats& stats,
                       bool collector_available);
    void mirrorWifiRssi(const NetInfo& interface);
    void mirrorCollectorHealth(std::string component, CollectorState state,
                               std::string reason = {});

    NetnsId netns() const noexcept { return netns_; }

private:
    std::optional<InterfaceId> resolveInterface(const std::string& name) const;
    NetworkEventHeader header(EventKind kind, EventSource source,
                              Validity validity,
                              std::optional<InterfaceId> interface,
                              std::optional<Status> status,
                              RealtimeTime realtime,
                              MonotonicTime monotonic);
    SampleId nextSampleId();
    EventId nextEventId();
    void storeMetric(MetricKey key, std::optional<MetricValue> value,
                     Validity validity, std::optional<Status> status,
                     RealtimeTime realtime, MonotonicTime monotonic);

    EventBus& bus_;
    MetricStore& metrics_;
    const Clock& clock_;
    NetnsId netns_;
    std::atomic<std::uint64_t> next_event_id_{1};
    std::atomic<std::uint64_t> next_sample_id_{1};
    struct InterfaceMirrorState {
        std::optional<InterfaceId> id;
        bool link_up{};
    };
    std::mutex interface_mutex_;
    std::map<std::string, InterfaceMirrorState> mirrored_interfaces_;
};

}  // namespace weaknet_dbus::v2
