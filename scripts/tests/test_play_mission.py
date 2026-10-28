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
        assert cmd[cmd.index("--duration") + 1] == "90"
        assert kwargs["env"]["MC2_PLAY_MISSION_SECONDS"] == "60"
        assert kwargs["env"]["MC2_RNG_SEED"] == "42"
        return type("Result", (), {"returncode": 1, "stdout":
            "[MISSION_VERDICT v1] verdict=PASS passed=2 failed=0 pending=0 time=45.00\n"
            "[SMOKE v1] event=tick_budget ticks=1860"})()
    monkeypatch.setattr(driver.subprocess, "run", run)
    assert driver.main(["--mission", "mc2_01", "--minutes", "1"]) == 0
    assert "verdict=PASS" in capsys.readouterr().out
