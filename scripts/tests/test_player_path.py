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


def test_mod_scenarios_are_outside_the_stock_dir_and_assert_something():
    mods = ROOT / "tests" / "player_path_mods"
    for f in sorted(mods.glob("*.pp")):
        assert not (pp.SCEN_DIR / f.name).exists(), f"{f.name}: mod scenario also in the stock dir"
        sc = pp.parse_scenario(f)
        assert sc["steps"][-1] == "mark done" and sc["expect"], f.name


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
    assert pp.main(["--install", "G:/mc2-installs/owner-test-2026-10-26"]) == 2


def test_save_redirect_is_required_and_per_run(tmp_path):
    import pytest
    with pytest.raises(SystemExit):
        pp.require_save_redirect({}, tmp_path)
    with pytest.raises(SystemExit):
        pp.require_save_redirect({"MC2_SAVE_DIR": str(pp.REAL_SAVE_DIR)}, tmp_path)
    with pytest.raises(SystemExit):
        pp.require_save_redirect({"MC2_SAVE_DIR": str(tmp_path.parent)}, tmp_path)
    ok = tmp_path / "savegame"
    assert pp.require_save_redirect({"MC2_SAVE_DIR": str(ok)}, tmp_path) == ok.resolve()


def test_fingerprint_must_match_head(monkeypatch, tmp_path):
    """merge-1119a gate1: player-path recorded PASS on the seed exe because it never checked the build fingerprint."""
    monkeypatch.delenv("MC2_SMOKE_REQUIRE_FINGERPRINT", raising=False)
    head = "a" * 40
    monkeypatch.setattr(pp.fingerprint, "resolve_reference", lambda root: {"mode": "head", "sha": head})
    banner = "[BUILD_FINGERPRINT v1] sha=%s dirty=0 branch=b built=now src=A:/x"
    assert pp.fingerprint_problems(banner % head[:12], tmp_path / "mc2.exe") == []
    assert any("MISMATCH" in p for p in pp.fingerprint_problems(banner % ("b" * 12), tmp_path / "mc2.exe"))
    assert pp.fingerprint_problems("no banner here", tmp_path / "mc2.exe")
