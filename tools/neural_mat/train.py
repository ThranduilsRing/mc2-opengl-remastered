"""Train the universal decoder as an auto-decoder: one learned latent per procedural material (study 1.1).

Targets come from enhance.evaluate (analytic enhanced BRDF). Loss = 0.95 f + 0.04 T + 0.01 R (f in a log-compressed
space), plus a small proxy-head term. Latents are clamped to [0,1] (BC7 texel range) and jittered by +-0.005 plus
BC7-like noise so the decoder tolerates the cook.

Full-scale command (box): see README ("Full-scale training").
"""
from __future__ import annotations
import argparse, json, time
from pathlib import Path
import numpy as np
import torch

from . import enhance as en, sample_params as sp
from .model import Decoder

W_F, W_T, W_R, W_PROXY = 0.95, 0.04, 0.01, 0.2
JITTER = 0.005
BC7_NOISE = 0.004


def _eval_chunk(args):
    p, wi, wo, nq = args
    return en.evaluate(p, wi, wo, nq=nq)


def build_dataset(n_mat: int, n_dir: int, seed: int, nq: int = 24, workers: int = 1):
    """params [M,P], dirs wi/wo [M,D,3], targets f [M,D,3], T [M,D], R [M,D]."""
    p, cls = sp.sample(n_mat, seed)
    wi, wo = sp.sample_dirs(n_mat * n_dir, seed + 1)
    pp = np.repeat(p, n_dir, axis=0)
    jobs = [(pp[i:i + 512], wi[i:i + 512], wo[i:i + 512], nq) for i in range(0, len(pp), 512)]
    if workers > 1:
        from multiprocessing import Pool
        with Pool(workers) as pool:
            res = pool.map(_eval_chunk, jobs)
    else:
        res = [_eval_chunk(j) for j in jobs]
    f = np.concatenate([r[0] for r in res]).reshape(n_mat, n_dir, 3)
    T = np.concatenate([r[1] for r in res]).reshape(n_mat, n_dir)
    R = np.concatenate([r[2] for r in res]).reshape(n_mat, n_dir)
    return dict(params=p, cls=cls, wi=wi.reshape(n_mat, n_dir, 3), wo=wo.reshape(n_mat, n_dir, 3), f=f, T=T, R=R)


def compress(f):
    return torch.log1p(f)


def losses(model, u, wi, wo, f, T, R):
    pf, pT, pR = model(u, wi, wo)
    lf = (compress(pf) - compress(f)).abs().mean()
    lt = (pT - T).abs().mean()
    lr = (pR - R).abs().mean()
    lp = (compress(model.proxy_f(u, wi, wo)) - compress(f)).abs().mean()
    return W_F * lf + W_T * lt + W_R * lr + W_PROXY * lp, lf


def train(ds, name="high", latents=8, steps=2000, batch_mat=64, lr=2e-3, seed=0, log=None):
    torch.manual_seed(seed)
    M, D = ds["f"].shape[:2]
    model = Decoder(name, latents)
    emb = torch.nn.Parameter(torch.rand(M, latents))
    opt = torch.optim.Adam([{"params": model.parameters()}, {"params": [emb], "lr": lr * 5}], lr=lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, steps)
    t = {k: torch.from_numpy(np.ascontiguousarray(ds[k])).float() for k in ("wi", "wo", "f", "T", "R")}
    g = torch.Generator().manual_seed(seed)
    hist = []
    for s in range(steps):
        idx = torch.randint(0, M, (batch_mat,), generator=g)
        u = emb[idx].clamp(0, 1)[:, None, :].expand(-1, D, -1)
        u = u + JITTER * (torch.rand_like(u) * 2 - 1) + BC7_NOISE * torch.randn_like(u)
        loss, lf = losses(model, u, t["wi"][idx], t["wo"][idx], t["f"][idx], t["T"][idx], t["R"][idx])
        opt.zero_grad()
        loss.backward()
        opt.step()
        sched.step()
        if s % 100 == 0 or s == steps - 1:
            hist.append((s, float(loss.detach()), float(lf.detach())))
            if log:
                log(f"step {s} loss {float(loss):.4f} f {float(lf):.4f}")
    return model, emb.detach().clamp(0, 1).numpy(), hist


def save(path, model, emb, hist):
    torch.save(dict(spec=model.spec(), state=model.state_dict(), latents=emb, hist=hist), path)


def load(path):
    c = torch.load(path, weights_only=False)
    m = Decoder(c["spec"]["name"], c["spec"]["latents"])
    m.load_state_dict(c["state"])
    return m.eval(), c["latents"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default="high", choices=["high", "low", "tiny"])
    ap.add_argument("--materials", type=int, default=1024)
    ap.add_argument("--dirs", type=int, default=64)
    ap.add_argument("--nq", type=int, default=24)
    ap.add_argument("--steps", type=int, default=3000)
    ap.add_argument("--batch-mat", type=int, default=64)
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--seed", type=int, default=0)
    a = ap.parse_args(argv)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    ds = build_dataset(a.materials, a.dirs, a.seed, a.nq, a.workers)
    print(f"dataset {time.time() - t0:.0f}s")
    model, emb, hist = train(ds, a.name, steps=a.steps, batch_mat=a.batch_mat, seed=a.seed, log=print)
    save(out / f"decoder_{a.name}.pt", model, emb, hist)
    (out / "train.json").write_text(json.dumps(dict(args=vars(a), hist=hist, seconds=time.time() - t0)))
    print(f"done {time.time() - t0:.0f}s")


if __name__ == "__main__":
    main()
