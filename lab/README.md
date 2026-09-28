# WeakNet Lab

WeakNet Lab is the Stage A reproducible-scenario and evaluation harness for the
real deterministic WeakNet stack. It creates two isolated Linux network
namespaces, starts the built `weaknet-dbus-server`, generates local traffic,
applies a bounded fault only to lab-owned interfaces, reads the structured
D-Bus diagnosis, and writes a machine-readable evaluation artifact.

The lab never injects incidents or root-cause hypotheses. Incident and
hypothesis results come from the production C++ engines.

## Safety model

- Namespace names are generated as `wnlab-c-<token>` and
  `wnlab-s-<token>`.
- Veth names are generated as `wnc-<token>` and `wns-<token>`.
- Faults are applied only inside those namespaces and only to those veths.
- The host default route and host qdiscs are never modified.
- Commands use bounded timeouts and record owned process PID/start-time pairs.
- Cleanup validates ownership names before removing anything.
- If cleanup privilege is unavailable while resources still exist, state is
  retained for a later privileged cleanup retry.
- Generated state and artifacts live under `lab/.state/` and `lab/output/`.

Do not rename state entries or manually point them at non-lab resources. The
cleanup code intentionally rejects names outside the ownership pattern.

## Prerequisites

Run from the repository root after building the C++ targets and creating the
repository Python environment:

```bash
./lab/weaknet-lab doctor
```

Live scenarios require Linux, `ip`, `tc`, `dbus-daemon`, the built
`weaknet-dbus-server` and `weaknetctl`, `.venv` with `dbus-next`, and one of:

- root;
- effective `CAP_NET_ADMIN` plus `CAP_SYS_ADMIN`; or
- passwordless `sudo -n`.

If a required capability is unavailable, `run` writes a `SKIP` evaluation
with the exact missing capabilities and performs no network mutation. `doctor`
reports only boolean configuration state; it never prints credentials.

The default binary search paths are `build/gcc-no-ebpf/bin` and `build/bin`.
Set `WEAKNET_LAB_BUILD_DIR` to another build tree when needed.

## Commands

```bash
./lab/weaknet-lab list
./lab/weaknet-lab doctor
./lab/weaknet-lab run healthy --no-ai
./lab/weaknet-lab run high-rtt --no-ai
./lab/weaknet-lab up uplink-unavailable
./lab/weaknet-lab status
./lab/weaknet-lab down
./lab/cleanup.sh
```

`run` always attempts scoped cleanup after pass, failure, or timeout. `up`
leaves one scenario active for inspection; use `status` and then `down`.
Only one active lab is permitted, so serialized behavior is explicit.

## Scenarios

| ID | Real setup/fault | Expected deterministic behavior |
|---|---|---|
| `healthy` | Local TCP echo, no injected fault | No incident or hypothesis |
| `high-rtt` | 300 ms server-egress netem delay | `HighTcpRtt` and `NetworkPathDegradation`, then recovery |
| `retransmission` | 25% server-egress netem loss | `ElevatedTcpRetransmission` and `NetworkPathDegradation` |
| `uplink-unavailable` | Remove only the client namespace default route | `UplinkUnavailable` and `UplinkAvailabilityProblem`, then recovery |
| `observability-gap` | eBPF-off daemon with unavailable physical Wi-Fi telemetry | `Degraded` or `Unknown`, without fabricated incidents or hypotheses |

Scenario manifests are versioned JSON files in `lab/scenarios/`. They declare
required capabilities, workload, bounded fault parameters, time windows,
expected incidents, expected/allowed hypothesis types, status constraints, and
recovery expectations.

## Evaluation artifacts

Each `run` creates `lab/output/<run-id>/evaluation.json` using
`weaknet.lab.evaluation.v1`. It records:

- setup `pass`/`skip`, final `PASS`/`FAIL`/`SKIP`;
- expected, observed, hit, and missing incidents/hypotheses;
- unexpected authoritative hypothesis escalation;
- detection and recovery latency;
- optional AI schema/grounding results when explicitly requested.

The schema reference is `lab/expected/evaluation.schema.json`. CLI captures,
daemon logs, and optional structured AI reports are stored beside the
evaluation for review. Generated artifacts are evidence for that run only;
the repository does not publish invented performance numbers.

## Optional AI checks

AI is disabled by default. `run <scenario>` is equivalent to `--no-ai`.
Explicit modes are:

```bash
./lab/weaknet-lab run high-rtt --explain
./lab/weaknet-lab run high-rtt --advise
```

These modes require an explicitly configured provider environment. The lab
does not source `.env.local`, print secrets, or make a provider call during
normal tests. The deterministic `weaknetctl diagnose` output is captured
first. Structured AI reports are then checked for schema success, unchanged
deterministic status/type/confidence, grounding, citations, and unchanged CLI
exit behavior.

For an offline UI/example path, use:

```bash
./lab/weaknet-lab fixture-demo --mode advise
```

This command is always labeled `SIMULATION`; it uses a canonical fixture and
the fake provider, not Linux telemetry.

## Tests

Offline tests make no network changes and no external provider calls:

```bash
.venv/bin/python -m unittest -v lab.tests.test_lab
```

The real healthy-stack integration test is opt-in and will otherwise skip:

```bash
WEAKNET_RUN_LAB_INTEGRATION=1 \
  .venv/bin/python -m unittest -v \
  lab.tests.test_lab.PrivilegedIntegrationTests
```

Run it only on a disposable or approved Linux host with the prerequisites
reported ready by `doctor`.
