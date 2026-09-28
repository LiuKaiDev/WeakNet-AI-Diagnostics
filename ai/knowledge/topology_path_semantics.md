# Topology and path availability semantics

## Uplink availability

`UplinkAvailabilityProblem` represents authoritative topology evidence that no
usable uplink is currently available. Unavailable is not healthy and is not a
claim that an ISP or remote service is down. Relevant observations include the
selected-uplink state, current link/address availability, route context, and
the topology generation that produced the diagnosis.

The next checks are to obtain a fresh topology snapshot, verify current link
and address state, and re-evaluate the selected-uplink and route context. A
missing or unavailable observation remains a capability limitation; it must not
be converted into a healthy value or treated as proof of another failure.

## Local routing and modeled paths

`LocalRoutingProblem` is reserved for route-unavailable or socket-route-conflict
evidence. A modeled route is not a guaranteed kernel route, and a diagnostic
interface index is not proof of routed egress. Route table, destination match,
namespace, generation, and ambiguity context are useful observations for
resolving uncertainty.

## Network path degradation

`NetworkPathDegradation` is a socket-scoped path hypothesis supported by
observations such as high TCP RTT or elevated retransmission. Retransmission is
not packet loss. Fresh gateway and configured-remote probes help distinguish
local gateway context from degradation that appears beyond it.

## Remote or upstream attribution

`RemoteOrUpstreamDegradation` means degradation appears beyond the immediate
local gateway under the deterministic probe rules. Gateway reachability does
not prove Internet health, and this hypothesis does not confirm ISP congestion
or a remote-server fault. Fresh target-scoped probes and their namespace,
address, and timestamp provenance are required for further attribution.
