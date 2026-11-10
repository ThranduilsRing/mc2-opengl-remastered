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


def test_sim_records_deployed_payload_identity_without_game(tmp_path, monkeypatch):
    from contextlib import nullcontext
    import gate_reuse
    import merge_gate
    import json
    seed = tmp_path / "seed"
    seed.mkdir()
    _manifest(seed / ".deployed_manifest.csv", {"mc2.exe": b"exe"})
    monkeypatch.setattr(laptop_gate, "G", tmp_path.as_posix())
    monkeypatch.setattr(laptop_gate, "r_subst", lambda: None)
    monkeypatch.setattr(laptop_gate, "gate_env", lambda: {})
    monkeypatch.setattr(laptop_gate, "sim_checkout", lambda *args: ("a" * 40, ""))
    monkeypatch.setattr(gate_reuse, "deployed_seed", lambda *args: seed)
    monkeypatch.setattr(merge_gate, "stock_copy", lambda *args, **kwargs: nullcontext(seed))
    monkeypatch.setattr(merge_gate, "ROW_RESULTS", {})
    def execute(steps, head):
        merge_gate.ROW_RESULTS.update({name: {"status": "PASS"} for name, _, _ in steps})
        return 0
    monkeypatch.setattr(merge_gate, "execute_parallel", execute)
    laptop_gate._r_sim("test", [])
    result = json.loads((tmp_path / "out/test/sim/summary.json").read_text())
    assert result["verdict"] == "PASS"
    assert result["manifest_sha256"] == hashlib.sha256((seed / ".deployed_manifest.csv").read_bytes()).hexdigest()


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
    assert laptop_gate.gate_env()["MC2_HOST_INFRA_OK"] == "1"   # the laptop (only) reports tool/data-less rows INFRA instead of red


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


def test_pack_skips_a_kept_candidate_built_from_another_commit(tmp_path, monkeypatch):
    # W138e: a merge-gate-candidate left by an older --keep-candidate run (src 6063ca51a) became the 2247a9a73 payload.
    monkeypatch.setattr(laptop_gate, "G", tmp_path.as_posix())
    base = tmp_path / "drv/E/mc2-installs"
    stale, seed = base / "merge-gate-candidate", base / "_gate-reuse/deployed"
    for p in (stale, seed):
        p.mkdir(parents=True)
    _manifest(stale / ".deployed_manifest.csv", {"mc2.exe": b"old"})   # src_commit "abc"
    _manifest(seed / ".deployed_manifest.csv", {"mc2.exe": b"new"})
    rows = (seed / ".deployed_manifest.csv").read_text(encoding="utf-8").replace(",abc,", ",def0123,")
    (seed / ".deployed_manifest.csv").write_text(rows, encoding="utf-8")
    assert laptop_gate.candidate_install() == stale            # no head: first present candidate, as before
    assert laptop_gate.candidate_install("def0123456789") == seed
    with pytest.raises(SystemExit):
        laptop_gate.candidate_install("0000000")


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
    _tree(lap, {"cache/vulkan/pipe.bin": (b"p", 5), "veg_diag.txt": (b"v", 5), "debug_state/crash_content.jsonl": (b"c", 5)})
    assert not any(r.startswith(("cache/", "veg_diag.txt", "debug_state/")) for r in rows(lap))


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
    shipped = {Path(h).stem: ROOT / h for h in laptop_gate.HELPERS}
    uploaded = {rel for rel, _src in laptop_gate.provision_files(eroot=ROOT)}
    todo, seen = [ROOT / "tools" / "laptop_gate.py"], set()
    while todo:
        path = todo.pop()
        for mod in _module_level_imports(path):
            assert mod in shipped, f"{path.name} imports {mod} at module level; add its repo path to laptop_gate.HELPERS"
            if mod not in seen:
                seen.add(mod)
                todo.append(shipped[mod])
    assert "queue_lock" in shipped   # r_run imports it from bin/ (p31 shared queue lock; a function-level import)
    for helper in laptop_gate.HELPERS:
        assert (ROOT / helper).is_file()
        assert "bin/" + Path(helper).name in uploaded
    # the candidate's farm runner never goes live before promote (W130 review P1): nothing is provisioned outside C:/mc2gate
    assert not any(":" in rel or rel.startswith("/") for rel in uploaded)
    assert "bin/laptop_gate.py" in uploaded
    assert {"drv/E/" + rel for rel in laptop_gate.E_EXTRA} <= uploaded
    # every generated deploy input the PC cooks from a PC-only source reaches the laptop clone (W129 run 3 pixel red)
    layers = {r for r in deploy_payload.TERRAIN_LAYER_PAYLOAD if "concrete_detail" in r or "cliff_rock" in r}
    assert layers == set(laptop_gate.LAYER_EXTRA)
    assert {"drv/A/Games/mc2-main/" + rel for rel in layers} <= uploaded


@pytest.mark.parametrize("gate_args", [["--stages", "deploy", "--keep-candidate"], ["--nightly"], []])
def test_remote_run_round_trips_merge_gate_args(monkeypatch, gate_args):
    """W130 run 1: `--stages deploy --keep-candidate` reached merge_gate as `deploy --stages --keep-candidate`."""
    cmd = laptop_gate.remote_run_cmd("130", gate_args)
    got = {}
    monkeypatch.setattr(laptop_gate, "r_run", lambda w, extra: got.update(w=w, extra=extra))
    monkeypatch.setattr(sys, "argv", cmd.split()[2:])
    assert laptop_gate.main() == 0
    assert got == {"w": "130", "extra": gate_args}


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


# ---- p31 P0: transactional result acceptance (stubbed scp: pull_one) ----
import json  # noqa: E402
import tarfile  # noqa: E402

HEAD = "a" * 40


def _fake_remote(tmp_path, *, head=HEAD, gate_rc=0, build_rc=0, drop=(), payload_ok=True, evidence=True):
    """A stub for laptop_gate.pull_one that 'pulls' from a fake remote out dir."""
    rem = tmp_path / "remote"
    rem.mkdir(exist_ok=True)
    tarp = rem / "payload.tar"
    with tarfile.open(tarp, "w") as t:
        pass
    (rem / "payload.sha256").write_text(("%s" % laptop_gate.sha256(tarp) if payload_ok else "0" * 64) + "  payload.tar\n")
    (rem / "summary.json").write_text(json.dumps({"head": head, "gate_rc": gate_rc, "build_rc": build_rc, "verdict": "PASS"}))
    (rem / "gate-verdict.json").write_text(json.dumps({"head": head, "verdict": "PASS"}))
    for n in ("gate.log", "gate.console.log", "build.log"):
        (rem / n).write_text("x")
    (rem / "evidence" / "ab").mkdir(parents=True, exist_ok=True)
    (rem / "evidence" / "ab" / "montage.jpg").write_bytes(b"jpg")

    def pull_one(remote, local, recursive=False):
        name = remote.rsplit("/", 1)[1]
        src = rem / name
        if name in drop or (name == "evidence" and not evidence) or not src.exists():   # scp of a missing file: rc != 0
            return False
        if recursive:
            import shutil
            shutil.copytree(src, Path(local) / name)
        else:
            Path(local).write_bytes(src.read_bytes())
        return True
    return pull_one


def _accept(tmp_path, monkeypatch, run_rc=0, pack_rc=0, **kw):
    monkeypatch.setattr(laptop_gate, "pull_one", _fake_remote(tmp_path, **kw))
    dest, inc = tmp_path / "proofs" / "7", tmp_path / "proofs" / "7.incoming"
    return laptop_gate.pull_and_accept("7", HEAD, run_rc, pack_rc, dest, inc), dest, inc


def test_laptop_accepts_a_complete_matching_result_and_pulls_the_montages(tmp_path, monkeypatch):
    (ok, why, sm), dest, inc = _accept(tmp_path, monkeypatch)
    assert ok, why
    assert (dest / "gate-verdict.json").is_file() and (dest / "evidence/ab/montage.jpg").is_file() and not inc.exists()


@pytest.mark.parametrize("kw,run_rc,pack_rc,needle", [
    ({}, 255, 0, "ssh exit"),
    ({}, 0, 1, "ssh exit"),
    ({"gate_rc": 1}, 0, 0, "gate_rc"),
    ({"build_rc": 2}, 0, 0, "build_rc"),
    ({"drop": ("gate-verdict.json",)}, 0, 0, "partial pull"),
    ({"drop": ("payload.tar",)}, 0, 0, "partial pull"),
    ({"evidence": False}, 0, 0, "evidence"),
    ({"head": "b" * 40}, 0, 0, "pushed stage sha"),
    ({"payload_ok": False}, 0, 0, "payload"),
])
def test_laptop_rejects_every_failure_mode_and_leaves_no_dest(tmp_path, monkeypatch, kw, run_rc, pack_rc, needle):
    (ok, why, _), dest, inc = _accept(tmp_path, monkeypatch, run_rc, pack_rc, **kw)
    assert not ok and needle in why
    assert not dest.exists() and not inc.exists() and (tmp_path / "proofs" / "7.rejected").is_dir()


def test_laptop_old_pass_cannot_survive_a_failed_pull(tmp_path, monkeypatch):
    old = tmp_path / "proofs" / "7"
    old.mkdir(parents=True)
    (old / "gate-verdict.json").write_text('{"verdict": "PASS"}')
    import shutil
    shutil.rmtree(old)   # pc_main clears dest before the run; a rejected pull must not recreate it
    (ok, _, _), dest, _ = _accept(tmp_path, monkeypatch, drop=("summary.json",))
    assert not ok and not dest.exists()


# ---- --sim (determinism + saveload on the gate's deployed candidate) and the campaign-mods mirror ----
def test_sim_plan_is_merge_gates_own_determinism_and_saveload_steps(tmp_path):
    import merge_gate
    inst, proof = tmp_path / "inst", tmp_path / "proof"
    steps = merge_gate.sim_plan(inst, proof)
    assert [n for n, _, _ in steps] == list(merge_gate.SIM_STAGES)
    by = {n: (c, e) for n, c, e in steps}
    gate = {n: c for n, c, _ in merge_gate.plan(1, proof)}
    for n, (cmd, env) in by.items():
        assert str(inst).replace("\\", "/") in " ".join(cmd) and "mc2-opengl" not in " ".join(cmd) and env["MC2_SMOKE_ARTIFACT_ROOT"] == str(proof / "smokes" / n)
        assert len(cmd) == len(gate[n])   # same shape as the gate's own step (install swapped only)
    assert "--headless-both" in by["determinism-mc2_01"][0]
    c24 = by["determinism-mc2_24"][0]
    assert c24[c24.index("--ticks") + 1] == "2600"
    for m in ("mc2_03", "mc2_10"):
        c = by["saveload-" + m][0]
        assert c[c.index("--mission") + 1] == m and "--mask" in c
    assert "--single" in by["determinism-mc2_03"][0]
    pair = {n: c for n, c, _ in merge_gate.sim_plan(inst, proof, ["determinism-mc2_03"], pair=True)}
    assert list(pair) == ["determinism-mc2_03"] and "--single" not in pair["determinism-mc2_03"]
    with pytest.raises(ValueError):
        merge_gate.sim_plan(inst, proof, ["determinism-mc2_16"])


def test_sim_verdict_needs_every_planned_row_green():
    ok = {"rc": 0, "rows": {"a": {"status": "PASS"}, "b": {"status": "REUSED: x"}}}
    assert laptop_gate.sim_verdict(ok, ["a", "b"]) == "PASS"
    assert laptop_gate.sim_verdict(dict(ok, rc=1), ["a", "b"]) == "FAIL"
    assert laptop_gate.sim_verdict(dict(ok, error="boom"), ["a", "b"]) == "FAIL"
    assert laptop_gate.sim_verdict(ok, ["a", "b", "c"]) == "FAIL"   # a planned row that never reported
    assert laptop_gate.sim_verdict(ok, []) == "FAIL"
    assert laptop_gate.sim_verdict({"rc": 0, "rows": {"a": {"status": "FAIL"}}}, ["a"]) == "FAIL"
    assert laptop_gate.sim_args(["determinism-mc2_01", "--pair"]) == (["determinism-mc2_01"], True)


@pytest.mark.parametrize("bad", ["head", "short-head", "window", "main-head", "main-window", "main-missing", "main-invalid", "missing", "red", "red-row", "ssh"])
def test_rejected_sim_never_changes_accepted_summary(tmp_path, bad):
    import json
    dest = tmp_path / "W"
    dest.mkdir()
    path = dest / "summary.json"
    main = {"head": "a" * 40, "window": "W", "sim": {"accepted": True}}
    sim = {"head": "a" * 40, "window": "W", "verdict": "PASS", "rc": 0, "rows": {"a": {"status": "PASS"}}}
    if bad == "head":
        sim["head"] = "b" * 40
    elif bad == "short-head":
        sim["head"] = "a" * 9
    elif bad == "window":
        sim["window"] = "other"
    elif bad == "main-head":
        main["head"] = "b" * 40
    elif bad == "main-window":
        main["window"] = "other"
    elif bad == "missing":
        sim.pop("rows")
    elif bad == "red":
        sim["verdict"] = "FAIL"
    elif bad == "red-row":
        sim["rows"]["a"]["status"] = "FAIL"
    if bad != "main-missing":
        path.write_text("invalid" if bad == "main-invalid" else json.dumps(main))
    before = path.read_bytes() if path.exists() else None
    assert laptop_gate.merge_sim_summary(dest, sim, "a" * 40, "W", run_rc=int(bad == "ssh"))
    assert (path.read_bytes() if path.exists() else None) == before
    assert json.loads((dest / "rejected-sim.json").read_text())["sim"] == sim


def test_merge_sim_summary_accepts_only_matching_identity(tmp_path):
    import json
    (tmp_path / "summary.json").write_text(json.dumps({"head": "a" * 40, "window": "W", "x": 1}))
    sim = {"head": "a" * 40, "window": "W", "verdict": "PASS", "rc": 0, "rows": {"a": {"status": "PASS"}}}
    assert laptop_gate.merge_sim_summary(tmp_path, sim, "a" * 40, "W") == ""
    assert json.loads((tmp_path / "summary.json").read_text()) == {"head": "a" * 40, "window": "W", "x": 1, "sim": sim}


@pytest.mark.parametrize("missing", ["pull", "summary", "branch", "wrong-head", "valid"])
def test_sim_main_missing_evidence_preserves_summary_and_sends_guard(tmp_path, monkeypatch, missing):
    from types import SimpleNamespace
    import json
    import subprocess
    dest = tmp_path / "W"
    dest.mkdir()
    path = dest / "summary.json"
    before = b'{"head":"accepted", "sim":{"accepted":true}}'
    if missing == "valid":
        before = json.dumps({"head": "a" * 40, "window": "W"}).encode()
    path.write_bytes(before)
    (dest / "sim").mkdir()
    (dest / "sim/summary.json").write_bytes(b"accepted sim")
    monkeypatch.setattr(laptop_gate, "PROOFS", tmp_path)
    for name in ("provision", "sync_mods"):
        monkeypatch.setattr(laptop_gate, name, lambda *a, **k: None)

    def expected(*a, **k):
        if missing == "branch":
            raise subprocess.CalledProcessError(1, a)
        return "a" * 40

    monkeypatch.setattr(laptop_gate.subprocess, "check_output", expected)
    calls = []
    monkeypatch.setattr(laptop_gate, "remote_py", lambda *a: a)
    monkeypatch.setattr(laptop_gate, "ssh", lambda cmd, **k: (calls.append(cmd) or (0, "")))
    def pull(remote, local, **kwargs):
        if missing in ("wrong-head", "valid"):
            (local / "sim").mkdir()
            (local / "sim/summary.json").write_text(json.dumps({"head": ("a" if missing == "valid" else "b") * 40,
                                                              "window": "W", "verdict": "PASS", "rc": 0,
                                                              "rows": {"a": {"status": "PASS"}}}))
        return missing != "pull"

    monkeypatch.setattr(laptop_gate, "pull_one", pull)
    assert laptop_gate.sim_main(SimpleNamespace(window="W", sim_rows="", sim_pair=False)) == (0 if missing == "valid" else 1)
    if missing == "valid":
        assert json.loads(path.read_text())["sim"]["head"] == "a" * 40
        assert json.loads((dest / "sim/summary.json").read_text())["head"] == "a" * 40
    else:
        assert path.read_bytes() == before
        assert (dest / "sim/summary.json").read_bytes() == b"accepted sim"
        assert json.loads((dest / "rejected-sim.json").read_text())["reason"]
    assert not (dest / "incoming-sim").exists()
    if missing == "branch":
        assert not calls
    else:
        assert "--expect-head=" + "a" * 40 in calls[0]


def test_sim_checkout_refuses_missing_branch_and_head_off_the_tip():
    import subprocess

    def fake(answers, calls=None):
        def git(*a):
            if calls is not None:
                calls.append(a)
            key = " ".join(a) if a[0] == "rev-parse" else a[0]
            rc, out = answers.get(key, (0, ""))
            return subprocess.CompletedProcess(a, rc, out, "")
        return git
    b = "claude/stage-W"
    head, why = laptop_gate.sim_checkout(fake({"rev-parse --verify -q " + b: (1, "")}), b)
    assert why and "missing" in why
    calls = []
    head, why = laptop_gate.sim_checkout(fake({"rev-parse " + b: (0, "b" * 40 + "\n")}, calls), b, "a" * 40)
    assert "expected staged head" in why and not any(a[0] == "checkout" for a in calls)
    head, why = laptop_gate.sim_checkout(fake({"rev-parse HEAD": (0, "b" * 40 + "\n"), "rev-parse " + b: (0, "a" * 40 + "\n")}), b, "a" * 40)
    assert "is not " + b in why
    head, why = laptop_gate.sim_checkout(fake({"rev-parse HEAD": (0, "a" * 40 + "\n"), "rev-parse " + b: (0, "a" * 40 + "\n")}), b, "a" * 40)
    assert (head, why) == ("a" * 40, "")


def test_mods_problems_and_idempotent_mirror_diff(tmp_path):
    src, dst = tmp_path / "src", tmp_path / "dst"
    mods = {f"{n}/data/a.bin": (b"x", 100) for n in laptop_gate.MODS_NAMES}
    _tree(src, mods)
    bad = laptop_gate.mods_problems(dst)
    assert len(bad) == len(laptop_gate.MODS_NAMES) and "MechCommanderOmnitech" in " ".join(bad)
    copy, touch, delete = laptop_gate.tree_diff(laptop_gate.tree_rows(src), laptop_gate.tree_rows(dst))
    assert len(copy) == len(mods) and not touch and not delete
    _tree(dst, mods)   # what the tar extract leaves (mtimes kept)
    assert laptop_gate.mods_problems(dst) == []
    assert laptop_gate.tree_diff(laptop_gate.tree_rows(src), laptop_gate.tree_rows(dst)) == ([], [], [])


def test_empty_remote_tree_never_deletes_root(tmp_path, monkeypatch):
    """W133: an empty laptop tree is written as "\n"; its "" row must not become "D|" (unlink of the mirror root itself)."""
    monkeypatch.setattr(laptop_gate, "PC_TMP", tmp_path)
    monkeypatch.setattr(laptop_gate, "ssh", lambda *a, **k: (0, ""))
    monkeypatch.setattr(laptop_gate, "scp", lambda src, dst, recursive=False: Path(dst).write_text("\n", encoding="utf-8"))
    assert laptop_gate.remote_rows(False, "mods-tree", "mods.tree.txt") == []
    root = tmp_path / "root"
    root.mkdir()
    ops = tmp_path / "ops"
    ops.write_text("D|\nD|.\n", encoding="utf-8")
    laptop_gate.r_apply(str(ops), root)
    assert root.is_dir()
