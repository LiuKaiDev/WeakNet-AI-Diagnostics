# Phase 3 internal data plane

## Implemented boundary

Phase 3 adds the internal C++20 `NetworkEvent`, `EventBus`, `MetricStore`, clock, and V1 observation bridge alongside the V1 model. The existing `com.example.WeakNet` session-bus methods/signals, C ABI, quality computation, `.bin` files, Ping behavior, netlink/TCP/eBPF implementations, and `NetInfo` fields still use the V1 path.

There is no external V2 wire schema in this phase. `kNetworkEventSchemaVersion` versions the in-process value contract only. Construction rejects an unsupported version, a zero event ID, a missing network-namespace identity, an invalid interface ID, or a payload whose type disagrees with the header kind.

## Types, identity, time, and validity

The data-plane types are in `weaknet_dbus::v2` so the retained V1 `NetworkEvent` remains source-compatible.

- `EventId`, `EventSequence`, `NetnsId`, `InterfaceId`, `SocketId`, and `SampleId` are value types, not pointer identities.
- `NetnsId` is the device/inode pair from `stat(2)` on `/proc/self/ns/net`. Tests can construct deterministic IDs without creating a privileged namespace.
- `InterfaceId` uses Linux ifindex as identity within the header/key's mandatory namespace. The observed interface name is metadata and is not used for equality or ordering.
- `SocketId` contains a cookie and generation. Phase 3 can store it, but V1 cannot supply trustworthy per-socket attribution; SocketTracker generation semantics remain Phase 5 work.
- Every event/sample carries both realtime and monotonic timestamps. The production adapter uses `SystemClock`; tests use the thread-safe `ManualClock`.
- `Validity` is one of `Valid`, `Unavailable`, `Stale`, `Partial`, or `Reset`. Metric values are optional typed variants; an unavailable sample with a numeric value is rejected, so unavailable never becomes zero.
- `Status` carries a stable `StatusCode` and bounded-by-producer explanatory context. Phase 3 adapter messages are fixed strings; it does not accept arbitrary JSON.

The implemented payloads are interface snapshot/change, selected-uplink observation, RTT, mirrored V1 TCP-loss, aggregate traffic, Wi-Fi RSSI, and collector/runtime health. Incident and root-cause payload behavior is intentionally absent.

## EventBus

`EventBus` is an in-process, multi-producer-safe bounded queue with one owned `std::jthread` dispatcher. `DaemonApplication` constructs it with capacity 1024; tests or future application configuration can select another positive capacity through the constructor.

### Ordering

The queue mutex is the admission point. Every accepted publication receives the next nonzero bus-owned `EventSequence`; wall-clock and observation-monotonic timestamps do not determine delivery order. Accepted publications from one producer preserve call order. Concurrent producers receive the total order in which they acquire admission. The dispatcher delivers in increasing accepted sequence, with gaps permitted for coalesced observations. Cross-thread physical observation ordering is not inferred.

Unsigned sequence increment has defined wrap behavior. Sequence `UINT64_MAX` may be assigned once; after it wraps to zero, later publications are rejected and counted as drops rather than receiving an ambiguous sequence.

### Saturation

Interface, uplink, and collector-health observations are non-replaceable. RTT, TCP-loss, traffic, and RSSI samples are replaceable gauges. A replaceable event is coalesced only when a queued event has the exact key:

```text
kind + source + netns(device,inode) + optional ifindex + optional socket(cookie,generation)
```

On a full queue, the older exact-key gauge is removed and the newest accepted observation is appended with a new sequence. This preserves dispatch sequence order and increments the coalesce count. If no exact key exists, or the new event is non-replaceable, publication is rejected and counted as a drop. No event is claimed to be lossless. Coalescing means `accepted_publications` can exceed `dispatched_publications` by the number of accepted observations superseded before dispatch.

Telemetry snapshots expose capacity, current depth, high-water mark, accepted, dispatched, drops, coalesces, callback failures, accepted-by-kind/source, and dropped-by-kind counters. Publications rejected before start, during/after stop, at non-coalescible saturation, or after sequence exhaustion count as drops.

### Subscriptions and shutdown

Subscriptions are move-only RAII tokens backed by weak ownership of bus state, so a token cannot dangle after bus destruction. Explicit unsubscribe and destruction are idempotent. An external unsubscribe removes registry visibility and waits for an already-running callback; when it returns, that subscription receives no future callback. Self-unsubscribe marks the entry inactive immediately without deadlocking the current callback.

The dispatcher copies matching subscriber ownership under the registry lock, then releases it before invoking callbacks. Slow callbacks therefore never hold registry locks. Supported callback reentrancy includes publish, subscribe, unsubscribe, and initiating stop. A callback-initiated stop cannot join its own dispatcher thread; the drain completes on return from callbacks and the next external stop/destruction performs the join. Callback exceptions are contained and counted.

`stop()` rejects new publications, drains the finite queue in sequence order, joins the dispatcher, and is idempotent. Existing subscription tokens remain safe after shutdown. `DaemonApplication` stops V1 producers before draining the bus and destroys the adapter/store only after the drain.

## MetricStore

`MetricKey` dimensions are network namespace, typed metric name, typed unit, source, optional interface, optional socket, and observation interval in milliseconds. Phase 3 metric values are `int64_t`, `uint64_t`, or `double`; a sample also contains `SampleId`, validity, both timestamps, and optional status.

Default limits are:

| Limit | Default |
|---|---:|
| Distinct series | 1024 |
| Samples per series | 256 |
| Total samples | 65536 |
| Retained age | 1 hour |
| Query results | 1024 |
| Stale after | 30 seconds |

All limits are constructor-configurable and must be positive (durations may be zero). A new key at the series limit deterministically evicts the least-recently-updated series; equal timestamps resolve by `MetricKey` map order. Per-series overflow evicts its oldest sample. Global overflow evicts the oldest sample across all series, again resolving ties by key order. Age retention is physically pruned during insertion and filtered during reads. Out-of-order samples, invalid identities, zero sample IDs, valid samples without values, and unavailable samples with values are rejected.

`latest()` returns `std::nullopt` for a missing or age-expired key, never numeric zero. Window queries are inclusive, chronological, and capped by the configured query maximum. Staleness is evaluated against the injected monotonic clock and changes only the returned copy to `Stale`; the stored observation is not mutated. `snapshot()` returns deep value copies, optionally filtered by namespace, so readers never retain references into mutable containers. No user callback runs from the store or under its mutex.

Telemetry exposes active series, sample count, series evictions, sample evictions, rejected samples, and stale results returned by queries.

## V1 observations mirrored

`V1ObservationAdapter` writes metrics directly to `MetricStore` and independently publishes the corresponding event. A saturated EventBus therefore does not erase metric history, and a rejected metric insert does not alter V1 state.

The current bridge points are:

- initial and changed interface snapshots: after `WeakNetMgr::updateInterfaces`; repeated identical snapshots are suppressed and removed interfaces produce `present=false`;
- initial and changed current-uplink state: after the existing V1 selection has updated `usingNow`;
- RTT: after each existing `updateRttAndStateSafe` probe cycle;
- TCP loss: after each V1 delta computation, plus explicit unavailable samples when no uplink or sampling failure exists, and `Reset` when aggregate counters decrease;
- traffic bytes/s, packets/s, and active flows: after the existing outer V1 traffic update point;
- Wi-Fi RSSI: after each existing Wi-Fi update cycle for interfaces V1 labels Wi-Fi;
- worker lifecycle and eBPF availability: at Phase 2 worker start/stop/degradation and eBPF startup status.

Mirrored V1 RTT, TCP-loss, traffic, RSSI, interface, and uplink values are marked `Partial` when present because V1 does not provide all trustworthy scope/semantic guarantees required by later collector phases. A failed/missing observation is `Unavailable` with no value. The current daemon namespace is recorded explicitly. `if_nametoindex` supplies interface identity; if it cannot, interface scope remains absent and the metric/event is unavailable rather than inventing an ID.

The V1 network-quality result is not mirrored as an authoritative raw metric because validity-aware quality semantics belong to Phase 7. Per-socket TCP facts are not mirrored because V1 aggregation cannot provide a trustworthy `SocketId`; that remains Phase 5. Inner traffic-analyzer intermediate samples are not duplicated; only the existing outer normalized update is mirrored. V1 RSSI normally has no samples while V1 interface type remains `Unknown`; the adapter is present without redesigning Wi-Fi discovery.

## Lifecycle and tests

Startup constructs the clock, bus, and store; starts the dispatcher; derives the current kernel namespace; constructs the V1 bridge; then starts V1 producers. Required namespace/bus setup failure uses the Phase 2 rollback path. Shutdown stops/join producers first, stops V1 forwarding, drains/stops EventBus, then releases D-Bus, V1 manager, bridge, and other application resources. The Phase 2 five-second contract remains tested.

CTest targets `network_event_schema`, `event_bus_concurrency`, `metric_store_semantics`, and `v1_observation_adapter` cover schema validation/value semantics, concurrent total ordering, reentrancy, RAII synchronization, exception containment, exact saturation counters, retention/cardinality/staleness, immutable concurrent snapshots, namespace/scope separation, fake-clock progression, and representative V1 mapping. Sanitizer results are reported from the actual validation run rather than asserted in this document.

## Deferred work

This phase does not redesign netlink/uplink selection, SocketTracker/TCP semantics, eBPF schema/hooks, probe matching, or Wi-Fi discovery. It adds no IncidentEngine, RootCauseEngine, V2/system-bus API, `weaknetctl`, lab, persistence replacement, Python, or AI behavior.
