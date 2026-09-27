# V2 Wi-Fi evidence

`WifiCollector` is the first V2 Wi-Fi evidence stage. It is an optional,
read-only collector owned by `DaemonApplication`. It does not diagnose a root
cause, open incidents, configure wireless state, or replace the legacy V1 RSSI
compatibility path.

## Architecture and identity

The collector receives the authoritative selected interface from the Phase 4
topology collector and queries Linux Generic Netlink `nl80211`. It resolves the
`nl80211` family through the Generic Netlink control family; no family ID is
hardcoded. The transport uses an owned nonblocking `AF_NETLINK` /
`NETLINK_GENERIC` socket, explicit request sequences, kernel-sender checks,
bounded poll/receive, multipart completion, `NLMSG_ERROR`, truncation, and
malformed-attribute handling.

The target identity is `NetnsId + ifindex`. Interface names are display
metadata only. A topology/interface change replaces the target and its
generation; observations from the previous ifindex are not carried forward.
Non-authoritative topology produces a `NoTarget` observation with no fabricated
interface or signal. An Ethernet or unsupported interface produces `NotWifi`.
The collector does not rediscover interfaces by name or shell out to `iw`,
`iwconfig`, or `nmcli`.

## Typed observation

`WifiObservation` is published as `EventKind::WifiObservation` from
`EventSource::WifiCollector`. It includes namespace, optional current
`InterfaceId`, interface generation, monotonic and realtime timestamps,
validity, capability/status, and optional fields:

- station/client interface type;
- associated/not-associated/unknown link state;
- binary BSSID;
- length-delimited SSID bytes;
- frequency in MHz when exposed;
- signal and average signal in signed dBm;
- TX/RX bitrate in kbps;
- TX retries, TX failures, RX/TX packets, and RX/TX bytes when the kernel
  exposes trustworthy attributes.

Absent attributes remain absent. A missing signal is not `0 dBm`, a missing
bitrate is not zero, and an unavailable counter is not zero. Signal values are
decoded as signed 8-bit dBm, so a kernel value representing `-61 dBm` remains
`-61`, not unsigned `195`.

Bitrate attributes use the nl80211 100-kbit/s unit and are converted exactly to
`bitrate_kbps`; the 32-bit attribute is preferred over the legacy 16-bit
attribute. No MCS, channel-width, throughput, retry percentage, or packet-loss
rate is derived.

## Association and capability semantics

Association is established from a station entry for a station-mode interface.
No station entry is `NotAssociated`; a permission, transport, malformed-reply,
or unsupported query remains `Unknown`/unavailable and is never rewritten as
disconnected. A selected interface being up, having an address, or having a
route does not prove association or Internet health.

Capability states distinguish `Available`, `NotWifi`, `NotAssociated`,
`Unsupported`, `PermissionDenied`, `TransportUnavailable`, `Error`, and
`NoTarget`. Missing nl80211 capability is not Wi-Fi disconnection. Partial
observations are valid typed evidence but are not healthy/unhealthy claims.

## Lifecycle and freshness

One stop-aware worker polls at a bounded five-second default interval. A target
change wakes the worker for a prompt refresh. The observation timestamp uses
the injected monotonic clock for freshness and realtime only for display. The
default freshness policy is ten seconds; there is no wall-clock comparison,
sub-second polling, or per-query thread. Failure to initialize or resolve
nl80211 degrades this optional component while the daemon continues.

Periodic polling is intentional for this first stage. Multicast association
notifications and a broader wireless framework are future work.

## Interpretation boundaries

RSSI is radio signal evidence, not network health. Low RSSI does not prove
interference, a local-link fault, or packet loss. A retry counter is a kernel
station statistic, not a packet-loss percentage. Not associated is not an
Internet outage without later topology/context correlation. RootCauseEngine
does not consume these fields in this stage; a later additive stage may use
them conservatively without changing these meanings.

The collector operates only in the daemon's current network namespace, like
the existing Phase 4 topology collector. It does not scan, discover APs,
roam, reconnect, control NetworkManager/wpa_supplicant, change regulatory or
power settings, or handle credentials.

## Compatibility and testing

The V1 wpa_supplicant/RSSI implementation remains source-compatible and is not
made authoritative for V2. Deterministic parser and collector tests use
synthetic Generic Netlink/nl80211 messages, including family resolution,
negative signal, exact bitrate conversion, optional counters, malformed
attributes, sequence/sender/error handling, station absence, target changes,
and non-authoritative topology. No live Wi-Fi device or host network mutation
is required.
