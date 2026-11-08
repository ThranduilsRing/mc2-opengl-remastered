"""laptop_gate: payload delta pack and deploy_payload --from-payload sha256 checks."""
import ast
import csv
import hashlib
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

import laptop_gate  # noqa: E402
import deploy_payload  # noqa: E402


def _manifest(path, files):
    with open(path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["manifest_version", "v1"])
        w.writerow(["relpath", "sha256", "bytes", "src_commit", "timestamp", "mtime_ns"])
        for rel, data in files.items():
            w.writerow([rel, hashlib.sha256(data).hexdigest(), len(data), "abc", "t", 0])


def test_pack_ships_only_candidate_files_that_differ(tmp_path, monkeypatch):
    gate = tmp_path / "gate"
    install = gate / "drv/E/mc2-installs/install-stock-abc"   # the candidate, not INSTALL (the published v0.4)
    (install / "shaders").mkdir(parents=True)
    files = {"mc2.exe": b"new", "shaders/a.glsl": b"same", "shaders/b.glsl": b"changed"}
    for rel, data in files.items():
        (install / rel).write_bytes(data)
    _manifest(install / ".deployed_manifest.csv", files)
    published = tmp_path / "published"
    published.mkdir()
    _manifest(published / ".deployed_manifest.csv", {"mc2.exe": b"old"})
    (gate / "in" / "7").mkdir(parents=True)
    (gate / "out" / "7").mkdir(parents=True)
    _manifest(gate / "in" / "7" / "pc.manifest.csv", {"mc2.exe": b"old", "shaders/a.glsl": b"same", "shaders/b.glsl": b"old"})
    monkeypatch.setattr(laptop_gate, "G", gate.as_posix())
    monkeypatch.setattr(laptop_gate, "INSTALL", published.as_posix())
    laptop_gate.r_pack("7")
    import tarfile
    names = sorted(tarfile.open(gate / "out" / "7" / "payload.tar").getnames())
    assert names == [".deployed_manifest.csv", "files/mc2.exe", "files/shaders/b.glsl"]
    assert "2 files" in (gate / "out" / "7" / "payload.sha256").read_text()


def test_from_payload_rejects_a_tampered_file(tmp_path):
    payload, target = tmp_path / "payload", tmp_path / "target"
    (payload / "files").mkdir(parents=True)
    target.mkdir()
    _manifest(payload / ".deployed_manifest.csv", {"mc2.exe": b"good"})
    (payload / "files" / "mc2.exe").write_bytes(b"evil")
    with pytest.raises(SystemExit):
        deploy_payload.deploy_from_payload(payload, target, tmp_path, "0" * 40)
    assert not (target / "mc2.exe").exists()


def test_from_payload_rejects_drifted_unchanged_file(tmp_path):
    payload, target = tmp_path / "payload", tmp_path / "target"
    (payload / "files").mkdir(parents=True)
    target.mkdir()
    _manifest(payload / ".deployed_manifest.csv", {"shaders/a.glsl": b"expected"})
    (target / "shaders").mkdir()
    (target / "shaders" / "a.glsl").write_bytes(b"x")
    with pytest.raises(SystemExit):
        deploy_payload.deploy_from_payload(payload, target, tmp_path, "0" * 40)


@pytest.mark.parametrize("sm,rc", [({"verdict": "PASS"}, 0), ({"verdict": "FAIL"}, 1), ({}, 1), ({"gate_rc": 1}, 1)])
def test_driver_exits_zero_only_on_pass(sm, rc):
    assert laptop_gate.exit_code(sm) == rc


def test_ref_sums_cover_both_pinned_kits_and_refcheck_flags_a_bad_file(tmp_path, monkeypatch):
    text = laptop_gate.ref_sums(ROOT)
    rels = [line.split("  ", 1)[1] for line in text.splitlines()]
    assert any(r.startswith("packages/mc2-industrial-kit/") for r in rels)
    assert any(r.startswith("packages/mc2-vehicle-kit/") for r in rels)
    assert "scifi-kit/mapping/inventory.csv" in rels
    monkeypatch.setattr(laptop_gate, "REF", str(tmp_path))
    (tmp_path / "a.txt").write_bytes(b"x")
    good = hashlib.sha256(b"x").hexdigest()
    (tmp_path / "S").write_text(f"{good}  a.txt\n{good}  b.txt\n", encoding="utf-8")
    assert laptop_gate.r_refcheck(tmp_path / "S") == ["ref mismatch: b.txt"]
    assert laptop_gate.r_refcheck(tmp_path / "none") == [f"missing {tmp_path / 'none'}"]


def test_toolchain_problems_names_each_missing_tool_and_skips_sccache_when_the_cache_is_off(tmp_path, monkeypatch):
    present = tmp_path / "vcvars64.bat"
    present.write_text("")

    def no_cmake():
        raise SystemExit(1)
    cn = type("cn", (), {"VCVARS": str(present), "NINJA": str(tmp_path / "ninja.exe"),
                         "SCCACHE_EXE": str(tmp_path / "sccache.exe"), "find_cmake": staticmethod(no_cmake)})
    monkeypatch.setattr(laptop_gate, "JOBS", str(tmp_path / "JOBS"))
    off = laptop_gate.toolchain_problems(cn, {"MC2_COMPILE_CACHE": "0"})
    assert off == [f"missing NINJA: {tmp_path / 'ninja.exe'}", f"missing JOBS file: {tmp_path / 'JOBS'}",
                   "missing cmake (find_cmake: VS2022 BuildTools or PATH)"]
    assert f"missing SCCACHE_EXE: {tmp_path / 'sccache.exe'}" in laptop_gate.toolchain_problems(cn, {})
    assert laptop_gate.gate_env()["MC2_COMPILE_CACHE"] == "0"


def test_published_problems_fails_a_stale_or_unrelated_before_install():
    history = {("2d042f852b04", "claude/stage-g125"), ("c0eba1a0ac1c", "claude/stage-g125")}
    anc = lambda a, b: (a, b) in history  # noqa: E731
    assert laptop_gate.published_problems("2d042f852b04", "2d042f852b04", "claude/stage-g125", anc) == []
    stale = laptop_gate.published_problems("c0eba1a0ac1c", "2d042f852b04", "claude/stage-g125", anc)
    assert len(stale) == 1 and "!= the PC's published v0.4 2d042f852b04" in stale[0]
    assert "not an ancestor" in laptop_gate.published_problems("deadbeef0000", "", "claude/stage-g125", anc)[0]
    assert laptop_gate.published_problems(None, "x", None, anc) == ["published install mc2.exe has no MC2FP1 fingerprint"]
    assert laptop_gate.manifest_delta({"a": {"sha256": "1"}, "b": {"sha256": "2"}}, {"a": {"sha256": "1"}}) == ["b"]


def test_pack_falls_back_to_the_deployed_seed_once_the_stock_copy_is_gone(tmp_path, monkeypatch):
    monkeypatch.setattr(laptop_gate, "G", tmp_path.as_posix())
    seed = tmp_path / "drv/E/mc2-installs/_gate-reuse/deployed"
    seed.mkdir(parents=True)
    _manifest(seed / ".deployed_manifest.csv", {})
    assert laptop_gate.candidate_install() == seed
    monkeypatch.setattr(laptop_gate, "G", (tmp_path / "none").as_posix())
    with pytest.raises(SystemExit):
        laptop_gate.candidate_install()


def _tree(root, files):
    import os
    for rel, (data, mtime) in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
        os.utime(p, (mtime, mtime))


def test_tree_diff_both_ways_and_sha_mode_touches_equal_content(tmp_path):
    pc, lap = tmp_path / "pc", tmp_path / "lap"
    _tree(pc, {"same.bin": (b"a", 100), "data/textures/missing.tga": (b"t", 100), "moved.bin": (b"m", 200),
               "edit.bin": (b"new", 300)})
    _tree(lap, {"same.bin": (b"a", 100), "moved.bin": (b"m", 999), "edit.bin": (b"old", 300),
                "data/terrain_layers/stale.ktx2": (b"s", 1), ".seeded": (b"ok", 1)})
    rows = laptop_gate.tree_rows
    assert laptop_gate.tree_diff(rows(pc, sha=True), rows(lap, sha=True)) == (
        ["data/textures/missing.tga", "edit.bin"], ["moved.bin"], ["data/terrain_layers/stale.ktx2"])
    # size+mtime: edit.bin (same size and mtime) is invisible; that is what the first sha-mode sync is for
    assert laptop_gate.tree_diff(rows(pc), rows(lap)) == (
        ["data/textures/missing.tga", "moved.bin"], [], ["data/terrain_layers/stale.ktx2"])
    bad = laptop_gate.tree_problems(rows(pc), rows(lap))
    assert bad[0].startswith("install tree 4:") and len(bad) == 5 and "pc-only data/textures/missing.tga|1|100" in bad
    assert laptop_gate.tree_problems(rows(pc, sha=True), rows(pc)) == []
    assert laptop_gate.chunks(["a", "b", "c"], {"a": 3, "b": 3, "c": 9}, limit=6) == [["a", "b"], ["c"]]
    # runtime outputs a game run rewrites mid-sync are not compared (W129 run 4)
    _tree(lap, {"cache/vulkan/pipe.bin": (b"p", 5), "veg_diag.txt": (b"v", 5)})
    assert not any(r.startswith(("cache/", "veg_diag.txt")) for r in rows(lap))


def test_extract_payload_clears_the_previous_payload(tmp_path):
    import tarfile
    (tmp_path / "payload/files").mkdir(parents=True)
    (tmp_path / "payload/files/stale.bin").write_bytes(b"old run")
    (tmp_path / "new.bin").write_bytes(b"x")
    with tarfile.open(tmp_path / "payload.tar", "w") as tar:
        tar.add(tmp_path / "new.bin", arcname="files/new.bin")
    (tmp_path / "payload.sha256").write_text(f"{laptop_gate.sha256(tmp_path / 'payload.tar')}  payload.tar\n")
    laptop_gate.extract_payload(tmp_path)
    assert [p.name for p in (tmp_path / "payload/files").iterdir()] == ["new.bin"]


def _module_level_imports(path):
    names = set()
    for node in ast.parse(path.read_text(encoding="utf-8")).body:
        if isinstance(node, (ast.Import, ast.ImportFrom, ast.Try)):
            for n in ast.walk(node):
                if isinstance(n, ast.Import):
                    names |= {a.name.split(".")[0] for a in n.names}
                elif isinstance(n, ast.ImportFrom) and n.module and not n.level:
                    names.add(n.module.split(".")[0])
    return names - set(sys.stdlib_module_names) - {"__future__"}


def test_every_module_the_remote_half_imports_is_provisioned():
    """The remote half runs from C:/mc2gate/bin alone (w126: console_child missing -> ModuleNotFoundError on --remote init).
    Every non-stdlib module laptop_gate.py or a helper imports at module level must be a HELPER provision() uploads."""
    shipped = {Path(h).stem for h in laptop_gate.HELPERS}
    uploaded = {rel for rel, _src in laptop_gate.provision_files(eroot=ROOT)}
    todo, seen = [ROOT / "tools" / "laptop_gate.py"], set()
    while todo:
        path = todo.pop()
        for mod in _module_level_imports(path):
            assert mod in shipped, f"{path.name} imports {mod} at module level; add {mod}.py to laptop_gate.HELPERS"
            if mod not in seen:
                seen.add(mod)
                todo.append(ROOT / "scripts" / f"{mod}.py")
    for helper in laptop_gate.HELPERS:
        assert (ROOT / "scripts" / helper).is_file()
        assert "bin/" + helper in uploaded
    assert "bin/laptop_gate.py" in uploaded
    assert {"drv/E/" + rel for rel in laptop_gate.E_EXTRA} <= uploaded
    # every generated deploy input the PC cooks from a PC-only source reaches the laptop clone (W129 run 3 pixel red)
    layers = {r for r in deploy_payload.TERRAIN_LAYER_PAYLOAD if "concrete_detail" in r or "cliff_rock" in r}
    assert layers == set(laptop_gate.LAYER_EXTRA)
    assert {"drv/A/Games/mc2-main/" + rel for rel in layers} <= uploaded


def test_provision_problems_names_missing_and_stale_files(tmp_path):
    (tmp_path / "bin").mkdir()
    assert "missing" in laptop_gate.provision_problems(tmp_path)[0]
    (tmp_path / "bin" / "console_child.py").write_bytes(b"ok")
    (tmp_path / "bin" / "stale.py").write_bytes(b"old")
    want = hashlib.sha256(b"ok").hexdigest()
    (tmp_path / "bin" / "PROVISION.sha256").write_text(
        f"{want}  bin/console_child.py\n{want}  bin/stale.py\n{want}  drv/E/catalog.json\n", encoding="utf-8")
    bad = laptop_gate.provision_problems(tmp_path)
    assert len(bad) == 2 and "stale" in bad[0] and "catalog.json" in bad[1]


@pytest.mark.parametrize("art_rc,inventory,reds", [(0, [], 0), (1, [], 1), (0, ["shader inventory: x"], 1), (1, ["y"], 2)])
def test_fast_red_reports_art_suite_and_shader_inventory_before_the_gate(tmp_path, monkeypatch, art_rc, inventory, reds):
    """W129: both laptop-environment reds surfaced only after a full gate; fast_red names each one before merge_gate runs."""
    monkeypatch.setattr(laptop_gate, "run", lambda cmd, log, **kw: (art_rc, 1.0))
    monkeypatch.setattr(deploy_payload, "shader_inventory_problems", lambda src, published: inventory)
    bad = laptop_gate.fast_red(tmp_path / "preflight.log", {})
    assert len(bad) == reds
    assert (tmp_path / "preflight.log").read_text().strip()


def test_from_payload_rejects_same_length_drift_of_an_unchanged_file(tmp_path):
    """p29 P1: the untouched-file check compares destination BYTES, not just the manifest size."""
    payload, target = tmp_path / "payload", tmp_path / "target"
    (payload / "files").mkdir(parents=True)
    (target / "shaders").mkdir(parents=True)
    _manifest(payload / ".deployed_manifest.csv", {"shaders/a.glsl": b"expected"})
    (target / "shaders" / "a.glsl").write_bytes(b"EXPECTED")   # same length, different bytes
    with pytest.raises(SystemExit):
        deploy_payload.deploy_from_payload(payload, target, tmp_path, "0" * 40)
