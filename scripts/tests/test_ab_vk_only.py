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
    (inst / "saved_bookmarks").mkdir()
    (inst / "saved_bookmarks" / "x.json").write_text("{}")
    (inst / ab.MANIFEST).write_text("manifest_version,v1\nrelpath,sha256,bytes,src_commit\n" +
        "\n".join(f"{p},{sha},1,{'a' * 40}" for p, sha in _identity()["payload"].items()))
    monkeypatch.setattr(ab, "exe_commit", lambda *args: "a" * 40)
    ea, eb = tmp_path / "a.exe", tmp_path / "b.exe"
    ea.write_bytes(b"A")
    eb.write_bytes(b"B")
    runs = []
    def fake_capture(install, bms, out, env, **kw):  # the staged install exists only while the run is live
        runs.append((install, Path(out).name, env, (Path(install) / "mc2.exe").read_bytes(), (Path(install) / "data.bin").exists()))
    monkeypatch.setattr(ab, "run_capture", fake_capture)
    monkeypatch.setattr(ab, "compare", lambda *a, **k: None)
    monkeypatch.setattr(ab, "base_problems", lambda *a, **k: [])  # fake exes carry no fingerprint; the guard has its own tests
    with pytest.raises(SystemExit) as ex:
        ab.main(["--bookmarks", "x", "--exe-a", str(ea), "--exe-b", str(eb), "--install", str(inst), "--out-root", str(tmp_path / "o")])
    assert ex.value.code == 0
    assert [Path(r[0]).name for r in runs] == ["install_a", "install_b"] and [r[1] for r in runs] == ["a", "b"]
    assert runs[0][3] == b"A" and runs[1][3] == b"B" and runs[0][4] and (inst / "mc2.exe").read_bytes() == b"orig"
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


def test_ensure_saved_bookmarks_copies_when_missing(tmp_path):
    inst = tmp_path / "inst"
    inst.mkdir()
    ref = tmp_path / "ref"
    (ref / "saved_bookmarks").mkdir(parents=True)
    (ref / "saved_bookmarks" / "a.json").write_text("{}")
    (ref / "saved_bookmarks" / "b.json").write_text("{}")
    ab.ensure_saved_bookmarks(inst, ref, [str(inst)])
    assert (inst / "saved_bookmarks" / "a.json").is_file() and (inst / "saved_bookmarks" / "b.json").is_file()
    # the reference is read-only: it keeps exactly its own bookmarks
    assert sorted(p.name for p in (ref / "saved_bookmarks").glob("*.json")) == ["a.json", "b.json"]


def test_ensure_saved_bookmarks_noop_when_present(tmp_path):
    inst = tmp_path / "inst"
    (inst / "saved_bookmarks").mkdir(parents=True)
    (inst / "saved_bookmarks" / "own.json").write_text("{}")
    ref = tmp_path / "ref"
    (ref / "saved_bookmarks").mkdir(parents=True)
    (ref / "saved_bookmarks" / "other.json").write_text("{}")
    ab.ensure_saved_bookmarks(inst, ref, [str(inst)])
    assert sorted(p.name for p in (inst / "saved_bookmarks").glob("*.json")) == ["own.json"]


def test_ensure_saved_bookmarks_fails_when_reference_empty(tmp_path):
    inst = tmp_path / "inst"
    inst.mkdir()
    ref = tmp_path / "ref"
    (ref / "saved_bookmarks").mkdir(parents=True)
    with pytest.raises(SystemExit) as e:
        ab.ensure_saved_bookmarks(inst, ref, [str(inst)])
    assert "has none either" in str(e.value)


def test_stage_exe_swap_replaces_exe_pdb_and_shaders(tmp_path, monkeypatch):
    b = tmp_path / "b"
    b.mkdir()
    (b / "mc2.exe").write_bytes(b"Bexe")
    (b / "mc2.pdb").write_bytes(b"Bpdb")
    (b / "data.bin").write_bytes(b"d")
    (b / "shaders/vulkan").mkdir(parents=True)
    (b / "shaders/vulkan/x.spv").write_bytes(b"Bspv")
    (b / "shaders/include").mkdir(parents=True)
    (b / "shaders/include/y.glsl").write_bytes(b"By")
    (b / ab.MANIFEST).write_text("manifest_version,v1\nrelpath,sha256,bytes,src_commit\n" +
        "\n".join(f"{p},{hashlib.sha256((b / p).read_bytes()).hexdigest()},{(b / p).stat().st_size},{'b' * 40}"
                  for p in ["mc2.exe", "shaders/vulkan/x.spv", "shaders/include/y.glsl"]))
    aexe_dir = tmp_path / "aexe"
    (aexe_dir / "shaders/vulkan").mkdir(parents=True)
    (aexe_dir / "mc2.exe").write_bytes(b"Aexe")
    (aexe_dir / "mc2.pdb").write_bytes(b"Apdb")
    (aexe_dir / "shaders/vulkan/x.spv").write_bytes(b"Aspv")
    (aexe_dir / "shaders/vulkan/z.spv").write_bytes(b"Zspv")  # a shader the B install did not ship
    monkeypatch.setattr(ab, "exe_commit", lambda *args: "c" * 40)
    dest = tmp_path / "a"
    ab.stage_exe_swap(b, aexe_dir / "mc2.exe", dest)
    assert (dest / "mc2.exe").read_bytes() == b"Aexe" and (dest / "mc2.pdb").read_bytes() == b"Apdb"
    assert (dest / "shaders/vulkan/x.spv").read_bytes() == b"Aspv"
    assert (dest / "shaders/vulkan/z.spv").read_bytes() == b"Zspv"
    assert not (dest / "shaders/include/y.glsl").exists()  # B's shader the swapped build does not ship
    assert (dest / "data.bin").read_bytes() == b"d"        # data stays B's (hardlinked)
    ident = ab.payload_identity(dest)  # the rewritten manifest verifies the swapped bytes
    assert ident["payload"]["mc2.exe"] == hashlib.sha256(b"Aexe").hexdigest()
    assert set(ident["payload"]) == {"mc2.exe", "shaders/vulkan/x.spv", "shaders/vulkan/z.spv"}


def test_exe_a_swap_mode_stages_and_records(tmp_path, monkeypatch):
    b = tmp_path / "b"
    b.mkdir()
    (b / "mc2.exe").write_bytes(b"Bexe")
    (b / "data.bin").write_bytes(b"d")
    (b / "shaders/vulkan").mkdir(parents=True)
    (b / "shaders/vulkan/x.spv").write_bytes(b"Bspv")
    (b / "saved_bookmarks").mkdir()
    (b / "saved_bookmarks" / "x.json").write_text("{}")
    (b / ab.MANIFEST).write_text("manifest_version,v1\nrelpath,sha256,bytes,src_commit\n" +
        "\n".join(f"{p},{hashlib.sha256((b / p).read_bytes()).hexdigest()},{(b / p).stat().st_size},{'c' * 40}"
                  for p in ["mc2.exe", "shaders/vulkan/x.spv"]))
    aexe_dir = tmp_path / "aexe"
    (aexe_dir / "shaders/vulkan").mkdir(parents=True)
    (aexe_dir / "mc2.exe").write_bytes(b"Aexe")
    (aexe_dir / "shaders/vulkan/x.spv").write_bytes(b"Aspv")
    monkeypatch.setattr(ab, "exe_commit", lambda *args: "c" * 40)
    seen = {}

    def fake_capture(install, bms, out, env, **kw):  # the staged A install exists only while the run is live
        if Path(out).name == "a":
            a_install = Path(install)
            seen.update(exe=(a_install / "mc2.exe").read_bytes(), spv=(a_install / "shaders/vulkan/x.spv").read_bytes(),
                        data=(a_install / "data.bin").read_bytes(), a_install=a_install)
    monkeypatch.setattr(ab, "run_capture", fake_capture)
    captured = {}
    monkeypatch.setattr(ab, "compare", lambda *a, **k: captured.update(k))
    monkeypatch.setattr(ab, "base_problems", lambda *a, **k: [])
    with pytest.raises(SystemExit) as ex:
        ab.main(["--bookmarks", "x", "--exe-a", str(aexe_dir / "mc2.exe"), "--install-b", str(b),
                 "--out-root", str(tmp_path / "o")])
    assert ex.value.code == 0
    assert captured["exe_a_swap"]["exe"] == str(aexe_dir / "mc2.exe")
    assert captured["exe_a_swap"]["b_install"] == str(b)
    assert seen["exe"] == b"Aexe" and seen["spv"] == b"Aspv" and seen["data"] == b"d"
    assert not seen["a_install"].exists()  # deleted when the run ended


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


def test_ensure_saved_bookmarks_never_writes_into_a_user_install(tmp_path):
    """P2: a real (not staged) install without bookmarks is refused and stays byte-for-byte unchanged."""
    inst = tmp_path / "inst"
    inst.mkdir()
    (inst / "mc2.exe").write_bytes(b"exe")
    before = sorted((p.relative_to(inst).as_posix(), p.read_bytes()) for p in inst.rglob("*") if p.is_file())
    ref = tmp_path / "ref"
    (ref / "saved_bookmarks").mkdir(parents=True)
    (ref / "saved_bookmarks" / "a.json").write_text("{}")
    with pytest.raises(SystemExit) as e:
        ab.ensure_saved_bookmarks(inst, ref, [])
    assert "will not write into it" in str(e.value)
    assert sorted((p.relative_to(inst).as_posix(), p.read_bytes()) for p in inst.rglob("*") if p.is_file()) == before
    assert not (inst / "saved_bookmarks").exists()
