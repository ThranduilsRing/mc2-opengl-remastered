"""ab.py refuses a cross-base A/B: commit ancestry from the install identity, data/shader manifest equality, CROSS-BASE stamp."""
from __future__ import annotations

import subprocess
import hashlib
import json
import sys
from pathlib import Path

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402
import console_child  # noqa: E402


def test_gate_stages_published_runtime_and_declared_mod_identity(tmp_path, monkeypatch):
    import csv
    import merge_gate
    import deploy_payload

    pub, inst = tmp_path / "published", tmp_path / "candidate"
    pub.mkdir()
    inst.mkdir()
    payload = {"mc2.exe": b"old", "SDL2.dll": b"sdl", "mc2res_64.dll": b"res",
               "crashpad_handler.exe": b"handler", "mods/example/mod.json": b"{}"}
    header = [["manifest_version", "v1"], ["relpath", "sha256", "bytes", "src_commit", "timestamp", "mtime_ns"]]
    published_rows = []
    for rel, content in payload.items():
        src = pub / rel
        src.parent.mkdir(parents=True, exist_ok=True)
        src.write_bytes(content)
        published_rows.append([rel, hashlib.sha256(content).hexdigest(), len(content), "old", "t", 0])
        if rel != "mods/example/mod.json":
            (inst / rel).write_bytes(b"stale")
    for target, rows in ((pub, published_rows), (inst, [["sdl2.dll", "stale", 5, "seed", "t", 0],
                                                    ["data/keep.fit", "keep", 4, "seed", "t", 0]])):
        with (target / ab.MANIFEST).open("w", newline="") as stream:
            csv.writer(stream).writerows(header + rows)
    def cook(target):
        for name in [f"mat{i}_normal.ktx2" for i in range(6)] + ["engine_normals.cookver"]:
            (Path(target) / "data/textures" / name).touch()
    monkeypatch.setattr(deploy_payload, "ensure_engine_normal_cooks", cook)
    merge_gate.stage_published(inst, pub)
    assert all((inst / rel).read_bytes() == content for rel, content in payload.items())
    with (inst / ab.MANIFEST).open(newline="") as stream:
        rows = list(csv.reader(stream))[2:]
    assert {r[0] for r in rows} == {*payload, "data/keep.fit"}
    assert all(r in rows for r in [[str(v) for v in row] for row in published_rows])
    before, _ = ab.read_manifest(pub)
    after, _ = ab.read_manifest(inst)
    assert ab.manifest_mismatches(before, {k: v for k, v in after.items() if k in payload}) == []


def _git(repo, *a):
    return subprocess.run(["git", "-C", str(repo), *a], capture_output=True, text=True, check=True,
                          **console_child.child_kwargs(redirected=True)).stdout.strip()


@pytest.fixture()
def repo(tmp_path, monkeypatch):
    r = tmp_path / "repo"
    r.mkdir()
    _git(r, "init", "-q")
    _git(r, "config", "user.email", "t@t")
    _git(r, "config", "user.name", "t")
    shas = []
    for n in range(2):
        (r / "f").write_text(str(n))
        _git(r, "add", "f")
        _git(r, "commit", "-q", "-m", str(n))
        shas.append(_git(r, "rev-parse", "HEAD"))
    _git(r, "checkout", "-q", "-b", "side", shas[0])
    (r / "g").write_text("g")
    _git(r, "add", "g")
    _git(r, "commit", "-q", "-m", "side")
    shas.append(_git(r, "rev-parse", "HEAD"))   # shas[2] is on a branch that is not a descendant of shas[1]
    monkeypatch.setattr(ab, "ROOT", r)
    return shas


def _install(d, commit, data=None, spv="s1", exe="e1"):
    d.mkdir(parents=True)
    rows = {"mc2.exe": exe, "shaders/vulkan/x.spv": spv, **(data or {"data/a.fit": "d1"})}
    lines = ["manifest_version,v1,,,,", "relpath,sha256,bytes,src_commit,timestamp,mtime_ns"]
    for rel, value in rows.items():
        path = d / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(value.encode())
    lines = lines[:2] + [f"{k},{hashlib.sha256(v.encode()).hexdigest()},{len(v)},{commit},t,0" for k, v in rows.items()]
    (d / ".deployed_manifest.csv").write_text("\n".join(lines) + "\n")
    (d / "saved_bookmarks").mkdir()  # ab.py never writes bookmarks into a non-staged install
    (d / "saved_bookmarks" / "bm.json").write_text("{}")
    return str(d)


def test_payload_identity_accepts_short_matching_fingerprint(tmp_path, repo, monkeypatch):
    install = _install(tmp_path / "i", repo[0])
    monkeypatch.setattr(ab, "exe_commit", lambda *args: repo[0][:12])
    assert ab.payload_identity(install)["commit"] == repo[0]


def test_exe_swap_checks_data_commit_declarations(tmp_path, repo, monkeypatch):
    rel = "data/model_overrides/models.json"
    install = Path(_install(tmp_path / "i", repo[1], data={rel: "lane data"}))
    source = Path(_install(tmp_path / "base", repo[0], data={rel: "base data"}))
    monkeypatch.setattr(ab, "exe_commit", lambda exe: repo[0])
    declarations = {repo[0]: {rel: hashlib.sha256(b"base data").hexdigest()},
                    repo[1]: {rel: hashlib.sha256(b"lane data").hexdigest()}}
    monkeypatch.setattr(ab.asset_parity, "intentional_changes", lambda commit, root=None: declarations[commit])
    stage = ab.stage_exe_swap(install, source / "mc2.exe", tmp_path / "stage")
    identity = ab.payload_identity(stage)
    assert identity["commit"] == repo[0] and identity["asset_commit"] == repo[1]
    assert identity["intentional_asset_changes"] == declarations[repo[1]]
    assert rel in ab.asset_parity.owned_paths(stage)
    (stage / rel).write_text("stale tar entry")
    with pytest.raises(ValueError, match="intentional asset hash mismatch"):
        ab.payload_identity(stage)
    with pytest.raises(ValueError, match="INTENTIONAL ASSET hash mismatch"):
        ab.asset_parity.owned_paths(stage)


def test_mixed_asset_commits_refused(tmp_path, repo):
    install = Path(_install(tmp_path / "i", repo[0], data={"data/a": "a", "data/b": "b"}))
    manifest = install / ab.MANIFEST
    text = manifest.read_text().replace(f",1,{repo[0]},t,0", f",1,{repo[1]},t,0", 1)
    manifest.write_text(text)
    with pytest.raises(ValueError, match="ambiguous asset commit"):
        ab.payload_identity(install)


@pytest.mark.parametrize("auxiliary", sorted(ab.asset_parity.AUXILIARY))
def test_exe_auxiliary_rows_do_not_define_asset_commit(tmp_path, auxiliary):
    published, candidate, third = "1" * 40, "2" * 40, "3" * 40
    install = Path(_install(tmp_path / "i", candidate))
    manifest = install / ab.MANIFEST
    text = manifest.read_text()
    text = text.replace(f",2,{candidate},t,0", f",2,{published},t,0", 1)
    for rel in sorted(ab.asset_parity.AUXILIARY):
        text += f"{rel},{hashlib.sha256(b'aux').hexdigest()},3,{published},t,0\n"
    manifest.write_text(text)
    payload, commit = ab.asset_parity.manifest_assets(install)
    assert commit == candidate
    assert ab.asset_parity.AUXILIARY <= payload.keys()
    manifest.write_text(text.replace(
        f"{auxiliary},{hashlib.sha256(b'aux').hexdigest()},3,{published},t,0",
        f"{auxiliary},{hashlib.sha256(b'aux').hexdigest()},3,{third},t,0"))
    with pytest.raises(ValueError, match="ambiguous asset commit"):
        ab.asset_parity.manifest_assets(install)


def test_named_repo_bookmarks_need_no_install_seed(tmp_path, monkeypatch):
    monkeypatch.setattr(ab, "ROOT", tmp_path)
    bookmarks = tmp_path / "tests/visual/bookmarks"
    bookmarks.mkdir(parents=True)
    (bookmarks / "day.json").write_text("{}")
    install = tmp_path / "install"
    ab.ensure_saved_bookmarks(install, install, [], bookmarks="day")
    assert not install.exists()
    with pytest.raises(SystemExit, match="not a staged copy"):
        ab.ensure_saved_bookmarks(install, install, [], bookmarks="missing")


def test_ancestor_and_matching_manifests_pass(tmp_path, repo):
    a = _install(tmp_path / "a", repo[0], exe="ea", spv="sa")
    b = _install(tmp_path / "b", repo[1], exe="eb", spv="sb")   # only mc2.exe and .spv differ
    assert ab.base_problems(install_a=a, install_b=b) == []
    assert ab.base_problems(install_a=a, install_b=a) == []


def test_non_ancestor_commit_is_refused_with_the_shas(tmp_path, repo):
    a = _install(tmp_path / "a", repo[1])
    b = _install(tmp_path / "b", repo[2])
    msg = ab.base_problems(install_a=a, install_b=b)[0]
    assert "not an ancestor" in msg and repo[1][:12] in msg and repo[2][:12] in msg
    assert "not an ancestor" in ab.base_problems(install_a=_install(tmp_path / "c", repo[1]), install_b=_install(tmp_path / "d", repo[0]))[0]


def test_data_mismatch_names_the_file(tmp_path, repo):
    a = _install(tmp_path / "a", repo[0], data={"data/a.fit": "d1", "data/only_a": "z"})
    b = _install(tmp_path / "b", repo[1], data={"data/a.fit": "d2"})
    msg = " ".join(ab.base_problems(install_a=a, install_b=b))
    assert "differs: data/a.fit" in msg and "only in A: data/only_a" in msg


def test_exe_swap_needs_descendant_of_install_commit(tmp_path, repo, monkeypatch):
    inst = _install(tmp_path / "i", repo[1])
    ea, eb = tmp_path / "a.exe", tmp_path / "b.exe"
    ea.write_bytes(b"A")
    eb.write_bytes(b"B")
    monkeypatch.setattr(ab, "exe_commit", lambda e: {str(ea): repo[1], str(eb): repo[2]}.get(str(e)))
    msgs = ab.base_problems(exe_a=str(ea), exe_b=str(eb), install=inst)
    assert len(msgs) == 1 and "exe B" in msgs[0] and "not an ancestor" in msgs[0]


def test_unknown_identity_is_a_problem(tmp_path, repo):
    a = tmp_path / "a"
    a.mkdir()
    (a / "mc2.exe").write_bytes(b"x")
    assert any("unknown" in m for m in ab.base_problems(install_a=str(a), install_b=str(a)))


def test_capture_identity_ignores_shader_sources_not_data():
    # W136: before (published) and after (candidate) carry the window's shader edits; data drift still refuses.
    a = {"mc2.exe": "1", "shaders/vulkan/t.frag": "s1", "shaders/vulkan/include/x.glsl": "g1", "data/a.fit": "d1"}
    b = {"mc2.exe": "2", "shaders/vulkan/t.frag": "s2", "shaders/vulkan/include/x.glsl": "g2", "data/a.fit": "d1"}
    assert ab.manifest_mismatches(a, b) == []
    assert ab.manifest_mismatches(a, {**b, "data/a.fit": "d2"}) == ["differs: data/a.fit (A d1 vs B d2)"]


def test_game_identity_excludes_only_editor_owned_rows(monkeypatch):
    monkeypatch.setattr(ab.asset_parity, "intentional_changes", lambda *a: {})
    editor = {p: "a" * 64 for p in ("Mission Editor.exe", "Mission Editor.pdb", "run-editor.bat",
                                     "tools/terrain_gen/tool.py")}
    game = {"mc2.exe": "e" * 64, "data/ui.fit": "a" * 64, "run-mc2.bat": "b" * 64}
    assert ab.candidate_mismatches({**editor, **game}, game, "a" * 40) == []
    assert ab.candidate_mismatches(game, {**editor, **game}, "a" * 40) == []
    changed = {**game, "data/ui.fit": "c" * 64}
    assert [p for p, _ in ab.candidate_mismatches(game, changed, "a" * 40)] == ["data/ui.fit"]
    assert [p for p, _ in ab.candidate_mismatches(game, {}, "a" * 40)] == ["data/ui.fit", "run-mc2.bat"]


@pytest.mark.parametrize("rel", ["Mission Editor.exe", "Mission Editor.pdb", "run-editor.bat",
                                 "tools/terrain_gen/tool.py"])
@pytest.mark.parametrize("archived", [False, True], ids=["install", "archived-capture"])
def test_game_identity_rejects_changed_shared_editor_rows(tmp_path, repo, monkeypatch, capsys, rel, archived):
    monkeypatch.setattr(ab.asset_parity, "intentional_changes", lambda *a: {})
    installs = [_install(tmp_path / side, repo[0], data={rel: value})
                for side, value in (("a", "original bytes"), ("b", "changed bytes"))]
    if archived:
        captures = [tmp_path / (side + "-capture") for side in ("a", "b")]
        for install, capture_dir in zip(installs, captures):
            ab.stamp_identity(capture_dir, ab.payload_identity(install))
        with pytest.raises(SystemExit, match="differs: " + rel):
            ab.main(["--compare-only", *map(str, captures), "--out-root", str(tmp_path / "cmp")])
    else:
        assert any("differs: " + rel in p for p in ab.base_problems(install_a=installs[0], install_b=installs[1]))
    assert "ABSENT-BY-DESIGN" not in capsys.readouterr().out


def test_game_identity_case_mismatched_editor_row_is_not_absent(capsys):
    # Carbon P2: the same editor file spelled differently on each side must not be excused as absent-by-design.
    a = {"mc2.exe": "e" * 64, "Mission Editor.exe": "a" * 64}
    b = {"mc2.exe": "e" * 64, "mission editor.exe": "b" * 64}
    lines = dict(ab._data_mismatch_entries(a, b, target="game"))
    assert "Mission Editor.exe" in lines and "mission editor.exe" in lines
    assert "ABSENT-BY-DESIGN" not in capsys.readouterr().out


@pytest.mark.parametrize("replacement", [None, "b" * 64])
def test_editor_identity_requires_authoring_files(monkeypatch, replacement):
    monkeypatch.setattr(ab.asset_parity, "intentional_changes", lambda *a: {})
    rel = "tools/terrain_gen/tool.py"
    a = {"Mission Editor.exe": "e" * 64, rel: "a" * 64}
    b = {"Mission Editor.exe": "f" * 64, **({rel: replacement} if replacement else {})}
    assert rel in dict(ab.candidate_mismatches(a, b, "a" * 40))
    # Even a shared game/editor install stays strict when comparing the editor target.
    assert rel in dict(ab.candidate_mismatches({**a, "mc2.exe": "e" * 64},
                                             {**b, "mc2.exe": "e" * 64}, "a" * 40, target="editor"))


@pytest.mark.parametrize("rel,after,accepted", [
    ("data/ui.fit", b"first\nsecond\n", True),
    ("data/ui.fit", b"first\nseconX\n", False),
    ("data/binary.fit", b"first\nsecond\n\0", False),
    ("data/control.fit", b"first\nsecond\n\x01", False),
    ("data/undefined.fit", b"first\nsecond\n\x81", False),
    ("data/cp1252.fit", b"first\nsecond\n\x93", True),
    ("data/utf8.fit", "first\nsecond\n\u00e9".encode(), True),
    *[("data/file." + ext, b"first\nsecond\n", False)
      for ext in ("exe", "dll", "pdb", "spv", "ktx2", "png", "tga", "bin")],
])
def test_eol_identity_is_narrow_and_logged(tmp_path, monkeypatch, capsys, rel, after, accepted):
    monkeypatch.setattr(ab.asset_parity, "intentional_changes", lambda *a: {})
    monkeypatch.setattr(ab, "ROOT", tmp_path / "absent-checkout")
    installs = [tmp_path / side for side in ("a", "b")]
    manifests = []
    before = after.replace(b"\n", b"\r\n") if rel != "data/ui.fit" else b"first\r\nsecond\r\n"
    for install, content in zip(installs, (before, after)):
        path = install / rel
        path.parent.mkdir(parents=True)
        path.write_bytes(content)
        manifests.append({rel: hashlib.sha256(content).hexdigest()})
    receipt = []
    mismatches = ab.candidate_mismatches(*manifests, "a" * 40, installs[1], installs[0],
                                       install_a=installs[0], identity_receipt=receipt)
    assert (mismatches == []) == accepted
    if accepted:
        expected = dict(path=rel, sha256_A=manifests[0][rel], sha256_B=manifests[1][rel],
                        normalized_sha256=hashlib.sha256(after).hexdigest())
        assert receipt == [expected]
        output = capsys.readouterr().out
        assert "EOL-ONLY" in output and all(str(value) in output for value in expected.values())
        ab.compare(installs[0], installs[1], tmp_path / "cmp", [], identity_receipt=receipt)
        assert json.loads((tmp_path / "cmp/summary.json").read_text())["eol_equivalences"] == [expected]
        # Historical captures can use checkout bytes only when both raw hashes reconstruct exactly.
        monkeypatch.setattr(ab, "ROOT", installs[1])
        assert ab.candidate_mismatches(*manifests, "a" * 40, baseline=tmp_path / "absent") == []
        (installs[1] / rel).write_bytes(b"stale manifest\n")
        assert ab.candidate_mismatches(*manifests, "a" * 40, installs[1], installs[0])
    else:
        assert receipt == []


def test_capture_identity_skips_only_deployed_shader_dirs():
    # W137: shaders/include and shaders/spv ship with the exe like shaders/vulkan; any other file under shaders/ still refuses.
    a = {"shaders/include/x.hglsl": "h1", "shaders/spv/p.pkg": "p1", "shaders/lut.dat": "l1", "shaders/fixtures/f.bin": "f1"}
    b = {"shaders/include/x.hglsl": "h2", "shaders/spv/p.pkg": "p2", "shaders/lut.dat": "l2", "shaders/fixtures/f.bin": "f2"}
    assert ab.manifest_mismatches(a, b) == ["differs: shaders/fixtures/f.bin (A f1 vs B f2)", "differs: shaders/lut.dat (A l1 vs B l2)"]
    assert ab.manifest_mismatches(a, {k: v for k, v in b.items() if k != "shaders/lut.dat"}) == [
        "only in A: shaders/lut.dat", "differs: shaders/fixtures/f.bin (A f1 vs B f2)"]


def test_git_diff_files_lists_changed_paths(tmp_path, repo):
    # repo: commit0 (f=0) -> commit1 (f=1); side branch from commit0 adds g. `git diff --name-only` is the window's payload.
    assert ab.git_diff_files(repo[0], repo[1]) == {"f"}
    assert ab.git_diff_files(repo[1], repo[0]) == {"f"}
    assert ab.git_diff_files(repo[0], repo[2]) == {"g"}
    assert ab.git_diff_files(repo[0], repo[0]) == set()
    assert ab.git_diff_files("no-such-commit", repo[0]) is None


def test_git_diff_files_lists_both_sides_of_a_rename(tmp_path, repo):
    # W138g: hud/mcui_high7/x.png -> hud/x.png was a rename; the removed old path is part of the window's payload too.
    r = ab.ROOT
    (r / "old").mkdir()
    (r / "old" / "icon.png").write_text("same bytes, long enough to be detected as a rename" * 4)
    _git(r, "add", "old")
    _git(r, "commit", "-q", "-m", "add")
    before = _git(r, "rev-parse", "HEAD")
    _git(r, "mv", "old/icon.png", "icon.png")
    _git(r, "commit", "-q", "-m", "move")
    assert ab.git_diff_files(before, _git(r, "rev-parse", "HEAD")) == {"old/icon.png", "icon.png"}


@pytest.mark.parametrize("rel", ["mc2.exe", "shaders/vulkan/x.spv"])
def test_manifest_byte_mismatch_refuses_before_capture(tmp_path, repo, monkeypatch, rel):
    install = _install(tmp_path / "install", repo[0])
    (Path(install) / rel).write_bytes(b"drift")
    monkeypatch.setattr(ab, "resolve_installs", lambda *args: (install, install))
    monkeypatch.setattr(ab, "run_capture", lambda *args, **kw: pytest.fail("launched a game with stale payload"))
    with pytest.raises(SystemExit, match="manifest mismatch"):
        ab.main(["--bookmarks", "x", "--install-a", install, "--install-b", install,
                 "--out-root", str(tmp_path / "out"), "--allow-base-mismatch"])


@pytest.mark.parametrize("fault", ["missing", "payload", "base", "exe"])
def test_compare_only_refuses_unknown_or_mismatched_identity(tmp_path, repo, fault):
    install = _install(tmp_path / "install", repo[0])
    identity = ab.payload_identity(install)
    for side in ("a", "b"):
        ab.stamp_identity(tmp_path / side, identity)
    path = tmp_path / "b/capture_run.json"
    data = json.loads(path.read_text())
    if fault == "missing":
        del data["install_identity"]
    elif fault == "payload":
        data["install_identity"]["payload"]["mc2.exe"] = "0" * 64
    elif fault == "exe":
        data["identity"] = {"exe": {"sha256": "0" * 64}}
    else:
        data["install_identity"]["commit"] = repo[2]
        other = json.loads((tmp_path / "a/capture_run.json").read_text())
        other["install_identity"]["commit"] = repo[1]
        (tmp_path / "a/capture_run.json").write_text(json.dumps(other))
    path.write_text(json.dumps(data))
    with pytest.raises(SystemExit, match="identit"):
        ab.main(["--compare-only", str(tmp_path / "a"), str(tmp_path / "b"), "--out-root", str(tmp_path / "cmp")])


def test_capture_sweep_records_verified_payload_and_refuses_drift(tmp_path, repo, monkeypatch):
    import capture
    import io
    install = Path(_install(tmp_path / "install", repo[0]))
    manifest = capture.capture_sweep(install, [], tmp_path / "capture", 1, 1, 1, True, False, io.StringIO())
    assert manifest["install_identity"] == ab.payload_identity(install)
    (install / "mc2.exe").write_bytes(b"drift")
    with pytest.raises(ValueError, match="manifest mismatch"):
        capture.capture_sweep(install, [], tmp_path / "capture", 1, 1, 1, True, False, io.StringIO())


def test_main_refuses_then_allows_and_stamps(tmp_path, repo, monkeypatch):
    a = _install(tmp_path / "a", repo[1])
    b = _install(tmp_path / "b", repo[2])
    argv = ["--bookmarks", "x", "--install-a", a, "--install-b", b, "--out-root", str(tmp_path / "o")]
    with pytest.raises(SystemExit, match="refusing an A/B across different bases"):
        ab.main(argv)
    seen = {}
    monkeypatch.setattr(ab, "resolve_installs", lambda args, out: (a, b))
    monkeypatch.setattr(ab, "run_capture", lambda *x, **kw: None)
    monkeypatch.setattr(ab, "compare", lambda *x, **k: seen.update(k))
    with pytest.raises(SystemExit) as ex:
        ab.main(argv + ["--allow-base-mismatch"])
    assert ex.value.code == 0 and seen["cross_base"] is True


def test_cross_base_montage_is_stamped(tmp_path):
    for side in "ab":
        (tmp_path / side / "bm").mkdir(parents=True)
        Image.new("RGB", (200, 100), (0, 0, 0)).save(tmp_path / side / "bm" / "bm.png")
    ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "plain", [], ["bm"])
    ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "xb", [], ["bm"], cross_base=True)
    plain = Image.open(tmp_path / "plain" / "bm" / "montage.jpg").convert("RGB")
    stamped = Image.open(tmp_path / "xb" / "bm" / "montage.jpg").convert("RGB")
    assert max(stamped.getpixel((12, 12))) > 100 and max(plain.getpixel((12, 12))) < 30
    assert '"cross_base": true' in (tmp_path / "xb" / "summary.json").read_text()


def test_exe_a_base_mismatch_warns(tmp_path, repo, monkeypatch):
    # repo: master commit0 -> commit1 (repo[0], repo[1]); side commit0 -> commit2 (repo[2]).
    r = ab.ROOT
    _git(r, "branch", "lane-base-ref", repo[1])   # the lane base branch tip = commit1
    monkeypatch.setattr(ab, "LANE_BASE_BRANCH", "lane-base-ref")
    # B = commit2 (side): lane_base = merge-base(commit2, commit1) = commit0.
    # A = commit0 (== lane_base): a clean A/B, no warning.
    assert ab.exe_a_base_check(repo[0], repo[2]) == (None, None)
    # A = commit1 (!= lane_base commit0): the A exe is not built from the lane base -> warn.
    line, mm = ab.exe_a_base_check(repo[1], repo[2])
    assert line == f"exe A is {repo[1]}, lane base is {repo[0]}: differences may be cross-base"
    assert mm == {"exe_a": repo[1], "lane_base": repo[0]}
    # B = commit1 (master tip): lane_base = merge-base(commit1, commit1) = commit1. A = commit0 != commit1 -> warn.
    line, mm = ab.exe_a_base_check(repo[0], repo[1])
    assert mm == {"exe_a": repo[0], "lane_base": repo[1]}
    # Unknown commits: the lane base cannot be determined, so no warning (never a refusal).
    assert ab.exe_a_base_check(None, repo[2]) == (None, None)
    assert ab.exe_a_base_check(repo[1], None) == (None, None)


def test_compare_writes_exe_a_base_mismatch(tmp_path):
    for side in "ab":
        (tmp_path / side / "bm").mkdir(parents=True)
        Image.new("RGB", (200, 100), (0, 0, 0)).save(tmp_path / side / "bm" / "bm.png")
    mm = {"exe_a": "a" * 40, "lane_base": "b" * 40}
    ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "cmp", [], ["bm"], exe_a_base_mismatch=mm)
    assert json.loads((tmp_path / "cmp/summary.json").read_text())["exe_a_base_mismatch"] == mm
    ab.compare(tmp_path / "a", tmp_path / "b", tmp_path / "cmp2", [], ["bm"])
    assert json.loads((tmp_path / "cmp2/summary.json").read_text())["exe_a_base_mismatch"] is None


def test_cooked_payload_accepts_pinned_outputs_only_when_the_pin_changed(tmp_path, repo):
    # W138h: deploy cooks data/tgl/128/<stem>.ktx2 (gitignored) from data/kit/tree-upscale.json; the window that adds or changes
    # the pins ships them, so --allow-data-delta accepts them at the pinned bytes. Without a pin change they keep refusing.
    r = ab.ROOT
    (r / "data" / "kit").mkdir(parents=True)
    (r / "data" / "kit" / "tree-upscale.json").write_text(json.dumps({"a_palm": {"ktx2_sha256": "ab" * 32}}))
    _git(r, "add", "data")
    _git(r, "commit", "-q", "-m", "pins")
    head = _git(r, "rev-parse", "HEAD")
    diff = ab.git_diff_files(repo[1], head)
    assert ab.cooked_payload(diff, head) == {"data/tgl/128/a_palm.ktx2": "ab" * 32}
    assert ab.cooked_payload(diff - {"data/kit/tree-upscale.json"}, head) == {}
    assert ab.cooked_payload(diff, repo[1]) == {}   # pin file unreadable at that commit: fail closed


def test_cooked_unpinned_output_is_payload_only_when_its_cook_script_changed():
    # W147: grass_turf_normal.ktx2 is gitignored and unpinned; the window that changes cook_grass_turf.py ships it.
    assert ab.cooked_unpinned({"tools/mc2texcook/cook_grass_turf.py"}) == {"data/terrain_layers/grass_turf_normal.ktx2"}
    assert ab.cooked_unpinned({"code/mech.cpp"}) == set()


def test_cooked_payload_malformed_pins_fail_closed(tmp_path, repo):
    r = ab.ROOT
    (r / "data" / "kit").mkdir(parents=True)
    (r / "data" / "kit" / "tree-upscale.json").write_text("{not json")
    _git(r, "add", "data")
    _git(r, "commit", "-q", "-m", "bad pins")
    head = _git(r, "rev-parse", "HEAD")
    assert ab.cooked_payload({"data/kit/tree-upscale.json"}, head) == {}


def _build_output_manifests(**diff):
    a = {"mc2.exe": "e" * 64, "crashpad_handler.exe": "a" * 64, "mc2res_64.dll": "a" * 64, "SDL2.dll": "a" * 64}
    return a, {**a, **{k: "b" * 64 for k in diff}}


def _identity(monkeypatch, changed, a, b):
    monkeypatch.setattr(ab.asset_parity, "intentional_changes", lambda *x: {})
    monkeypatch.setattr(ab, "git_diff_files", lambda *x: changed)
    return [p for p, _ in ab.candidate_mismatches(a, b, "b" * 40, commit_a="a" * 40)]


def test_crashpad_handler_drift_is_a_build_output(monkeypatch, capsys):
    a, b = _build_output_manifests(**{"crashpad_handler.exe": 1})
    assert _identity(monkeypatch, {"data/x.fit"}, a, b) == []
    assert "[ab] BUILD-OUTPUT crashpad_handler.exe" in capsys.readouterr().out


def test_mc2res_drift_without_source_change_is_accepted(monkeypatch, capsys):
    a, b = _build_output_manifests(**{"mc2res_64.dll": 1})
    assert _identity(monkeypatch, {"data/x.fit", "GameOS/foo.cpp"}, a, b) == []
    assert "[ab] BUILD-OUTPUT mc2res_64.dll" in capsys.readouterr().out


@pytest.mark.parametrize("src", ["res/libmain.cpp", "test_scripts/res_conv/strings.res.cpp", "CMakeLists.txt"])
def test_mc2res_drift_with_source_change_refuses(monkeypatch, src):
    a, b = _build_output_manifests(**{"mc2res_64.dll": 1})
    assert _identity(monkeypatch, {src}, a, b) == ["mc2res_64.dll"]


def test_mc2res_drift_with_unknown_diff_refuses(monkeypatch):
    a, b = _build_output_manifests(**{"mc2res_64.dll": 1})
    assert _identity(monkeypatch, None, a, b) == ["mc2res_64.dll"]


def test_other_dll_drift_still_refuses(monkeypatch):
    a, b = _build_output_manifests(**{"SDL2.dll": 1, "crashpad_handler.exe": 1, "mc2res_64.dll": 1})
    assert _identity(monkeypatch, set(), a, b) == ["SDL2.dll"]


@pytest.mark.parametrize("name", ["mc2res_64.dll", "crashpad_handler.exe"])
def test_build_output_missing_on_one_side_still_refuses(monkeypatch, name):
    a, b = _build_output_manifests()
    assert _identity(monkeypatch, set(), a, {k: v for k, v in b.items() if k != name}) == [name]


def test_unknown_candidate_commit_does_not_crash(monkeypatch):
    monkeypatch.setattr(ab.asset_parity, "intentional_changes", lambda *x: {})
    a, b = _build_output_manifests(**{"mc2res_64.dll": 1})
    assert [p for p, _ in ab.candidate_mismatches(a, b, None, commit_a="a" * 40)] == ["mc2res_64.dll"]
