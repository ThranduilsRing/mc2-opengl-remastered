"""gate-parity: the PC pre-gate matches the laptop gate (W138h x4 and W147 x4 laptop runs). Offline; no game, no gate run.

Replays: (1) gate_gen --check without --rebuild (MC2_VEGETATION_MAX json 5M vs C++ 10M, W147 run 3); (2) EditRel built by default on a host
without MFC (W147 runs 1-2, afxwin.h missing); (3) a gitignored PC-cooked output absent or stale (tree sidecars, grass turf normal)."""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import types
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

from tools import lane_merge as lm  # noqa: E402
import console_child  # noqa: E402
import cooked_outputs  # noqa: E402
import gate_hosts  # noqa: E402
import laptop_gate  # noqa: E402
import deploy_payload  # noqa: E402
import sim_determinism as sd  # noqa: E402
from visual_verify import ab  # noqa: E402


def _r(rc, out=""):
    return types.SimpleNamespace(returncode=rc, stdout=out)


# 1) gate_gen --rebuild ---------------------------------------------------------------------------------------------------------

def test_pre_gate_gate_gen_runs_the_laptop_contracts_form(monkeypatch, tmp_path, capsys):
    seen = []
    monkeypatch.setattr(lm, "run", lambda r, cmd, env=None: seen.append(cmd) or _r(0))
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("OK", ""))
    assert lm.pre_gate(str(tmp_path)) == 0
    gg = [c for c in seen if any(a.endswith("gate_gen.py") for a in c)]
    assert len(gg) == 1 and gg[0][-2:] == ["--check", "--rebuild"]
    row = lm.contracts_row_command(str(ROOT), "gate_gen_check")
    assert row and "--rebuild" in row
    assert lm.command_parity(str(tmp_path), gg[0], row).startswith("PARTIAL")   # the row also runs a pytest after it


def test_gate_gen_without_rebuild_is_reported_as_differing():
    row = lm.contracts_row_command(str(ROOT), "gate_gen_check")
    no_rebuild = [sys.executable, "-B", str(ROOT / "tools" / "gate_gen.py"), "--check"]
    assert lm.command_parity(str(ROOT), no_rebuild, row).startswith("DIFFERS")
    same = [sys.executable, "-B", str(ROOT / "tools" / "gate_gen.py"), "--check", "--rebuild"]
    assert lm.command_parity(str(ROOT), same, "py -3 tools/gate_gen.py --check --rebuild") == "SAME"
    assert lm.command_parity(str(ROOT), same, None) == "no contracts row"


# 2) host capabilities ----------------------------------------------------------------------------------------------------------

def test_laptop_has_no_mfc_and_the_editor_stage_is_skipped_there():
    assert gate_hosts.capability("mfc", "pc") is True
    assert gate_hosts.capability("mfc", "laptop") is False
    assert gate_hosts.capability("mfc", "some-new-host") is False   # fail closed
    verdict, detail = gate_hosts.editor_stage("laptop")
    assert verdict == "SKIPPED" and "mfc=no" in detail
    assert gate_hosts.editor_stage("pc")[0] == "RUN"


def test_current_host_comes_from_the_env_then_the_default():
    assert gate_hosts.current_host({}) == "pc"
    assert gate_hosts.current_host({"MC2_GATE_HOST": " Laptop "}) == "laptop"


def test_the_laptop_gate_environment_names_the_laptop_host():
    env = laptop_gate.gate_env()
    assert env["MC2_GATE_HOST"] == "laptop"
    assert gate_hosts.capability("mfc", env["MC2_GATE_HOST"]) is False


def test_pre_gate_prints_the_editor_stage_line(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(lm, "run", lambda r, cmd, env=None: _r(0))
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("OK", ""))
    monkeypatch.setenv("MC2_GATE_HOST", "laptop")
    assert lm.pre_gate(str(tmp_path)) == 0
    assert "pre-gate: editor-stage: SKIPPED (host laptop: mfc=no" in capsys.readouterr().out


def test_cmake_default_targets_read_the_host_table():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    assert "include(cmake/GateHosts.cmake)" in text and "NOT MC2_EDITOR_IN_ALL OR NOT MC2_HOST_MFC" in text


def _cmake():
    found = shutil.which("cmake")
    if found:
        return found
    cache = Path("A:/Games/mc2-main/build64-ninja/CMakeCache.txt")
    if cache.is_file():
        for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
            if line.startswith("CMAKE_COMMAND:INTERNAL=") and Path(line.split("=", 1)[1]).is_file():
                return line.split("=", 1)[1]
    return None


@pytest.mark.parametrize("host,want", [("laptop", "OFF"), ("pc", "ON"), ("unknown-host", "OFF"), ("", "ON")])
def test_cmake_module_sets_mc2_host_mfc(tmp_path, host, want):
    cmake = _cmake()
    if not cmake:
        pytest.skip("no cmake on this host")
    script = tmp_path / "probe.cmake"
    script.write_text(f'include("{(ROOT / "cmake" / "GateHosts.cmake").as_posix()}")\nmc2_read_gate_host_caps()\nmessage("RESULT MFC=${{MC2_HOST_MFC}}")\n')
    env = {k: v for k, v in os.environ.items() if k != "MC2_GATE_HOST"}
    if host:
        env["MC2_GATE_HOST"] = host
    # background-launch: allow cmake -P runs a script file, windowless (child_kwargs); no engine, no window
    r = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True, env=env, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr
    assert f"RESULT MFC={want}" in r.stdout + r.stderr


# 3) the cooked-output manifest -------------------------------------------------------------------------------------------------

FAKE_COOK = b"x"   # what _tree writes for every required single-file output


@pytest.fixture(autouse=True)
def _fake_pin_hashes(monkeypatch):
    """The real manifest pins the real 22 MB grass turf bytes; fixture trees write FAKE_COOK, so pin its hash instead."""
    real = cooked_outputs.load()
    for r in real["unpinned"]:
        if r.get("sha256"):
            r["sha256"] = hashlib.sha256(FAKE_COOK).hexdigest()
    monkeypatch.setattr(cooked_outputs, "load", lambda path=cooked_outputs.MANIFEST: real)


def _tree(tmp_path, stems=("a_oak", "b_pine"), grass=True):
    """A tree with every cook script and the tree pin file the manifest names, plus (optionally) the cooked outputs at the pinned bytes."""
    m = cooked_outputs.load()
    (tmp_path / "tools").mkdir(parents=True, exist_ok=True)
    shutil.copy(cooked_outputs.MANIFEST, tmp_path / "tools" / "cooked_outputs.json")
    for e in m["pinned"]:
        (tmp_path / e["cook"]).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / e["cook"]).write_text("# cook")
    for r in m["unpinned"]:
        (tmp_path / r["cook"]).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / r["cook"]).write_text("# cook")
    pins = {}
    for stem in stems:
        data = stem.encode()
        pins[stem] = {"ktx2_sha256": hashlib.sha256(data).hexdigest()}
        out = tmp_path / "data" / "tgl" / "128" / f"{stem}.ktx2"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(data)
    pin = tmp_path / m["pinned"][0]["pin_file"]
    pin.parent.mkdir(parents=True, exist_ok=True)
    pin.write_text(json.dumps(pins))
    if grass:
        for r in m["unpinned"]:
            if r["required"]:
                (tmp_path / r["out"]).parent.mkdir(parents=True, exist_ok=True)
                (tmp_path / r["out"]).write_bytes(b"x")
    return m


def test_a_complete_cooked_tree_has_no_problems(tmp_path):
    _tree(tmp_path)
    assert cooked_outputs.problems(str(tmp_path)) == []
    assert lm.cooked_outputs_check(str(tmp_path)) == ("OK", "")


def test_missing_tree_sidecars_are_a_clear_pre_gate_red(tmp_path):   # W138h: only-in-B tree sidecars on the laptop compare
    _tree(tmp_path)
    for stem in ("a_oak", "b_pine"):
        (tmp_path / "data" / "tgl" / "128" / f"{stem}.ktx2").unlink()
    bad = cooked_outputs.problems(str(tmp_path))
    assert len(bad) == 1 and "2 of 2 tree_upscale outputs absent" in bad[0] and "gen_stock_trees.py" in bad[0]
    verdict, detail = lm.cooked_outputs_check(str(tmp_path))
    assert verdict == "FAIL" and "tree_upscale" in detail


def test_stale_tree_sidecar_is_not_the_pinned_bytes(tmp_path):
    _tree(tmp_path)
    (tmp_path / "data" / "tgl" / "128" / "a_oak.ktx2").write_bytes(b"old cook")
    bad = cooked_outputs.problems(str(tmp_path))
    assert len(bad) == 1 and "1 of 2 tree_upscale outputs not the pinned bytes: data/tgl/128/a_oak.ktx2" in bad[0]


def test_missing_grass_turf_normal_is_a_clear_pre_gate_red(tmp_path):   # W147 run 4: only-in-B grass_turf_normal.ktx2
    _tree(tmp_path)
    (tmp_path / "data" / "terrain_layers" / "grass_turf_normal.ktx2").unlink()
    bad = cooked_outputs.problems(str(tmp_path))
    assert bad == ["required cooked output data/terrain_layers/grass_turf_normal.ktx2 is absent; "
                   "cook with tools/mc2texcook/cook_grass_turf.py (or deploy_payload.py) before the gate"]


def test_optional_albedo_layers_are_not_required(tmp_path):
    _tree(tmp_path)
    assert not (tmp_path / "data" / "terrain_layers" / "rock_albedo.ktx2").exists()
    assert cooked_outputs.problems(str(tmp_path)) == []


def test_missing_cook_script_or_pin_file_is_named(tmp_path):
    _tree(tmp_path)
    (tmp_path / "tools" / "mc2texcook" / "cook_grass_turf.py").unlink()
    (tmp_path / "data" / "kit" / "tree-upscale.json").unlink()
    bad = "\n".join(cooked_outputs.problems(str(tmp_path)))
    assert "cook script tools/mc2texcook/cook_grass_turf.py does not exist" in bad
    assert "pin file data/kit/tree-upscale.json does not exist" in bad


def test_pre_gate_fails_on_a_cooked_output_red(monkeypatch, tmp_path, capsys):
    monkeypatch.setattr(lm, "run", lambda r, cmd, env=None: _r(0))
    monkeypatch.setattr(lm, "data_fingerprint_check", lambda r: ("OK", ""))
    monkeypatch.setattr(lm, "cooked_outputs_check", lambda r: ("FAIL", "required cooked output x is absent"))
    assert lm.pre_gate(str(tmp_path)) == 1
    out = capsys.readouterr().out
    assert "pre-gate: cooked_outputs: FAIL" in out and "PRE-GATE FAIL: cooked_outputs" in out


def test_cooked_outputs_check_skips_a_tree_without_the_manifest(tmp_path):
    assert lm.cooked_outputs_check(str(tmp_path))[0] == "SKIPPED"


def test_the_manifest_agrees_with_deploy_payload():
    m = cooked_outputs.load()
    assert {r["out"] for r in m["unpinned"]} == set(deploy_payload.TERRAIN_LAYER_PAYLOAD)
    tree = cooked_outputs.pinned_entry("tree_upscale", m)
    assert tree["pin_file"] == deploy_payload.TREE_UPSCALE_PINS and tree["cook"] == deploy_payload.TREE_UPSCALE_COOK
    pins = json.loads((ROOT / tree["pin_file"]).read_text(encoding="utf-8"))
    assert set(cooked_outputs.expand_pins(tree, pins)) == set(deploy_payload.tree_upscale_payload(str(ROOT)))
    by_out = {r["out"]: r["cook"] for r in m["unpinned"]}
    assert by_out[deploy_payload.GRASS_TURF_REL] == deploy_payload.GRASS_TURF_COOK
    assert by_out["data/terrain_layers/concrete_detail_pad.ktx2"] == deploy_payload.CONCRETE_DETAIL_COOK
    assert cooked_outputs.manifest_problems(m, str(ROOT)) == []


def test_every_reader_takes_its_list_from_the_manifest():
    m = cooked_outputs.load()
    # compare-captures
    assert set(ab.COOKED_PAYLOAD_PINS) == {e["pin_file"] for e in m["pinned"]}
    assert ab.cooked_unpinned({"tools/mc2texcook/cook_grass_turf.py"}) == {"data/terrain_layers/grass_turf_normal.ktx2"}
    assert {"data/terrain_layers/concrete_detail_road.ktx2"} <= ab.cooked_unpinned({"tools/mc2texcook/cook_concrete_detail.py"})
    # data fingerprint
    assert sd.TREE_UPSCALE_PINS.replace("\\", "/").endswith("data/kit/tree-upscale.json")
    pins = json.loads((ROOT / "data/kit/tree-upscale.json").read_text(encoding="utf-8"))
    assert set(sd.render_only_sidecar_sizes()) == {f"data/tgl/128/{s}.ktx2" for s in pins}
    # laptop provision: published + worktree cover every required output, once
    prov = set(laptop_gate.cooked_extra("published", ROOT)) | set(laptop_gate.cooked_extra("worktree", ROOT))
    assert {r["out"] for r in m["unpinned"] if r["required"]} <= prov
    assert set(cooked_outputs.expand_pins(m["pinned"][0], pins)) <= prov
    assert not set(laptop_gate.cooked_extra("published", ROOT)) & set(laptop_gate.cooked_extra("worktree", ROOT))


def test_stock_data_contract_reads_the_manifest_shape():
    # background-launch: allow runs the stock-data checker with the current Python, windowless; no engine, no window
    r = subprocess.run([sys.executable, "-B", str(ROOT / "scripts" / "check-stock-mission-data.py"), "--no-data", "--quiet"],
                       capture_output=True, text=True, **console_child.child_kwargs(redirected=True))
    assert r.returncode == 0, r.stderr


# 4) the parity table -----------------------------------------------------------------------------------------------------------

def test_parity_table_lists_every_gate_row():
    import merge_gate
    rows = [s[0] for s in merge_gate.plan(1)]
    steps = [("gate_gen", [sys.executable, "-B", str(ROOT / "tools" / "gate_gen.py"), "--check", "--rebuild"]),
             ("stock_mission_data", [sys.executable, "-B", str(ROOT / "scripts" / "check-stock-mission-data.py"), "--no-data", "--quiet"])]
    text = "\n".join(lm.parity_table(str(ROOT), [s[0] for s in steps], steps))
    for row in rows:
        if row != "contracts":
            assert f"  {row} | NOT RUN" in text, row
    assert "gate_gen_check | gate_gen: PARTIAL" in text
    assert "stock_mission_data | stock_mission_data: SAME" in text
    assert "compare-captures | NOT RUN (inputs checked by cooked_outputs)" in text
    assert "determinism-mc2_10 | NOT RUN (inputs checked by data_fingerprint)" in text


def test_parity_table_names_the_inputs_row_for_deploy(tmp_path):
    lines = lm.parity_table(str(tmp_path), [], [], gate_rows=["deploy"])
    assert lines[1] == "  deploy | NOT RUN (inputs checked by exe-fingerprint)"


# 5) review extras (W147 Argon review): the grass turf normal is pinned by hash --------------------------------------------------

def test_a_stale_grass_turf_normal_is_not_the_pinned_bytes(tmp_path):
    _tree(tmp_path)
    (tmp_path / "data" / "terrain_layers" / "grass_turf_normal.ktx2").write_bytes(b"an older cook")
    bad = cooked_outputs.problems(str(tmp_path))
    assert len(bad) == 1 and "grass_turf_normal.ktx2 is not the pinned bytes" in bad[0]


def test_the_real_manifest_pins_the_grass_turf_sha256():
    real = json.loads(Path(cooked_outputs.MANIFEST).read_text(encoding="utf-8"))
    pinned = {r["out"]: r.get("sha256") for r in real["unpinned"]}
    sha = pinned["data/terrain_layers/grass_turf_normal.ktx2"]
    assert sha and len(sha) == 64 and int(sha, 16) >= 0


def test_compare_captures_accepts_the_grass_turf_only_at_the_pinned_bytes():
    real = json.loads(Path(cooked_outputs.MANIFEST).read_text(encoding="utf-8"))
    out = "data/terrain_layers/grass_turf_normal.ktx2"
    sha = {r["out"]: r.get("sha256") for r in real["unpinned"]}[out]
    changed = {"tools/mc2texcook/cook_grass_turf.py"}
    head = "HEAD"   # the committed manifest carries the pin
    assert ab.cooked_unpinned_accepted(changed, head, {out: sha}) == {out}
    assert ab.cooked_unpinned_accepted(changed, head, {out: "0" * 64}) == set()   # other bytes: refused
    assert ab.cooked_unpinned_accepted(changed, head, {}) == set()                # not in the capture: refused
    assert ab.cooked_unpinned_accepted({"code/mech.cpp"}, head, {out: sha}) == set()
    assert ab.cooked_unpinned_accepted(changed, "no-such-commit", {out: sha}) == set()   # unreadable manifest: fail closed


def test_laptop_gate_refuses_an_absent_or_stale_grass_turf_before_uploading(tmp_path):
    _tree(tmp_path)
    laptop_gate.cooked_preflight(tmp_path)   # clean tree: no exit
    grass = tmp_path / "data" / "terrain_layers" / "grass_turf_normal.ktx2"
    grass.write_bytes(b"stale")
    with pytest.raises(SystemExit) as stale:
        laptop_gate.cooked_preflight(tmp_path)
    assert "grass_turf_normal.ktx2 is not the pinned bytes" in str(stale.value)
    grass.unlink()
    with pytest.raises(SystemExit) as absent:
        laptop_gate.cooked_preflight(tmp_path)
    assert "grass_turf_normal.ktx2 is absent" in str(absent.value)


# (c) the deploy-fixture cook tolerance is scoped to test modules that deploy, and does not depend on import order ----------------

def test_stock_tree_tolerance_is_scoped_by_the_test_modules_source(tmp_path):
    import importlib.util   # by path: `import conftest` is whichever conftest the suite imported first
    spec = importlib.util.spec_from_file_location("scripts_tests_conftest", Path(__file__).with_name("conftest.py"))
    conftest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(conftest)
    dep = tmp_path / "test_dep.py"
    dep.write_text("import deploy_payload\n")
    pkg = tmp_path / "test_pkg.py"
    pkg.write_text("from scripts import deploy_payload\n")
    both = tmp_path / "test_both.py"
    both.write_text("import deploy_payload\nimport scripts.deploy_payload\n")
    other = tmp_path / "test_other.py"
    other.write_text("import json\n")
    mod = lambda p: types.SimpleNamespace(__file__=str(p))
    assert conftest.deploy_modules_named_by(mod(dep)) == ["deploy_payload"]
    assert "scripts.deploy_payload" in conftest.deploy_modules_named_by(mod(pkg))
    assert conftest.deploy_modules_named_by(mod(both)) == ["scripts.deploy_payload", "deploy_payload"]
    assert conftest.deploy_modules_named_by(mod(other)) == []
    assert conftest.deploy_modules_named_by(types.SimpleNamespace()) == []


def test_stock_tree_tolerance_applies_only_to_deploying_modules(request):
    # this module names no deploy_payload by import but mentions it in source -> it IS a deploying module here; the patch must be on
    assert importlib_import("deploy_payload").ALLOW_STOCK_TREES is True


def importlib_import(name):
    import importlib
    return importlib.import_module(name)
