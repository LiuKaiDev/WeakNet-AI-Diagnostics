# Root cause semantics

## Local link and insufficient evidence

`LocalLinkSuspected` is a deterministic hypothesis based on observed local-path evidence. It is suspected, not confirmation of interference or access-point failure. `InsufficientEvidence` means that observations needed to resolve uncertainty are unavailable; missing evidence is not proof of a failure.

## Remote and routing boundaries

`RemoteOrUpstreamDegradation` describes evidence attributed beyond the local link. Gateway reachability or latency does not prove an ISP failure. A modeled route is not a guaranteed kernel route, and an interface index is not proof of egress.

## TCP interpretation

High TCP RTT and retransmission evidence are distinct observations. Retransmission is not equivalent to packet loss. Retrieval should use these terms to find interpretation and next-observation guidance, not to manufacture a root cause.
