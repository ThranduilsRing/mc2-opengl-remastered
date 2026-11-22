import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
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
        "data/tgl/HangarGLB.mcasset.json"}


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
