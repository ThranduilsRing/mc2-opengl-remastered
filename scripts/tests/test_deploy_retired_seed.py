import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import console_child
import deploy_payload as dp


def test_prune_retired_seed_payload_only_removes_matching_bytes(tmp_path):
    tgl = tmp_path / "data" / "tgl"
    tgl.mkdir(parents=True)
    (tgl / "quonset.ini").write_bytes(b"modded by the owner")
    (tgl / "QuonsetGLB.glb").write_bytes(b"x")
    dp.prune_retired_seed_payload(str(tmp_path))
    assert (tgl / "quonset.ini").exists() and (tgl / "QuonsetGLB.glb").exists()
    assert set(dp.RETIRED_SEED_PAYLOAD) == {
        "data/tgl/quonset.ini", "data/tgl/QuonsetGLB.glb", "data/tgl/QuonsetGLB.mcasset.json",
        "data/tgl/hangar.ini", "data/tgl/hangar.ini.cookstamp", "data/tgl/HangarGLB.glb",
        "data/tgl/HangarGLB.mcasset.json", "data/tgl/quonset.ini.cookstamp"}


def test_prune_retired_seed_payload_removes_exact_retired_file(tmp_path, monkeypatch):
    tgl = tmp_path / "data" / "tgl"
    tgl.mkdir(parents=True)
    f = tgl / "quonset.ini"
    f.write_bytes(b"retired")
    monkeypatch.setitem(dp.RETIRED_SEED_PAYLOAD, "data/tgl/quonset.ini", dp.sha256_file(f))
    dp.prune_retired_seed_payload(str(tmp_path))
    assert not f.exists()


def test_asset_parity_does_not_sync_retired_payload_back(tmp_path, monkeypatch):
    import asset_parity as ap
    ref, inst = tmp_path / "ref", tmp_path / "inst"
    for root in (ref, inst):
        (root / "data" / "tgl").mkdir(parents=True)
        (root / "data" / "tgl" / "keep.ini").write_bytes(b"k")
    (ref / "data" / "tgl" / "hangar.ini").write_bytes(b"retired")
    monkeypatch.setitem(dp.RETIRED_SEED_PAYLOAD, "data/tgl/hangar.ini",
                        dp.sha256_file(ref / "data" / "tgl" / "hangar.ini"))
    assert ap.compare(inst, ref, skip=set()).missing == []


def test_prune_retired_seed_payload_honours_preserve(tmp_path, monkeypatch):
    tgl = tmp_path / "data" / "tgl"
    tgl.mkdir(parents=True)
    kept, gone = tgl / "quonset.ini", tgl / "hangar.ini"
    for f in (kept, gone):
        f.write_bytes(b"retired")
        monkeypatch.setitem(dp.RETIRED_SEED_PAYLOAD, "data/tgl/" + f.name, dp.sha256_file(f))
    dp.prune_retired_seed_payload(str(tmp_path), preserve={"Data\\tgl\\quonset.ini"})
    assert kept.exists() and not gone.exists()


def test_retired_seed_hashes_match_real_blobs():
    import hashlib
    import subprocess
    repo = Path(__file__).resolve().parents[2]
    seed = Path("G:/mc2-installs/_lean-seed")
    checked = 0
    for rel, digest in dp.RETIRED_SEED_PAYLOAD.items():
        known = set()
        r = subprocess.run(["git", "show", f"c43ed3a2b^:{rel}"], cwd=repo, capture_output=True,
                           **console_child.child_kwargs(redirected=True))
        if r.returncode == 0:
            known.add(hashlib.sha256(r.stdout).hexdigest())
        if (seed / rel).is_file():
            known.add(dp.sha256_file(seed / rel))
        if not known:
            continue   # in neither git nor the seed (history shallow, seed absent): nothing to compare against
        assert digest in known, f"{rel}: pinned {digest} matches neither the c43ed3a2b^ blob nor the lean seed"
        checked += 1
    if not checked:
        import pytest
        pytest.skip("no git history and no lean seed available")


def test_retired_seed_payload_is_not_pinned_as_stock():
    import json
    repo = Path(__file__).resolve().parents[2]
    stock = json.loads((repo / "data/kit/stock-textures.json").read_text())["sha256"]
    retired = set(dp.RETIRED_SEED_PAYLOAD.values())
    assert not [k for k, v in stock.items() if retired & set(v if isinstance(v, list) else [v])]


def test_prune_retired_seed_payload_honours_directory_prefix(tmp_path, monkeypatch):
    tgl = tmp_path / "data" / "tgl"
    tgl.mkdir(parents=True)
    kept, gone = tgl / "quonset.ini", tgl / "hangar.ini"
    for f in (kept, gone):
        f.write_bytes(b"retired")
        monkeypatch.setitem(dp.RETIRED_SEED_PAYLOAD, "data/tgl/" + f.name, dp.sha256_file(f))
    dp.prune_retired_seed_payload(str(tmp_path), preserve={"data/tgl"})
    assert kept.exists() and gone.exists()


def test_prune_retired_seed_payload_partial_prefix_does_not_preserve(tmp_path, monkeypatch):
    tgl = tmp_path / "data" / "tgl"
    tgl.mkdir(parents=True)
    kept = tgl / "quonset.ini"
    kept.write_bytes(b"retired")
    monkeypatch.setitem(dp.RETIRED_SEED_PAYLOAD, "data/tgl/quonset.ini", dp.sha256_file(kept))
    dp.prune_retired_seed_payload(str(tmp_path), preserve={"data/tg"})
    assert not kept.exists()
