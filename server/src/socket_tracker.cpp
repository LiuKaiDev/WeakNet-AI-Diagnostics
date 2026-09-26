#include "socket_tracker.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>

#if defined(__linux__)
#include <cerrno>
#include <fcntl.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace weaknet_dbus::v2 {

SocketLifecycleTable::SocketLifecycleTable(SocketLifecycleConfig config)
    : config_(config) {
    if (config_.max_active_entries == 0 || config_.max_closed_entries == 0) {
        throw std::invalid_argument("SocketLifecycleTable bounds must be positive");
    }
}

bool SocketLifecycleTable::validInput(const SocketObservationInput& input) noexcept {
    if (!input.present || input.tuple.netns.inode == 0) return false;
    if (input.tuple.protocol != SocketProtocol::Tcp) return false;
    if (input.tuple.family != input.tuple.local.family ||
        input.tuple.family != input.tuple.remote.family) return false;
    return input.tuple.family == 2 || input.tuple.family == 10;  // AF_INET/AF_INET6
}

SocketGeneration SocketLifecycleTable::nextGeneration() {
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("socket lifecycle generation exhausted");
    }
    return SocketGeneration{next_generation_++};
}

SocketId SocketLifecycleTable::newIdentity(
    const SocketTuple& tuple, std::optional<KernelSocketCookie> cookie) {
    return SocketId{tuple.netns, cookie, nextGeneration()};
}

SocketObservation SocketLifecycleTable::makeObservation(
    const SocketId& id, const SocketObservationInput& input) const {
    return SocketObservation{id, input.tuple.netns, input.tuple, input.observed_at, input.monotonic_at,
                             input.source, input.validity, SocketLifecycleState::Active, true,
                             input.tcp_state, input.diag_ifindex, input.tcp_info,
                             input.tcp_info_malformed};
}

TcpIntervalResult TcpIntervalCalculator::calculate(const TcpInfoObservation& previous,
                                                   const TcpInfoObservation& current) {
    if (previous.id != current.id) {
        return {std::nullopt,
                previous.id.netns == current.id.netns && previous.id.cookie == current.id.cookie
                    ? std::optional<TcpMetricUnavailableReason>(TcpMetricUnavailableReason::GenerationChanged)
                    : std::optional<TcpMetricUnavailableReason>(TcpMetricUnavailableReason::IdentityChanged)};
    }
    if (current.monotonic_at <= previous.monotonic_at) {
        return {std::nullopt, TcpMetricUnavailableReason::NonIncreasingTimestamp};
    }
    if (previous.validity != Validity::Valid || current.validity != Validity::Valid ||
        previous.malformed || current.malformed || !previous.raw || !current.raw) {
        return {std::nullopt, TcpMetricUnavailableReason::FieldUnavailable};
    }

    TcpIntervalMetrics metrics;
    metrics.id = current.id;
    metrics.interval_start = previous.monotonic_at;
    metrics.interval_end = current.monotonic_at;
    metrics.elapsed_seconds = std::chrono::duration<double>(
        current.monotonic_at - previous.monotonic_at).count();
    metrics.rtt_us = current.raw->rtt_us;
    metrics.rttvar_us = current.raw->rttvar_us;
    metrics.snd_cwnd = current.raw->snd_cwnd;
    metrics.snd_ssthresh = current.raw->snd_ssthresh;
    bool has_metric = metrics.rtt_us || metrics.rttvar_us || metrics.snd_cwnd || metrics.snd_ssthresh;
    bool counter_reset = false;
    bool zero_denominator = false;

    const auto rate = [&](const auto& before, const auto& after, auto& destination) {
        if (!before || !after) return std::optional<std::uint64_t>{};
        if (*after < *before) { counter_reset = true; return std::optional<std::uint64_t>{}; }
        const auto delta = static_cast<std::uint64_t>(*after) - static_cast<std::uint64_t>(*before);
        destination = static_cast<double>(delta) / metrics.elapsed_seconds;
        has_metric = true;
        return std::optional<std::uint64_t>(delta);
    };
    const auto acked = rate(previous.raw->bytes_acked, current.raw->bytes_acked,
                            metrics.tx_acked_bytes_per_sec);
    const auto received = rate(previous.raw->bytes_received, current.raw->bytes_received,
                               metrics.rx_bytes_per_sec);
    const auto segs_out = rate(previous.raw->segs_out, current.raw->segs_out,
                               metrics.segs_out_per_sec);
    const auto segs_in = rate(previous.raw->segs_in, current.raw->segs_in,
                              metrics.segs_in_per_sec);
    const auto data_out = rate(previous.raw->data_segs_out, current.raw->data_segs_out,
                               metrics.data_segs_out_per_sec);
    const auto data_in = rate(previous.raw->data_segs_in, current.raw->data_segs_in,
                              metrics.data_segs_in_per_sec);
    (void)acked; (void)received; (void)segs_out; (void)segs_in; (void)data_in;

    if (previous.raw->total_retrans && current.raw->total_retrans) {
        if (*current.raw->total_retrans < *previous.raw->total_retrans) {
            counter_reset = true;
        } else {
            metrics.delta_total_retrans = static_cast<std::uint64_t>(*current.raw->total_retrans) -
                                          static_cast<std::uint64_t>(*previous.raw->total_retrans);
            has_metric = true;
        }
    }
    if (data_out && metrics.delta_total_retrans) {
        metrics.delta_data_segs_out = *data_out;
        if (*data_out == 0) zero_denominator = true;
        else metrics.retransmission_segment_ratio =
            static_cast<double>(*metrics.delta_total_retrans) / static_cast<double>(*data_out);
    }
    if (!has_metric) {
        return {std::nullopt, counter_reset ? std::optional<TcpMetricUnavailableReason>(TcpMetricUnavailableReason::CounterReset)
                            : zero_denominator ? std::optional<TcpMetricUnavailableReason>(TcpMetricUnavailableReason::ZeroDenominator)
                                               : std::optional<TcpMetricUnavailableReason>(TcpMetricUnavailableReason::FieldUnavailable)};
    }
    if (counter_reset) {
        metrics.unavailable_reason = TcpMetricUnavailableReason::CounterReset;
        metrics.validity = Validity::Partial;
    } else if (zero_denominator) {
        metrics.unavailable_reason = TcpMetricUnavailableReason::ZeroDenominator;
        metrics.validity = Validity::Partial;
    }
    return {std::move(metrics), std::nullopt};
}

SocketTracker::SocketTracker(EventBus& bus, const Clock& clock, NetnsId netns,
                             SocketLifecycleConfig lifecycle_config,
                             std::chrono::milliseconds interval,
                             SocketTrackerTestHooks hooks, MetricStore* metrics)
    : bus_(bus), metrics_(metrics), clock_(clock), netns_(netns), lifecycle_(lifecycle_config),
      interval_(interval), hooks_(std::move(hooks)) {
    if (netns_.inode == 0 || interval_ <= std::chrono::milliseconds::zero() ||
        hooks_.recovery_retry_initial <= std::chrono::milliseconds::zero() ||
        hooks_.recovery_retry_max < hooks_.recovery_retry_initial) {
        throw std::invalid_argument("SocketTracker requires valid namespace and bounded intervals");
    }
}

SocketTracker::~SocketTracker() { stop(); }

bool SocketTracker::openSocket() {
#if defined(__linux__)
    if (hooks_.open_socket) {
        const auto fd = hooks_.open_socket();
        if (fd < 0) return false;
        socket_.reset(fd);
        return true;
    }
    const auto fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0) return false;
    int receive_buffer = 1024 * 1024;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)) != 0) {
        ::close(fd); return false;
    }
    sockaddr_nl address{}; address.nl_family = AF_NETLINK;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd); return false;
    }
    const auto flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        ::close(fd); return false;
    }
    socket_.reset(fd);
    return true;
#else
    return false;
#endif
}

void SocketTracker::markFailure(const std::string& reason) {
    std::lock_guard lock(mutex_);
    ++telemetry_.reconciliation_failures;
    telemetry_.degraded = true;
    telemetry_.last_error = reason;
}

bool SocketTracker::dumpFamily(std::uint8_t family, std::vector<SocketDiagRecord>& output,
                               std::stop_token token) {
    if (hooks_.dump) {
        const auto result = hooks_.dump(family, token);
        if (!result) return false;
        output.insert(output.end(), result->begin(), result->end());
        return true;
    }
#if defined(__linux__)
    const auto fd = socket_.get();
    if (fd < 0) return false;
    static std::atomic<std::uint32_t> sequence{1000};
    const auto request_sequence = sequence.fetch_add(1);
    inet_diag_req_v2 request{};
    request.sdiag_family = family;
    request.sdiag_protocol = IPPROTO_TCP;
    request.idiag_ext = static_cast<std::uint8_t>(1U << (INET_DIAG_INFO - 1));
    request.idiag_states = 0xffffffffU;
    nlmsghdr header{};
    header.nlmsg_len = NLMSG_LENGTH(sizeof(request));
    header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    header.nlmsg_seq = request_sequence;
    std::array<std::byte, NLMSG_SPACE(sizeof(request))> wire{};
    std::memcpy(wire.data(), &header, sizeof(header));
    std::memcpy(wire.data() + NLMSG_HDRLEN, &request, sizeof(request));
    sockaddr_nl destination{}; destination.nl_family = AF_NETLINK;
    iovec vector{wire.data(), header.nlmsg_len};
    msghdr message{}; message.msg_name = &destination; message.msg_namelen = sizeof(destination);
    message.msg_iov = &vector; message.msg_iovlen = 1;
    if (::sendmsg(fd, &message, 0) < 0) return false;
    std::array<std::byte, 256 * 1024> buffer{};
    const auto deadline = clock_.monotonicNow() + std::chrono::seconds(5);
    bool complete = false;
    while (!complete && !token.stop_requested()) {
        if (clock_.monotonicNow() >= deadline) {
            std::lock_guard lock(mutex_); ++telemetry_.timeouts; return false;
        }
        pollfd descriptor{fd, POLLIN, 0};
        const auto ready = ::poll(&descriptor, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; return false; }
        if (ready == 0) continue;
        sockaddr_nl sender{}; iovec receive_vector{buffer.data(), buffer.size()};
        msghdr receive{}; receive.msg_name = &sender; receive.msg_namelen = sizeof(sender);
        receive.msg_iov = &receive_vector; receive.msg_iovlen = 1;
        const auto length = ::recvmsg(fd, &receive, MSG_DONTWAIT);
        if (length < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (errno == ENOBUFS) { std::lock_guard lock(mutex_); ++telemetry_.overflow_events; }
            return false;
        }
        if ((receive.msg_flags & MSG_TRUNC) != 0) {
            std::lock_guard lock(mutex_); ++telemetry_.truncations; return false;
        }
        const auto parsed = SocketDiagParser::parse(buffer.data(), static_cast<std::size_t>(length),
                                                    sender.nl_pid, request_sequence, netns_);
        if (parsed.malformed || parsed.dump_interrupted) {
            std::lock_guard lock(mutex_);
            if (parsed.dump_interrupted) ++telemetry_.interrupted_dumps;
            else ++telemetry_.malformed_messages;
            return false;
        }
        for (const auto& item : parsed.messages) {
            if (item.kind == SocketDiagMessageKind::Error) {
                if (item.error_code != 0) { std::lock_guard lock(mutex_); ++telemetry_.netlink_errors; return false; }
            } else if (item.kind == SocketDiagMessageKind::Socket && item.socket) {
                output.push_back(*item.socket);
            } else if (item.kind == SocketDiagMessageKind::Done) {
                if (item.dump_interrupted) { std::lock_guard lock(mutex_); ++telemetry_.interrupted_dumps; return false; }
                complete = true;
            }
        }
    }
    return complete;
#else
    (void)family; (void)output; (void)token; return false;
#endif
}

bool SocketTracker::reconcile(std::stop_token token) {
    {
        std::lock_guard lock(mutex_);
        ++telemetry_.reconciliation_attempts;
    }
    std::vector<SocketDiagRecord> records;
    if (!dumpFamily(AF_INET, records, token) || !dumpFamily(AF_INET6, records, token)) {
        markFailure("socket_dump_failed");
        return false;
    }
    std::vector<SocketObservation> before;
    std::vector<SocketObservation> after;
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        before = lifecycle_.active();
        if (!lifecycle_.beginSnapshot(SocketSnapshotDisposition::Authoritative)) {
            markFailure("snapshot_already_in_progress"); return false;
        }
        bool accepted = true;
        for (const auto& record : records) {
            SocketObservationInput input;
            input.tuple = record.tuple; input.cookie = record.cookie;
            input.observed_at = clock_.realtimeNow(); input.monotonic_at = clock_.monotonicNow();
            input.tcp_state = record.tcp_state; input.diag_ifindex = record.diag_ifindex;
            input.tcp_info = record.tcp_info;
            input.tcp_info_malformed = record.tcp_info_malformed;
            if (!lifecycle_.observe(input)) { accepted = false; break; }
        }
        if (!accepted) {
            lifecycle_.abortSnapshot();
            { std::lock_guard lock(mutex_); ++telemetry_.capacity_exhaustions; }
            markFailure("active_capacity_exhausted");
            return false;
        }
        if (!lifecycle_.commitSnapshot()) { markFailure("snapshot_commit_failed"); return false; }
        after = lifecycle_.active();
    }
    std::vector<SocketObservation> closed;
    for (const auto& item : before) {
        const auto found = std::find_if(after.begin(), after.end(), [&](const auto& value) { return value.id == item.id; });
        if (found == after.end()) {
            auto copy = item;
            copy.observed_at = clock_.realtimeNow();
            copy.monotonic_at = clock_.monotonicNow();
            copy.lifecycle = SocketLifecycleState::Closed;
            copy.present = false;
            copy.validity = Validity::Stale;
            closed.push_back(std::move(copy));
        }
    }
    publishCommitted(before, after, closed);
    {
        std::lock_guard lock(mutex_);
        ++telemetry_.reconciliation_successes; ++telemetry_.authoritative_commits;
        telemetry_.parsed_socket_count += records.size(); telemetry_.active_socket_count = after.size();
        telemetry_.degraded = false; telemetry_.last_error.clear();
        telemetry_.last_successful_reconciliation = clock_.realtimeNow();
    }
    return true;
}

void SocketTracker::publishCommitted(const std::vector<SocketObservation>& before,
                                     const std::vector<SocketObservation>& after,
                                     const std::vector<SocketObservation>& closed) {
    auto changed = [&](const SocketObservation& value) {
        const auto found = std::find_if(before.begin(), before.end(), [&](const auto& old) { return old.id == value.id; });
        return found == before.end() || found->tuple != value.tuple || found->tcp_state != value.tcp_state ||
               found->diag_ifindex != value.diag_ifindex;
    };
    auto publish = [&](const SocketObservation& value) {
        NetworkEventHeader header{kNetworkEventSchemaVersion,
            EventId{next_event_id_.fetch_add(1)}, {}, EventKind::SocketObservation,
            EventSource::SocketTracker, value.observed_at, value.monotonic_at,
            value.netns, std::nullopt, value.id, value.validity, std::nullopt};
        (void)bus_.publish(NetworkEvent(std::move(header), value));
    };
    for (const auto& item : after) if (changed(item)) publish(item);
    for (const auto& item : closed) publish(item);

    SocketRouteAttributor* attributor = nullptr;
    TopologySnapshot topology;
    UplinkSelection uplink;
    {
        std::lock_guard lock(mutex_);
        attributor = route_attributor_;
        topology = route_topology_;
        uplink = route_uplink_;
    }
    if (attributor) {
        std::vector<SocketRouteContextObservation> contexts;
        {
            std::lock_guard lock(mutex_);
            for (const auto& item : closed) route_contexts_.erase(item.id);
            for (const auto& item : after) {
                auto context = attributor->attribute(item, topology, uplink);
                route_contexts_[item.id] = context;
                contexts.push_back(std::move(context));
            }
        }
        for (const auto& context : contexts) {
            NetworkEventHeader header{kNetworkEventSchemaVersion,
                EventId{next_event_id_.fetch_add(1)}, {}, EventKind::SocketRouteObservation,
                EventSource::SocketTracker, clock_.realtimeNow(), clock_.monotonicNow(),
                context.netns, context.selected_interface, context.socket_id,
                context.attribution_status == RouteAttributionStatus::Available ? Validity::Valid :
                    context.attribution_status == RouteAttributionStatus::Ambiguous ? Validity::Partial :
                    context.attribution_status == RouteAttributionStatus::Unavailable ? Validity::Unavailable : Validity::Partial,
                std::nullopt};
            (void)bus_.publish(NetworkEvent(std::move(header), context));
        }
    }

    std::vector<TcpInfoObservation> raw_events;
    std::vector<TcpIntervalMetrics> interval_events;
    const auto recordReason = [&](TcpMetricUnavailableReason reason) {
        std::lock_guard lock(mutex_);
        switch (reason) {
            case TcpMetricUnavailableReason::IdentityChanged:
            case TcpMetricUnavailableReason::GenerationChanged:
                ++telemetry_.intervals_identity_rejected; break;
            case TcpMetricUnavailableReason::FieldUnavailable:
            case TcpMetricUnavailableReason::NoPreviousSample:
            case TcpMetricUnavailableReason::PartialObservation:
                ++telemetry_.intervals_field_unavailable; break;
            case TcpMetricUnavailableReason::CounterReset:
                ++telemetry_.intervals_counter_reset; break;
            case TcpMetricUnavailableReason::ZeroDenominator:
                ++telemetry_.intervals_zero_denominator; break;
            case TcpMetricUnavailableReason::NonIncreasingTimestamp:
                ++telemetry_.intervals_timestamp_rejected; break;
        }
    };
    {
        std::lock_guard lifecycle_lock(lifecycle_mutex_);
        for (auto iterator = tcp_baselines_.begin(); iterator != tcp_baselines_.end();) {
            const auto found = std::find_if(after.begin(), after.end(),
                [&](const auto& value) { return value.id == iterator->first; });
            if (found == after.end() || !found->tcp_info) iterator = tcp_baselines_.erase(iterator);
            else ++iterator;
        }
        for (const auto& item : after) {
            TcpInfoObservation current{item.id, item.netns, item.tuple, item.tcp_state,
                                       item.tcp_info, item.observed_at, item.monotonic_at,
                                       item.tcp_info ? Validity::Valid : Validity::Unavailable,
                                       item.tcp_info_malformed};
            raw_events.push_back(current);
            if (!current.raw) {
                std::lock_guard lock(mutex_);
                if (current.malformed) ++telemetry_.tcp_info_attributes_seen;
                ++telemetry_.tcp_info_unavailable;
                if (current.malformed) ++telemetry_.tcp_info_malformed;
                continue;
            }
            {
                std::lock_guard lock(mutex_);
                ++telemetry_.tcp_info_attributes_seen;
            }
            const auto previous = tcp_baselines_.find(current.id);
            if (previous == tcp_baselines_.end()) {
                tcp_baselines_[current.id] = current;
                std::lock_guard lock(mutex_);
                ++telemetry_.interval_baselines_created;
                continue;
            }
            const auto result = TcpIntervalCalculator::calculate(previous->second, current);
            if (result.metrics) {
                interval_events.push_back(*result.metrics);
                if (result.metrics->validity == Validity::Valid) {
                    std::lock_guard lock(mutex_);
                    ++telemetry_.valid_intervals_computed;
                }
                if (result.metrics->unavailable_reason) {
                    recordReason(*result.metrics->unavailable_reason);
                }
            } else if (result.reason) {
                recordReason(*result.reason);
            }
            tcp_baselines_[current.id] = current;
        }
    }
    for (const auto& observation : raw_events) {
        NetworkEventHeader header{kNetworkEventSchemaVersion,
            EventId{next_event_id_.fetch_add(1)}, {}, EventKind::TcpInfoObservation,
            EventSource::SocketTracker, observation.observed_at, observation.monotonic_at,
            observation.netns, std::nullopt, observation.id, observation.validity, std::nullopt};
        (void)bus_.publish(NetworkEvent(std::move(header), observation));
    }
    for (const auto& metrics : interval_events) {
        const auto elapsed_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                metrics.interval_end - metrics.interval_start).count());
        const auto realtime = clock_.realtimeNow();
        NetworkEventHeader header{kNetworkEventSchemaVersion,
            EventId{next_event_id_.fetch_add(1)}, {}, EventKind::TcpIntervalMetric,
            EventSource::SocketTracker, realtime, metrics.interval_end,
            metrics.id.netns, std::nullopt, metrics.id, metrics.validity, std::nullopt};
        (void)bus_.publish(NetworkEvent(std::move(header), metrics));
        if (!metrics_) continue;
        const auto store = [&](MetricName name, MetricUnit unit, MetricValue value) {
            MetricKey key{metrics.id.netns, name, unit, EventSource::SocketTracker,
                          std::nullopt, metrics.id, elapsed_ms};
            metrics_->insert(MetricSample{SampleId{next_sample_id_.fetch_add(1)},
                                          std::move(key), std::move(value), metrics.validity,
                                          realtime, metrics.interval_end, std::nullopt});
        };
        if (metrics.rtt_us) store(MetricName::TcpRttUs, MetricUnit::Microseconds,
                                  static_cast<std::uint64_t>(*metrics.rtt_us));
        if (metrics.rttvar_us) store(MetricName::TcpRttvarUs, MetricUnit::Microseconds,
                                     static_cast<std::uint64_t>(*metrics.rttvar_us));
        if (metrics.snd_cwnd) store(MetricName::TcpSndCwnd, MetricUnit::Count,
                                    static_cast<std::uint64_t>(*metrics.snd_cwnd));
        if (metrics.snd_ssthresh) store(MetricName::TcpSndSsthresh, MetricUnit::Count,
                                        static_cast<std::uint64_t>(*metrics.snd_ssthresh));
        if (metrics.tx_acked_bytes_per_sec) store(MetricName::TcpTxAckedBytesPerSecond,
                                                  MetricUnit::BytesPerSecond,
                                                  *metrics.tx_acked_bytes_per_sec);
        if (metrics.rx_bytes_per_sec) store(MetricName::TcpRxBytesPerSecond,
                                            MetricUnit::BytesPerSecond,
                                            *metrics.rx_bytes_per_sec);
        if (metrics.segs_out_per_sec) store(MetricName::TcpSegsOutPerSecond,
                                            MetricUnit::SegmentsPerSecond,
                                            *metrics.segs_out_per_sec);
        if (metrics.segs_in_per_sec) store(MetricName::TcpSegsInPerSecond,
                                           MetricUnit::SegmentsPerSecond,
                                           *metrics.segs_in_per_sec);
        if (metrics.data_segs_out_per_sec) store(MetricName::TcpDataSegsOutPerSecond,
                                                 MetricUnit::SegmentsPerSecond,
                                                 *metrics.data_segs_out_per_sec);
        if (metrics.data_segs_in_per_sec) store(MetricName::TcpDataSegsInPerSecond,
                                                MetricUnit::SegmentsPerSecond,
                                                *metrics.data_segs_in_per_sec);
        if (metrics.delta_total_retrans) store(MetricName::TcpDeltaTotalRetrans,
                                               MetricUnit::Count,
                                               static_cast<std::uint64_t>(*metrics.delta_total_retrans));
        if (metrics.retransmission_segment_ratio) store(
            MetricName::TcpRetransmissionSegmentRatio, MetricUnit::Ratio,
            *metrics.retransmission_segment_ratio);
    }
}

bool SocketTracker::start() {
    if (running_.exchange(true)) return true;
    if (!openSocket()) {
        running_.store(false);
        std::lock_guard lock(mutex_);
        telemetry_.degraded = true;
        telemetry_.last_error = "transport_open_failed";
        return false;
    }
    (void)reconcile({});
    try { worker_ = std::jthread([this](std::stop_token token) { loop(token); }); }
    catch (...) {
        running_.store(false); socket_.reset();
        return false;
    }
    return true;
}

void SocketTracker::loop(std::stop_token token) {
    auto retry = hooks_.recovery_retry_initial;
    while (!token.stop_requested()) {
        const auto degraded = telemetry().degraded;
        const auto delay = degraded ? retry : interval_;
        for (auto elapsed = std::chrono::milliseconds::zero(); elapsed < delay && !token.stop_requested(); elapsed += std::chrono::milliseconds(50))
            std::this_thread::sleep_for(std::min(std::chrono::milliseconds(50), delay - elapsed));
        if (token.stop_requested()) break;
        if (reconcile(token)) retry = hooks_.recovery_retry_initial;
        else retry = std::min(hooks_.recovery_retry_max, retry * 2);
    }
}

void SocketTracker::stop() noexcept {
    if (!running_.exchange(false)) return;
    if (worker_.joinable()) { worker_.request_stop(); worker_.join(); }
    socket_.reset();
}

std::vector<SocketObservation> SocketTracker::active() const {
    std::lock_guard lock(lifecycle_mutex_);
    return lifecycle_.active();
}

bool SocketTracker::reconcileForTests() { return reconcile({}); }

void SocketTracker::setRouteAttributor(SocketRouteAttributor* attributor) {
    std::lock_guard lock(mutex_);
    route_attributor_ = attributor;
}

void SocketTracker::recomputeRouteContexts(const TopologySnapshot& topology,
                                           const UplinkSelection& selected_uplink) {
    SocketRouteAttributor* attributor = nullptr;
    const auto active_sockets = active();
    {
        std::lock_guard lock(mutex_);
        route_topology_ = topology;
        route_uplink_ = selected_uplink;
        attributor = route_attributor_;
        route_contexts_.clear();
    }
    if (!attributor) return;
    for (const auto& item : active_sockets) {
        auto context = attributor->attribute(item, topology, selected_uplink);
        {
            std::lock_guard lock(mutex_);
            route_contexts_[item.id] = context;
        }
        NetworkEventHeader header{kNetworkEventSchemaVersion,
            EventId{next_event_id_.fetch_add(1)}, {}, EventKind::SocketRouteObservation,
            EventSource::SocketTracker, clock_.realtimeNow(), clock_.monotonicNow(),
            context.netns, context.selected_interface, context.socket_id,
            context.attribution_status == RouteAttributionStatus::Available ? Validity::Valid :
                context.attribution_status == RouteAttributionStatus::Ambiguous ? Validity::Partial :
                context.attribution_status == RouteAttributionStatus::Unavailable ? Validity::Unavailable : Validity::Partial,
            std::nullopt};
        (void)bus_.publish(NetworkEvent(std::move(header), context));
    }
}

std::map<SocketId, SocketRouteContextObservation> SocketTracker::routeContexts() const {
    std::lock_guard lock(mutex_);
    return route_contexts_;
}

SocketTrackerTelemetry SocketTracker::telemetry() const {
    SocketTrackerTelemetry result;
    { std::lock_guard lock(mutex_); result = telemetry_; }
    { std::lock_guard lifecycle_lock(lifecycle_mutex_); result.active_socket_count = lifecycle_.activeCount(); }
    return result;
}

bool SocketLifecycleTable::beginSnapshot(SocketSnapshotDisposition disposition) {
    if (snapshot_in_progress_) return false;
    snapshot_in_progress_ = true;
    disposition_ = disposition;
    pending_.clear();
    pending_weak_.clear();
    pending_cookie_.clear();
    pending_seen_.clear();
    pending_superseded_.clear();
    return true;
}

std::optional<SocketId> SocketLifecycleTable::pendingWeak(const SocketTuple& tuple) const {
    const auto iterator = pending_weak_.find(tuple);
    if (iterator == pending_weak_.end()) return std::nullopt;
    if (pending_superseded_.contains(iterator->second)) return std::nullopt;
    return iterator->second;
}

std::optional<SocketId> SocketLifecycleTable::pendingCookie(const CookieKey& key) const {
    const auto iterator = pending_cookie_.find(key);
    if (iterator == pending_cookie_.end()) return std::nullopt;
    if (pending_superseded_.contains(iterator->second)) return std::nullopt;
    return iterator->second;
}

void SocketLifecycleTable::erasePending(const SocketId& id) {
    pending_.erase(id);
    pending_seen_.erase(id);
    for (auto iterator = pending_weak_.begin(); iterator != pending_weak_.end();) {
        if (iterator->second == id) iterator = pending_weak_.erase(iterator);
        else ++iterator;
    }
    for (auto iterator = pending_cookie_.begin(); iterator != pending_cookie_.end();) {
        if (iterator->second == id) iterator = pending_cookie_.erase(iterator);
        else ++iterator;
    }
}

void SocketLifecycleTable::supersede(const SocketId& id) {
    const bool was_active = active_.contains(id);
    erasePending(id);
    if (was_active) pending_superseded_.insert(id);
}

void SocketLifecycleTable::rememberClosed(SocketObservation observation) {
    observation.lifecycle = SocketLifecycleState::Closed;
    observation.present = false;
    observation.validity = Validity::Stale;
    closed_.push_back(std::move(observation));
    while (closed_.size() > config_.max_closed_entries) closed_.pop_front();
}

std::optional<SocketResolution> SocketLifecycleTable::observe(
    const SocketObservationInput& input) {
    if (!snapshot_in_progress_ || disposition_ == SocketSnapshotDisposition::Failed ||
        !validInput(input)) return std::nullopt;

    std::optional<SocketId> resolved;
    bool inconsistent = false;
    bool created = false;
    if (input.cookie) {
        const CookieKey key{input.tuple.netns, *input.cookie};
        if (const auto candidate = pendingCookie(key)) {
            const auto iterator = pending_.find(*candidate);
            if (iterator != pending_.end() && iterator->second.tuple == input.tuple) {
                resolved = *candidate;
            } else {
                inconsistent = true;
                supersede(*candidate);
            }
        }
        if (!resolved) {
            const auto active = cookie_active_.find(key);
            if (active != cookie_active_.end() && !pending_superseded_.contains(active->second)) {
                const auto iterator = active_.find(active->second);
                if (iterator != active_.end() && iterator->second.tuple == input.tuple) {
                    resolved = active->second;
                } else {
                    inconsistent = true;
                    supersede(active->second);
                }
            }
        }
        if (!resolved) {
            if (const auto weak_pending = pendingWeak(input.tuple)) {
                inconsistent = true;
                supersede(*weak_pending);
            } else {
                const auto weak_active = weak_active_.find(input.tuple);
                if (weak_active != weak_active_.end() &&
                    !pending_superseded_.contains(weak_active->second)) {
                    inconsistent = true;
                    supersede(weak_active->second);
                }
            }
        }
    } else {
        if (const auto weak = pendingWeak(input.tuple)) {
            resolved = *weak;
        } else {
            const auto active = weak_active_.find(input.tuple);
            if (active != weak_active_.end() && !pending_superseded_.contains(active->second)) {
                resolved = active->second;
            } else {
                // Losing cookie visibility is an identity uncertainty. Do
                // not silently merge into a cookie-backed lifecycle; reset it
                // conservatively if the tuple is the only available clue.
                std::optional<SocketId> cookie_pending_to_reset;
                for (const auto& [pending_id, observation] : pending_) {
                    if (pending_id.cookie && observation.tuple == input.tuple &&
                        !pending_superseded_.contains(pending_id)) {
                        inconsistent = true;
                        cookie_pending_to_reset = pending_id;
                        break;
                    }
                }
                if (cookie_pending_to_reset) supersede(*cookie_pending_to_reset);
                std::optional<SocketId> cookie_active_to_reset;
                for (const auto& [active_id, observation] : active_) {
                    if (active_id.cookie && observation.tuple == input.tuple &&
                        !pending_superseded_.contains(active_id)) {
                        inconsistent = true;
                        cookie_active_to_reset = active_id;
                        break;
                    }
                }
                if (cookie_active_to_reset) supersede(*cookie_active_to_reset);
            }
        }
    }

    if (!resolved) {
        if (active_.size() + pending_.size() >= config_.max_active_entries) return std::nullopt;
        resolved = newIdentity(input.tuple, input.cookie);
        created = true;
    }

    const auto observation = makeObservation(*resolved, input);
    pending_[*resolved] = observation;
    pending_seen_.insert(*resolved);
    if (input.cookie) pending_cookie_[CookieKey{input.tuple.netns, *input.cookie}] = *resolved;
    else pending_weak_[input.tuple] = *resolved;
    return SocketResolution{observation, created, inconsistent};
}

bool SocketLifecycleTable::commitSnapshot() {
    if (!snapshot_in_progress_) return false;
    if (disposition_ != SocketSnapshotDisposition::Authoritative) {
        clearCandidate();
        return true;
    }

    std::vector<SocketId> to_close;
    for (const auto& [id, observation] : active_) {
        (void)observation;
        if (!pending_seen_.contains(id) || pending_superseded_.contains(id)) to_close.push_back(id);
    }
    for (const auto& id : to_close) {
        const auto iterator = active_.find(id);
        if (iterator == active_.end()) continue;
        rememberClosed(iterator->second);
        active_.erase(iterator);
    }
    for (const auto& [id, observation] : pending_) {
        if (!pending_superseded_.contains(id)) active_[id] = observation;
    }

    weak_active_.clear();
    cookie_active_.clear();
    for (const auto& [id, observation] : active_) {
        if (id.cookie) cookie_active_[CookieKey{id.netns, *id.cookie}] = id;
        else weak_active_[observation.tuple] = id;
    }
    clearCandidate();
    return true;
}

void SocketLifecycleTable::clearCandidate() noexcept {
    snapshot_in_progress_ = false;
    disposition_ = SocketSnapshotDisposition::Failed;
    pending_.clear();
    pending_weak_.clear();
    pending_cookie_.clear();
    pending_seen_.clear();
    pending_superseded_.clear();
}

void SocketLifecycleTable::abortSnapshot() noexcept { clearCandidate(); }

bool SocketLifecycleTable::close(const SocketId& id) {
    if (snapshot_in_progress_) return false;
    const auto iterator = active_.find(id);
    if (iterator == active_.end()) return false;
    rememberClosed(iterator->second);
    active_.erase(iterator);
    weak_active_.clear();
    cookie_active_.clear();
    for (const auto& [active_id, observation] : active_) {
        if (active_id.cookie) cookie_active_[CookieKey{active_id.netns, *active_id.cookie}] = active_id;
        else weak_active_[observation.tuple] = active_id;
    }
    return true;
}

std::vector<SocketObservation> SocketLifecycleTable::active() const {
    std::vector<SocketObservation> result;
    result.reserve(active_.size());
    for (const auto& [id, observation] : active_) {
        (void)id;
        result.push_back(observation);
    }
    return result;
}

std::vector<SocketObservation> SocketLifecycleTable::recentlyClosed() const {
    return {closed_.begin(), closed_.end()};
}

}  // namespace weaknet_dbus::v2
