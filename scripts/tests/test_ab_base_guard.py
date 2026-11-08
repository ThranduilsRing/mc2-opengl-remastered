"""ab.py refuses a cross-base A/B: commit ancestry from the install identity, data/shader manifest equality, CROSS-BASE stamp."""
from __future__ import annotations

import subprocess
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
    lines += [f"{k},{v},1,{commit},t,0" for k, v in rows.items()]
    (d / ".deployed_manifest.csv").write_text("\n".join(lines) + "\n")
    (d / "mc2.exe").write_bytes(b"x")   # no embedded fingerprint: identity falls back to the manifest row
    return str(d)


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


def test_main_refuses_then_allows_and_stamps(tmp_path, repo, monkeypatch):
    a = _install(tmp_path / "a", repo[1])
    b = _install(tmp_path / "b", repo[2])
    argv = ["--bookmarks", "x", "--install-a", a, "--install-b", b, "--out-root", str(tmp_path / "o")]
    with pytest.raises(SystemExit, match="refusing an A/B across different bases"):
        ab.main(argv)
    seen = {}
    monkeypatch.setattr(ab, "resolve_installs", lambda args, out: (a, b))
    monkeypatch.setattr(ab, "run_capture", lambda *x: None)
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
