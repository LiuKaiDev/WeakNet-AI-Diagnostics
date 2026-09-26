#include "socket_tracker.hpp"

#include <cmath>
#include <atomic>
#include <iostream>
#include <netinet/in.h>
#include <utility>

using namespace weaknet_dbus::v2;

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

SocketTuple tuple(NetnsId ns) {
    SocketTuple value;
    value.netns = ns;
    value.protocol = SocketProtocol::Tcp;
    value.family = AF_INET;
    value.local.family = AF_INET;
    value.remote.family = AF_INET;
    value.local.address = {127, 0, 0, 1};
    value.remote.address = {127, 0, 0, 1};
    value.local.port = 40000;
    value.remote.port = 443;
    return value;
}

TcpInfoRaw raw(std::uint64_t acked, std::uint64_t received,
               std::uint32_t retrans, std::uint32_t data_out) {
    TcpInfoRaw value;
    value.rtt_us = 2000;
    value.rttvar_us = 400;
    value.snd_cwnd = 10;
    value.snd_ssthresh = 20;
    value.bytes_acked = acked;
    value.bytes_received = received;
    value.segs_out = static_cast<std::uint32_t>(acked / 10);
    value.segs_in = static_cast<std::uint32_t>(received / 10);
    value.data_segs_out = data_out;
    value.data_segs_in = data_out;
    value.total_retrans = retrans;
    return value;
}

TcpInfoObservation observation(SocketId id, SocketTuple socket_tuple,
                               TcpInfoRaw value, std::int64_t seconds) {
    return TcpInfoObservation{id, id.netns, std::move(socket_tuple), TcpSocketState{1},
                              std::move(value), RealtimeTime{},
                              MonotonicTime{std::chrono::seconds(seconds)}, Validity::Valid, false};
}
}

int main() {
    bool ok = true;
    const NetnsId ns{1, 2};
    const auto socket_tuple = tuple(ns);
    const SocketId id{ns, KernelSocketCookie{9}, SocketGeneration{1}};
    const auto previous = observation(id, socket_tuple, raw(100, 200, 2, 10), 1);
    const auto current = observation(id, socket_tuple, raw(300, 500, 5, 20), 3);
    const auto result = TcpIntervalCalculator::calculate(previous, current);
    ok &= expect(result.metrics && result.metrics->delta_total_retrans == 3,
                 "normal TCP counter delta was not computed");
    ok &= expect(result.metrics && result.metrics->tx_acked_bytes_per_sec == 100.0 &&
                     result.metrics->rx_bytes_per_sec == 150.0,
                 "byte rates used incorrect units");
    ok &= expect(result.metrics && result.metrics->retransmission_segment_ratio == 0.3,
                 "retransmission ratio was not computed from data segments");
    ok &= expect(result.metrics && result.metrics->rtt_us == 2000 &&
                     result.metrics->snd_cwnd == 10,
                 "TCP gauges were not retained as current values");

    const SocketId other_generation{ns, KernelSocketCookie{9}, SocketGeneration{2}};
    const auto generation_result = TcpIntervalCalculator::calculate(
        previous, observation(other_generation, socket_tuple, raw(300, 500, 5, 20), 3));
    ok &= expect(!generation_result.metrics && generation_result.reason ==
                     TcpMetricUnavailableReason::GenerationChanged,
                 "generation change produced an interval delta");

    const SocketId other_identity{ns, KernelSocketCookie{10}, SocketGeneration{1}};
    const auto identity_result = TcpIntervalCalculator::calculate(
        previous, observation(other_identity, socket_tuple, raw(300, 500, 5, 20), 3));
    ok &= expect(!identity_result.metrics && identity_result.reason ==
                     TcpMetricUnavailableReason::IdentityChanged,
                 "identity change produced an interval delta");

    const auto reset_result = TcpIntervalCalculator::calculate(
        previous, observation(id, socket_tuple, raw(50, 500, 1, 20), 3));
    ok &= expect(reset_result.metrics && !reset_result.metrics->tx_acked_bytes_per_sec &&
                     reset_result.metrics->unavailable_reason == TcpMetricUnavailableReason::CounterReset,
                 "counter reset was converted into a valid rate");

    auto missing_previous_raw = raw(100, 200, 2, 10);
    missing_previous_raw.bytes_acked.reset();
    const auto missing_result = TcpIntervalCalculator::calculate(
        observation(id, socket_tuple, std::move(missing_previous_raw), 1), current);
    ok &= expect(missing_result.metrics && !missing_result.metrics->tx_acked_bytes_per_sec,
                 "missing previous field became numeric zero/rate");

    const auto timestamp_result = TcpIntervalCalculator::calculate(
        previous, observation(id, socket_tuple, raw(300, 500, 5, 20), 1));
    ok &= expect(!timestamp_result.metrics && timestamp_result.reason ==
                     TcpMetricUnavailableReason::NonIncreasingTimestamp,
                 "non-increasing timestamp produced an interval");

    const auto zero_denominator = TcpIntervalCalculator::calculate(
        previous, observation(id, socket_tuple, raw(100, 500, 5, 10), 3));
    ok &= expect(zero_denominator.metrics && zero_denominator.metrics->delta_total_retrans == 3 &&
                     !zero_denominator.metrics->retransmission_segment_ratio &&
                     zero_denominator.metrics->unavailable_reason == TcpMetricUnavailableReason::ZeroDenominator,
                 "zero data-segment denominator became zero ratio");

    const auto repeated = TcpIntervalCalculator::calculate(
        previous, observation(id, socket_tuple, raw(300, 500, 22, 20), 3));
    ok &= expect(repeated.metrics && repeated.metrics->retransmission_segment_ratio &&
                     *repeated.metrics->retransmission_segment_ratio > 1.0,
                 "retransmission ratio was silently clamped");

    ManualClock clock(RealtimeTime{}, MonotonicTime{});
    EventBus bus;
    std::atomic<unsigned> event_count{0};
    auto subscription = bus.subscribe(
        EventFilter{std::nullopt, EventSource::SocketTracker, ns},
        [&](const NetworkEvent&) { event_count.fetch_add(1); });
    ok &= expect(bus.start(), "TCP EventBus did not start");
    MetricStore store(clock);
    int phase = 0;
    SocketTrackerTestHooks hooks;
    hooks.dump = [&](std::uint8_t family, std::stop_token) -> std::optional<std::vector<SocketDiagRecord>> {
        if (family == AF_INET6) {
            if (phase == 2) return std::nullopt;
            return std::vector<SocketDiagRecord>{};
        }
        SocketDiagRecord record;
        record.tuple = socket_tuple;
        record.cookie = KernelSocketCookie{9};
        record.tcp_state = TcpSocketState{1};
        record.tcp_info = raw(phase == 0 ? 100 : (phase == 1 ? 300 : 500), 200, 1, 10);
        return std::vector<SocketDiagRecord>{record};
    };
    SocketTracker tracker(bus, clock, ns, SocketLifecycleConfig{},
                          std::chrono::seconds(5), hooks, &store);
    ok &= expect(tracker.reconcileForTests() && tracker.active().size() == 1,
                 "initial authoritative TCP inventory did not establish baseline");
    clock.advance(std::chrono::seconds(2));
    phase = 1;
    ok &= expect(tracker.reconcileForTests(), "second authoritative TCP inventory failed");
    const MetricKey rate_key{ns, MetricName::TcpTxAckedBytesPerSecond,
                             MetricUnit::BytesPerSecond, EventSource::SocketTracker,
                             std::nullopt, id, 2000};
    const auto rate_sample = store.latest(rate_key);
    ok &= expect(rate_sample && rate_sample->value &&
                     std::get<double>(*rate_sample->value) == 100.0,
                 "committed TCP interval was not stored with the expected rate");
    phase = 2;
    clock.advance(std::chrono::seconds(2));
    ok &= expect(!tracker.reconcileForTests() && tracker.active().size() == 1,
                 "failed IPv6 reconciliation changed committed inventory");
    phase = 3;
    clock.advance(std::chrono::seconds(2));
    ok &= expect(tracker.reconcileForTests(), "TCP inventory did not recover after failed cycle");
    const auto recovered_sample = store.latest(rate_key);
    ok &= expect(recovered_sample && recovered_sample->value &&
                     std::get<double>(*recovered_sample->value) == 100.0,
                 "failed cycle incorrectly reset the TCP baseline");
    bus.stop();
    ok &= expect(event_count.load() >= 3,
                 "committed raw/interval TCP observations were not published");

    return ok ? 0 : 1;
}
