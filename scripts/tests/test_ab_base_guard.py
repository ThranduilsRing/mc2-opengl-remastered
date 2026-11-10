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
