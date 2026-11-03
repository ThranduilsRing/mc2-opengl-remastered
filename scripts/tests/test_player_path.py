"""run_player_path.py: scenario parsing, env hygiene, and an end-to-end run against a fake engine (no game launch)."""
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import run_player_path as pp  # noqa: E402


def test_stock_scenarios_parse_and_end_with_mark_done():
    names = sorted(p.stem for p in pp.SCEN_DIR.glob("*.pp"))
    assert {"new_campaign", "load_game"} <= set(names)
    for n in names:
        sc = pp.parse_scenario(pp.SCEN_DIR / f"{n}.pp")
        assert sc["steps"][-1] == "mark done", n
        assert sc["expect"], f"{n}: a scenario without an expected log line proves nothing"
        assert not any(s.startswith("MC2_BOOT_TO") for s in sc["steps"]), "no boot shortcuts: the menu must be clicked"


def test_parse_scenario_directives(tmp_path):
    f = tmp_path / "x.pp"
    f.write_text("#! timeout 12\n#! expect foo\n#! forbid bar\n# comment\nawait ready 5\nclick 1 2\nmark done\n", encoding="utf-8")
    sc = pp.parse_scenario(f)
    assert sc["timeout"] == 12.0 and sc["expect"] == ["foo"] and "bar" in sc["forbid"]
    assert sc["steps"] == ["await ready 5", "click 1 2", "mark done"]


def test_env_strips_smoke_and_boot_shortcuts(monkeypatch, tmp_path):
    for k in ("MC2_SMOKE_MODE", "MC2_BOOT_TO_BAY", "MC2_BOOT_TO_MISSION", "MC2_BOOT_VIDEO", "MC2_MENU_CANARY_SKIP_INTRO"):
        monkeypatch.setenv(k, "1")
    env = pp.build_env({"MC2_SIM_OVERLAP": "0"}, tmp_path / "s.script")
    assert not any(k in env for k in ("MC2_SMOKE_MODE", "MC2_BOOT_TO_BAY", "MC2_BOOT_TO_MISSION", "MC2_BOOT_VIDEO"))
    assert env["MC2_INPUT_SCRIPT"].endswith("s.script") and env["MC2_LOG"] == "1" and env["MC2_SIM_OVERLAP"] == "0"
    assert "FIRST60" in env["MC2_DIAG_TAGS"].split(",")


def test_refuses_owner_install(capsys):
    assert pp.main(["--install", "E:/mc2-installs/owner-test-2026-10-26"]) == 2
