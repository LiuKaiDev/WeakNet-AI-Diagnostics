# Phase 5.1/5.2 socket identity, lifecycle, and inventory

## Phase 5.1 scope

Phase 5.1 defines the in-process V2 data model used by future socket
observation work. It does not open `NETLINK_SOCK_DIAG`, request `TCP_INFO`,
sample live sockets, calculate retransmission/RTT deltas, or replace the V1
TCP monitor.

The implementation is `SocketLifecycleTable` in
`server/include/socket_tracker.hpp`. It is a deterministic, bounded model with
explicit snapshot boundaries and no worker thread or kernel dependency.

## Identity

A four-tuple (local/remote address and port) is descriptive metadata, not a
persistent identity. `SocketTuple` includes the namespace, TCP protocol,
address family, and typed IPv4/IPv6 endpoints. Endpoint addresses remain raw
bytes; string conversion is diagnostic only.

`SocketId` contains:

- `NetnsId`, so equal cookies or tuples in different namespaces cannot match;
- an optional `KernelSocketCookie` when Linux supplies one; cookie value zero
  is still an available cookie, not an unavailable sentinel;
- a daemon-local `SocketGeneration` allocated monotonically from one.

Generation values are process-local and are not persisted across daemon
restarts. The generation is the final discriminator when a tuple is reused,
when cookie availability changes, or when a cookie is observed with
incompatible metadata.

## Reconciliation boundaries

The table is driven as:

```text
beginSnapshot(disposition)
    -> observe(tuple, optional cookie, timestamps)
    -> commitSnapshot() or abortSnapshot()
```

For an authoritative snapshot, active identities observed in the candidate
remain active. Previously active identities absent from the complete candidate
are closed and removed from the active table. Their bounded closed records are
retained only for recent lifecycle/history inspection; they do not cause a
future observation to reuse the identity.

Partial and failed snapshots discard their candidate observations and preserve
all active lifecycle state. Absence in an incomplete or failed observation is
never interpreted as a close. An explicit `close(SocketId)` is an authoritative
removal indication and closes only that exact identity.

## Cookie and tuple behavior

When a cookie is available, namespace plus cookie is the strongest anchor. A
matching tuple reuses the active identity. If the same active cookie appears
with incompatible tuple metadata, the table conservatively allocates a new
generation and reports `inconsistent_metadata`; it does not silently mutate
the old identity.

When a cookie is unavailable, an active tuple may match only within the current
live lifecycle. A complete authoritative disappearance terminates that weak
identity. Reappearance of the same tuple receives a new generation. A
cookie-backed identity is never guessed from a cookie-free observation.

Cookie reuse after authoritative disappearance also receives a new generation.
This intentionally favors an identity reset over unsafe history merging.

## Bounds and events

The active table and recently closed history have explicit configurable bounds
(`4096` active and `1024` closed entries by default). Closed records are
discarded oldest-first once the history bound is reached. No persistent cache or
global singleton is used.

`SocketObservation` is an additive Phase 3 event payload containing the resolved
`SocketId`, tuple, namespace, timestamps, source, validity, TCP-state metadata,
and minimal active/closed lifecycle state. Phase 5.1 alone emits no live socket
events; Phase 5.2 publishes only committed authoritative changes.

## Phase 5.2 NETLINK_SOCK_DIAG inventory

`SocketTracker` owns one nonblocking `AF_NETLINK`/`NETLINK_SOCK_DIAG` socket in
the daemon's current network namespace. It requests all TCP states for both
IPv4 and IPv6 and validates kernel sender PID 0, request sequence, multipart
completion, errors, truncation, interruption, and bounded receive deadlines.
`EAGAIN` is only a wait condition and never an empty-table result.

`SocketDiagParser` extracts binary endpoints, network-order ports, Linux TCP
state, `idiag_if`, and the two-word kernel cookie. Linux stores the native
low 32 bits in word 0 and high 32 bits in word 1, so they are combined as
`word0 | (word1 << 32)` without byte-order conversion. Two
`INET_DIAG_NOCOOKIE` words mean unavailable while
zero remains valid. Unknown attributes are ignored; malformed known attributes
reject the candidate. TCP state is metadata only. `idiag_if` is retained as
`diag_ifindex` evidence, not routed egress or selected uplink attribution.

IPv4 and IPv6 dumps are transactional: both must complete before observations
are passed to the lifecycle table and committed as one authoritative snapshot.
Failure or partial visibility aborts the candidate and preserves the
last-known-good active state. Events are emitted only after commit. Startup
transport failure is degraded but optional to the daemon; post-start dump
failures keep the worker alive and use bounded recovery backoff. Active
capacity exhaustion rejects the entire candidate rather than committing a
partial table. The one socket observes only its creation namespace; traversal
of other namespaces is future work. Inventory cadence is five seconds and is
not a TCP metric cadence.

## Deferred work

Phase 5.3 handles TCP_INFO field semantics/version availability, same-identity
interval deltas, and retransmission/RTT metric definitions. This phase does
not calculate TCP metrics, parse TCP_INFO deeply, attribute routed egress,
attribute processes/cgroups, correlate eBPF, traverse namespaces, or replace
the V1 TCP monitor.
