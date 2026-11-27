"""The report keeps the latest state and requires an explicit engine verdict."""
import importlib.util
from pathlib import Path
import pytest

spec = importlib.util.spec_from_file_location("play_mission", Path(__file__).resolve().parents[2] / "tools/play_mission.py")
driver = importlib.util.module_from_spec(spec)
spec.loader.exec_module(driver)


def test_state_changes_and_timeout():
    verdict, rows = driver.parse_report("""
[MISSION_OBJECTIVES v1] id=2 text_key=42 state=PENDING primary=true hidden=false
[MISSION_OBJECTIVES v1] id=2 text_key=42 state=PASS primary=true hidden=false
[MISSION_OBJECTIVES v1] id=3 text_key=43 state=FAIL primary=false hidden=false
[MISSION_VERDICT v1] verdict=TIMEOUT passed=1 failed=1 pending=0 time=60.00
""")
    assert rows[2]["state"] == "PASS"
    assert rows[3]["state"] == "FAIL"
    assert "verdict=TIMEOUT" in verdict


def test_missing_verdict_is_not_success():
    assert driver.parse_report("[HEADLESS] tick budget reached") == (None, {})


def test_driver_uses_headless_harness_and_requested_budget(monkeypatch, capsys):
    def run(cmd, **kwargs):
        assert "run_smoke.py" in cmd[1]
        assert "--headless" in cmd
        assert cmd[cmd.index("--ticks") + 1] == "1860"
        assert cmd[cmd.index("--sim-speed") + 1] == "1"
        assert cmd[cmd.index("--duration") + 1] == "90"
        assert kwargs["env"]["MC2_PLAY_MISSION_SECONDS"] == "60"
        assert kwargs["env"]["MC2_RNG_SEED"] == "42"
        return type("Result", (), {"returncode": 1, "stdout":
            "[MISSION_VERDICT v1] verdict=PASS passed=2 failed=0 pending=0 time=45.00\n"
            "[SMOKE v1] event=tick_budget ticks=1860\n"
            "[SMOKE v1] event=summary result=pass\n"
            "| mc2_01 | FAIL | heartbeat_freeze_play |\n"})()
    monkeypatch.setattr(driver.subprocess, "run", run)
    assert driver.main(["--mission", "mc2_01", "--minutes", "1"]) == 0
    assert "verdict=PASS" in capsys.readouterr().out


def test_batch_repeats_and_continues_after_failure(monkeypatch, capsys):
    calls = []
    def run(name, install, seconds, batch):
        calls.append((name, seconds, batch))
        return name == "mc2_01"
    monkeypatch.setattr(driver, "run_mission", run)
    assert driver.main(["--batch", "mc2_01,mc2_03", "mc2_01", "--minutes", "1"]) == 1
    assert calls == [("mc2_01", 60, True), ("mc2_03", 60, True), ("mc2_01", 60, True)]
    assert "passed=2 failed=1 total=3" in capsys.readouterr().out


def test_harness_failure_overrides_engine_pass(monkeypatch, capsys):
    monkeypatch.setattr(driver.subprocess, "run", lambda *a, **k: type("Result", (), {
        "returncode": 2, "stdout": "[MISSION_VERDICT v1] verdict=PASS time=1.00"})())
    assert driver.main(["--batch", "mc2_01"]) == 1
    text = capsys.readouterr().out
    assert "verdict=ERROR" in text
    assert "passed=0 failed=1 total=1" in text


@pytest.mark.parametrize("failure", [
    "| mc2_01 | FAIL | sim_overlap_violation |\n",
    "[RULES] HARD FAIL: unexpected policy\n",
])
def test_tick_budget_does_not_hide_real_guard_failures(monkeypatch, capsys, failure):
    text = ("[MISSION_VERDICT v1] verdict=PASS passed=1\n"
            "[SMOKE v1] event=tick_budget ticks=1860\n"
            "[SMOKE v1] event=summary result=pass\n"
            "| mc2_01 | FAIL | heartbeat_freeze_play |\n" + failure)
    monkeypatch.setattr(driver.subprocess, "run", lambda *a, **k: type("Result", (), {
        "returncode": 1, "stdout": text})())
    assert driver.main(["--batch", "mc2_01"]) == 1
    assert "reason=harness_failed" in capsys.readouterr().out


def test_goto_and_mod_flags_set_env_and_allow_any_mission(monkeypatch):
    def run(cmd, **kwargs):
        env = kwargs["env"]
        assert env["MC2_PLAY_MISSION_GOTO"] == "1706,5560"
        assert (env["MC2_ACTIVE_MOD"], env["MC2_MOD_DEPS"], env["MC2_BOOT_TO_BAY"]) == ("MC2X-DEE", "mc2x-compat", "campaign")
        assert "--allow-any-mission" in cmd
        return type("Result", (), {"returncode": 0, "stdout": "[MISSION_VERDICT v1] verdict=PASS passed=1\n"})()
    monkeypatch.setattr(driver.subprocess, "run", run)
    assert driver.main(["--mission", "basindrylands", "--goto", "1706,5560", "--mod", "MC2X-DEE:mc2x-compat:campaign"]) == 0


def test_no_goto_or_mod_leaves_run_unchanged(monkeypatch):
    def run(cmd, **kwargs):
        assert "MC2_PLAY_MISSION_GOTO" not in kwargs["env"] or kwargs["env"]["MC2_PLAY_MISSION_GOTO"] == __import__("os").environ.get("MC2_PLAY_MISSION_GOTO")
        assert "--allow-any-mission" not in cmd
        return type("Result", (), {"returncode": 0, "stdout": "[MISSION_VERDICT v1] verdict=PASS passed=1\n"})()
    monkeypatch.setattr(driver.subprocess, "run", run)
    monkeypatch.delenv("MC2_PLAY_MISSION_GOTO", raising=False)
    assert driver.main(["--mission", "mc2_01"]) == 0


@pytest.mark.parametrize("ai,reason,value", [
    ("stock", "player-stock", 0), ("smarter", "player-smarter-magic-1.45", 1)])
def test_ai_selects_options_and_requires_matching_policy(tmp_path, monkeypatch, ai, reason, value):
    options = tmp_path / "options.cfg"
    original = b"FITini\r\n[MechCommander2]\r\nl GameRulesAI = 0\r\n"
    options.write_bytes(original)
    monkeypatch.setenv("MC2_BRAIN_POLICY", "techbrain")
    def run(cmd, **kwargs):
        assert options.read_text().count("GameRulesAI") == 1
        assert f"l GameRulesAI = {value}" in options.read_text()
        assert "MC2_BRAIN_POLICY" not in kwargs["env"]
        assert "--allow-nondefault-rules" in cmd
        return type("Result", (), {"returncode": 0, "stdout":
            f"[BRAIN_POLICY] mission=mc2_01 policy=legacy reason={reason}\n"
            "[MISSION_OBJECTIVES v1] id=0 text_key=42 state=PASS primary=true hidden=false\n"
            "[MISSION_VERDICT v1] verdict=PASS time=30\n"})()
    monkeypatch.setattr(driver.subprocess, "run", run)
    assert driver.main(["--mission", "mc2_01", "--install", str(tmp_path), "--ai", ai]) == 0
    assert options.read_bytes() == original


@pytest.mark.parametrize("policy,extra,objective", [
    ("techbrain reason=player-commander", "", "PASS"),
    ("legacy reason=player-smarter-magic-1.45 magic-profile-missing,stock-brain", "", "PASS"),
    ("legacy reason=player-smarter-magic-1.45", "[ABL_FALLBACK]", "PASS"),
    ("legacy reason=player-smarter-magic-1.45", "[ABL_FATAL]", "PASS"),
    ("legacy reason=player-smarter-magic-1.45", "", "PENDING")])
def test_ai_rejects_invalid_proof(tmp_path, monkeypatch, policy, extra, objective):
    (tmp_path / "options.cfg").write_text("[MechCommander2]\n")
    monkeypatch.setattr(driver.subprocess, "run", lambda *a, **k: type("Result", (), {
        "returncode": 0, "stdout": f"[BRAIN_POLICY] mission=mc2_01 policy={policy}\n{extra}\n"
        f"[MISSION_OBJECTIVES v1] id=0 text_key=42 state={objective} primary=true hidden=false\n"
        "[MISSION_VERDICT v1] verdict=PASS time=30\n"})())
    assert driver.main(["--mission", "mc2_01", "--install", str(tmp_path), "--ai", "smarter"]) == 1


def test_ai_restores_options_when_harness_raises(tmp_path, monkeypatch):
    options = tmp_path / "options.cfg"
    original = b"[MechCommander2]\r\n"
    options.write_bytes(original)
    def fail(*args, **kwargs):
        raise OSError("launch failed")
    monkeypatch.setattr(driver.subprocess, "run", fail)
    with pytest.raises(OSError):
        driver.main(["--mission", "mc2_01", "--install", str(tmp_path), "--ai", "stock"])
    assert options.read_bytes() == original
