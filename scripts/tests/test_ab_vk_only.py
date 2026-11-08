"""tools/visual_verify/ab.py is a VK-only before/after tool: no --skip-gl, two builds in, montage + MAD out."""
from __future__ import annotations

import json
import hashlib
import sys
from pathlib import Path

import pytest
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "visual_verify"))
import ab  # noqa: E402


@pytest.fixture(autouse=True)
def ancestry_covered_elsewhere(monkeypatch):
    monkeypatch.setattr(ab, "commit_order_problem", lambda *args: None)


def _identity():
    payload = {"mc2.exe": hashlib.sha256(b"orig").hexdigest(), "shaders/vulkan/x.spv": hashlib.sha256(b"spv").hexdigest()}
    return dict(commit="a" * 40, payload=payload, manifest=payload.copy())


def _shot(d, bm, color):
    (d / bm).mkdir(parents=True)
    Image.new("RGB", (40, 20), color).save(d / bm / f"{bm}.png")
    ab.stamp_identity(d, _identity())


def test_compare_only(tmp_path):
    _shot(tmp_path / "a", "bm", (10, 10, 10))
    _shot(tmp_path / "b", "bm", (30, 10, 10))
    with pytest.raises(SystemExit) as ex:
        ab.main(["--compare-only", str(tmp_path / "a"), str(tmp_path / "b"), "--out-root", str(tmp_path / "o"), "--crop", "0,0,0.5,1"])
    assert ex.value.code == 0
    s = json.loads((tmp_path / "o" / "summary.json").read_text())
    assert s["rows"][0]["mad"] == pytest.approx(20 / 3, abs=0.01)
    assert (tmp_path / "o" / "bm" / "montage.jpg").exists() and (tmp_path / "o" / "bm" / "diff.jpg").exists()


def test_exe_mode_stages_installs_and_runs_both(tmp_path, monkeypatch):
    inst = tmp_path / "inst"
    inst.mkdir()
    (inst / "mc2.exe").write_bytes(b"orig")
    (inst / "data.bin").write_bytes(b"d")
    (inst / "shaders/vulkan").mkdir(parents=True)
    (inst / "shaders/vulkan/x.spv").write_bytes(b"spv")
    (inst / ab.MANIFEST).write_text("manifest_version,v1\nrelpath,sha256,bytes,src_commit\n" +
        "\n".join(f"{p},{sha},1,{'a' * 40}" for p, sha in _identity()["payload"].items()))
    monkeypatch.setattr(ab, "exe_commit", lambda *args: "a" * 40)
    ea, eb = tmp_path / "a.exe", tmp_path / "b.exe"
    ea.write_bytes(b"A")
    eb.write_bytes(b"B")
    runs = []
    monkeypatch.setattr(ab, "run_capture", lambda install, bms, out, env: runs.append((install, Path(out).name, env)))
    monkeypatch.setattr(ab, "compare", lambda *a, **k: None)
    monkeypatch.setattr(ab, "base_problems", lambda *a, **k: [])  # fake exes carry no fingerprint; the guard has its own tests
    with pytest.raises(SystemExit) as ex:
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst), "--out-root", str(tmp_path / "o")])
    assert ex.value.code == 0
    assert [Path(r[0]).name for r in runs] == ["install_a", "install_b"] and [r[1] for r in runs] == ["a", "b"]
    assert (Path(runs[0][0]) / "mc2.exe").read_bytes() == b"A" and (Path(runs[1][0]) / "mc2.exe").read_bytes() == b"B"
    assert (Path(runs[0][0]) / "data.bin").exists() and (inst / "mc2.exe").read_bytes() == b"orig"
    assert "MC2_RENDER_BACKEND=vulkan" in runs[0][2]


def test_rejects_mixed_and_missing(tmp_path):
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--install-a", "p", "--exe-b", "q", "--out-root", str(tmp_path)])
    with pytest.raises(SystemExit):
        ab.main(["--bookmarks", "x", "--out-root", str(tmp_path)])


def test_seed_install_copies_from_reference_and_fails_clearly(tmp_path):
    ref, inst = tmp_path / "ref", tmp_path / "inst"
    (ref / "saved_bookmarks").mkdir(parents=True)
    (ref / "data/textures").mkdir(parents=True)
    (ref / "saved_bookmarks/bm1.json").write_text("{}")
    (ref / "data/textures/x.burnin.dds").write_text("t")
    inst.mkdir()
    ab.seed_install(inst, "bm1", ref)
    assert (inst / "saved_bookmarks/bm1.json").is_file() and (inst / "data/textures/x.burnin.dds").is_file()
    with pytest.raises(SystemExit) as e:
        ab.seed_install(inst, "bm1,nope", ref)
    assert "nope.json" in str(e.value)
    (inst / "data/textures/x.burnin.dds").unlink()
    (ref / "data/textures/x.burnin.dds").unlink()
    with pytest.raises(SystemExit) as e:
        ab.seed_install(inst, "bm1", ref)
    assert "burnin" in str(e.value)


def _run_compare_only(tmp_path):
    with pytest.raises(SystemExit) as ex:
        ab.main(["--compare-only", str(tmp_path / "a"), str(tmp_path / "b"), "--out-root", str(tmp_path / "o")])
    return ex.value.code


def test_compare_only_fails_when_one_side_lacks_a_bookmark(tmp_path):
    _shot(tmp_path / "a", "bm", (10, 10, 10))
    _shot(tmp_path / "a", "only_a", (10, 10, 10))
    _shot(tmp_path / "b", "bm", (10, 10, 10))
    assert _run_compare_only(tmp_path) == 1
    assert "only_a: side B" in "".join(json.loads((tmp_path / "o" / "summary.json").read_text())["problems"])


def test_compare_only_fails_on_an_empty_png(tmp_path):
    _shot(tmp_path / "a", "bm", (10, 10, 10))
    _shot(tmp_path / "b", "bm", (10, 10, 10))
    (tmp_path / "b" / "bm" / "bm.png").write_bytes(b"")
    assert _run_compare_only(tmp_path) == 1


def test_compare_only_fails_when_nothing_was_compared(tmp_path):
    (tmp_path / "a").mkdir()
    (tmp_path / "b").mkdir()
    for side in ("a", "b"):
        ab.stamp_identity(tmp_path / side, _identity())
    assert _run_compare_only(tmp_path) == 1
