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
    assert gate.execute([("mods", ["mods"], {})], "head", runner=runner) == 1
    assert [n for n, _ in gate.FAILED_STAGES] == ["mods"]


def test_promotion_does_not_require_the_slim_row():
    executed = {n: {"status": "PASS"} for n in nightly_gate.stage_names(False)}
    executed["mods-slim"] = {"status": "ADVISORY FAIL (1) (non-blocking; needs a dedicated fix lane)"}
    assert lane_merge_stage.nightly_unmet(dict(tier=1, executed=executed)) == []
    del executed["mods-slim"]
    assert lane_merge_stage.nightly_unmet(dict(tier=1, executed=executed)) == []


def test_nightly_without_a_window_drops_the_slim_row(monkeypatch, capsys, tmp_path):
    import datetime
    monday = datetime.date(2026, 11, 9)
    monkeypatch.setattr(nightly_gate, "datetime", types.SimpleNamespace(date=types.SimpleNamespace(today=lambda: monday)))
    monkeypatch.setattr(nightly_gate, "PROOF_ROOT", str(tmp_path))
    monkeypatch.setattr(nightly_gate, "refusal", lambda: None)
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "no window here")
    assert nightly_gate.main(["--dry-run"]) == 0
    out = capsys.readouterr().out
    assert "mods-slim dropped" in out and "mods-slim" not in out.split("--stages", 1)[1]
    monkeypatch.setattr(nightly_gate, "git", lambda *a: "merge w136: x")
    assert nightly_gate.main(["--dry-run"]) == 0
    head, stages = capsys.readouterr().out.split("--stages", 1)
    assert "--window 136" in head and "mods-slim" in stages
