"""Offline tests of the nightly slim, non-blocking mods check (owner 2026-11-08): planned nightly only, reported per mod, never gating."""
import os
from pathlib import Path
import sys
import types

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import merge_gate as gate  # noqa: E402
import nightly_gate  # noqa: E402
import lane_merge_stage  # noqa: E402

TABLE = """[mod_smoke] MC2X-CVE-G: mission plan [('metropolitan', 'campaign')]
[mod_smoke] MC2X-REAP3: mission plan [('reap3m01', 'campaign')]
[mod_smoke] MC2-Exodus: mission plan [('a_0_1', 'campaign')]
| mod        | mission      | result         | health | content / budget | note |
| ---------- | ------------ | -------------- | ------ | ---------------- | ---- |
| MC2X-CVE-G | metropolitan | INFRA(timeout) | -      | ? / 7            |      |
| MC2X-REAP3 | reap3m01     | PASS           | ok     | 2 / 2            |      |
"""


@pytest.fixture
def offline(monkeypatch):
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "gate_env", lambda *a, **kw: {})
    monkeypatch.setattr(gate, "ACTIVE_STATE", None)
    gate.FAILED_STAGES.clear()
    gate.ROW_RESULTS.clear()
    yield
    gate.FAILED_STAGES.clear()
    gate.ROW_RESULTS.clear()


def test_slim_is_nightly_only_and_full_battery_stays_weekly():
    assert gate.tier_includes("mods-slim", 1) and not gate.tier_includes("mods-slim", 0) and not gate.tier_includes("mods-slim", 2)
    assert gate.tier_includes("mods", 2) and not gate.tier_includes("mods", 1)
    nightly, weekly = nightly_gate.stage_names(False), nightly_gate.stage_names(True)
    assert "mods-slim" in nightly and "mods" not in nightly
    assert "mods" in weekly and "mods-slim" not in weekly
    assert "mods-slim" not in nightly_gate.required_stages(nightly)


def test_slim_command_is_short_with_cold_grace_and_exe_reference():
    cmd = next(c for n, c, _ in gate.plan(1) if n == "mods-slim")
    exe = cmd[cmd.index("--exe") + 1]
    assert cmd[cmd.index("--ref-exe") + 1] == exe
    assert int(cmd[cmd.index("--duration") + 1]) <= 10 and "--no-front-end" in cmd and int(cmd[cmd.index("--grace-s") + 1]) >= 300
    staged = dict((n, c) for n, c, _ in gate.install_plan([("mods-slim", cmd, {})], Path("E:/mc2-installs/x")))["mods-slim"]
    assert "--stage-root" in staged and staged.count("--duration") == 1


def test_every_slim_flag_exists_in_mod_smokes_parser():
    """meta-1417 N1: merge_gate passed --grace-s to a mod_smoke.py that had dropped it, so argparse killed the row every night."""
    import re
    cmd = next(c for n, c, _ in gate.plan(1) if n == "mods-slim")
    at = next(i for i, a in enumerate(cmd) if a.replace("\\", "/").endswith("tools/mod_smoke.py"))
    source = Path(__file__).resolve().parents[2] / "tools" / "mod_smoke.py"
    declared = set(re.findall(r"""add_argument\(\s*['"](--[a-z0-9-]+)['"]""", source.read_text(encoding="utf-8")))
    flags = {a for a in cmd[at + 1:] if a.startswith("--")}
    assert flags and flags <= declared, sorted(flags - declared)


def test_per_mod_results_parse_the_battery_table():
    assert gate.mod_battery_results(TABLE) == {"MC2X-CVE-G": "INFRA(timeout)", "MC2X-REAP3": "PASS", "MC2-Exodus": "NO RESULT"}
    assert gate.mod_battery_results("") == {}


@pytest.mark.parametrize("rc,word", [(1, "ADVISORY FAIL (1)"), (3, "ADVISORY INFRA (3)")])
def test_red_slim_row_reports_but_never_fails_the_gate(offline, capsys, rc, word):
    runner = lambda command, **kw: types.SimpleNamespace(returncode=rc if command[0] == "slim" else 0, stdout=TABLE)
    steps = [("tier1", ["tier1"], {}), ("mods-slim", ["slim"], {})]
    assert gate.execute(steps, "head", runner=runner) == 0
    assert gate.FAILED_STAGES == []
    row = gate.ROW_RESULTS["mods-slim"]
    assert row["status"].startswith(word) and row["mods"]["MC2X-CVE-G"] == "INFRA(timeout)"
    assert gate.verdict_of(0, steps)[0] == "PASS"
    assert "mods-slim per-mod: MC2X-CVE-G=INFRA(timeout)" in capsys.readouterr().out


def test_a_red_blocking_row_still_fails(offline):
    runner = lambda command, **kw: types.SimpleNamespace(returncode=1, stdout="")
    assert gate.execute([("tier1", ["tier1"], {})], "head", runner=runner) == 1   # the full mods battery is ADVISORY too (owner 2026-11-08)
    assert [n for n, _ in gate.FAILED_STAGES] == ["tier1"]


def test_promotion_does_not_require_the_slim_row():
    executed = {n: {"status": "PASS"} for n in nightly_gate.stage_names(False)}
    executed["mods-slim"] = {"status": "ADVISORY FAIL (1) (non-blocking; needs a dedicated fix lane)"}
    assert lane_merge_stage.nightly_unmet(dict(tier=1, executed=executed)) == []
    del executed["mods-slim"]
    assert lane_merge_stage.nightly_unmet(dict(tier=1, executed=executed)) == []


def test_nightly_without_a_window_keeps_the_slim_row(monkeypatch):
    import datetime
    monday = datetime.date(2026, 11, 9)
    monkeypatch.setattr(nightly_gate, "nightly_base", lambda: "b" * 40)
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "no window here")
    plan, why = nightly_gate.make_plan(False, monday)
    assert why is None
    cmd = plan["cmd"]
    assert "mods-slim" in cmd[cmd.index("--stages") + 1:] and "--window" not in cmd
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "merge w136: x")
    plan, why = nightly_gate.make_plan(False, monday)
    assert why is None
    cmd = plan["cmd"]
    assert cmd[cmd.index("--window") + 1] == "136" and "mods-slim" in cmd[cmd.index("--stages") + 1:]


@pytest.mark.parametrize("paths,extra", [
    (["docs/notes.md"], set()),
    (["code/mission_update.cpp"], {"determinism-mc2_24", "saveload-mc2_01"}),
    (["code/mc2movie.cpp"], {"player-path"}),
    (["code/logisticscomponent.cpp", "code/mechlabscreen.cpp"], set()),
    (["code/campaign.cpp"], {"player-path"}),
    (["code/mission_update.cpp", "code/mainmenu.cpp", "data/defs/ui.fit"],
     {"determinism-mc2_24", "saveload-mc2_01", "player-path"}),
])
def test_merge_stage_budget_pin(paths, extra):
    scheduled = gate.plan(1)
    planned = [s for s in scheduled if gate.tier_includes(s[0], 0)]
    steps, _ = gate.promote_for_diff(planned, scheduled, planned, paths, [], False)
    assert gate.promoted_stages(paths) == extra
    assert {n for n, _, _ in steps} == {"contracts", "harnesses", "deploy", "tier1"} | extra
    assert gate.tier_includes("editor", 1)


@pytest.mark.parametrize("reused", [False, True])
def test_declared_visuals_never_capture_before(monkeypatch, reused, capsys):
    import capture_reuse
    monkeypatch.setattr(capture_reuse, "available", lambda *a, **k: Path("verified") if reused else None)
    monkeypatch.setattr(gate, "gate_env", lambda *a: {})
    steps = gate.settle_capture_reference(gate.merge_visuals(gate.plan(1)))
    assert "capture-before" not in [n for n, _, _ in steps]
    assert [n for n, _, _ in steps] == ["capture-after", "compare-captures"] if reused else [n for n, _, _ in steps] == ["capture-after"]
    if not reused:
        assert "VISUAL after-only:" in capsys.readouterr().out
    assert gate.advisory_status("compare-captures", "FAIL", 0).startswith(gate.ADVISORY_PREFIX)
    assert gate.advisory_status("compare-captures", "FAIL", 1) == "FAIL"


def test_gate_budget_is_advisory_and_excludes_lease_wait(monkeypatch, capsys):
    monkeypatch.setattr(gate, "ROW_RESULTS", {"tier1": {"seconds": 900}})
    gate.gate_budget(0, 1300, 200)
    gate.gate_budget(1, 1300, 0)
    assert not capsys.readouterr().out
    gate.gate_budget(0, 1300, 0)
    assert "[GATE_BUDGET] OVER" in capsys.readouterr().out


def test_child_lease_wait_is_excluded_from_gate_budget(tmp_path, monkeypatch, capsys):
    import json
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "TIMINGS", {})
    monkeypatch.setattr(gate, "ROW_RESULTS", {})
    monkeypatch.setattr(gate, "FAILED_STAGES", [])
    # An old report in the same proof root must not inflate this run's wait.
    (tmp_path / "report.json").write_text(json.dumps({"host_wait": {"lease_wait_s": 900}}))
    def runner(*a, **k):
        child = tmp_path / "new"
        child.mkdir()
        (child / "report.json").write_text(json.dumps({"host_wait": {"lease_wait_s": 200}}))
        return types.SimpleNamespace(returncode=0, stdout="", gate_elapsed=1300)
    assert gate.execute([("tier1", ["smoke"], {"MC2_SMOKE_ARTIFACT_ROOT": str(tmp_path)})],
                        "head", runner=runner) == 0
    assert gate.TIMINGS["host_lease_wait"] == 200
    assert gate.ROW_RESULTS["tier1"]["host_lease_wait"] == 200
    gate.gate_budget(0, 1300, gate.TIMINGS["host_lease_wait"])
    assert "[GATE_BUDGET] OVER" not in capsys.readouterr().out


class _SkewClock:
    """The time module with a monotonic clock a fake lease wait can advance."""
    skew = 0.0

    def __getattr__(self, name):
        import time
        return getattr(time, name)

    def monotonic(self):
        import time
        return time.monotonic() + self.skew


def _budget_fixture(monkeypatch):
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "TIMINGS", {})
    monkeypatch.setattr(gate, "ROW_RESULTS", {})
    monkeypatch.setattr(gate, "FAILED_STAGES", [])


def test_child_lease_timeout_wait_is_excluded_from_gate_budget(tmp_path, monkeypatch, capsys):
    """The real run_smoke.main() lease-timeout exit (no report.json yet) under the real gate.execute()."""
    from unittest.mock import patch
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
    from scripts import run_smoke
    from scripts.smoke_lib import host_lease
    _budget_fixture(monkeypatch)
    clock = _SkewClock()

    def timed_out(*a, **k):
        clock.skew += 1200.0   # the 20-minute bounded lease wait
        raise host_lease.HostLeaseBusy("host game lease not granted after the wait")
    exe = tmp_path / "mc2.exe"
    exe.write_bytes(b"")
    ok = type("V", (), {"ok": True, "banner": lambda self: ""})()

    def runner(command, cwd, env):
        root = env["MC2_SMOKE_ARTIFACT_ROOT"]
        with patch.object(sys, "argv", ["run_smoke.py", "--exe", str(exe)]), \
             patch.object(run_smoke._exe_guard, "check_exe", return_value=ok), \
             patch.dict(run_smoke.os.environ, {"MC2_SMOKE_REQUIRE_FINGERPRINT": "0", "MC2_SMOKE_ARTIFACT_ROOT": root}), \
             patch.object(run_smoke._fingerprint, "resolve_reference",
                          side_effect=ValueError("cannot resolve fingerprint commit reference HEAD")), \
             patch.object(run_smoke, "ARTIFACT_ROOT", Path(root)), \
             patch.object(run_smoke, "time", clock), \
             patch.object(host_lease, "acquire", side_effect=timed_out):
            with pytest.raises(SystemExit) as exc:
                run_smoke.main()
        assert not list(Path(root).rglob("report.json"))   # the timeout exit is before any report
        return types.SimpleNamespace(returncode=exc.value.code, stdout="", gate_elapsed=1300)
    assert gate.execute([("tier1", ["smoke"], {"MC2_SMOKE_ARTIFACT_ROOT": str(tmp_path / "smokes")})],
                        "head", runner=runner) != 0   # the row stays red
    assert gate.TIMINGS["host_lease_wait"] == pytest.approx(1200, abs=10)
    assert gate.ROW_RESULTS["tier1"]["host_lease_wait"] == pytest.approx(1200, abs=10)
    gate.gate_budget(0, 1300, gate.TIMINGS["host_lease_wait"])
    assert "[GATE_BUDGET] OVER" not in capsys.readouterr().out


def test_gate_own_lease_timeout_is_lease_wait(tmp_path, monkeypatch, capsys):
    """A capture row whose own host hold times out never runs its child: the wait is lease wait, not stage time."""
    from smoke_lib import host_lease
    _budget_fixture(monkeypatch)
    monkeypatch.delenv(host_lease.BATCH_ENV, raising=False)
    monkeypatch.setattr(gate, "check_capture_commit", lambda *a: None)
    clock = _SkewClock()
    monkeypatch.setattr(gate, "time", clock)

    def timed_out(*a, **k):
        clock.skew += 1200.0
        raise host_lease.HostLeaseBusy("batch merge-gate: not held after the wait")
    monkeypatch.setattr(host_lease, "hold", timed_out)
    ran = []
    row = ("capture-after", ["py", "capture.py", "--install", str(tmp_path), "--out", str(tmp_path / "after")], {})
    assert gate.execute([row], "head", runner=lambda *a, **k: ran.append(1)) != 0
    assert not ran
    assert gate.TIMINGS["host_lease_wait"] == pytest.approx(1200, abs=10)
    assert gate.ROW_RESULTS["capture-after"]["host_lease_wait"] == pytest.approx(1200, abs=10)
    gate.gate_budget(0, 1300, gate.TIMINGS["host_lease_wait"])
    assert "[GATE_BUDGET] OVER" not in capsys.readouterr().out


@pytest.mark.parametrize("tier", [0, 1])
def test_visual_compare_only_blocks_nightly(monkeypatch, tier):
    monkeypatch.setattr(gate, "git", lambda *a: "head")
    monkeypatch.setattr(gate, "log_lease_holders", lambda: None)
    monkeypatch.setattr(gate, "FAILED_STAGES", [])
    monkeypatch.setattr(gate, "ROW_RESULTS", {})
    result = types.SimpleNamespace(returncode=1, stdout="MAD and montage path")
    assert gate.execute([("compare-captures", ["compare"], {"MC2_GATE_TIER": str(tier)})], "head",
                        runner=lambda *a, **k: result) == tier
    assert bool(gate.FAILED_STAGES) == bool(tier)
