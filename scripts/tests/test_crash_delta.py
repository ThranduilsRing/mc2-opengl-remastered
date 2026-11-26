"""gate-devlost-1: a lost Vulkan device or a crash bundle fails the merge gate and every smoke/capture, whatever the row said.
The fixture logs are the device-lost bundle crash_2026-11-26_093443_761_23624_284100843 (G:/mc2-installs/merge-gate/crashes, 2026-11-26). No game."""
from __future__ import annotations

from contextlib import nullcontext
import io
from pathlib import Path
import sys
import types

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402
import capture  # noqa: E402
import merge_gate as gate  # noqa: E402
import run_visual_capture as rvc  # noqa: E402
from smoke_lib import crash_delta, gates, host_lease, logparse  # noqa: E402

FIXTURE = Path(__file__).parent / "fixtures" / "crash-device-lost"
LOST_LOG = (FIXTURE / "crash.txt").read_text(encoding="utf-8") + (FIXTURE / "last_trace.txt").read_text(encoding="utf-8")
BUNDLE = r"G:\mc2-installs\merge-gate\crashes\crash_2026-11-26_093443_761_23624_284100843"
DEVICE_LOST_TXT = "Reason: Vulkan device lost\nFingerprint: MC2FP1|x\n"
SENTINEL_TXT = ("MC2 Remastered Crash Report\nFingerprint: MC2FP1|x\nReason: unclean exit: session pid 3274 ended with no exit record "
                "and no crash bundle (no handler ran: external kill)\n")


def _bundle(crashes, label, text, minidump=False):
    path = crashes / f"crash_2026-11-26_{label}"
    path.mkdir(parents=True)
    (path / "crash.txt").write_text(text, encoding="utf-8")
    if minidump:
        (path / "minidump.dmp").write_bytes(b"MDMP")
    return path


@pytest.fixture
def offline(monkeypatch):
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "gate_env", lambda *a, **kw: {})
    monkeypatch.setattr(gate, "ACTIVE_STATE", None)
    monkeypatch.setattr(gate, "game_batch", lambda: nullcontext({}))
    monkeypatch.setattr(host_lease, "gate_capture_lease", lambda *a: nullcontext())
    for name in ("log_lease_holders", "check_capture_commit"):
        monkeypatch.setattr(gate, name, lambda *a: None)
    shared = (gate.FAILED_STAGES, gate.ROW_RESULTS, gate.CRASHES, crash_delta._REPORTED)
    for item in shared:
        item.clear()
    yield
    for item in shared:
        item.clear()


def _row(tmp_path, name, command, written, rc=0):
    """One gate row whose child leaves the bundles `written(crashes)` builds; -> (steps, {label: dir}, gate rc)."""
    (tmp_path / "install").mkdir(exist_ok=True)
    (tmp_path / "install" / "mc2.exe").write_bytes(b"MZ")
    made = {}

    def runner(cmd, **kw):
        made.update(written(tmp_path / "install" / "crashes"))
        return types.SimpleNamespace(returncode=rc, stdout="")

    steps = [(name, command, {"MC2_GATE_TIER": "0"})]
    return steps, made, gate.execute(steps, "head", runner=runner)


def test_device_lost_log_names_the_reason_and_the_bundle():
    assert crash_delta.log_crash(LOST_LOG) == ("Vulkan device lost", BUNDLE)
    assert crash_delta.log_crash(f"[CRASHBUNDLE] device_lost bundle={BUNDLE}") == ("device_lost", BUNDLE)
    assert crash_delta.log_crash("[HEARTBEAT] frames=3") is None
    assert logparse.parse_log(LOST_LOG).crash_handler_hit
    # The record of an earlier harness-killed session is printed by the next launch: not that process's crash.
    sentinel = f"[CRASHBUNDLE] Reason: unclean exit: session pid 3274 bundle={BUNDLE}"
    assert crash_delta.log_crash(sentinel) is None and not logparse.parse_log(sentinel).crash_handler_hit


def test_capture_pose_is_crash_with_its_bundle_never_missing(tmp_path, monkeypatch):
    log = tmp_path / "mc2_10_capture.log"
    log.write_text(LOST_LOG, encoding="utf-8")
    result = {"mission": "mc2_10", "bookmarks": [], "all_present": False, "engine_capture_fired": True,
              "crash": crash_delta.log_crash(log.read_text(encoding="utf-8")), "log": str(log)}
    monkeypatch.setattr(ab, "game_executable", lambda install: tmp_path / "mc2.exe")
    monkeypatch.setattr(ab, "payload_identity", lambda install: {})
    monkeypatch.setattr(capture.ms, "identity_block", lambda **kw: {})
    monkeypatch.setattr(capture, "run_pose", lambda **kw: result)
    out = io.StringIO()
    manifest = capture.capture_sweep(tmp_path, [tmp_path / "mc2_10_saved_204523.json"], tmp_path / "out", 1, 1, 1, True, False, out)
    assert f"CRASH(Vulkan device lost) bundle={BUNDLE}" in out.getvalue() and "MISSING" not in out.getvalue()
    assert manifest["poses"][0]["crash"] == ["Vulkan device lost", BUNDLE]
    assert rvc.fail_reasons(result)[0] == f"crash: Vulkan device lost (bundle {BUNDLE})"


def test_gate_reds_a_real_bundle_under_an_advisory_row_and_names_it_in_the_verdict(offline, tmp_path):
    install = tmp_path / "install"
    _bundle(install / "crashes", "0_old", (FIXTURE / "crash.txt").read_text(encoding="utf-8"), minidump=True)   # an earlier run's: not this row's
    written = lambda crashes: {"lost": _bundle(crashes, "1_lost", DEVICE_LOST_TXT),
                               "fatal": _bundle(crashes, "2_fatal", (FIXTURE / "crash.txt").read_text(encoding="utf-8"), minidump=True)}
    steps, made, rc = _row(tmp_path, "capture-after", ["cap", "--install", str(install)], written, rc=1)
    assert rc == 1 and [n for n, _ in gate.FAILED_STAGES] == ["capture-after"]   # tier 0 made this row ADVISORY on 11-26
    status = gate.ROW_RESULTS["capture-after"]["status"]
    assert status == f"FAIL: crash bundle {made['lost']} (Vulkan device lost), {made['fatal']} (Code: 0xE0004D46)"
    assert gate.verdict_of(rc, steps)[0] == "FAIL"
    data = gate.write_verdict([tmp_path / "v.json"], head="head", branch="b", verdict="FAIL", steps=steps, skipped=[],
                              artifacts={}, timings={}, cleanup_errors=[])
    assert data["crashes"] == [dict(row="capture-after", dir=str(made["lost"]), reason="Vulkan device lost"),
                               dict(row="capture-after", dir=str(made["fatal"]), reason="Code: 0xE0004D46")]
    assert data["failed"] == [dict(name="capture-after", status=status)] and data["advisory"] == []


def test_non_blocking_row_stays_advisory_for_its_own_fail_but_not_for_a_crash(offline, tmp_path):
    exe = ["slim", "--exe", str(tmp_path / "install" / "mc2.exe")]
    _, _, rc = _row(tmp_path, "mods-slim", exe, lambda crashes: {}, rc=1)
    assert rc == 0 and gate.FAILED_STAGES == [] and gate.ROW_RESULTS["mods-slim"]["status"].startswith("ADVISORY FAIL")
    for item in (gate.FAILED_STAGES, gate.ROW_RESULTS):
        item.clear()
    _, _, rc = _row(tmp_path, "mods-slim", exe, lambda crashes: {"lost": _bundle(crashes, "1_lost", DEVICE_LOST_TXT)})
    assert rc == 1 and [n for n, _ in gate.FAILED_STAGES] == ["mods-slim"]


def test_sentinel_record_and_earlier_bundles_stay_informational(offline, tmp_path):
    install = tmp_path / "install"
    _bundle(install / "crashes", "0_old", (FIXTURE / "crash.txt").read_text(encoding="utf-8"), minidump=True)
    _, made, rc = _row(tmp_path, "capture-after", ["cap", "--install", str(install)],
                       lambda crashes: {"sentinel": _bundle(crashes, "1_unclean", SENTINEL_TXT)}, rc=1)
    assert crash_delta.real(made["sentinel"]) is None and gate.CRASHES == []
    assert rc == 0 and gate.ROW_RESULTS["capture-after"]["status"] == "ADVISORY: FAIL (1)"   # the 11-26 path for everything but a crash


def test_crash_after_the_summary_fails_the_run():
    clean = "[INSTR v1] enabled:\n[SMOKE v1] event=summary result=pass\n"
    def run(log):
        return gates.evaluate(logparse.parse_log(log), gates.GateConfig(), exit_code=0, walltime_s=1.0)
    ok = run(clean)
    late = run(clean + "[CRASH] FAST-EXIT code=0xC0000005 bundle=X\n")
    assert set(late.buckets) == set(ok.buckets) | {"crash_after_summary"} and not late.passed
    reported = run(clean.replace("result=pass", "result=fail reason=x stage=play") + "fatal backend exit 3: Vulkan device lost\n")
    assert reported.buckets[:2] == ["engine_reported_fail", "crash_after_summary"]
