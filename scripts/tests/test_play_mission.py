"""The report keeps the latest state and requires an explicit engine verdict."""
import importlib.util
from pathlib import Path

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
            "[SMOKE v1] event=tick_budget ticks=1860"})()
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
