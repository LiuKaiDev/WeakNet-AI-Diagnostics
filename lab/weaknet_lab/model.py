"""Scenario contracts, capability reporting, and semantic evaluation."""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
from typing import Any, Callable, Mapping


SCENARIO_SCHEMA_VERSION = "weaknet.lab.scenario.v1"
EVALUATION_SCHEMA_VERSION = "weaknet.lab.evaluation.v1"
SUPPORTED_FAULTS = {"none", "netem-delay", "netem-loss", "remove-default-route"}
SUPPORTED_WORKLOADS = {"none", "tcp-echo"}
AI_MODES = {"none", "explain", "advise"}


def _strings(value: Any, name: str) -> tuple[str, ...]:
    if not isinstance(value, list) or any(not isinstance(item, str) or not item for item in value):
        raise ValueError(f"{name} must be a list of non-empty strings")
    return tuple(value)


@dataclass(frozen=True)
class Scenario:
    schema_version: str
    scenario_id: str
    title: str
    purpose: str
    required_capabilities: tuple[str, ...]
    workload: str
    fault: str
    fault_options: Mapping[str, Any]
    observation_timeout_seconds: int
    warmup_seconds: int
    expected_incidents: tuple[str, ...]
    expected_hypothesis_types: tuple[str, ...]
    allowed_hypothesis_types: tuple[str, ...]
    allowed_statuses: tuple[str, ...]
    recovery: bool
    recovery_timeout_seconds: int
    require_nonhealthy_status: bool = False

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> "Scenario":
        allowed = {
            "schema_version", "scenario_id", "title", "purpose",
            "required_capabilities", "workload", "fault", "fault_options",
            "observation_timeout_seconds", "warmup_seconds", "expected_incidents",
            "expected_hypothesis_types", "allowed_hypothesis_types",
            "allowed_statuses", "recovery", "recovery_timeout_seconds",
            "require_nonhealthy_status",
        }
        unknown = set(value) - allowed
        if unknown:
            raise ValueError(f"scenario has unsupported fields: {sorted(unknown)}")
        if value.get("schema_version") != SCENARIO_SCHEMA_VERSION:
            raise ValueError("unsupported scenario schema_version")
        scenario_id = value.get("scenario_id")
        if not isinstance(scenario_id, str) or not scenario_id or any(
            character not in "abcdefghijklmnopqrstuvwxyz0123456789-" for character in scenario_id
        ):
            raise ValueError("scenario_id must use lowercase letters, digits, and hyphens")
        workload = value.get("workload")
        fault = value.get("fault")
        if workload not in SUPPORTED_WORKLOADS:
            raise ValueError(f"unsupported workload: {workload}")
        if fault not in SUPPORTED_FAULTS:
            raise ValueError(f"unsupported fault: {fault}")
        options = value.get("fault_options", {})
        if not isinstance(options, Mapping):
            raise ValueError("fault_options must be an object")
        observation = value.get("observation_timeout_seconds")
        warmup = value.get("warmup_seconds")
        recovery_timeout = value.get("recovery_timeout_seconds")
        if not isinstance(observation, int) or not 1 <= observation <= 120:
            raise ValueError("observation_timeout_seconds must be between 1 and 120")
        if not isinstance(warmup, int) or not 0 <= warmup <= 60:
            raise ValueError("warmup_seconds must be between 0 and 60")
        if not isinstance(recovery_timeout, int) or not 1 <= recovery_timeout <= 120:
            raise ValueError("recovery_timeout_seconds must be between 1 and 120")
        recovery = value.get("recovery")
        if not isinstance(recovery, bool):
            raise ValueError("recovery must be boolean")
        for field in ("title", "purpose"):
            if not isinstance(value.get(field), str) or not value[field].strip():
                raise ValueError(f"{field} must be a non-empty string")
        statuses = _strings(value.get("allowed_statuses"), "allowed_statuses")
        if not set(statuses).issubset({"Healthy", "Degraded", "Unknown"}):
            raise ValueError("allowed_statuses contains an unsupported status")
        expected_hypotheses = _strings(
            value.get("expected_hypothesis_types"), "expected_hypothesis_types"
        )
        allowed_hypotheses = _strings(
            value.get("allowed_hypothesis_types"), "allowed_hypothesis_types"
        )
        if not set(expected_hypotheses).issubset(allowed_hypotheses):
            raise ValueError("expected hypotheses must be included in allowed hypotheses")
        require_nonhealthy = value.get("require_nonhealthy_status", False)
        if not isinstance(require_nonhealthy, bool):
            raise ValueError("require_nonhealthy_status must be boolean")
        return cls(
            schema_version=SCENARIO_SCHEMA_VERSION,
            scenario_id=scenario_id,
            title=value["title"],
            purpose=value["purpose"],
            required_capabilities=_strings(
                value.get("required_capabilities"), "required_capabilities"
            ),
            workload=workload,
            fault=fault,
            fault_options=dict(options),
            observation_timeout_seconds=observation,
            warmup_seconds=warmup,
            expected_incidents=_strings(value.get("expected_incidents"), "expected_incidents"),
            expected_hypothesis_types=expected_hypotheses,
            allowed_hypothesis_types=allowed_hypotheses,
            allowed_statuses=statuses,
            recovery=recovery,
            recovery_timeout_seconds=recovery_timeout,
            require_nonhealthy_status=require_nonhealthy,
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "schema_version": self.schema_version,
            "scenario_id": self.scenario_id,
            "title": self.title,
            "purpose": self.purpose,
            "required_capabilities": list(self.required_capabilities),
            "workload": self.workload,
            "fault": self.fault,
            "fault_options": dict(self.fault_options),
            "observation_timeout_seconds": self.observation_timeout_seconds,
            "warmup_seconds": self.warmup_seconds,
            "expected_incidents": list(self.expected_incidents),
            "expected_hypothesis_types": list(self.expected_hypothesis_types),
            "allowed_hypothesis_types": list(self.allowed_hypothesis_types),
            "allowed_statuses": list(self.allowed_statuses),
            "recovery": self.recovery,
            "recovery_timeout_seconds": self.recovery_timeout_seconds,
            "require_nonhealthy_status": self.require_nonhealthy_status,
        }


def load_scenario(path: Path) -> Scenario:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot load scenario {path.name}: {error}") from error
    if not isinstance(value, Mapping):
        raise ValueError("scenario document must be an object")
    scenario = Scenario.from_dict(value)
    if path.stem != scenario.scenario_id:
        raise ValueError("scenario filename must match scenario_id")
    return scenario


@dataclass(frozen=True)
class CapabilityReport:
    linux: bool
    ip: bool
    tc: bool
    netns: bool
    privileged: bool
    privilege_reason: str
    session_dbus: bool
    dbus_daemon: bool
    weaknetd: bool
    weaknetctl: bool
    weaknetd_path: str | None
    weaknetctl_path: str | None
    python_venv: bool
    dbus_next: bool
    dashscope_configured: bool

    def to_public_dict(self) -> dict[str, Any]:
        return {
            "linux": self.linux,
            "ip": self.ip,
            "tc": self.tc,
            "netns": self.netns,
            "privileged": self.privileged,
            "privilege_reason": self.privilege_reason,
            "session_dbus": self.session_dbus,
            "dbus_daemon": self.dbus_daemon,
            "weaknetd": self.weaknetd,
            "weaknetctl": self.weaknetctl,
            "weaknetd_path": self.weaknetd_path,
            "weaknetctl_path": self.weaknetctl_path,
            "python_venv": self.python_venv,
            "dbus_next": self.dbus_next,
            "dashscope_configured": self.dashscope_configured,
        }


def _effective_capability(bit: int, status_path: Path = Path("/proc/self/status")) -> bool:
    try:
        for line in status_path.read_text(encoding="utf-8").splitlines():
            if line.startswith("CapEff:"):
                return bool(int(line.split()[1], 16) & (1 << bit))
    except (OSError, ValueError, IndexError):
        return False
    return False


def _find_binary(repo_root: Path, name: str) -> Path | None:
    configured = os.environ.get("WEAKNET_LAB_BUILD_DIR", "")
    candidates = []
    if configured:
        candidates.append(Path(configured) / "bin" / name)
    candidates.extend([
        repo_root / "build" / "gcc-no-ebpf" / "bin" / name,
        repo_root / "build" / "bin" / name,
    ])
    return next((path.resolve() for path in candidates if path.is_file() and os.access(path, os.X_OK)), None)


def detect_capabilities(
    repo_root: Path,
    *,
    environ: Mapping[str, str] | None = None,
    which: Callable[[str], str | None] = shutil.which,
    run: Callable[..., subprocess.CompletedProcess[str]] = subprocess.run,
) -> CapabilityReport:
    values = os.environ if environ is None else environ
    linux = platform.system() == "Linux"
    has_ip, has_tc = bool(which("ip")), bool(which("tc"))
    if os.geteuid() == 0:
        privileged, privilege_reason = True, "running as root"
    elif _effective_capability(12) and _effective_capability(21):
        privileged, privilege_reason = True, "effective CAP_NET_ADMIN and CAP_SYS_ADMIN"
    else:
        sudo_ready = False
        if which("sudo"):
            try:
                sudo_ready = run(
                    ["sudo", "-n", "true"], check=False, capture_output=True,
                    text=True, timeout=2,
                ).returncode == 0
            except (OSError, subprocess.TimeoutExpired):
                sudo_ready = False
        privileged = sudo_ready
        privilege_reason = (
            "passwordless sudo available" if sudo_ready
            else "requires root/CAP_NET_ADMIN+CAP_SYS_ADMIN or passwordless sudo"
        )
    venv_python = repo_root / ".venv" / "bin" / "python"
    dbus_next = False
    if venv_python.is_file():
        try:
            dbus_next = run(
                [str(venv_python), "-c", "import dbus_next"], check=False,
                capture_output=True, text=True, timeout=5,
            ).returncode == 0
        except (OSError, subprocess.TimeoutExpired):
            pass
    daemon = _find_binary(repo_root, "weaknet-dbus-server")
    cli = _find_binary(repo_root, "weaknetctl")
    dbus_daemon = bool(which("dbus-daemon"))
    return CapabilityReport(
        linux=linux,
        ip=has_ip,
        tc=has_tc,
        netns=linux and has_ip and privileged,
        privileged=privileged,
        privilege_reason=privilege_reason,
        session_dbus=bool(values.get("DBUS_SESSION_BUS_ADDRESS")) or dbus_daemon,
        dbus_daemon=dbus_daemon,
        weaknetd=daemon is not None,
        weaknetctl=cli is not None,
        weaknetd_path=str(daemon) if daemon else None,
        weaknetctl_path=str(cli) if cli else None,
        python_venv=venv_python.is_file(),
        dbus_next=dbus_next,
        dashscope_configured=(
            values.get("WEAKNET_LLM_PROVIDER", "").lower() == "dashscope"
            and bool(values.get("DASHSCOPE_API_KEY"))
        ),
    )


def missing_capabilities(scenario: Scenario, report: CapabilityReport) -> list[str]:
    values = report.to_public_dict()
    missing = []
    for capability in scenario.required_capabilities:
        if capability == "cap_net_admin":
            ready = report.privileged
        else:
            ready = bool(values.get(capability, False))
        if not ready:
            missing.append(capability)
    return missing


def evaluate_diagnosis(
    scenario: Scenario,
    diagnosis: Mapping[str, Any],
    *,
    detection_latency_ms: int | None,
    recovered: bool | None = None,
    recovery_latency_ms: int | None = None,
    run_id: str = "test",
    ai: Mapping[str, Any] | None = None,
) -> dict[str, Any]:
    incidents = diagnosis.get("incidents", [])
    hypotheses = diagnosis.get("hypotheses", [])
    if not isinstance(incidents, list) or not isinstance(hypotheses, list):
        raise ValueError("diagnosis must contain incident and hypothesis arrays")
    observed_incidents = sorted({
        str(item.get("type")) for item in incidents if isinstance(item, Mapping) and item.get("type")
    })
    observed_hypotheses = sorted({
        str(item.get("type")) for item in hypotheses if isinstance(item, Mapping) and item.get("type")
    })
    status = str(diagnosis.get("status", "Unknown"))
    incident_hits = sorted(set(scenario.expected_incidents) & set(observed_incidents))
    hypothesis_hits = sorted(set(scenario.expected_hypothesis_types) & set(observed_hypotheses))
    missing_incidents = sorted(set(scenario.expected_incidents) - set(observed_incidents))
    missing_hypotheses = sorted(set(scenario.expected_hypothesis_types) - set(observed_hypotheses))
    unexpected = sorted(set(observed_hypotheses) - set(scenario.allowed_hypothesis_types))
    status_ok = status in scenario.allowed_statuses
    if scenario.require_nonhealthy_status:
        status_ok = status_ok and status != "Healthy"
    detected = not missing_incidents and not missing_hypotheses and status_ok and not unexpected
    if scenario.recovery and recovered is not True:
        passed = False
    else:
        passed = detected
    result = {
        "schema_version": EVALUATION_SCHEMA_VERSION,
        "scenario": scenario.scenario_id,
        "run_id": run_id,
        "result": "PASS" if passed else "FAIL",
        "setup": "pass",
        "detected": detected,
        "status": status,
        "expected_incidents": list(scenario.expected_incidents),
        "observed_incidents": observed_incidents,
        "incident_hits": incident_hits,
        "missing_incidents": missing_incidents,
        "expected_hypotheses": list(scenario.expected_hypothesis_types),
        "observed_hypotheses": observed_hypotheses,
        "hypothesis_hits": hypothesis_hits,
        "missing_hypotheses": missing_hypotheses,
        "unexpected_authoritative_escalation": unexpected,
        "detection_latency_ms": detection_latency_ms,
        "recovered": recovered,
        "recovery_latency_ms": recovery_latency_ms,
        "ai": dict(ai or {"attempted": False}),
    }
    validate_evaluation(result)
    return result


def skipped_evaluation(scenario: Scenario, run_id: str, reasons: list[str]) -> dict[str, Any]:
    result = {
        "schema_version": EVALUATION_SCHEMA_VERSION,
        "scenario": scenario.scenario_id,
        "run_id": run_id,
        "result": "SKIP",
        "setup": "skip",
        "skip_reasons": list(reasons),
        "detected": False,
        "status": "Unknown",
        "expected_incidents": list(scenario.expected_incidents),
        "observed_incidents": [],
        "incident_hits": [],
        "missing_incidents": list(scenario.expected_incidents),
        "expected_hypotheses": list(scenario.expected_hypothesis_types),
        "observed_hypotheses": [],
        "hypothesis_hits": [],
        "missing_hypotheses": list(scenario.expected_hypothesis_types),
        "unexpected_authoritative_escalation": [],
        "detection_latency_ms": None,
        "recovered": None,
        "recovery_latency_ms": None,
        "ai": {"attempted": False},
    }
    validate_evaluation(result)
    return result


def validate_evaluation(value: Mapping[str, Any]) -> None:
    required = {
        "schema_version", "scenario", "run_id", "result", "setup", "detected",
        "status", "expected_incidents", "observed_incidents", "incident_hits",
        "missing_incidents", "expected_hypotheses", "observed_hypotheses",
        "hypothesis_hits", "missing_hypotheses", "unexpected_authoritative_escalation",
        "detection_latency_ms", "recovered", "recovery_latency_ms", "ai",
    }
    missing = required - set(value)
    if missing:
        raise ValueError(f"evaluation is missing fields: {sorted(missing)}")
    if value.get("schema_version") != EVALUATION_SCHEMA_VERSION:
        raise ValueError("unsupported evaluation schema_version")
    if value.get("result") not in {"PASS", "FAIL", "SKIP"}:
        raise ValueError("evaluation result is invalid")
    if value.get("setup") not in {"pass", "fail", "skip"}:
        raise ValueError("evaluation setup state is invalid")
    if not isinstance(value.get("detected"), bool):
        raise ValueError("evaluation detected must be boolean")
    for name in (
        "expected_incidents", "observed_incidents", "incident_hits", "missing_incidents",
        "expected_hypotheses", "observed_hypotheses", "hypothesis_hits",
        "missing_hypotheses", "unexpected_authoritative_escalation",
    ):
        if not isinstance(value.get(name), list):
            raise ValueError(f"evaluation {name} must be an array")
    if not isinstance(value.get("ai"), Mapping) or not isinstance(value["ai"].get("attempted"), bool):
        raise ValueError("evaluation ai must include attempted boolean")


def public_doctor_lines(report: CapabilityReport) -> list[str]:
    values = report.to_public_dict()
    ordered = (
        "linux", "ip", "tc", "netns", "privileged", "session_dbus",
        "weaknetd", "weaknetctl", "python_venv", "dbus_next", "dashscope_configured",
    )
    lines = [f"{name}: {'yes' if values[name] else 'no'}" for name in ordered]
    lines.append(f"privilege: {report.privilege_reason}")
    if report.weaknetd_path:
        lines.append(f"weaknetd path: {report.weaknetd_path}")
    if report.weaknetctl_path:
        lines.append(f"weaknetctl path: {report.weaknetctl_path}")
    return lines
