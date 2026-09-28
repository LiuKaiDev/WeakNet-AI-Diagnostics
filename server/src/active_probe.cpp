#include "active_probe.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/ip_icmp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>

#include "scoped_fd.hpp"

namespace weaknet_dbus::v2 {
namespace {

constexpr std::uint8_t kIpv4Family = AF_INET;
constexpr std::uint64_t kMaxPollSliceMs = 50;

Validity validityFor(ProbeStatus status) noexcept {
    switch (status) {
        case ProbeStatus::Success: return Validity::Valid;
        case ProbeStatus::Timeout:
        case ProbeStatus::Unreachable:
        case ProbeStatus::InvalidReply:
        case ProbeStatus::Error: return Validity::Partial;
        case ProbeStatus::TransportUnavailable:
        case ProbeStatus::NoTarget: return Validity::Unavailable;
    }
    return Validity::Unavailable;
}

std::string statusReason(ProbeStatus status) {
    switch (status) {
        case ProbeStatus::Success: return {};
        case ProbeStatus::Timeout: return "reply timeout";
        case ProbeStatus::Unreachable: return "validated destination unreachable";
        case ProbeStatus::TransportUnavailable: return "ICMP transport unavailable";
        case ProbeStatus::NoTarget: return "probe target unavailable";
        case ProbeStatus::InvalidReply: return "no matching ICMP echo reply";
        case ProbeStatus::Error: return "probe transport error";
    }
    return "probe error";
}

}  // namespace

ActiveProbe::ActiveProbe(EventBus& bus, const Clock& clock, NetnsId netns,
                         ProbeConfig config, ActiveProbeTestHooks hooks)
    : bus_(bus), clock_(clock), netns_(netns), config_(std::move(config)),
      hooks_(std::move(hooks)) {
    if (config_.interval < std::chrono::seconds(2))
        config_.interval = std::chrono::seconds(2);
    if (config_.timeout <= std::chrono::milliseconds::zero() ||
        config_.timeout > config_.interval)
        config_.timeout = std::min(std::chrono::milliseconds(1000),
                                   std::chrono::duration_cast<std::chrono::milliseconds>(config_.interval));
    remote_ = parseRemote(config_.remote_ipv4, netns_);
    if (!remote_) {
        telemetry_.degraded = true;
        telemetry_.capability_reason = "invalid numeric remote IPv4 target";
    }
}

ActiveProbe::~ActiveProbe() { stop(); }

bool ActiveProbe::start() {
    std::lock_guard lock(mutex_);
    if (running_) return true;
    if (!config_.enabled) {
        telemetry_.degraded = true;
        telemetry_.capability_reason = "active probes disabled";
        return true;
    }
    running_ = true;
    worker_ = std::jthread([this](std::stop_token token) { loop(token); });
    return true;
}

void ActiveProbe::stop() noexcept {
    std::jthread worker;
    {
        std::lock_guard lock(mutex_);
        if (!running_ && !worker_.joinable()) return;
        running_ = false;
        worker = std::move(worker_);
    }
    wake_.notify_all();
    if (worker.joinable()) {
        worker.request_stop();
        worker.join();
    }
}

std::optional<ProbeTarget> ActiveProbe::parseRemote(std::string_view literal, NetnsId netns) {
    ProbeTarget target;
    target.kind = ProbeTargetKind::Remote;
    target.netns = netns;
    target.family = AF_INET;
    if (literal.empty() || inet_pton(AF_INET, std::string(literal).c_str(), target.address.data()) != 1)
        return std::nullopt;
    target.provenance = "configured numeric remote target";
    return target;
}

std::optional<ProbeTarget> ActiveProbe::selectGateway(
    const TopologySnapshot& snapshot, const UplinkSelection& uplink, NetnsId netns) {
    if (!snapshot.authoritative || !uplink.interface || uplink.validity == Validity::Unavailable)
        return std::nullopt;
    for (const auto& route : snapshot.routes) {
        if (!route.present || !route.isDefault() || !route.gateway ||
            !route.output_ifindex || *route.output_ifindex != uplink.interface->ifindex ||
            route.family != AF_INET || !route.multipath.empty()) continue;
        ProbeTarget target;
        target.kind = ProbeTargetKind::Gateway;
        target.netns = netns;
        target.family = AF_INET;
        target.address = *route.gateway;
        target.interface = uplink.interface;
        target.generation = snapshot.generation;
        target.provenance = route.identity();
        return target;
    }
    return std::nullopt;
}

void ActiveProbe::updateTopology(const TopologySnapshot& snapshot,
                                 const UplinkSelection& uplink) {
    std::lock_guard lock(mutex_);
    const bool explicit_no_uplink = snapshot.authoritative && !uplink.interface;
    topology_ = snapshot;
    uplink_ = uplink;
    if (snapshot.authoritative) {
        gateway_ = selectGateway(snapshot, uplink, netns_);
        if (explicit_no_uplink) remote_.reset();
        else remote_ = parseRemote(config_.remote_ipv4, netns_);
    } else if (gateway_) {
        auto stale = *gateway_;
        stale.provenance = "stale last-known-good gateway; topology degraded/unavailable; " + stale.provenance;
        gateway_ = std::move(stale);
        remote_.reset();
    } else {
        remote_.reset();
    }
    wake_.notify_all();
}

std::optional<ProbeTarget> ActiveProbe::gatewayTarget() const {
    std::lock_guard lock(mutex_);
    return gateway_;
}

std::optional<ProbeTarget> ActiveProbe::remoteTarget() const {
    std::lock_guard lock(mutex_);
    return remote_;
}

ProbeTelemetry ActiveProbe::telemetry() const {
    std::lock_guard lock(mutex_);
    return telemetry_;
}

ProbeAttemptResult ActiveProbe::nativeProbe(const ProbeTarget& target, std::uint64_t sequence,
                                            std::chrono::milliseconds timeout,
                                            std::stop_token token) const {
    if (target.family != AF_INET)
        return {ProbeStatus::TransportUnavailable, std::nullopt, "IPv4 transport only in this stage"};
    ScopedFd socket_fd(::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP));
    if (!socket_fd) {
        const int error = errno;
        return {error == EACCES || error == EPERM ? ProbeStatus::TransportUnavailable
                                                   : ProbeStatus::Error,
                std::nullopt, std::strerror(error)};
    }

    icmphdr request{};
    request.type = ICMP_ECHO;
    request.code = 0;
    request.un.echo.id = htons(static_cast<std::uint16_t>(::getpid()));
    request.un.echo.sequence = htons(static_cast<std::uint16_t>(sequence));
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    std::memcpy(&destination.sin_addr, target.address.data(), sizeof(destination.sin_addr));
    const auto started = std::chrono::steady_clock::now();
    if (::sendto(socket_fd.get(), &request, sizeof(request), 0,
                 reinterpret_cast<sockaddr*>(&destination), sizeof(destination)) < 0) {
        const int error = errno;
        return {error == EACCES || error == EPERM ? ProbeStatus::TransportUnavailable
                                                   : ProbeStatus::Error,
                std::nullopt, std::strerror(error)};
    }

    bool invalid_reply = false;
    while (!token.stop_requested()) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        if (elapsed >= timeout) break;
        pollfd descriptor{socket_fd.get(), POLLIN, 0};
        const auto remaining = timeout - elapsed;
        const int wait_ms = static_cast<int>(std::min<std::int64_t>(
            remaining.count(), static_cast<std::int64_t>(kMaxPollSliceMs)));
        const int ready = ::poll(&descriptor, 1, wait_ms);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return {ProbeStatus::Error, std::nullopt, std::strerror(errno)};
        }
        if (ready == 0) continue;
        std::array<std::byte, 512> buffer{};
        sockaddr_in source{};
        socklen_t source_length = sizeof(source);
        const auto received = ::recvfrom(socket_fd.get(), buffer.data(), buffer.size(), 0,
                                          reinterpret_cast<sockaddr*>(&source), &source_length);
        if (received < static_cast<ssize_t>(sizeof(icmphdr)) ||
            source.sin_addr.s_addr != destination.sin_addr.s_addr) {
            invalid_reply = true;
            continue;
        }
        const auto* reply = reinterpret_cast<const icmphdr*>(buffer.data());
        if (reply->type != ICMP_ECHOREPLY || reply->code != 0 ||
            reply->un.echo.id != request.un.echo.id ||
            reply->un.echo.sequence != request.un.echo.sequence) {
            invalid_reply = true;
            continue;
        }
        const auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started).count();
        return {ProbeStatus::Success, static_cast<std::uint64_t>(std::max<std::int64_t>(0, rtt)), {}};
    }
    if (token.stop_requested()) return {ProbeStatus::Error, std::nullopt, "probe stopped"};
    return {invalid_reply ? ProbeStatus::InvalidReply : ProbeStatus::Timeout,
            std::nullopt, invalid_reply ? "reply identity did not match request" : "reply timeout"};
}

bool ActiveProbe::probeTarget(const std::optional<ProbeTarget>& target,
                              ProbeTargetKind kind, std::stop_token token) {
    ProbeTarget effective;
    if (target) effective = *target;
    else {
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            generation = topology_.generation;
        }
        effective.kind = kind;
        effective.netns = netns_;
        effective.family = AF_INET;
        effective.generation = generation;
        effective.provenance = "no authoritative target";
    }
    const auto sequence = next_sequence_.fetch_add(1);
    if (!target) {
        publish(effective, sequence, {ProbeStatus::NoTarget, std::nullopt, "no authoritative/configured target"});
        return false;
    }
    ProbeAttemptResult result = hooks_.probe
        ? hooks_.probe(effective, sequence, config_.timeout, token)
        : nativeProbe(effective, sequence, config_.timeout, token);
    publish(effective, sequence, std::move(result));
    return true;
}

bool ActiveProbe::probeOnceForTests() {
    std::optional<ProbeTarget> gateway;
    std::optional<ProbeTarget> remote;
    {
        std::lock_guard lock(mutex_);
        gateway = gateway_;
        remote = remote_;
    }
    const bool first = probeTarget(gateway, ProbeTargetKind::Gateway, {});
    const bool second = probeTarget(remote, ProbeTargetKind::Remote, {});
    return first || second;
}

void ActiveProbe::loop(std::stop_token token) {
    while (!token.stop_requested()) {
        probeOnceForTests();
        std::unique_lock lock(mutex_);
        wake_.wait_for(lock, token, config_.interval, [&] { return !running_; });
    }
}

void ActiveProbe::record(const ProbeObservation& observation) {
    std::lock_guard lock(mutex_);
    if (observation.status != ProbeStatus::NoTarget) ++telemetry_.attempts;
    switch (observation.status) {
        case ProbeStatus::Success:
            ++telemetry_.successes;
            telemetry_.last_success_at = observation.observed_at;
            break;
        case ProbeStatus::Timeout: ++telemetry_.timeouts; break;
        case ProbeStatus::Unreachable: ++telemetry_.unreachable; break;
        case ProbeStatus::InvalidReply: ++telemetry_.invalid_replies; break;
        case ProbeStatus::TransportUnavailable:
            ++telemetry_.transport_errors;
            ++telemetry_.unavailable_capability;
            telemetry_.degraded = true;
            telemetry_.capability_reason = observation.failure_reason;
            break;
        case ProbeStatus::NoTarget: ++telemetry_.no_targets; break;
        case ProbeStatus::Error: ++telemetry_.transport_errors; break;
    }
}

void ActiveProbe::publish(const ProbeTarget& target, std::uint64_t sequence,
                          ProbeAttemptResult result) {
    ProbeObservation observation;
    observation.target = target;
    observation.netns = netns_;
    observation.observed_at = clock_.realtimeNow();
    observation.monotonic_at = clock_.monotonicNow();
    observation.sequence = sequence;
    observation.status = result.status;
    observation.rtt_us = result.status == ProbeStatus::Success ? result.rtt_us : std::nullopt;
    observation.validity = validityFor(result.status);
    observation.transport = hooks_.probe ? "test" : "icmp_datagram";
    observation.failure_reason = result.reason.empty() ? statusReason(result.status) : std::move(result.reason);
    record(observation);

    std::optional<InterfaceId> interface = target.interface;
    NetworkEventHeader header{kNetworkEventSchemaVersion,
        EventId{next_event_id_.fetch_add(1)}, {}, EventKind::ProbeObservation,
        EventSource::ActiveProbe, observation.observed_at, observation.monotonic_at,
        netns_, interface, std::nullopt, observation.validity, std::nullopt};
    (void)bus_.publish(NetworkEvent(std::move(header), std::move(observation)));
}

}  // namespace weaknet_dbus::v2
