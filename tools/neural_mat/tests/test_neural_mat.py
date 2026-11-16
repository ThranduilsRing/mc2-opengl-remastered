"""Neural-material workbench tests (CPU, no network, no GPU)."""
import hashlib
import json
import sys
import time
from pathlib import Path

import numpy as np
import pytest
import torch

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from tools.neural_mat import (enhance, export_nmat, fit_latents, golden_vectors, licence_gate,  # noqa: E402
                              quantize, reference_decode, sample_params, train)
from tools import gate_hosts  # noqa: E402

FIXTURE = Path(__file__).parent / "fixtures" / "matsynth_metadata.json"
# sha256 of the exported tiny .nmat (torch CPU, 1 thread, fixed seeds). A different torch/CPU build may legitimately
# change it; regenerate by running the test and copying the printed digest after checking the PSNR test still passes.
TINY_HASH = "b0bd9c5254a4319a39018f902b11e8fef2bde25ae0ee888d4dc71c44a9fe33cf"


def test_sampler_is_deterministic_and_in_range():
    a, ca = sample_params.sample(512, 7)
    b, cb = sample_params.sample(512, 7)
    assert np.array_equal(a, b) and np.array_equal(ca, cb)
    assert not np.array_equal(a, sample_params.sample(512, 8)[0])
    assert np.bincount(ca, minlength=5).min() > 60                      # 20 % per class, infill included
    assert (a[:, 8:16].sum(1) <= 1.0 + 1e-5).all() and (a[:, 0] >= 0.08 - 1e-6).all()
    d1 = sample_params.sample_dirs(64, 3)
    assert np.array_equal(d1[0], sample_params.sample_dirs(64, 3)[0]) and (d1[0][:, 2] > 0).all()


def test_white_furnace_energy_bounded():
    p, _ = sample_params.sample(300, 11)
    wi, _ = sample_params.sample_dirs(300, 12)
    E = enhance.furnace(p, wi, nq=128)
    assert np.isfinite(E).all() and E.max() <= 1.0 + 0.03


def test_energy_gate_refuses_gaining_material():
    # a decoder whose last layer is biased to a huge BRDF must be refused by the export gate
    model = train.Decoder("tiny", 8).eval()
    with torch.no_grad():
        model.layers[-1].bias[:3] = 4.0
    u = np.full((2, 8), 0.5, np.float32)
    q = quantize.quantize(model, np.tile(u, (64, 1)), np.tile([[0, 0, 1.0]], (128, 1)).astype(np.float32), np.tile([[0, 0, 1.0]], (128, 1)).astype(np.float32))
    with pytest.raises(export_nmat.EnergyGainError):
        export_nmat.check_energy(q, u)


def test_licence_gate_refuses_non_cc0_from_fixture():
    rows = licence_gate.load_metadata(FIXTURE)
    ok = {r["name"] for r, _ in licence_gate.select(rows)}
    refused = dict(licence_gate.refused(rows))
    assert ok == {"acg_asphalt_021", "cc0_metal_fixture"}
    assert "deschaintre" in refused["2136_basic_steel"] and "deschaintre" in refused["deschaintre_cc0_trap"]
    assert "CC-BY 4.0" in refused["ccby_concrete"] and "''" in refused["no_license_wood"]
    prov = licence_gate.provenance_row(rows[[r["name"] for r in rows].index("acg_asphalt_021")])
    assert prov["licence"] == "CC0 1.0" and set(prov) >= {"asset", "author", "licence_url", "source_url", "files", "local"}


def _tiny(tmp):
    torch.set_num_threads(1)
    ds = train.build_dataset(48, 16, seed=1, nq=8)
    model, emb, _ = train.train(ds, "tiny", steps=250, batch_mat=24, seed=1)
    # fit two "materials" (4 texels each) against the frozen decoder
    p, _ = sample_params.sample(8, 99)
    wi, wo = sample_params.sample_dirs(8 * 16, 100)
    pp = np.repeat(p, 16, 0)
    f, T, R = enhance.evaluate(pp, wi, wo, nq=8)
    sh = (8, 16)
    u, loss = fit_latents.fit(model, wi.reshape(*sh, 3), wo.reshape(*sh, 3), f.reshape(*sh, 3), T.reshape(sh), R.reshape(sh),
                              pool=emb, steps=40, k=8)
    wi_c, wo_c = sample_params.sample_dirs(256, 5)
    q = quantize.quantize(model, emb[np.arange(256) % len(emb)], wi_c, wo_c)
    path = Path(tmp) / "tiny.nmat"
    digest = export_nmat.write_nmat(path, q, meta={"tiny": True})
    return model, emb, q, u, path, digest


def test_tiny_pipeline_under_60s_with_fixed_hash(tmp_path):
    t0 = time.time()
    model, emb, q, u, path, digest = _tiny(tmp_path)
    assert time.time() - t0 < 60
    assert u.shape == (8, 8) and ((u >= 0) & (u <= 1)).all()
    (tmp_path / "again").mkdir()
    digest2 = _tiny(tmp_path / "again")[-1]
    assert digest == digest2                                           # deterministic
    if gate_hosts.current_host() != "pc":
        pytest.skip(f"TINY_HASH is pinned to the PC's torch/CPU numerics (gate host {gate_hosts.current_host()})")
    assert digest == TINY_HASH, digest


def test_quantized_reference_matches_float_model(tmp_path):
    model, emb, q, _, _, _ = _tiny(tmp_path)
    wi, wo = sample_params.sample_dirs(512, 777)                         # held-out directions
    u = emb[np.arange(512) % len(emb)]
    with torch.no_grad():
        f, T, R = (x.numpy() for x in model(torch.from_numpy(u), torch.from_numpy(wi), torch.from_numpy(wo)))
    fq, Tq, Rq = reference_decode.decode(q, u, wi, wo)
    assert quantize.psnr(np.log1p(fq), np.log1p(f)) >= 40.0
    assert quantize.psnr(Tq, T, 1.0) >= 40.0 and quantize.psnr(Rq, R, 1.0) >= 40.0


def test_nmat_round_trip(tmp_path):
    _, _, q, _, path, _ = _tiny(tmp_path)
    q2, head = export_nmat.read_nmat(path)
    assert head["format_version"] == 1 and q2["spec"] == q["spec"]
    for a, b in zip(q["layers"], q2["layers"]):
        assert a["kind"] == b["kind"] and np.array_equal(a["W"], b["W"]) and np.array_equal(a["b"], b["b"])
        if a["kind"] == "int8":
            assert np.array_equal(a["sw"], b["sw"]) and a["sx"] == b["sx"]
    wi, wo = sample_params.sample_dirs(32, 4)
    u = np.random.default_rng(0).uniform(0, 1, (32, 8)).astype(np.float32)
    for x, y in zip(reference_decode.decode(q, u, wi, wo), reference_decode.decode(q2, u, wi, wo)):
        assert np.array_equal(x, y)


def test_golden_vector_round_trip(tmp_path):
    model, emb, q, _, _, _ = _tiny(tmp_path)
    wi, wo = sample_params.sample_dirs(48, 21)
    u = emb[:48 % len(emb)].repeat(2, 0)[:48].astype(np.float32)
    u = np.resize(emb, (48, 8)).astype(np.float32)
    net = golden_vectors.build(model, q, u, wi, wo, "tiny16x3", rows=48, width=64)
    path = tmp_path / "g.json"
    golden_vectors.write(path, [net])
    doc = golden_vectors.read(path)
    assert doc["format_version"] == 1 and doc["nets"][0]["rows"] == 48
    n = doc["nets"][0]
    assert np.allclose(golden_vectors.run_float_tier(n).ravel(), n["expected"], atol=1e-9)
    assert np.allclose(np.asarray(n["nmat_ext"]["expected_f"]).reshape(48, 3), reference_decode.decode(q, u, wi, wo)[0], atol=1e-6)
    # FP16 exactness of the float tier
    w = np.asarray(n["layer_params"][0]["w"], np.float32)
    assert np.array_equal(w, w.astype(np.float16).astype(np.float32))
