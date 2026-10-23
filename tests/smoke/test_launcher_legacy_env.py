"""The launcher maps the retired render-option keys of launcher_env.json to options.cfg once (Gates/render_options_legacy_env.h).

WHY: a player whose launcher_env.json says "MC2_CPU_PHONG_RETIRE": "0" must keep CSM off after the options moved into options.cfg; before the
migration the key was ignored and the choice reset silently. The tests run the real launcher headless (`--sync-options`) on a temp copy.
Skipped when the launcher is not built in build64-ninja.
"""
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child  # noqa: E402  (windowless console children, scripts/console_child.py)

NINJA_TREE = Path(os.environ.get("MC2_NINJA_TREE", ROOT / "build64-ninja"))
LAUNCHER = NINJA_TREE / "out" / "mc2_launcher" / "mc2-launcher.exe"
needs_launcher = pytest.mark.skipif(not LAUNCHER.is_file(), reason="mc2-launcher.exe is not built in build64-ninja")

# The launcher these tests run must be built from the source on disk: a test against an exe older than its sources grades the old code. The launcher's
# object file has a dependency record in Ninja's log (every header it really includes), so the exe is compared with exactly those files and a merge that only
# touches unrelated files cannot turn this red. Without the record the four files the tests exercise are compared by modification time.
LAUNCHER_SOURCES = [ROOT / "tools" / "mc2_launcher" / "mc2_launcher.cpp", ROOT / "Gates" / "render_options.h",
                    ROOT / "Gates" / "render_options_cfg.h", ROOT / "Gates" / "render_options_legacy_env.h"]
LAUNCHER_OBJ = "out/tools/mc2_launcher/CMakeFiles/mc2_launcher.dir/mc2_launcher.cpp.obj"


def _recorded_inputs():
    """The files Ninja recorded as inputs of the launcher's object (its source and every header it includes); None when there is no record."""
    try:
        from configure_ninja import NINJA
        r = subprocess.run([NINJA, "-C", str(NINJA_TREE), "-t", "deps", LAUNCHER_OBJ], capture_output=True, text=True, timeout=120,
                           **console_child.child_kwargs(redirected=True))
    except (ImportError, OSError, subprocess.SubprocessError):
        return None
    paths = [NINJA_TREE / ln.strip() for ln in r.stdout.splitlines() if ln.startswith((" ", "\t")) and ln.strip()]
    return ([LAUNCHER_SOURCES[0]] + paths) if r.returncode == 0 and paths else None


@pytest.fixture(autouse=True)
def launcher_is_current(record_property):
    if not LAUNCHER.is_file():
        return
    built = LAUNCHER.stat().st_mtime
    record_property("launcher_exe", str(LAUNCHER))
    inputs = _recorded_inputs() or LAUNCHER_SOURCES
    stale = sorted({p.name for p in inputs if p.is_file() and p.stat().st_mtime > built + 1})
    assert not stale, f"{LAUNCHER} is older than {stale}: rebuild it (py -3 scripts/ninja_build.py mc2_launcher) before trusting this test"

CFG = "FITini \r\nFITini \r\n\r\n[MechCommander2]\r\nl MusicVolume = 128\r\nb CpuPhongRetire = TRUE\r\nb SunAxisFix = FALSE\r\n"


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


def sync_file(d):
    return json.loads((d / "launcher_env.json").read_text())


@needs_launcher
def test_old_choices_survive_into_options_cfg_and_the_legacy_keys_are_dropped(tmp_path):
    d = install(tmp_path, {"MC2_CPU_PHONG_RETIRE": "0", "MC2_SUN_AXIS_FIX": "1", "MC2_PROP_SHADOW_RECEIVE": "1",
                           "MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR": "0.3", "MC2_FPS_CAP": "165", "last_campaign": "mymod"})
    sync(d)
    assert cfg_value(d, "CpuPhongRetire") == "FALSE"          # the old "CSM off" is kept, over the TRUE the file had
    assert cfg_value(d, "SunAxisFix") == "TRUE"
    assert cfg_value(d, "PropShadowReceive") is None
    assert cfg_value(d, "StaticPropIblSunRotateFloor") == "0.3"
    assert cfg_value(d, "MusicVolume") == "128"          # lines the launcher does not own are untouched
    after = json.loads((d / "launcher_env.json").read_text())
    assert not {"MC2_CPU_PHONG_RETIRE", "MC2_SUN_AXIS_FIX", "MC2_PROP_SHADOW_RECEIVE", "MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR"} & set(after)
    assert after["MC2_FPS_CAP"] == "165" and after["last_campaign"] == "mymod"   # the rest of the file is kept
    lines = log_lines(d)
    assert len(lines) == 1 and "found=3" in lines[0] and "applied=3" in lines[0] and "CpuPhongRetire=FALSE" in lines[0]


@needs_launcher
def test_the_migration_happens_once(tmp_path):
    d = install(tmp_path, {"MC2_CPU_PHONG_RETIRE": "0"})
    sync(d)
    sync(d)
    assert cfg_value(d, "CpuPhongRetire") == "FALSE"
    assert len(log_lines(d)) == 1


@needs_launcher
def test_a_value_already_stored_under_the_new_name_wins(tmp_path):
    d = install(tmp_path, {"MC2_CPU_PHONG_RETIRE": "0", "CpuPhongRetire": "1"})
    sync(d)
    assert cfg_value(d, "CpuPhongRetire") == "TRUE"
    assert "kept_existing=1" in log_lines(d)[0] and "applied=0" in log_lines(d)[0]
    assert "MC2_CPU_PHONG_RETIRE" not in json.loads((d / "launcher_env.json").read_text())


@needs_launcher
def test_a_meaningless_old_value_keeps_the_default_and_is_counted(tmp_path):
    d = install(tmp_path, {"MC2_STATIC_PROP_IBL_SUN_ROTATE_FLOOR": "banana", "MC2_SUN_AXIS_FIX": "yes please"})
    sync(d)
    assert cfg_value(d, "StaticPropIblSunRotateFloor") == "0.45"   # the engine default: no guess was made from "banana"
    assert cfg_value(d, "SunAxisFix") == "FALSE"
    assert "unmapped=2" in log_lines(d)[0]


@needs_launcher
def test_a_legacy_key_overrides_the_options_cfg_value_of_its_row(tmp_path):
    # options.cfg is not a newer choice: the launcher writes every row's default there at Launch, so only a new NAME in launcher_env.json wins.
    d = install(tmp_path, {"MC2_STATIC_PROP_IBL_SH": "1"}, cfg=CFG + "b StaticPropIblSh = FALSE\r\n")
    sync(d)
    assert cfg_value(d, "StaticPropIblSh") == "TRUE"
    assert "applied=1" in log_lines(d)[0]


@needs_launcher
def test_keys_of_deleted_gates_are_counted_named_and_dropped_once(tmp_path):
    d = install(tmp_path, {"MC2_TERRAIN_POM": "1", "MC2_GONE_GATE": "1", "MC2_CLOUD_SHADOW": "1", "MC2_FPS_CAP": "165",
                           "last_campaign": "MC2_TERRAIN_POM", "last_base": "mymod"})
    sync(d)
    after = sync_file(d)
    assert "MC2_TERRAIN_POM" not in after and "MC2_GONE_GATE" not in after            # gone
    assert after["MC2_CLOUD_SHADOW"] == "1" and after["MC2_FPS_CAP"] == "165"             # live rows and graphics keys stay
    assert after["last_campaign"] == "MC2_TERRAIN_POM" and after["last_base"] == "mymod"   # a value that looks like a key is a value
    lines = log_lines(d)
    assert len(lines) == 1 and "found=0" in lines[0] and "dropped_unknown=2" in lines[0]
    assert "MC2_TERRAIN_POM" in lines[0] and "MC2_GONE_GATE" in lines[0] and "MC2_CLOUD_SHADOW" not in lines[0]
    sync(d)
    assert len(log_lines(d)) == 1                                                          # the second start finds nothing to do


@needs_launcher
def test_the_names_in_the_log_line_are_capped_at_eight_but_the_count_is_complete(tmp_path):
    d = install(tmp_path, {f"MC2_DEAD_GATE_{i:02d}": "1" for i in range(11)})
    sync(d)
    line = log_lines(d)[0]
    assert "dropped_unknown=11" in line and line.count("MC2_DEAD_GATE_") == 8 and "+3_more" in line


@needs_launcher
def test_headless_sync_of_a_minimal_file_writes_the_gui_graphics_defaults(tmp_path):
    d = install(tmp_path, {"MC2_CPU_PHONG_RETIRE": "0"})        # no MC2_STATICPROP_TEX_TIER / MC2_SHADOW_MAP_SIZE in the file
    sync(d)
    after = sync_file(d)
    assert after["MC2_STATICPROP_TEX_TIER"] == "512" and cfg_value(d, "ShadowQuality") == "2" and "MC2_SHADOW_MAP_SIZE" not in after   # tier High / shadows High, not index 0


@needs_launcher
def test_a_failed_rewrite_is_logged_as_failed_and_leaves_the_keys_for_the_next_start(tmp_path):
    d = install(tmp_path, {"MC2_CPU_PHONG_RETIRE": "0"})
    (d / "launcher_env.json.tmp").mkdir()                   # the atomic replace cannot create its temporary file
    before = (d / "launcher_env.json").read_bytes()
    sync(d)
    assert (d / "launcher_env.json").read_bytes() == before
    lines = log_lines(d)
    assert len(lines) == 1 and "event=migrate_failed" in lines[0] and "migrated_legacy_keys" not in lines[0]
    (d / "launcher_env.json.tmp").rmdir()
    sync(d)                                                 # writable again: the next start does the migration
    assert "event=migrated_legacy_keys" in log_lines(d)[-1] and "MC2_CPU_PHONG_RETIRE" not in sync_file(d)


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
    options_cfg_from_the_start = {"PostCAS", "PostCASSharpness", "TonemapMode", "ShadowQuality", "SpatialAA"}
    assert sorted(o for _, o in mapped) == sorted(set(options) - options_cfg_from_the_start)
    assert len({e for e, _ in mapped}) == len(mapped)
