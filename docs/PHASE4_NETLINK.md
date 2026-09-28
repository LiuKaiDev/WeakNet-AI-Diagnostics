# Phase 4 Netlink topology collector

> Historical implementation note. Socket, diagnosis, D-Bus V2, CLI, lab and
> AI/RAG components mentioned as later work are now implemented.

## Boundary and ownership

Phase 4 adds one authoritative `weaknet_dbus::v2::NetlinkCollector`. It owns a read-only `NETLINK_ROUTE` socket, the dump/notification lifecycle, a bounded reconciled topology snapshot, selected-uplink policy, and collector telemetry. The collector is owned by `DaemonApplication` and runs an owned `std::jthread`; no netlink worker is detached.

The old `NetInterfaceManager` and `UsingInterfaceManager` sources remain as compatibility classes for source/build compatibility, but `DaemonApplication` no longer starts the old listener and `WeakNetMgr` uses the Phase 4 collector whenever it is installed. Thus there is one active rtnetlink discovery/listener path in the daemon. The old classes are fallback compatibility code only and are not authoritative.

## Namespace and identity

The collector targets the daemon's current namespace. Its `NetnsId` is the Phase 3 `(st_dev, st_ino)` identity from `/proc/self/ns/net`; every link, address, route, event, and snapshot carries that identity. No `setns()` is used in Phase 4. Future explicit namespace collection must create the socket in a dedicated collector context.

An interface is identified by namespace plus Linux ifindex. The current name is mutable metadata and supports punctuation such as `-`, `.`, and `_`. Link rename changes metadata without changing identity. A deleted link removes dependent addresses/routes from the reconciled snapshot. If an ifindex is later reused, the new link is a new current fact; persistent generation semantics are intentionally deferred.

## Socket and groups

The collector creates one `AF_NETLINK/SOCK_RAW|SOCK_CLOEXEC` `NETLINK_ROUTE` socket, sets a bounded receive buffer (1 MiB), binds `RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE`, and sets nonblocking mode. Production code performs no `ip`, `route`, `/proc/net/route`, or network mutation.

## Initial dump and receive semantics

Startup distinguishes transport availability from observed topology availability. Failure to create, configure, bind, or make the `NETLINK_ROUTE` socket nonblocking is a collector start failure: no worker starts and `running()` remains false. Once that transport is open, startup performs four bounded multipart dumps: links (`RTM_GETLINK`), addresses (`RTM_GETADDR`), IPv4 routes, and IPv6 routes. Each request has a unique process-local sequence. Replies must come from kernel sender PID 0 and either match the request sequence or be unsolicited sequence 0. Each dump is accepted only after `NLMSG_DONE`; `NLM_F_DUMP_INTR`, `NLMSG_ERROR`, malformed lengths/attributes, `MSG_TRUNC`, timeout, socket errors, or missing completion fail that reconciliation. Immediate `EAGAIN` is not an empty-success result: the collector polls in bounded 100 ms waits until completion or a five-second deadline.

An initial reconciliation failure after the socket opens does not fail collector startup or close the socket. The worker starts, `running()` is true, and telemetry is degraded. With no earlier successful state, the exposed snapshot remains empty and non-authoritative; a failed dump is not interpreted as an authoritative empty topology. If an authoritative snapshot already exists, any later failed or partial candidate is discarded and that last-known-good snapshot remains authoritative.

Notifications can interleave with dump replies. The collector deliberately keeps sequence-0 multicast mutations out of the in-progress dump candidate. A relevant link/address/route notification observed during any dump marks the reconciliation as raced; the candidate is rejected even if every multipart dump reaches `NLMSG_DONE`, and the last-known-good snapshot remains authoritative. Race and forced-resync counters make the lost opportunity visible, and the bounded retry path performs a clean reconciliation later. This conservative strategy avoids resurrecting a stale dump row after a delete/update notification without requiring a generation/epoch merge algorithm. Notifications are applied directly only while no dump candidate is active.

After startup, notifications are processed from the same socket and parser. Healthy operation reconciles approximately every 30 seconds. Degraded startup, dump/notification races, ENOBUFS, malformed notifications, and uncertain notification application use a collector-local retry backoff beginning at 250 ms and doubling to a five-second cap; successful reconciliation resets the backoff and restores the normal interval. ENOBUFS never means “current state is complete”: it increments overflow/forced-resync telemetry, marks observation degraded, and preserves the last-known-good snapshot. A malformed or unsupported notification is not partially applied; the collector records the parse/apply failure and requests bounded reconciliation. This rate-bounds persistent failures while recovering substantially sooner than 30 seconds. Polling and every dump observe the worker stop token, so stop interrupts retry processing and closes the owned socket after the worker joins. A successful recovery commits the complete candidate atomically, marks it authoritative and non-partial, recomputes the selected uplink, publishes normal state changes, clears `telemetry.degraded` and `last_error`, and preserves the same socket/worker lifecycle.

## Topology model

`TopologySnapshot` contains immutable-on-read copies of links, addresses, routes, namespace identity, and authoritative/partial flags.

Links retain ifindex/name, flags, `IFLA_OPERSTATE`, carrier, link type, and presence. Addresses retain family, interface, prefix length, raw 16-byte address, scope, flags, and add/delete state. IPv4 and IPv6 are both parsed.

Routes retain family, destination/prefix, effective table (`RTA_TABLE` overrides `rtm_table`), priority/metric, protocol, scope, type, optional output interface, optional gateway, preferred source, and individual multipath nexthops (ifindex, gateway, hops, flags). Route identity includes namespace and all of those modeled dimensions, preserves multiplicity, and canonicalizes multipath nexthop ordering. A delete notification is matched against every modeled field it actually carries; if omitted fields leave multiple stored routes possible, no route is guessed or removed and a full reconciliation is requested. A route without a gateway is an on-link route, not an invented gateway or zero address.

Multipath is retained as a vector and never flattened into a single authoritative route. A compatibility representative may be selected, but it is explicitly marked `partial` and evidence says `representative=min-ranked-nexthop`.

## Uplink policy

Phase 4 intentionally models only the main/default tables (`RT_TABLE_MAIN` and `RT_TABLE_DEFAULT`); it does not claim full Linux policy-routing/FIB equivalence. It does not model `ip rule`, source rules, fwmarks, VRFs, or per-flow ECMP hashing.

Eligible candidates are usable non-loopback links referenced by unicast default routes. The deterministic rank is:

1. main table before default table;
2. lower route priority/metric;
3. IPv4 before IPv6;
4. non-multipath before multipath;
5. lower ifindex;
6. lexicographic complete route identity.

The method flags expose whether usable IPv4 and/or IPv6 defaults exist. A gatewayless default is eligible and evidence says `gateway=on-link`. A multipath choice is `partial`, retains all nexthops, and evidence states the representative rule. No usable route yields `unavailable` for an authoritative snapshot, or `stale` for a non-authoritative snapshot. Evidence includes table, metric, family, ifindex, gateway/on-link, multipath, and tie-break details.

## Phase 3 events and V1 compatibility

The collector publishes typed `LinkObservation`, `AddressObservation`, `RouteObservation`, and `UplinkObservation` values through EventBus only. Events carry schema version, collector source, namespace, timestamps, interface identity where applicable, and valid/partial status. Reconciliation publishes only differences; unchanged state is not replayed.

`WeakNetMgr::collectCurrentInterfaces()` derives the V1 interface list from the authoritative topology snapshot and selected policy. `updateCurrentUsing()` consumes the same selected result. Existing `ListInterfaces`, `GetInterfaces`, `HealthCheck`, C ABI, and current V1 `NetInfo` fields remain unchanged externally. The Phase 3 adapter remains for non-topology V1 observations; native Phase 4 link/uplink events originate from the collector, avoiding duplicate V1 link/uplink mirror publication in the new path.

## Telemetry and health

Telemetry exposes successful reconciliations, failed/interrupted dumps, parser errors, truncations, overflow events, resync requests, forced resync requests, notifications processed, notifications observed during reconciliation, rejected reconciliation races, ambiguous route deletes, notification apply failures, state changes published, current link/address/route counts, degraded state, and the last error. Initial reconciliation failure is reported as degraded runtime health; the daemon continues with bounded empty/non-authoritative topology rather than terminating solely for optional observation loss. A transport or worker start failure is separately reported as `transport_or_worker_start_failed` and leaves the collector stopped.

`RuntimeHealth` is still a direct startup/lifecycle registry rather than a subscription to collector telemetry. It records degraded initial reconciliation correctly, but this small Phase 4.1 change does not add a cross-component recovery propagation framework: after a later successful retry, collector telemetry and topology recover immediately while the startup `RuntimeHealth` entry may remain degraded until another lifecycle update or daemon restart.

## Tests and prerequisites

`netlink_topology` is a deterministic, unprivileged parser/policy/fake-reconciliation test. It covers link rename/punctuation, IPv4/IPv6 address and route parsing, default route/priority, multipart completion/interruption, malformed lengths/known attributes, unexpected sequence/sender, metric selection, on-link defaults, unsupported tables, multipath partial semantics and ordering, preferred-source route identity, namespace identity, route multiplicity, exact/ambiguous route deletion, ifindex reuse cleanup, EventBus sequence/scope, V1 topology compatibility, transactional fake reconciliation (including empty/EAGAIN-equivalent, incomplete/timeout-equivalent, failed-candidate, and conservative dump/notification-race rejection), transport-start failure, degraded initial reconciliation recovery, ENOBUFS recovery, malformed-notification resync, telemetry recovery, and bounded stop during retry. It does not mutate the host network.

`netlink_namespace_integration` attempts `unshare(CLONE_NEWNET)` and starts a real collector inside the disposable namespace. It returns CTest `SKIP_RETURN_CODE=77` with the raw `unshare` error when the runner lacks the namespace prerequisite; it never falls back to changing the host namespace.

The ordinary CTest suite does not create veths, routes, or addresses: the namespace test only exercises collector startup in a disposable namespace and skips when `CAP_SYS_ADMIN`/namespace creation is unavailable. No host namespace mutation was performed. Sanitizer and compiler status is reported from the actual validation run.

## Subsequent implementation

The topology collector now feeds SocketTracker route attribution,
IncidentEngine, RootCauseEngine, D-Bus V2, `weaknetctl`, ActiveProbe, Wi-Fi
evidence and WeakNet Lab. Full Linux policy-routing equivalence remains outside
the modeled topology boundary.
