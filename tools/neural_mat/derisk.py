"""De-risk run (N2): three materials -> universal decoder -> per-texel latents -> BC7 -> FLIP numbers.

Materials: a remastered-building ORM material (the shipped corrugatedsteel006a_orm.ktx2, neutral albedo), a MatSynth
CC0 metal and a MatSynth CC0 concrete (both through the licence gate). Reports FLIP at mip 0 and an RTS-distance mip
(box latent mips and prefiltered-fitted mip), GGX-proxy-lit vs neural-lit FLIP under night lights, and the BC7
latent error. CPU only; ~15-30 min.

  py -3 -m tools.neural_mat.derisk --work G:/mc2-tmp/neural-n2/run --shards <parquet...> --orm <orm.ktx2>
"""
from __future__ import annotations
import argparse, json, time
from pathlib import Path
import numpy as np
import torch

from . import (enhance as en, export_nmat as ex, fit_latents as fl, golden_vectors as gv, matsynth_ingest as mi,
               quantize as qz, reference_decode as rd, sample_params as sp, train as tr)

EDGE, MIP = 32, 2                                   # texture edge; RTS-distance mip (4x4 footprint)
D = 32                                              # directions per texel for targets / fits
WO_VIEW = np.array([0.30, 0.0, np.sqrt(1 - 0.09)], np.float32)
SUN = [(np.array([-0.40, 0.30, np.sqrt(1 - 0.25)], np.float32), 3.0)]
NIGHT = [(np.array([0.80, 0.10, np.sqrt(1 - 0.65)], np.float32), 5.0),
         (np.array([-0.55, -0.55, np.sqrt(1 - 0.605)], np.float32), 4.0),
         (np.array([0.0, 0.92, np.sqrt(1 - 0.8464)], np.float32), 3.0)]


def s2l(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def maps_to_params(base_lin, rough, metal):
    n = len(rough)
    p = np.zeros((n, en.P), np.float32)
    p[:, 0] = np.clip(rough, 0.08, 1.0)
    p[:, 1], p[:, 2], p[:, 3] = 2.4, 1.0, 2.0
    p[:, 4:7] = np.clip(base_lin, 0.02, 1.0)
    p[:, 7] = 0.04
    p[:, 8], p[:, 9] = 0.68 * metal, 0.17 * metal
    p[:, 10], p[:, 11] = 0.6 * (1 - metal), 0.15 * (1 - metal)
    p[:, 16], p[:, 17], p[:, 18], p[:, 19] = 0.5, 0.5, 0.2, 8.0
    return p, (base_lin * (1 - metal)[:, None]).astype(np.float32)


def load_orm(path, edge=EDGE):
    from PIL import Image
    import io
    from tools.content_pipeline import texcook
    vk, w, h, levels = texcook.read_ktx2(Path(path).read_bytes())
    im = Image.open(io.BytesIO(ex._dds_bc7(levels[0], w, h))).convert("RGBA").resize((edge, edge), Image.BOX)
    a = np.asarray(im, np.float32) / 255.0
    ao, rough, metal = a[..., 0], a[..., 1], a[..., 2]
    base = np.full((edge, edge, 3), 0.60, np.float32)       # no shipped albedo: neutral steel grey, AO darkens the diffuse base
    return dict(base=base, rough=rough, metal=metal, ao=ao)


def targets(params, diff, ao, wi, wo, nq):
    """Per-texel reference f/T/R over shared directions [D]: arrays [N,D,...]; diffuse albedo for the reference render."""
    N = len(params)
    pp = np.repeat(params, D, 0)
    f, T, R = en.evaluate(pp, np.tile(wi, (N, 1)), np.tile(wo, (N, 1)), nq=nq)
    return f.reshape(N, D, 3), T.reshape(N, D), R.reshape(N, D)


def box(a, m):
    """Average texel footprints: a [E*E,...] -> [(E>>m)^2,...]."""
    e = EDGE
    x = a.reshape(e, e, *a.shape[1:])
    k = 1 << m
    x = x.reshape(e // k, k, e // k, k, *a.shape[1:]).mean((1, 3))
    return x.reshape(-1, *a.shape[1:])


def shade(f, T, diff, lights, wo, ao=None, use_T=True):
    """Radiance [N,3] under directional lights; f [N,3], T [N] already evaluated at each light: lists per light."""
    out = np.zeros((len(f[0]), 3), np.float32)
    for (wi, inten), fl_, Tl in zip(lights, f, T):
        d = diff * (Tl[:, None] if use_T else 1.0) / np.pi
        if ao is not None:
            d = d * ao[:, None]
        out += (fl_ + d) * wi[2] * inten
    return out


def to_ldr(rad, exposure):
    x = rad * exposure
    x = x / (1 + x)
    return np.clip(np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(np.maximum(x, 1e-9), 1 / 2.4) - 0.055), 0, 1)


def flip_mean(ref, test, edge):
    import flip_evaluator as flip
    up = 128 // edge
    r = np.repeat(np.repeat(ref.reshape(edge, edge, 3), up, 0), up, 1).astype(np.float32)
    t = np.repeat(np.repeat(test.reshape(edge, edge, 3), up, 0), up, 1).astype(np.float32)
    _, mean, _ = flip.evaluate(r, t, "LDR", inputsRGB=True, applyMagma=False, computeMeanError=True)
    return float(mean)


def eval_lights(q, u, lights, wo):
    """Decode (f, T) per light for per-texel latents u [N,L] with the quantized reference decoder."""
    fs, Ts = [], []
    for wi, _ in lights:
        n = len(u)
        f, T, _ = rd.decode(q, u, np.broadcast_to(wi, (n, 3)).astype(np.float32), np.broadcast_to(wo, (n, 3)).astype(np.float32))
        fs.append(f)
        Ts.append(T)
    return fs, Ts


def proxy_lights(q, u, lights, wo):
    return [rd.proxy_f(q, u, np.broadcast_to(wi, (len(u), 3)).astype(np.float32), np.broadcast_to(wo, (len(u), 3)).astype(np.float32))
            for wi, _ in lights]


def reference_lights(params, lights, wo):
    fs, Ts = [], []
    for wi, _ in lights:
        n = len(params)
        f, T, _ = en.evaluate(params, np.broadcast_to(wi, (n, 3)).astype(np.float32), np.broadcast_to(wo, (n, 3)).astype(np.float32), nq=96)
        fs.append(f)
        Ts.append(T)
    return fs, Ts


def bc7_chain(u, scale, bias):
    """BC7 (texconv, -nogpu) round trip of the latent texture's box mip chain; returns decoded latent per mip."""
    from tools.content_pipeline import texcook
    a, b = ex.latent_to_rgba(u.reshape(EDGE, EDGE, -1), scale, bias)
    out = [[], []]
    for k, img in enumerate((a, b)):
        for lv in texcook.mip_chain(img, "neural_latent"):
            out[k].append(ex.bc7_roundtrip(lv))
    return [ex.rgba_to_latent(out[0][m], out[1][m], scale, bias).reshape(-1, u.shape[-1]) for m in range(len(out[0]))]


def run_material(name, mat, model, q, emb, log, nq):
    t0 = time.time()
    e = EDGE
    base_lin = s2l(mat["base"]).reshape(-1, 3)
    params, diff = maps_to_params(base_lin, mat["rough"].reshape(-1), mat["metal"].reshape(-1))
    ao = mat.get("ao")
    ao = None if ao is None else ao.reshape(-1)
    wi, wo = sp.sample_dirs(D, 4242)
    wi = np.broadcast_to(wi, (D, 3))
    f, T, R = targets(params, diff, ao, wi, wo, nq)
    log(f"{name}: targets {time.time() - t0:.0f}s")
    WI, WO = [np.ascontiguousarray(np.broadcast_to(x, (len(params), D, 3))) for x in (wi, wo)]
    u0, floss = fl.fit(model, WI, WO, f, T, R, pool=emb, steps=150, k=64)
    log(f"{name}: fit {time.time() - t0:.0f}s loss {floss.mean():.4f}")
    # reference prefiltered shading per mip (analytic enhanced BRDF per texel, then box average)
    res = {}
    gain = {}
    for tag, lights in (("sun", SUN), ("night", NIGHT)):
        fs, Ts = reference_lights(params, lights, WO_VIEW)
        ref_rad = shade(fs, Ts, diff, lights, WO_VIEW, ao)
        gain[tag] = 0.5 / float(np.mean(ref_rad))
        res[tag] = dict(ref=ref_rad, fs=fs, Ts=Ts)
    scale, bias = ex.latent_scale_bias(u0.reshape(e, e, -1))
    bc = bc7_chain(u0, scale, bias)
    out = dict(fit_loss_mean=float(floss.mean()), fit_loss_p95=float(np.percentile(floss, 95)))
    # BC7 latent error (mip 0, latent units; channel range is scale)
    err = np.abs(bc[0] - u0)
    out["bc7_latent_err"] = dict(mean=float(err.mean()), max=float(err.max()), rel_to_range=float((err / scale).mean()),
                                 note="decoded BC7 minus fitted latent, latent units (0..1)")
    # mip-0 FLIP: fitted float / quantized decoder with exact latents, and with BC7 latents
    def neural_img(tag, lights, uu, m):
        fn, Tn = eval_lights(q, uu, lights, WO_VIEW)
        return shade(fn, Tn, box(diff, m) if m else diff, lights, WO_VIEW, None if ao is None else (box(ao, m) if m else ao))
    flipd = {}
    for tag, lights in (("sun", SUN), ("night", NIGHT)):
        ref = res[tag]["ref"]
        ex_ = gain[tag]
        flipd[f"{tag}_mip0_exact_latent"] = flip_mean(to_ldr(ref, ex_), to_ldr(neural_img(tag, lights, u0, 0), ex_), e)
        flipd[f"{tag}_mip0_bc7_latent"] = flip_mean(to_ldr(ref, ex_), to_ldr(neural_img(tag, lights, bc[0], 0), ex_), e)
        # RTS-distance mip: reference = footprint-averaged radiance (prefiltered truth)
        refm = box(ref, MIP)
        em = e >> MIP
        flipd[f"{tag}_mip{MIP}_box_latent_bc7"] = flip_mean(to_ldr(refm, ex_), to_ldr(neural_img(tag, lights, bc[MIP], MIP), ex_), em)
    # prefiltered-fitted mip: fit mip-MIP latents to the footprint-averaged targets, BC7 round trip of that level only
    em = e >> MIP
    fm, Tm, Rm = box(f, MIP), box(T, MIP), box(R, MIP)
    n_m = em * em
    um, _ = fl.fit(model, np.ascontiguousarray(np.broadcast_to(wi, (n_m, D, 3))), np.ascontiguousarray(np.broadcast_to(wo, (n_m, D, 3))), fm, Tm, Rm, pool=emb, steps=150, k=64)
    a, b = ex.latent_to_rgba(um.reshape(em, em, -1), scale, bias)
    umb = ex.rgba_to_latent(ex.bc7_roundtrip(a), ex.bc7_roundtrip(b), scale, bias).reshape(-1, um.shape[-1])
    for tag, lights in (("sun", SUN), ("night", NIGHT)):
        ex_ = gain[tag]
        flipd[f"{tag}_mip{MIP}_fitted_latent_bc7"] = flip_mean(to_ldr(box(res[tag]["ref"], MIP), ex_),
                                                              to_ldr(neural_img(tag, lights, umb, MIP), ex_), em)
    # night lights: GGX proxy (z-only head, T=1 diffuse) vs neural, mip 0 against the analytic reference
    ref = res["night"]["ref"]
    ex_ = gain["night"]
    pf = proxy_lights(q, bc[0], NIGHT, WO_VIEW)
    prox = shade(pf, [np.ones(len(u0), np.float32)] * len(NIGHT), diff, NIGHT, WO_VIEW, None if ao is None else ao, use_T=False)
    flipd["night_mip0_ggx_proxy"] = flip_mean(to_ldr(ref, ex_), to_ldr(prox, ex_), e)
    flipd["night_mip0_neural"] = flipd["night_mip0_bc7_latent"]
    out["flip"] = flipd
    try:
        out["energy_gate_max"] = ex.check_energy(q, u0[::16])
    except ex.EnergyGainError as err:
        out["energy_gate_max"] = f"REFUSED: {err}"
    out["seconds"] = time.time() - t0
    return out, u0, dict(scale=scale, bias=bias)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--work", required=True)
    ap.add_argument("--shards", nargs="+", required=True)
    ap.add_argument("--orm", required=True)
    ap.add_argument("--materials", type=int, default=2048)
    ap.add_argument("--dirs", type=int, default=64)
    ap.add_argument("--steps", type=int, default=4000)
    ap.add_argument("--nq", type=int, default=24)
    ap.add_argument("--workers", type=int, default=3)
    ap.add_argument("--budget-min", type=float, default=45.0)
    a = ap.parse_args(argv)
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    T0 = time.time()

    def log(s):
        print(f"[{(time.time() - T0) / 60:5.1f} min] {s}", flush=True)

    mats = {"building_orm:corrugatedsteel006a": load_orm(a.orm)}
    picked, refused = mi.load(a.shards, ("metal", "concrete"), per_category=1, edge=EDGE)
    for k, v in picked.items():
        mats[k] = dict(base=v["basecolor"], rough=v["roughness"], metal=v["metallic"])
    mi.write_provenance(work / "matsynth_provenance.json", picked)
    log(f"materials {list(mats)}; refused by licence gate: {len(refused)}")
    ds = tr.build_dataset(a.materials, a.dirs, 0, a.nq, a.workers)
    log("dataset built")
    model, emb, hist = tr.train(ds, "high", steps=a.steps, seed=0, log=lambda s: log(s) if "00 " in s else None)
    tr.save(work / "decoder_high.pt", model, emb, hist)
    log("decoder trained")
    wi_c, wo_c = sp.sample_dirs(4096, 5)
    q = qz.quantize(model, emb[np.arange(4096) % len(emb)], wi_c, wo_c)
    log("quantized: " + json.dumps(q["report"]))
    wi_h, wo_h = sp.sample_dirs(2048, 777)
    uh = emb[np.arange(2048) % len(emb)]
    with torch.no_grad():
        fh, Th, Rh = (x.numpy() for x in model(torch.from_numpy(uh), torch.from_numpy(wi_h), torch.from_numpy(wo_h)))
    fq, Tq, Rq = rd.decode(q, uh, wi_h, wo_h)
    report = dict(config=vars(a), quant_report=q["report"], refused=refused, materials={},
                  decoder_psnr_db=dict(f_log1p=qz.psnr(np.log1p(fq), np.log1p(fh)), T=qz.psnr(Tq, Th, 1.0), R=qz.psnr(Rq, Rh, 1.0)),
                  train_final=hist[-1])
    for name, mat in mats.items():
        if time.time() - T0 > a.budget_min * 60:
            report["materials"][name] = {"skipped": "time budget"}
            continue
        r, u0, sb = run_material(name, mat, model, q, emb, log, a.nq)
        report["materials"][name] = r
        stem = name.split(":")[1]
        report["materials"][name]["latent_manifest"] = ex.write_latent_textures(work / "latents", stem, u0.reshape(EDGE, EDGE, -1))
        log(f"{name}: {json.dumps(r['flip'])}")
    digest = ex.write_nmat(work / "decoder_high.nmat", q, meta={"derisk": True})
    nets = [gv.build(model, q, np.resize(emb, (48, 8)).astype(np.float32), *sp.sample_dirs(48, 31), "high14x64x5", rows=48)]
    gv.write(work / "golden.json", nets)
    report["nmat_sha256"], report["nmat_bytes"] = digest, (work / "decoder_high.nmat").stat().st_size
    report["wall_minutes"] = (time.time() - T0) / 60
    (work / "report.json").write_text(json.dumps(report, indent=2, default=float), encoding="utf-8")
    log("done")


if __name__ == "__main__":
    main()
