# Phase 2 runtime lifecycle

## Implemented boundary

Phase 2 makes the existing V1 daemon lifecycle owned and stoppable without changing collector meaning. `DaemonApplication` owns the D-Bus service, manager, signal waiter, and all monitor workers. The retained `ServerContext` is only a non-owning view whose lifetime is bounded by the application.

The daemon still uses the V1 session-bus endpoint, state model, netlink parsers, TCP calculations, eBPF hooks/maps, quality rules, and C ABI. This phase does not introduce `NetworkEvent`, EventBus, MetricStore, SocketTracker, incident logic, root-cause logic, or the production system-bus API.

## Startup and rollback

Required startup resources are acquired in this order:

1. validated runtime directories and logging;
2. blocked SIGINT/SIGTERM mask and owned signal waiter;
3. core manager state and owned helper resources;
4. session-bus connection, well-known name, service object, and object path;
5. V1 event forwarding;
6. owned monitor workers.

A required failure calls the same idempotent reverse-order `stop()` path used by normal shutdown. Optional collector failures are recorded in `RuntimeHealth` as degraded and do not prevent D-Bus service startup.

## Shutdown contract

Normal SIGINT/SIGTERM shutdown has a five-second deadline on the supported runtime path. The signal waiter uses `sigtimedwait`; no asynchronous signal handler calls C++ or libdbus. Periodic waits and traffic sampling windows are stop-aware, D-Bus dispatch polls at a finite interval, sock_diag receives have finite timeouts, and workers are joined in reverse startup order.

No long-lived production thread is detached. `stop()` is idempotent.

Shutdown releases, in dependency order:

- periodic monitor workers;
- the current-uplink listener and traffic sampler;
- queued/active Ping work;
- event forwarding;
- D-Bus object registration, name, and connection reference;
- BPF links before the BPF object;
- owned descriptors and Wi-Fi local socket path;
- logging and the temporary signal mask.

## Bounded D-Bus Ping

The V1 `Ping(s) -> s` contract is unchanged, but execution no longer occurs in the sole D-Bus dispatch context. A single owned executor accepts at most eight queued requests and reports `com.example.WeakNet.Error.Busy` on overflow.

The raw V1 Ping operation runs in the private `libexec/weaknet/weaknet-ping-helper`. This helper is required because libc hostname resolution through `getaddrinfo()` has no reliable cooperative cancellation contract. The daemon can therefore kill and reap an active helper on timeout or shutdown without detaching a thread. The helper calls the existing `NetPing` implementation and preserves its success/failure result formatting; it does not change probe matching, IPv4 behavior, or interpretation.

## Runtime paths

The daemon resolves writable paths without consulting its current working directory:

| Variable | Default |
|---|---|
| `WEAKNET_STATE_DIR` | `$XDG_STATE_HOME/weaknet`, then `$HOME/.local/state/weaknet`, then a per-UID `/tmp` fallback |
| `WEAKNET_LOG_DIR` | `$WEAKNET_STATE_DIR/log` |
| `WEAKNET_RUNTIME_DIR` | `$XDG_RUNTIME_DIR/weaknet`, then a per-UID `/tmp` fallback |
| `WEAKNET_BPF_OBJECT` | executable/install-relative object lookup when unset |

`signal_changed.bin` and `get_reply.bin` retain their V1 binary encoding and now live under the state directory. The compatibility client uses the same state-directory resolution. The Wi-Fi control client uses the runtime directory for its owned local socket.

All configured paths must be absolute. Required directories are created and validated during startup.

## Runtime health compatibility

`RuntimeHealth` is a direct mutex-protected registry with `starting`, `running`, `disabled`, `degraded`, `failed`, and `stopped` states. It is not an event bus or metric store.

The existing `HealthCheck` D-Bus method keeps its `() -> (s)` signature and existing network-quality JSON fields. It adds one `runtime_health` JSON member containing overall and per-component lifecycle status.

## Known later-phase debt

Phase 2 intentionally leaves these semantics unchanged:

- duplicated/incomplete rtnetlink parsing and uplink policy;
- TCP-loss denominator, interface attribution, and socket-generation behavior;
- eBPF hooks, map schema, IPv4-only behavior, and attribution;
- Ping reply matching, IPv6 support, and target-reachability interpretation;
- Wi-Fi discovery and the dormant wpa_supplicant auto-start path;
- validity-aware quality scoring;
- the V1 event manager and persisted binary format;
- system-bus policy, authorization, and the production API.

## Sanitizers

Configure project-code sanitizers with:

```bash
-DWEAKNET_SANITIZER=address-undefined
-DWEAKNET_SANITIZER=thread
```

ASan/UBSan is a Phase 2 release gate. TSan is also built and run where supported; an environment-level TSan startup failure must be recorded as a skip rather than suppressed or treated as a pass.

At the Phase 2 implementation checkpoint, GCC 13.3 eBPF-off/on, Clang 18.1 eBPF-on, tests-disabled, and GCC ASan/UBSan configurations pass. GCC TSan binaries build, but execution is skipped on the Ubuntu 24.04.5 WSL2 host (kernel `6.18.33.2-microsoft-standard-WSL2`, glibc 2.39) because the TSan runtime aborts before test code with `ThreadSanitizer: unexpected memory mapping`. No suppression is used and this skip is not recorded as a pass.
