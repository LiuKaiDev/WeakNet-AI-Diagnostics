"""Small deterministic AI V2 diagnosis fixtures; no model output is encoded."""

from __future__ import annotations

from ai.v2.schemas.diagnosis import DIAGNOSIS_SCHEMA_VERSION, DiagnosisSnapshot


def _snapshot(hypothesis: dict, limitations: list[str] | None = None) -> DiagnosisSnapshot:
    return DiagnosisSnapshot.from_dict({
        "schema_version": DIAGNOSIS_SCHEMA_VERSION,
        "snapshot_timestamp_ms": 1700000000000,
        "status": "Degraded",
        "limitations": limitations or [],
        "topology": {"authoritative": True, "uplink_ifindex": 3},
        "incidents": [
            {"id": "incident:tcp:1", "type": "HighTcpRtt", "state": "Active",
             "severity": "Warning", "scope": {"netns": {"device": 1, "inode": 2}, "generation": 1},
             "opened_at_ms": 1699999999000, "updated_at_ms": 1700000000000,
             "evidence": [{"kind": "HighTcpRtt", "value": 250000, "unit": "Microseconds"}]},
        ],
        "hypotheses": [hypothesis],
    })


def local_link_suspected() -> DiagnosisSnapshot:
    return _snapshot({
        "hypothesis_id": "hypothesis:local:1", "type": "LocalLinkSuspected",
        "state": "Active", "confidence": "Medium", "scope": {"ifindex": 3},
        "opened_at_ms": 1699999999000, "updated_at_ms": 1700000000000, "occurrence": 1,
        "supporting_evidence": [
            {"kind": "HighTcpRtt"}, {"kind": "GatewayProbeHighRtt"},
            {"kind": "WifiSignalVeryWeak"},
        ],
        "contradicting_evidence": [],
        "missing_evidence": [{"kind": "GatewayDeviceHealthUnavailable"}, {"kind": "WifiRfEvidenceUnavailable"}],
    }, ["AP/RF health is not directly observed"])


def remote_upstream_degradation() -> DiagnosisSnapshot:
    return _snapshot({
        "hypothesis_id": "hypothesis:remote:1", "type": "RemoteOrUpstreamDegradation",
        "state": "Active", "confidence": "Medium", "scope": {"ifindex": 3},
        "opened_at_ms": 1699999999000, "updated_at_ms": 1700000000000, "occurrence": 1,
        "supporting_evidence": [
            {"kind": "HighTcpRtt"}, {"kind": "GatewayProbeReachable"},
            {"kind": "RemoteProbeHighRtt"}, {"kind": "WifiAssociated"},
            {"kind": "WifiSignalNormal"},
        ],
        "contradicting_evidence": [], "missing_evidence": [],
    })


def insufficient_evidence() -> DiagnosisSnapshot:
    return _snapshot({
        "hypothesis_id": "hypothesis:insufficient:1", "type": "InsufficientEvidence",
        "state": "Active", "confidence": "Low", "scope": {"ifindex": 3},
        "opened_at_ms": 1699999999000, "updated_at_ms": 1700000000000, "occurrence": 1,
        "supporting_evidence": [{"kind": "HighTcpRtt"}],
        "contradicting_evidence": [],
        "missing_evidence": [
            {"kind": "GatewayProbeUnavailable"}, {"kind": "WifiEvidenceUnavailable"},
        ],
    }, ["Gateway and Wi-Fi telemetry unavailable"])

