#include "socket_tracker.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

using namespace weaknet_dbus::v2;

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

SocketTuple tuple(NetnsId netns, std::uint8_t family, std::uint16_t local_port,
                  std::uint16_t remote_port) {
    SocketTuple result;
    result.netns = netns;
    result.family = family;
    result.protocol = SocketProtocol::Tcp;
    result.local.family = family;
    result.remote.family = family;
    result.local.port = local_port;
    result.remote.port = remote_port;
    if (family == AF_INET) {
        result.local.address = {192, 0, 2, 10};
        result.remote.address = {198, 51, 100, 20};
    } else {
        result.local.address[0] = 0x20;
        result.local.address[1] = 0x01;
        result.local.address[15] = 10;
        result.remote.address[0] = 0x20;
        result.remote.address[1] = 0x01;
        result.remote.address[15] = 20;
    }
    return result;
}

SocketObservationInput input(const SocketTuple& value,
                              std::optional<KernelSocketCookie> cookie = std::nullopt) {
    SocketObservationInput result;
    result.tuple = value;
    result.cookie = cookie;
    result.source = EventSource::SocketTracker;
    result.validity = Validity::Valid;
    return result;
}

bool sameId(const SocketObservation& left, const SocketObservation& right) {
    return left.id == right.id;
}

}  // namespace

int main() {
    bool ok = true;
    const NetnsId first_namespace{1, 100};
    const NetnsId second_namespace{1, 101};
    const auto tcp4 = tuple(first_namespace, AF_INET, 40000, 443);
    const auto tcp6 = tuple(first_namespace, AF_INET6, 40000, 443);

    SocketLifecycleTable table;
    ok &= expect(table.beginSnapshot(SocketSnapshotDisposition::Authoritative),
                 "initial authoritative snapshot did not begin");
    const auto first = table.observe(input(tcp4, KernelSocketCookie{7}));
    ok &= expect(first && first->created_generation, "first cookie-backed socket was not new");
    ok &= expect(table.commitSnapshot(), "initial authoritative snapshot did not commit");

    ok &= expect(table.beginSnapshot(SocketSnapshotDisposition::Authoritative),
                 "same-socket snapshot did not begin");
    const auto same = table.observe(input(tcp4, KernelSocketCookie{7}));
    ok &= expect(same && !same->created_generation && first && sameId(first->observation, same->observation),
                 "same namespace/cookie did not preserve identity");
    ok &= expect(table.commitSnapshot(), "same-socket snapshot did not commit");

    ok &= expect(table.beginSnapshot(SocketSnapshotDisposition::Authoritative),
                 "different-cookie snapshot did not begin");
    const auto different_cookie = table.observe(input(tcp4, KernelSocketCookie{8}));
    ok &= expect(different_cookie && different_cookie->observation.id != first->observation.id,
                 "different cookie reused socket identity");
    table.commitSnapshot();

    SocketLifecycleTable namespace_table;
    const auto other_namespace_tuple = tuple(second_namespace, AF_INET, 40000, 443);
    namespace_table.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto namespace_first = namespace_table.observe(input(tcp4, KernelSocketCookie{9}));
    const auto namespace_second = namespace_table.observe(input(other_namespace_tuple, KernelSocketCookie{9}));
    ok &= expect(namespace_first && namespace_second &&
                     namespace_first->observation.id != namespace_second->observation.id,
                 "namespace boundary did not isolate cookie identities");
    namespace_table.commitSnapshot();

    SocketLifecycleTable weak_table;
    weak_table.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto weak_first = weak_table.observe(input(tcp4));
    const auto weak_repeat = weak_table.observe(input(tcp4));
    ok &= expect(weak_first && weak_repeat && sameId(weak_first->observation, weak_repeat->observation),
                 "weak tuple did not remain stable within one snapshot");
    weak_table.commitSnapshot();
    const auto weak_id = weak_first->observation.id;
    weak_table.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    weak_table.commitSnapshot();
    ok &= expect(weak_table.activeCount() == 0 && weak_table.closedCount() == 1,
                 "authoritative weak-socket disappearance did not close lifecycle");
    weak_table.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto weak_reuse = weak_table.observe(input(tcp4));
    weak_table.commitSnapshot();
    ok &= expect(weak_reuse && weak_reuse->observation.id != weak_id,
                 "reused weak tuple inherited the old lifecycle identity");

    SocketLifecycleTable partial_table;
    partial_table.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto partial_first = partial_table.observe(input(tcp4));
    partial_table.commitSnapshot();
    partial_table.beginSnapshot(SocketSnapshotDisposition::Partial);
    partial_table.commitSnapshot();
    ok &= expect(partial_table.activeCount() == 1,
                 "partial snapshot incorrectly closed missing socket");
    partial_table.beginSnapshot(SocketSnapshotDisposition::Failed);
    ok &= expect(!partial_table.observe(input(tuple(first_namespace, AF_INET, 40001, 443))),
                 "failed snapshot accepted observations");
    partial_table.commitSnapshot();
    partial_table.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto partial_same = partial_table.observe(input(tcp4));
    partial_table.commitSnapshot();
    ok &= expect(partial_first && partial_same && sameId(partial_first->observation, partial_same->observation),
                 "successful snapshot did not preserve active identity after partial failure");

    SocketLifecycleTable cookie_reuse;
    cookie_reuse.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto cookie_first = cookie_reuse.observe(input(tcp4, KernelSocketCookie{42}));
    cookie_reuse.commitSnapshot();
    cookie_reuse.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    cookie_reuse.commitSnapshot();
    cookie_reuse.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto cookie_second = cookie_reuse.observe(input(tcp4, KernelSocketCookie{42}));
    cookie_reuse.commitSnapshot();
    ok &= expect(cookie_first && cookie_second && cookie_first->observation.id != cookie_second->observation.id,
                 "reused cookie inherited the old lifecycle identity");

    SocketLifecycleTable inconsistent;
    inconsistent.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto inconsistent_first = inconsistent.observe(input(tcp4, KernelSocketCookie{77}));
    const auto changed_tuple = tuple(first_namespace, AF_INET, 40001, 443);
    const auto inconsistent_second = inconsistent.observe(input(changed_tuple, KernelSocketCookie{77}));
    ok &= expect(inconsistent_first && inconsistent_second &&
                     inconsistent_second->inconsistent_metadata &&
                     inconsistent_first->observation.id != inconsistent_second->observation.id,
                 "inconsistent same-cookie metadata was silently merged");
    inconsistent.commitSnapshot();
    ok &= expect(inconsistent.activeCount() == 1,
                 "inconsistent cookie reconciliation retained multiple active generations");

    SocketLifecycleTable families;
    families.beginSnapshot(SocketSnapshotDisposition::Authoritative);
    const auto ipv4 = families.observe(input(tcp4, KernelSocketCookie{100}));
    const auto ipv6 = families.observe(input(tcp6, KernelSocketCookie{101}));
    families.commitSnapshot();
    ok &= expect(ipv4 && ipv6 && ipv4->observation.id != ipv6->observation.id &&
                     tcp4.local.toString() != tcp6.local.toString(),
                 "IPv4/IPv6 endpoint identity or display conversion collided");

    SocketLifecycleTable bounded(SocketLifecycleConfig{16, 2});
    for (std::uint16_t port = 41000; port < 41005; ++port) {
        bounded.beginSnapshot(SocketSnapshotDisposition::Authoritative);
        const auto observation = bounded.observe(input(tuple(first_namespace, AF_INET, port, 443)));
        ok &= expect(static_cast<bool>(observation), "bounded lifecycle rejected a fresh socket");
        bounded.commitSnapshot();
        bounded.beginSnapshot(SocketSnapshotDisposition::Authoritative);
        bounded.commitSnapshot();
    }
    ok &= expect(bounded.closedCount() == 2,
                 "closed lifecycle retention exceeded configured bound");

    if (first) {
        NetworkEventHeader header{kNetworkEventSchemaVersion, EventId{9001}, {},
            EventKind::SocketObservation, EventSource::SocketTracker,
            first->observation.observed_at, first->observation.monotonic_at,
            first->observation.tuple.netns, std::nullopt, first->observation.id,
            Validity::Valid, std::nullopt};
        NetworkEvent event(header, first->observation);
        const auto copied = std::get<SocketObservation>(event.payload());
        ok &= expect(copied.id == first->observation.id && copied.tuple == first->observation.tuple,
                     "socket observation event payload did not round-trip");
    }

    return ok ? 0 : 1;
}
