# Phase 5.1 socket identity and lifecycle model

## Scope

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
`SocketId`, tuple, namespace, timestamps, source, validity, and minimal
active/closed lifecycle state. Phase 5.1 emits no live socket events because no
live collector exists yet; it only establishes the copyable typed payload for
future `SocketTracker` integration.

## Deferred work

The following remain later Phase 5 work: sock_diag transport and
`INET_DIAG_INFO` parsing, TCP_INFO sampling, socket state collection,
interface/process/cgroup attribution, compatible counter deltas, retransmission
and RTT metrics, eBPF correlation, and V1 TCP migration.
