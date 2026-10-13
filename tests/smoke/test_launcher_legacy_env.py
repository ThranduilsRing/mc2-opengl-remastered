"""The launcher maps the retired render-option keys of launcher_env.json to options.cfg once (Gates/render_options_legacy_env.h).

WHY: a player whose launcher_env.json says "MC2_SHADOW_CSM": "0" must keep CSM off after the options moved into options.cfg; before the
migration the key was ignored and the choice reset silently. The tests run the real launcher headless (`--sync-options`) on a temp copy.
Skipped when the launcher is not built in build64-ninja.
"""
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child  # noqa: E402  (windowless console children, scripts/console_child.py)

LAUNCHER = ROOT / "build64-ninja" / "out" / "mc2_launcher" / "mc2-launcher.exe"
needs_launcher = pytest.mark.skipif(not LAUNCHER.is_file(), reason="mc2-launcher.exe is not built in build64-ninja")

CFG = "FITini \r\nFITini \r\n\r\n[MechCommander2]\r\nl MusicVolume = 128\r\nb ShadowCSM = TRUE\r\nb SunAxisFix = FALSE\r\n"


def install(tmp_path, env_json, cfg=CFG):
    d = tmp_path / "inst"
    d.mkdir()
    shutil.copy2(LAUNCHER, d / "mc2-launcher.exe")
    (d / "launcher_env.json").write_text(json.dumps(env_json, indent=2), encoding="ascii")
    (d / "options.cfg").write_bytes(cfg.encode("ascii"))
    return d


def sync(d):
    # background-launch: allow the launcher's --sync-options mode edits two files and exits before it creates a window; it never starts the game
    r = subprocess.run([str(d / "mc2-launcher.exe"), "--sync-options"], cwd=d, timeout=60, **console_child.child_kwargs())
    assert r.returncode == 0


def cfg_value(d, key):
    m = re.search(rf"^\w {key} = (.*?)\r?$", (d / "options.cfg").read_text(), re.M)
    return m.group(1) if m else None


def log_lines(d):
    p = d / "launcher.log"
    return [l for l in p.read_text().splitlines() if "[LAUNCHER_ENV v1]" in l] if p.is_file() else []


@needs_launcher
def test_old_choices_survive_into_options_cfg_and_the_legacy_keys_are_dropped(tmp_path):
    d = install(tmp_path, {"MC2_SHADOW_CSM": "0", "MC2_SUN_AXIS_FIX": "1", "MC2_PROP_SHADOW_RECEIVE": "1",
                           "MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR": "0.3", "MC2_FPS_CAP": "165", "last_campaign": "mymod"})
    sync(d)
    assert cfg_value(d, "ShadowCSM") == "FALSE"          # the old "CSM off" is kept, over the TRUE the file had
    assert cfg_value(d, "SunAxisFix") == "TRUE"
    assert cfg_value(d, "PropShadowReceive") == "1"
    assert cfg_value(d, "StaticPropIblSunRotateFloor") == "0.3"
    assert cfg_value(d, "MusicVolume") == "128"          # lines the launcher does not own are untouched
    after = json.loads((d / "launcher_env.json").read_text())
    assert not {"MC2_SHADOW_CSM", "MC2_SUN_AXIS_FIX", "MC2_PROP_SHADOW_RECEIVE", "MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR"} & set(after)
    assert after["MC2_FPS_CAP"] == "165" and after["last_campaign"] == "mymod"   # the rest of the file is kept
    lines = log_lines(d)
    assert len(lines) == 1 and "found=4" in lines[0] and "applied=4" in lines[0] and "ShadowCSM=FALSE" in lines[0]


@needs_launcher
def test_the_migration_happens_once(tmp_path):
    d = install(tmp_path, {"MC2_SHADOW_CSM": "0"})
    sync(d)
    sync(d)
    assert cfg_value(d, "ShadowCSM") == "FALSE"
    assert len(log_lines(d)) == 1


@needs_launcher
def test_a_value_already_stored_under_the_new_name_wins(tmp_path):
    d = install(tmp_path, {"MC2_SHADOW_CSM": "0", "ShadowCSM": "1"})
    sync(d)
    assert cfg_value(d, "ShadowCSM") == "TRUE"
    assert "kept_existing=1" in log_lines(d)[0] and "applied=0" in log_lines(d)[0]
    assert "MC2_SHADOW_CSM" not in json.loads((d / "launcher_env.json").read_text())


@needs_launcher
def test_a_meaningless_old_value_keeps_the_default_and_is_counted(tmp_path):
    d = install(tmp_path, {"MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR": "banana", "MC2_SUN_AXIS_FIX": "yes please"})
    sync(d)
    assert cfg_value(d, "StaticPropIblSunRotateFloor") == "0.45"   # the engine default: no guess was made from "banana"
    assert cfg_value(d, "SunAxisFix") == "FALSE"
    assert "unmapped=2" in log_lines(d)[0]


@needs_launcher
def test_a_file_without_legacy_keys_is_left_alone(tmp_path):
    d = install(tmp_path, {"MC2_FPS_CAP": "165"})
    before = (d / "launcher_env.json").read_bytes()
    sync(d)
    assert (d / "launcher_env.json").read_bytes() == before
    assert log_lines(d) == []


def test_every_environment_option_has_exactly_one_legacy_name():
    table = (ROOT / "Gates" / "render_options_legacy_env.h").read_text()
    mapped = re.findall(r'\{"(MC2_[A-Z0-9_]+)",\s*"(\w+)"\}', table)
    options = re.findall(r'detail::\w+Opt\s*\(\s*"(\w+)"', (ROOT / "Gates" / "render_options.h").read_text())
    options_cfg_from_the_start = {"PostCAS", "PostCASSharpness", "TonemapMode"}
    assert sorted(o for _, o in mapped) == sorted(set(options) - options_cfg_from_the_start)
    assert len({e for e, _ in mapped}) == len(mapped)
