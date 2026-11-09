"""Fit per-texel latents against a frozen decoder (the cheap per-material step: no decoder retraining).

Init = best of K random training latents per texel (by loss), then Adam refinement with latents clamped to [0,1]
(BC7 texel range). Targets may be box-filtered over a texel footprint, which is how mip levels are fitted
(prefiltered-mip fitting, s26 design): fit(model, ..., targets of the averaged footprint).
"""
from __future__ import annotations
import numpy as np
import torch

from .train import compress, W_F, W_T, W_R


def _loss(model, u, wi, wo, f, T, R):
    pf, pT, pR = model(u, wi, wo)
    per = (W_F * (compress(pf) - compress(f)).abs().mean(dim=(-1, -2))
           + W_T * (pT - T).abs().mean(-1) + W_R * (pR - R).abs().mean(-1))
    return per                                   # [N] per texel


def fit(model, wi, wo, f, T, R, pool=None, steps=200, lr=0.04, k=48, seed=0):
    """wi/wo [N,D,3], f [N,D,3], T/R [N,D] (numpy). Returns (latents [N,L] float32, final per-texel loss [N])."""
    model.eval()
    for q in model.parameters():
        q.requires_grad_(False)
    g = torch.Generator().manual_seed(seed)
    t = [torch.from_numpy(np.ascontiguousarray(a)).float() for a in (wi, wo, f, T, R)]
    N = t[0].shape[0]
    L = model.L
    cand = torch.rand(k, L, generator=g) if pool is None else torch.from_numpy(np.asarray(pool)).float()[
        torch.randint(0, len(pool), (k,), generator=g)]
    best = torch.zeros(N, L)
    best_l = torch.full((N,), float("inf"))
    with torch.no_grad():
        for c in cand:
            u = c[None, None, :].expand(N, t[0].shape[1], L)
            l = _loss(model, u, *t)
            m = l < best_l
            best[m], best_l[m] = c, l[m]
    z = torch.nn.Parameter(best.clone())
    opt = torch.optim.Adam([z], lr=lr)
    for _ in range(steps):
        u = z.clamp(0, 1)[:, None, :].expand(N, t[0].shape[1], L)
        l = _loss(model, u, *t).sum()
        opt.zero_grad()
        l.backward()
        opt.step()
    with torch.no_grad():
        u = z.clamp(0, 1)
        final = _loss(model, u[:, None, :].expand(N, t[0].shape[1], L), *t)
    return u.numpy().astype(np.float32), final.numpy()
