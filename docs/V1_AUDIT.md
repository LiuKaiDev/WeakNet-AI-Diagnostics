# WeakNet V1 audit

## Audit scope and status

This document records a source-level audit of commit `ce03d84` on 2026-09-17. The audit covered every tracked source, header, build/configuration file, script, document, client example, manual test, and experimental Python tool in the repository.

This is an implementation audit, not a V2 implementation. No runtime behavior or build logic was changed during this phase.

The audit host exposes `/sys/kernel/btf/vmlinux`, but it does not have `make`, CMake, `g++`, `clang`, `bpftool`, or the queried pkg-config development packages installed. Consequently, source and command-path checks were possible, but a clean native build and privileged runtime validation were not. Historical success claims in older documentation were not treated as current test evidence.

## Executive summary

V1 has a useful Linux-native prototype core:

- rtnetlink discovery of interfaces with default routes;
- a second rtnetlink path for current-uplink changes;
- raw ICMP RTT probing bound to an interface;
- `NETLINK_SOCK_DIAG`/`TCP_INFO` inspection;
- wpa_supplicant control-socket RSSI access;
- a libbpf loader and CO-RE field reads in a small eBPF program;
- a C++ network model, quality assessor, event object, D-Bus service, C client ABI, and manual CLI validation tool.

Those pieces demonstrate the intended platform direction, but V1 is not yet a dependable observability or root-cause system. Metric semantics are sometimes incorrect, lifecycle ownership is unsafe, interfaces are weakly versioned, and tests cannot establish correctness. There is no metric store, incident state machine, root-cause engine, reproducible lab, production service unit, or benchmark/evaluation harness.

The best V2 path is an incremental extraction and hardening of useful V1 code. The target should not be a clean-slate rewrite.

## Repository inventory and module responsibilities

| Area | V1 responsibility | Audit assessment |
|---|---|---|
| `server/src/main.cpp` | Calls `start_server()` | Minimal and reusable as a composition entry point after renaming. |
| `server/src/server.cpp` | Constructs the daemon, starts monitors, owns the main D-Bus loop | Monolithic orchestration with mixed ownership and no shutdown path; should be decomposed incrementally. |
| `server/src/dbus_service.cpp` | D-Bus methods and signals | Useful compatibility reference; lacks introspection/versioning and performs blocking work on dispatch. |
| `server/src/looper.cpp` | Blocking libdbus dispatch loop | Small but uninterruptible in practice and leaks its thread-local singleton. |
| `server/src/net_iface.cpp` | Snapshot of UP, non-loopback interfaces with IPv4/IPv6 default routes | Useful rtnetlink parsing knowledge; receive/reconciliation logic is fragile. |
| `server/src/using_iface.cpp` | Long-lived rtnetlink listener selecting a current uplink | Duplicates route/link parsing and owns a detached worker with no stop. |
| `server/src/net_ping.cpp` | IPv4 raw-ICMP echo bound with `SO_BINDTODEVICE` | Useful active-probe seed; permission, reply matching, IPv6, timing, and concurrency need hardening. |
| `server/src/net_tcp.cpp` | sock_diag dump plus `tcp_info` aggregation and an approximate retransmission ratio | Socket discovery is useful; the current loss-rate calculation is not semantically sound. |
| `server/src/net_wifiriss.cpp` | wpa_supplicant control-socket `SIGNAL_POLL` | Potentially reusable behind a Wi-Fi collector; auto-starting wpa_supplicant with `system()` is inappropriate for a daemon. |
| `server/src/*_monitor.cpp` | Periodic RTT, RSSI, and TCP-loss loops | Polling behavior exists, but several threads are detached and emit D-Bus directly. |
| `server/src/flow_rate.bpf.c` | Outbound IPv4 TCP/UDP flow byte/packet counters | A useful experiment, not yet portable or complete eBPF observability. |
| `server/src/net_traffic.cpp` | libbpf open/load/attach and map snapshot sampling | Useful loader experiment; no generated skeleton, cleanup, shared ABI, or concurrency control. |
| `server/src/traffic_analyzer.cpp` | Background traffic sampling and anomaly logging | Duplicates sampling with the outer monitor and performs long blocking windows. |
| `server/src/traffic_anomaly_detector.cpp` | Heuristic flow-history anomaly routines | Not wired into the daemon's effective diagnosis path; rules have divide-by-zero/time-thread-safety risks. |
| `server/src/weak_netmgr.cpp` | Central mutable interface list and metric updates | The nearest V1 equivalent to state aggregation; its single mutex covers blocking I/O and should be split. |
| `server/include/net_info.hpp` | Per-interface mutable state model | Useful domain vocabulary, but lacks validity, timestamp, namespace, units, and provenance. |
| `server/src/network_quality_assessor.cpp` | Weighted quality score and JSON details | Deterministic seed logic, but unknown data and metric meaning are handled incorrectly. |
| `server/src/event_manager.cpp` | In-process callback vectors and D-Bus forwarding | Defines a `NetworkEvent`, but it is not a thread-safe event bus and has no backpressure. |
| `server/src/serializer.cpp` | Ad-hoc binary persistence for one reply and one signal | Demonstration side channel; unsafe as a durable or IPC format. |
| `server/src/logger.cpp` | glog configuration | Basic logging wrapper is reusable in concept; initialization/order and structured fields need work. |
| `client/client.cpp` | libdbus C++ client behind a C ABI | Valuable compatibility surface; subscription and global lifetime semantics need repair. |
| `client/test_client.cpp` | CLI commands and manual API checks | Useful smoke-tool seed, not an automated test suite or benchmark. |
| Other client examples | Demonstrations | Not built; at least one contains a compile-time typo. |
| `optional/experimental/log-analysis-tools/` | Log capture, rule analysis, several RAG prototypes | Correctly non-core, highly duplicated and not evaluated; should be quarantined then consolidated later. |

## Current architecture

```text
test-client / libweaknet.so
          |
          | session D-Bus: com.example.WeakNet
          v
weaknet-dbus-server (single process)
  |-- main libdbus dispatch loop
  |-- interface snapshot thread ---------- rtnetlink dumps
  |-- current-uplink polling thread ------- UsingInterfaceManager listener
  |-- detached RTT thread ---------------- raw ICMP
  |-- detached RSSI thread --------------- wpa_supplicant socket
  |-- TCP-loss thread -------------------- sock_diag + TCP_INFO
  |-- outer traffic thread --------------- WeakNetMgr updates
  |     `-- inner TrafficAnalyzer thread -- libbpf/map sampling
  `-- network-quality thread ------------- weighted heuristics

Shared mutable center: WeakNetMgr::current_interfaces_
Notification path: monitor -> detached sender -> DbusService/EventManager
Experimental AI path: daemon text logs -> Python regex -> rules/FAISS/LLM
```

### Startup and steady-state flow

1. `start_server()` initializes glog and connects to the session bus.
2. It requests `com.example.WeakNet`, registers `/com/example/WeakNet`, installs the global event manager, constructs `WeakNetMgr`, and takes an initial interface snapshot.
3. It starts interface, uplink, RTT, RSSI, TCP-loss, traffic, and network-quality activity.
4. The main thread blocks in `dbus_connection_read_write_dispatch(..., -1)`.
5. Monitors mutate copies or the central `current_interfaces_` vector and directly cause D-Bus signals.
6. `HealthCheck` snapshots the vector, scores the selected/first interface, and returns hand-built JSON.

There is no signal handler or other code that sets `ServerContext::running` to false. Normal service shutdown, joining, D-Bus unregister/unref, eBPF detach, and object deletion are therefore absent.

## Build system and dependencies

### Observed build

The root `Makefile` invokes `server/Makefile`, then directly compiles:

- `server/bin/weaknet-dbus-server` from all server `.cpp` files;
- `server/build/flow_rate.bpf.o` from `flow_rate.bpf.c`;
- `client/lib/libweaknet.so` from `client.cpp` and a second compilation of `serializer.cpp`;
- `client/bin/test-client` linked against that shared library.

Both Makefiles specify C++17, not C++20. `CC` is assigned `g++`; there is no `CXX` separation. Dependency discovery uses shell-expanded `pkg-config` fragments. The server always links `libbpf`, so the source-level `HAVE_LIBBPF` fallback does not make libbpf an optional build dependency.

### Required and runtime-sensitive dependencies

- Linux UAPI headers and a Linux kernel;
- libdbus-1 and a session bus;
- glog;
- libbpf, libelf, zlib and BPF-capable clang/LLVM;
- `server/vmlinux.h`, generated manually from the running host's BTF;
- sufficient privileges/capabilities for BPF loading, sock_diag visibility, raw ICMP, and device binding;
- wpa_supplicant and its control socket for RSSI;
- a compatible kernel symbol set for the two kprobes.

### Build-system gaps and risks

- No `CMakeLists.txt`, exported targets, CTest, install rules, package metadata, systemd unit, D-Bus policy, or CI definition.
- `server/vmlinux.h` is ignored and mandatory, but the Makefiles and `install.sh` do not generate it.
- `flow_rate.bpf.c` hard-codes `__TARGET_ARCH_x86`.
- Header dependency lists are incomplete; changing several included headers will not force a server rebuild.
- The eBPF object is mandatory at build time even though runtime code describes eBPF as degradable.
- The shared library has no SONAME/API symbol policy/versioned package configuration.
- Examples and `test_network_quality.cpp` are not built. `example_usage.cpp` contains `std::chrono::steady_-clock`, which is invalid C++.
- `install.sh` mutates the host, omits some documented prerequisites such as `bpftool`, starts a server without guaranteeing a session bus, and treats the CLI's unreliable exit status as test evidence.
- Relative runtime paths depend on launching from the repository root. Launching from another directory changes log, persistence, and BPF-object resolution.

## Threading and ownership model

| Activity | Ownership in V1 | Period/blocking behavior | Main risks |
|---|---|---|---|
| Main D-Bus loop | Main thread | Infinite blocking dispatch | No stop signal; Ping/DNS runs in this dispatch thread for up to seconds. |
| Interface snapshot | `ctx.iface_thread`, joinable | Snapshot then 10 s sleep | Exceptions escape the thread; local snapshot logs default metrics; signal workers detach. |
| Uplink monitor wrapper | `ctx.using_thread`, joinable | Central-state update then 10 s sleep | Wraps a second background listener; signal workers detach. |
| Rtnetlink uplink listener | `Impl::worker`, detached | Nonblocking recv with 50 ms polling | No stop/destruction; start race before `running=true`; raw back-pointer lifetime. |
| RTT monitor | Detached temporary thread | Raw ping per interface then 10 s sleep | Captures stack `ServerContext*`; central mutex is held during DNS/ping. |
| RSSI monitor | Detached temporary thread | Socket work then 10 s sleep | Captures stack context; central mutex held during socket connection and possible process launch. |
| TCP-loss monitor | `ctx.tcp_loss_thread`, joinable | Sock-diag dumps then 10 s sleep | Previous sample is not reset when active interface changes. |
| Outer traffic monitor | `ctx.traffic_analysis_thread`, joinable | Re-samples and sleeps 10 s | Calls blocking 5 s + 5 s sampling while holding the central mutex. |
| Inner traffic analyzer | `unique_ptr<std::thread>`, joinable by its owner | Approximately 1 s + 5 s + 5 s samples per loop | Concurrently samples the same BPF map and duplicates outer work. |
| Quality monitor | `ctx.network_quality_thread`, joinable | Snapshot then 15 s sleep | Emits through unsynchronized global event manager. |
| Signal sends | Many detached short-lived threads | Flush D-Bus and sometimes overwrite a file | Unbounded creation, context lifetime hazards, file races, event-counter races. |

### Shared state

`WeakNetMgr::current_interfaces_` is protected by `iface_mutex_`, and readers usually receive a copy. That is a useful starting discipline. However, “safe” update methods keep that mutex held while performing raw pings, wpa_supplicant I/O, and two multi-second eBPF sampling windows. This serializes unrelated collectors and can delay D-Bus health snapshots.

Other shared state is not adequately protected:

- `NetworkEventManager` callback vectors, `server_ctx_`, and `monitoring_active_`;
- the static non-atomic event counter in `emitEvent()`;
- the raw-ICMP static sequence number;
- `WeakNetMgr::traffic_analyzer_` construction/access;
- `NetTrafficAnalyzer` attachment handles and concurrent sampling;
- the C client's global `g_client` and static quality callback;
- `std::localtime()` use in anomaly code;
- serialized signal files written by multiple detached threads.

## IPC model

### D-Bus endpoint

- Bus: session bus (`DBUS_BUS_SESSION`)
- Well-known name: `com.example.WeakNet`
- Object path: `/com/example/WeakNet`
- Interface: `com.example.WeakNet`
- Name acquisition: `DBUS_NAME_FLAG_REPLACE_EXISTING`

### Methods

| Member | Input | Output | Behavior |
|---|---|---|---|
| `Get` | `()` | `(s)` | Returns the fixed greeting `Hello from WeakNet Server`; also overwrites `get_reply.bin`. |
| `ListInterfaces` | `()` | `(as)` | Returns current interface names. |
| `GetInterfaces` | `()` | `(as)` | Alias of `ListInterfaces`. |
| `HealthCheck` | `()` | `(s)` | Returns a hand-built JSON string for one interface. |
| `Ping` | `(s)` | `(s)` or `com.example.WeakNet.Error` | Performs synchronous IPv4 raw ICMP from the D-Bus dispatch thread. A failed ICMP attempt is normally encoded as a successful string reply. |

### Signals

| Member | Signature | Notes |
|---|---|---|
| `Changed` | `(si)` | Generic change. Most producers pass counter `0`; interface-local `change_counter` is never incremented. |
| `InterfaceChanged` | `(si)` | Event-manager signal, often emitted in addition to `Changed`. |
| `ConnectionModeChanged` | `(si)` | Event-manager signal, often emitted in addition to `Changed`. |
| `NetworkQualityChanged` | `(ssi)` | Message (including a source prefix), JSON details, counter. |

There is no introspection XML, properties interface, capability/status method, stable schema version, object-manager model, D-Bus policy, or system-bus service activation. The service does not implement standard peer/introspection methods itself.

### File side channel

`signal_changed.bin` and `get_reply.bin` are CWD-relative demonstration files. The encoding uses native host representation even though comments claim little-endian, has no magic/version/checksum/size limit/atomic replacement, and does not reject trailing bytes. `GetInterfaces` does not update the `Get` reply file, so the client offline API commonly observes a missing or irrelevant fixed greeting. This mechanism should not become the V2 metric store.

## Networking collectors

### Interface and default-route discovery

`SnapshotCollector` sends `RTM_GETLINK`, IPv4 `RTM_GETROUTE`, and IPv6 `RTM_GETROUTE` dumps and intersects UP/non-loopback links with routes whose destination prefix is zero and which contain both `RTA_OIF` and `RTA_GATEWAY`.

Risks:

- The socket is made nonblocking before a request is sent; an immediate `EAGAIN` ends a dump without polling for its response.
- Sequence numbers and sender PID are not validated; truncation and dump interruption are not handled.
- On-link defaults without `RTA_GATEWAY`, multipath routes, routing rules/tables, route metrics, and multiple routes per interface are mishandled or ignored.
- Route state is stored as a set, so deleting one of multiple defaults can remove the entire interface indication.
- Link deletion is inferred from flags/change masks rather than the netlink message type passed to the handler.
- Returned interface type is always `Unknown`, `isDefaultRoute` is always false, and addresses are not collected despite older documentation claims.

`UsingInterfaceManager` duplicates most of this parser, chooses the first element from unordered sets, prefers any IPv4 default, and does not consider route metric. Selection can therefore be nondeterministic. It does provide the valuable shape of a long-lived rtnetlink change listener.

### RTT probe

`NetPing` opens an IPv4 raw ICMP socket, binds it to an interface, sends one echo, waits with `select`, and calculates RTT from an embedded wall-clock timestamp.

Risks include CAP_NET_RAW/root requirements, IPv4-only behavior, global DNS resolution not bound to the observed interface, a data race on the static sequence, failure on the first unrelated ICMP packet instead of continuing until the matching reply, and sensitivity to wall-clock changes. RTT failure also changes `NetInfo::state` to `Down`, conflating target reachability with link state.

### TCP_INFO/sock_diag collector

The collector requests all IPv4 and IPv6 TCP sockets with `INET_DIAG_INFO`, filters on `idiag_if`, and aggregates `tcpi_total_retrans`. It constructs an “out segments” denominator from current `tcpi_unacked + tcpi_retrans + tcpi_sacked`, falling back to total link-layer TX packets only when that aggregate is zero.

This does not measure interval TCP loss correctly:

- `idiag_if` is usually a bound-device index, not necessarily the egress interface chosen by routing, so ordinary sockets may be excluded.
- `tcpi_total_retrans` is lifetime-per-live-socket; the aggregate can decrease when sockets close.
- unacked/retrans/SACK fields are gauges, not cumulative sent-segment counters, so delta arithmetic is invalid.
- the fallback mixes an interface-wide L2 cumulative denominator with a live-TCP retransmission numerator.
- interface changes do not reset `prevStats` in the monitor.
- `inSegs` is never actually derived.
- header comments promise `/proc/net/snmp` fallback, but no such implementation exists.

The sock_diag enumeration and parsing should feed a V2 `SocketTracker`, but the V1 loss percentage should not be preserved as authoritative.

### Wi-Fi RSSI

The client talks to a wpa_supplicant UNIX datagram control socket and parses `SIGNAL_POLL`. It may create control directories and execute wpa_supplicant through a constructed shell command.

All collected interfaces currently have `NetType::Unknown`, so the RSSI update loop skips every interface. If it were reached, the singleton reuses socket/path state without a mutex, reconnects without first closing a successful prior socket, and may interfere with system network management by auto-starting wpa_supplicant. V2 should prefer nl80211 where feasible and treat wpa_supplicant as a read-only fallback.

## Existing eBPF implementation

### Kernel program

`flow_rate.bpf.c` defines:

- an LRU hash `current_sec` keyed by IPv4 source/destination address, ports, and protocol;
- a one-entry `cfg_iface` hash map;
- a kprobe on `ip_queue_xmit` for outbound TCP;
- a kprobe on `udp_sendmsg` for outbound UDP;
- byte, packet, and current PID aggregation.

The program uses `vmlinux.h` and `BPF_CORE_READ`, so some field accesses use CO-RE relocations. That alone does not make the complete implementation portable.

### User-space loader

`NetTrafficAnalyzer` opens and loads the raw object with libbpf, writes the desired ifindex, auto-attaches programs, obtains the map FD, then takes two map snapshots around a sleep to calculate rates and top flows. It also stores per-flow history for heuristic anomalies.

### eBPF correctness and portability risks

- `__TARGET_ARCH_x86` is hard-coded.
- The build uses a host-generated, ignored `vmlinux.h` and does not generate a libbpf skeleton.
- Kprobe symbol/ABI availability is kernel-dependent; the comment names `__ip_queue_xmit` while the section attaches to `ip_queue_xmit`.
- At the selected TCP entry point `skb->dev` may be unset, making configured interface filtering drop valid traffic.
- UDP is explicitly not interface-filtered, so an analyzer “bound” to one interface still counts UDP across interfaces.
- Only outbound IPv4 is modeled; there is no receive path, IPv6, network namespace, direction, socket cookie, cgroup, UID, or stable process identity.
- PID attribution can be overwritten by later contexts and is not necessarily the originating application for retransmission/kernel work.
- Kernel/user key/value layouts are independently redeclared with no static layout assertions or versioning.
- Attachment/map state has no lock while multiple threads sample it.
- Links and the BPF object are never destroyed during normal service lifecycle.
- Degraded runtime returns zeros that are indistinguishable from genuine zero traffic.

## Network state and quality model

`NetInfo` combines topology, link state, active-route selection, RTT, RSSI, TCP loss, and traffic into a mutable interface record. It has no sample time, age, validity enum, error cause, namespace, ifindex, source, units type, or confidence.

The quality assessor weights RTT 30%, TCP loss 30%, RSSI 20%, and traffic heuristics 20%. Unknown RTT/TCP loss receive a neutral 50, but the RSSI sentinel `-1000` is treated as a genuine very weak signal because only `0` is considered unknown. This penalizes wired interfaces. Average packet size and flow count are also treated as quality signals without a validated causal basis. Hand-built JSON does not escape strings.

This scorer is deterministic and can supply candidate rules/tests, but it must be rebuilt around valid/unknown/stale semantics and evidence rather than promoted directly into the V2 root-cause engine.

## Event model

V1 already names `NetworkEvent` and includes type, message, wall-clock timestamp, source, JSON details, and priority. `NetworkEventManager` stores one callback vector per enum and forwards events to D-Bus.

It is not yet the target `EventBus`:

- no queue, capacity, delivery contract, subscriber token, replay, ordering rule, or overflow telemetry;
- registration, removal, iteration, context, and event counter are unsynchronized;
- unregister removes all callbacks of a type;
- priority and `monitoring_active_` do not affect behavior;
- callbacks execute inline and can invalidate their own vector;
- only interface and connection-mode default callbacks are registered;
- RTT/TCP/RSSI convenience events are not used by their monitors;
- a quality signal has a different payload signature from the other event signals.

## Client library and CLI

The shared library exposes a C ABI for initialization, interface listing, health, file access, Ping, generic changes, typed events, quality events, and version text. Internally, one libdbus connection is serialized by `ioMutex_`; signal queues are bounded to 128 elements under `queueMutex_`.

Useful properties are the small C ABI, bounded queues, fixed timeouts for method calls, and explicit buffer sizes. Risks and contract mismatches include:

- `g_client` initialization, cleanup, and use are not synchronized; concurrent cleanup can cause use-after-free.
- Failed initialization leaves a non-null disconnected client, preventing a later retry.
- “connected” only means a session-bus connection exists, not that WeakNet owns its name.
- generic event subscription ignores the supplied callback; unsubscribe is an unconditional no-op.
- network-quality subscription blocks in an infinite dispatch loop despite its name; its callback return semantics are the opposite of the header comment.
- the static quality callback is process-global and unsynchronized.
- repeated subscriptions can add duplicate match rules.
- generic event `source` is fabricated as `event_manager`, because source is not on the wire.
- `weaknet_get_event_types` omits `NetworkQualityChanged`, while documents advertise it.
- pointer/size arguments and truncation are not robustly validated.
- “no event” and operational errors share a boolean/error-buffer channel.
- client LOG calls occur without initializing the repository's logger wrapper.
- file reads are CWD-relative and not an IPC-safe offline cache.

`test-client` is a combined CLI and manual harness rather than the target `weaknetctl`. Several early-return paths skip cleanup, `main()` ignores test failures and returns zero, `test-lib` invokes an unsupported `lib-test` command, and the “all” path can wait forever for a quality event.

## Experimental AI and log-analysis code

The optional directory contains a log-capture subprocess, a static network knowledge dictionary, an interactive wrapper, a simple rules-plus-LLM analyzer, and three substantially duplicated FAISS/LangChain variants.

Positive boundary: none of this Python is linked into or imported by the C++ daemon/client.

Observed limitations:

- The input contract is emoji/localized presentation logs parsed by duplicated regular expressions, not a versioned event/incident schema.
- Exact `HH:MM:SS` matching loses related samples collected in different seconds and has no date, timezone, monotonic time, namespace, or incident ID.
- Interface regexes use `\w+`, excluding valid names containing characters such as `-` or `.`.
- `log_capture.py` uses a relative server `cwd` that does not resolve to the repository's server directory from the documented location; if corrected to the binary directory, V1's root-relative BPF/log paths would still break.
- Blocking `readline()` can prevent duration-based capture from stopping; capture threads and process cleanup are incomplete.
- Placeholder API keys are hard-coded and truthy, so tools try external initialization instead of cleanly selecting local mode.
- Dependencies are broad lower bounds, include unused packages, omit `langchain-community`, and have no lockfile.
- Some variants pass an OpenAI SDK client where LangChain expects a LangChain language-model object; one path may call FAISS with `embeddings=None`.
- The “local” hash embedding uses Python's randomized `hash()`, so saved vectors are not stable across processes.
- Multiple imports are unused, code is heavily copied, and no unit tests/evaluation set substantiate retrieval or diagnosis quality.
- The installer refers to a nonexistent `network_diagnosis_tool.py` and installs/upgrades packages globally unless an optional flag is used.
- Raw logs can contain sensitive network identifiers; there is no redaction policy or prompt-injection boundary.

The static knowledge and parser fixtures can inform a future evaluation corpus, but V2 AI should consume redacted structured incident bundles and remain advisory.

## Known technical debt

The most consequential debt is structural rather than cosmetic:

- one daemon translation unit coordinates construction, collectors, state mutation, D-Bus, and shutdown assumptions;
- raw owning pointers and process-lifetime singletons obscure resource ownership;
- two rtnetlink implementations duplicate parsing and can disagree about the active interface;
- collectors write directly into a shared mutable interface model and emit D-Bus signals directly;
- blocking I/O and sampling windows execute under the broad interface-state mutex;
- metric values lack validity, age, provenance, namespace, and typed-unit metadata;
- external and internal contracts are implicit constants/hand-built JSON rather than versioned schemas;
- build dependency discovery, generated BPF inputs, runtime paths, and optional-feature behavior are not reproducible;
- CLI, smoke tests, demonstrations, and benchmarks are conflated;
- documentation contains historical success and portability claims that are stronger than current executable evidence;
- experimental AI code copies parsers and analysis logic instead of sharing a stable structured input contract.

This debt should be paid down in the roadmap order. Fixing individual races while retaining detached ownership, or adding diagnosis rules before metric validity is established, would preserve the underlying failure modes.

## Correctness risks

Highest-priority observed correctness risks are:

1. The TCP “loss rate” denominator and aggregated lifetime retransmission deltas do not represent sent/retransmitted segments over an interval.
2. Nonblocking rtnetlink dumps can terminate on `EAGAIN` before receiving data, producing false empty-interface snapshots.
3. Interface-scoped sock_diag filtering via `idiag_if` omits many normally routed sockets.
4. Interface types never leave `Unknown`, disabling RSSI, while `-1000` RSSI is scored as real degradation.
5. Traffic is initialized on hard-coded `eth0`; UDP ignores the interface filter.
6. Interface churn replaces all accumulated metrics with new default `NetInfo` objects.
7. Raw Ping treats an unrelated first ICMP response as failure and blocks all D-Bus request dispatch.
8. Network quality can look precise even when every source is missing/degraded.
9. File persistence is racy, native-endian, non-atomic, unversioned, and semantically disconnected from `GetInterfaces`.
10. Client subscriptions, callbacks, event-type reporting, and unsubscribe behavior disagree with their public documentation.

## Concurrency and lifecycle risks

- Detached long-lived threads retain pointers to stack-owned `ServerContext`.
- Joinable members have no reachable join path and would call `std::terminate` if the context unwound.
- No signal handling or stop propagation exists.
- Detached signal threads are unbounded and can outlive service/context objects.
- Event callbacks and counter increments race across producers.
- The central state mutex is held during seconds of network I/O and traffic sampling.
- The BPF analyzer is sampled concurrently by inner and outer traffic loops without a sampling lock.
- Singleton implementation objects and resources are intentionally leaked or never stopped.
- Client global lifetime, callback lifetime, and cleanup are unsafe under concurrent use.
- An exception escaping the interface, uplink, TCP-loss, or quality thread terminates the process.

## Portability risks

- Linux-only headers and facilities are fundamental, which is appropriate, but non-Linux stubs are incomplete and create a misleading cross-platform impression.
- Hard-coded x86 BPF target; host-specific `vmlinux.h`; kernel-symbol kprobes.
- Default interface `eth0`, external probe `223.5.5.5`, and Alibaba/RHEL-focused assumptions.
- Session-bus availability and CWD-dependent paths.
- Raw-socket/BPF privileges vary across kernel versions and container policies.
- wpa_supplicant paths and `/sbin` assumptions do not cover NetworkManager/iwd/nl80211 environments.
- Native-endian serialization and struct-layout duplication.
- `inet_ntoa`, `std::localtime`, and glog/pkg-config behavior vary by runtime/toolchain.
- No explicit minimum kernel/libbpf/compiler matrix.

## Security and operational risks

- A locally callable Ping method accepts arbitrary hostnames and performs privileged raw-network work synchronously.
- `system()` is used to start wpa_supplicant with strings containing interface/configuration data.
- No system-bus policy, caller authorization, rate limiting, or resource quota exists.
- Logs and optional AI inputs may disclose interfaces, addresses, ports, PIDs, and destinations.
- No systemd sandbox, capability minimization, privilege drop, watchdog, health status, or bounded shutdown.
- The external model path has no redaction, consent, retention, timeout/retry budget, or output-trust policy.

## Test coverage and missing tests

There is no automated unit-test target and no measured coverage. The existing CLI checks require a live service and often the public Internet. They do not isolate the host network, assert kernel events, or reliably fail the process.

Missing test groups include:

- serialization bounds, truncation, endianness, malformed files, and atomic replacement;
- netlink multipart parsing, `NLMSG_ERROR`, dump interruption, route metrics, multipath, on-link routes, link deletion, and interface-name edge cases;
- socket identity, interface attribution, counter resets, socket close/open churn, TCP_INFO field availability, IPv4/IPv6, and namespace separation;
- raw Ping matching, timeout, privilege errors, DNS failure, clock behavior, and concurrency;
- BPF load/attach degradation, map ABI layout, IPv6, ingress/egress, interface filtering, namespace identity, lost events, and cleanup;
- event ordering, unsubscribe, callback reentrancy, saturation, drops, and multi-producer races;
- metric validity/staleness and quality calculations with missing data;
- deterministic incident and root-cause scenarios (not present in V1);
- D-Bus signatures, errors, introspection, service disappearance/restart, timeouts, and authorization;
- C ABI null pointers, zero/small buffers, truncation, retry, and concurrent init/cleanup;
- graceful SIGTERM, partial startup failure, collector failure, shutdown deadlines, and leak checks;
- namespace/netem lab scenarios and cleanup;
- optional AI parser, retrieval, groundedness, redaction, injection resistance, offline mode, and external failure;
- reproducible microbenchmarks and end-to-end resource/latency measurements.

No current timing printed by `test-client` should be used as a V2 baseline result. It can only motivate a future benchmark definition.

## Reusable V1 components

“Reusable” means preserve and harden incrementally, not copy unchanged.

| Component/concept | Reuse path |
|---|---|
| rtnetlink message parsing and default-route intent | Consolidate into one owned `NetlinkCollector` with blocking poll/epoll, sequence validation, reconciliation, and fixture tests. |
| sock_diag and `INET_DIAG_INFO` request/parsing | Move into `SocketTracker`; define stable identity and valid per-socket counter deltas. |
| raw-ICMP interface binding | Put behind an optional active-probe collector with correct reply loop, timestamps, privileges, IPv6, and deterministic tests. |
| wpa_supplicant `SIGNAL_POLL` parser | Keep only as a read-only fallback behind a Wi-Fi collector; remove process management. |
| libbpf open/load/map concepts and flow key vocabulary | Replace build/attach ABI around a generated skeleton and shared versioned types while preserving observable intent. |
| `NetInfo` vocabulary | Split topology/status from timestamped metric samples; preserve familiar names through adapters. |
| deterministic quality thresholds | Convert into validity-aware rules and tests; treat them as one signal, not root cause. |
| `NetworkEvent` name and event-type vocabulary | Evolve into the unified versioned event schema. |
| C client ABI | Maintain as a compatibility library over the versioned D-Bus API until an explicit deprecation decision. |
| bounded client-side queues | Preserve the bounded-resource principle and add drop/error telemetry. |
| glog wrapper intent | Keep a centralized logging facade, adding structured identifiers and safe initialization. |
| static AI knowledge and sample log cases | Curate into reviewed knowledge/evaluation fixtures after removing claims not backed by evidence. |

## Components that should be replaced or retired

| V1 component | Replacement direction |
|---|---|
| `ServerContext` raw pointers plus mixed detached/joinable orchestration | RAII `weaknetd` application composition with owned `std::jthread`s and stop tokens. |
| Duplicate snapshot/listener netlink implementations | One reusable netlink transport/parser and topology/uplink state machine. |
| `WeakNetMgr` as a mutex around all I/O and state | Collectors produce events outside locks; `EventBus` and `MetricStore` own delivery/state. |
| Direct monitor-to-D-Bus calls and detached send threads | Publish `NetworkEvent`; a D-Bus adapter consumes stable snapshots/events. |
| V1 TCP-loss percentage | SocketTracker-derived retransmission metrics with explicit scope/validity, tested against controlled flows. |
| Hard-coded `eth0` traffic analyzer and current kprobe pair | Portable libbpf CO-RE observability selected by capability, with tested attribution and graceful fallback. |
| Ad-hoc `.bin` side channel | Versioned MetricStore snapshots/export format with atomic writes only if persistence is required. |
| Hand-built JSON strings | A tested serializer with schema/version and correct escaping. |
| `NetworkEventManager` callback vectors | Bounded, thread-safe `EventBus` with subscription tokens and defined ordering/backpressure. |
| `test-client` as test framework/benchmark | `weaknetctl` plus real unit, D-Bus integration, lab, and benchmark targets. |
| Host-mutating `install.sh` as build/test authority | CMake presets/toolchain documentation, package/install targets, and scoped lab scripts. |
| Python parsing of human logs | Structured incident bundle export; one optional diagnostic/evaluation package. |
| Duplicated RAG variants and hard-coded model configuration | One optional provider-neutral agent with offline tests, redaction, citations, and measured evaluation. |

## V1-to-V2 gap summary

| Target V2 element | V1 status |
|---|---|
| `weaknetd` | Prototype exists under a different name; lifecycle and boundaries need extraction. |
| `weaknetctl` | `test-client` offers commands but mixes tests and CLI behavior. |
| eBPF observability | Experimental outbound IPv4 counters exist; CO-RE delivery and attribution incomplete. |
| `SocketTracker` | Sock-diag parsing exists; no stable tracker/history/valid delta model. |
| Unified `NetworkEvent` | Name exists; schema and bus semantics incomplete. |
| `EventBus` | Not present. |
| `MetricStore` | Not present; mutable latest interface vector only. |
| `IncidentEngine` | Not present. |
| `RootCauseEngine` | Not present; only a quality score and traffic heuristics. |
| WeakNet Lab | Not present. |
| Optional LLM agent | Experimental scripts exist but no stable input contract or evaluation. |
| Evaluation/benchmarks | Not present; manual examples are not evidence. |
