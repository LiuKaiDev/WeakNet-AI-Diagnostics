from __future__ import annotations

import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

from lab.weaknet_lab.model import (
    CapabilityReport,
    detect_capabilities,
    evaluate_diagnosis,
    load_scenario,
    public_doctor_lines,
    skipped_evaluation,
    validate_evaluation,
)
from lab.weaknet_lab.runner import (
    CommandTimedOut,
    ResourceNames,
    build_parser,
    cleanup_lab,
    command_fixture_demo,
    run_bounded,
    scenarios,
)


ROOT = Path(__file__).resolve().parents[2]
SCENARIO_ROOT = ROOT / "lab" / "scenarios"


def capability_report(*, privileged: bool = False) -> CapabilityReport:
    return CapabilityReport(
        linux=True,
        ip=True,
        tc=True,
        netns=privileged,
        privileged=privileged,
        privilege_reason=("running as root" if privileged else "privilege unavailable"),
        session_dbus=True,
        dbus_daemon=True,
        weaknetd=True,
        weaknetctl=True,
        weaknetd_path="/tmp/weaknet-dbus-server",
        weaknetctl_path="/tmp/weaknetctl",
        python_venv=True,
        dbus_next=True,
        dashscope_configured=False,
    )


class ScenarioContractTests(unittest.TestCase):
    def test_all_scenarios_parse_and_have_unique_ids(self) -> None:
        loaded = scenarios()
        self.assertGreaterEqual(len(loaded), 4)
        self.assertLessEqual(len(loaded), 6)
        self.assertEqual(len({item.scenario_id for item in loaded}), len(loaded))
        self.assertEqual(
            {item.scenario_id for item in loaded},
            {path.stem for path in SCENARIO_ROOT.glob("*.json")},
        )

    def test_scenario_filename_must_match_id(self) -> None:
        source = json.loads((SCENARIO_ROOT / "healthy.json").read_text(encoding="utf-8"))
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "wrong-name.json"
            path.write_text(json.dumps(source), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "filename must match"):
                load_scenario(path)

    def test_parser_defaults_to_no_ai(self) -> None:
        arguments = build_parser().parse_args(["run", "healthy"])
        self.assertEqual(arguments.ai_mode, "none")

    def test_capability_detection_reports_passwordless_sudo_without_secrets(self) -> None:
        completed = subprocess.CompletedProcess([], 0, "", "")

        def fake_which(name: str) -> str | None:
            return f"/usr/bin/{name}" if name in {"ip", "tc", "sudo", "dbus-daemon"} else None

        with mock.patch("lab.weaknet_lab.model.platform.system", return_value="Linux"), mock.patch(
            "lab.weaknet_lab.model.os.geteuid", return_value=1000
        ), mock.patch("lab.weaknet_lab.model._effective_capability", return_value=False), mock.patch(
            "lab.weaknet_lab.model._find_binary", return_value=None
        ):
            report = detect_capabilities(
                ROOT,
                environ={
                    "WEAKNET_LLM_PROVIDER": "dashscope",
                    "DASHSCOPE_API_KEY": "never-print-this",
                },
                which=fake_which,
                run=lambda *args, **kwargs: completed,
            )
        self.assertTrue(report.privileged)
        self.assertTrue(report.netns)
        self.assertEqual(report.privilege_reason, "passwordless sudo available")
        self.assertTrue(report.dashscope_configured)
        self.assertNotIn("never-print-this", "\n".join(public_doctor_lines(report)))


class EvaluationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.scenario = load_scenario(SCENARIO_ROOT / "high-rtt.json")

    def test_expected_incident_and_hypothesis_pass(self) -> None:
        result = evaluate_diagnosis(
            self.scenario,
            {
                "status": "Degraded",
                "incidents": [{"type": "HighTcpRtt"}],
                "hypotheses": [{"type": "NetworkPathDegradation", "confidence": "Medium"}],
            },
            detection_latency_ms=10000,
            recovered=True,
            recovery_latency_ms=5000,
        )
        self.assertEqual(result["result"], "PASS")
        self.assertTrue(result["detected"])
        self.assertEqual(result["incident_hits"], ["HighTcpRtt"])
        self.assertEqual(result["hypothesis_hits"], ["NetworkPathDegradation"])

    def test_missing_detection_fails(self) -> None:
        result = evaluate_diagnosis(
            self.scenario,
            {"status": "Degraded", "incidents": [], "hypotheses": []},
            detection_latency_ms=None,
            recovered=True,
            recovery_latency_ms=5000,
        )
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["missing_incidents"], ["HighTcpRtt"])
        self.assertEqual(result["missing_hypotheses"], ["NetworkPathDegradation"])

    def test_authoritative_escalation_is_visible_and_fails(self) -> None:
        result = evaluate_diagnosis(
            self.scenario,
            {
                "status": "Degraded",
                "incidents": [{"type": "HighTcpRtt"}],
                "hypotheses": [
                    {"type": "NetworkPathDegradation"},
                    {"type": "LocalWifiInterference"},
                ],
            },
            detection_latency_ms=10000,
            recovered=True,
            recovery_latency_ms=5000,
        )
        self.assertEqual(result["result"], "FAIL")
        self.assertEqual(result["unexpected_authoritative_escalation"], ["LocalWifiInterference"])

    def test_recovery_requirement_is_enforced(self) -> None:
        result = evaluate_diagnosis(
            self.scenario,
            {
                "status": "Degraded",
                "incidents": [{"type": "HighTcpRtt"}],
                "hypotheses": [{"type": "NetworkPathDegradation"}],
            },
            detection_latency_ms=10000,
            recovered=False,
            recovery_latency_ms=None,
        )
        self.assertEqual(result["result"], "FAIL")
        self.assertTrue(result["detected"])

    def test_skip_result_validates(self) -> None:
        result = skipped_evaluation(self.scenario, "skip-test", ["missing capability: netns"])
        validate_evaluation(result)
        self.assertEqual(result["result"], "SKIP")
        self.assertEqual(result["setup"], "skip")


class SafetyTests(unittest.TestCase):
    def test_resource_names_reject_non_lab_targets(self) -> None:
        ResourceNames.create("abcdef").validate()
        with self.assertRaisesRegex(Exception, "non-lab namespace"):
            ResourceNames.from_state({
                "token": "abcdef",
                "client_namespace": "default",
                "server_namespace": "wnlab-s-abcdef",
                "client_interface": "wnc-abcdef",
                "server_interface": "wns-abcdef",
            })

    def test_bounded_command_times_out(self) -> None:
        started = time.monotonic()
        with self.assertRaises(CommandTimedOut):
            run_bounded([sys.executable, "-c", "import time; time.sleep(2)"], timeout=0.05)
        self.assertLess(time.monotonic() - started, 1.0)

    def test_cleanup_is_idempotent_without_state(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "missing.json"
            self.assertEqual(cleanup_lab(path, report=capability_report()), [])
            self.assertEqual(cleanup_lab(path, report=capability_report()), [])

    def test_unprivileged_cleanup_retains_state_when_resources_exist(self) -> None:
        resources = ResourceNames.create("abcdef")
        state = {
            "resources": resources.to_dict(),
            "processes": {},
        }
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "active.json"
            path.write_text(json.dumps(state), encoding="utf-8")
            with mock.patch("lab.weaknet_lab.runner._resources_present", return_value=True):
                actions = cleanup_lab(path, report=capability_report(privileged=False))
            self.assertTrue(path.exists())
            self.assertIn("retained state for cleanup retry", actions)

    def test_doctor_output_does_not_expose_secret(self) -> None:
        secret = "secret-value-that-must-not-appear"
        with mock.patch.dict(os.environ, {"DASHSCOPE_API_KEY": secret}, clear=False):
            output = "\n".join(public_doctor_lines(capability_report()))
        self.assertNotIn(secret, output)
        self.assertIn("dashscope_configured: no", output)

    def test_fixture_demo_is_explicitly_labeled_simulation(self) -> None:
        arguments = build_parser().parse_args(["fixture-demo", "--mode", "advise"])
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            return_code = command_fixture_demo(arguments)
        self.assertEqual(return_code, 0)
        self.assertIn("SIMULATION", output.getvalue())
        self.assertIn('"simulation": true', output.getvalue())


@unittest.skipUnless(
    os.environ.get("WEAKNET_RUN_LAB_INTEGRATION") == "1",
    "set WEAKNET_RUN_LAB_INTEGRATION=1 on a privileged Linux host",
)
class PrivilegedIntegrationTests(unittest.TestCase):
    def test_healthy_scenario_real_stack(self) -> None:
        result = subprocess.run(
            [str(ROOT / "lab" / "weaknet-lab"), "run", "healthy", "--no-ai"],
            cwd=ROOT,
            text=True,
            capture_output=True,
            timeout=90,
            check=False,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("[result] PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
