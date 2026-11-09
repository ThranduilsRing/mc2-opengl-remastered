"""Analytic enhanced-BRDF evaluation (study 1.1): 8 non-diffuse lobes in a tangent frame (z up).

Parameter layout (P = 20; the paper has 22, its exact lobe forms are unpublished):
  0 r  1 p  2 q  3 s        roughness, core exponent (alpha_core = r^p), haze exponent and scale (alpha_haze = s*r^q)
  4:7 conductor F0 rgb      7 dielectric F0
  8:16 lobe weights         cond core, cond haze, diel core, diel haze, sheen(dust), clearcoat, inner scatter, subcutaneous
  16 sheen roughness  17 sheen tint  18 coat roughness  19 inner-scatter exponent
Outputs: f (BRDF, RGB, no cosine), T (transmission albedo to the diffuse base = 1 - E(wi)), R (luminance reflectance E(wo)).
E(w) is the directional albedo of the non-diffuse lobes, integrated with a GGX half-vector + cosine mixture
(Hammersley, deterministic), so the white-furnace test is exact up to quadrature noise.
"""
from __future__ import annotations
import numpy as np

P = 20
LOBES = ("cond_core", "cond_haze", "diel_core", "diel_haze", "sheen", "coat", "inner", "subcut")
EPS = 1e-6


def _dot(a, b):
    return np.sum(a * b, axis=-1)


def _norm(v):
    return v / np.maximum(np.linalg.norm(v, axis=-1, keepdims=True), EPS)


def alphas(p):
    r = p[..., 0]
    a_core = np.clip(r ** p[..., 1], 0.02, 1.0)
    a_haze = np.clip(p[..., 3] * r ** p[..., 2], a_core, 1.0)
    return a_core, a_haze


def _ggx_D(nh, a):
    d = nh * nh * (a * a - 1) + 1
    return a * a / (np.pi * d * d + EPS)


def _g1(c, a):
    a2 = a * a
    return 2 * c / (c + np.sqrt(a2 + (1 - a2) * c * c) + EPS)


def _schlick(f0, vh):
    return f0 + (1 - f0) * ((1 - np.clip(vh, 0, 1)) ** 5)[..., None]


def _up(wi, wo):
    return (wi[..., 2] > 0) & (wo[..., 2] > 0)


def _ggx_lobe(wi, wo, a, f0):
    """GGX D*F*G/(4 ci co), separable Smith G; f [..., 3] without cosine."""
    h = _norm(wi + wo)
    ci, co = wi[..., 2], wo[..., 2]
    g = _g1(ci, a) * _g1(co, a)
    f = (_ggx_D(h[..., 2], a) * g / (4 * ci * co + EPS))[..., None] * _schlick(f0, _dot(wi, h))
    return np.where(_up(wi, wo)[..., None], f, 0.0)


def _sheen(wi, wo, a, tint):
    h = _norm(wi + wo)
    ci, co, nh = wi[..., 2], wo[..., 2], np.clip(h[..., 2], 0, 1)
    inv = 1.0 / np.maximum(a, 0.05)
    d = (2 + inv) * np.maximum(1 - nh * nh, 0) ** (inv / 2) / (2 * np.pi)
    v = 1.0 / (4 * (ci + co - ci * co) + EPS)
    f = (d * v * tint)[..., None] * np.ones(3)
    return np.where(_up(wi, wo)[..., None], f, 0.0)


def _inner(wi, wo, k):
    m = np.stack([-wi[..., 0], -wi[..., 1], wi[..., 2]], -1)
    c = np.clip(_dot(m, wo), 0, 1)
    f = (k + 2) / (2 * np.pi) * c ** k
    return np.where(_up(wi, wo), f, 0.0)[..., None] * np.ones(3)


def eval_lobes(p, wi, wo):
    """Per-lobe contributions [..., 8, 3], weights and the clearcoat attenuation applied."""
    a_c, a_h = alphas(p)
    w = p[..., 8:16]
    ones = np.ones(p.shape[:-1] + (3,))
    f0c, f0d, f0coat = p[..., 4:7], p[..., 7:8] * ones, 0.04 * ones
    atten = 1 - w[..., 5] * _schlick(f0coat, wi[..., 2])[..., 0]
    out = [
        w[..., 0, None] * _ggx_lobe(wi, wo, a_c, f0c),
        w[..., 1, None] * _ggx_lobe(wi, wo, a_h, f0c),
        w[..., 2, None] * _ggx_lobe(wi, wo, a_c, f0d),
        w[..., 3, None] * _ggx_lobe(wi, wo, a_h, f0d),
        w[..., 4, None] * _sheen(wi, wo, p[..., 16], p[..., 17]) * 0.25,
        w[..., 5, None] * _ggx_lobe(wi, wo, np.clip(p[..., 18], 0.02, 1), f0coat),
        w[..., 6, None] * _inner(wi, wo, p[..., 19]) * 0.5,
        w[..., 7, None] * (0.5 / np.pi) * _up(wi, wo)[..., None] * ones,
    ]
    out = np.stack(out, axis=-2)
    keep = np.ones(8)
    keep[5] = 0.0                       # the coat lobe itself is not attenuated by the coat
    base = 1 - (1 - atten[..., None, None]) * keep[:, None]
    return out * base


def eval_f(p, wi, wo):
    return eval_lobes(p, wi, wo).sum(axis=-2)


def _hammersley(n):
    i = np.arange(n)
    b = np.zeros(n)
    f, x = 0.5, i.copy()
    while x.any():
        b += f * (x & 1)
        x >>= 1
        f *= 0.5
    return (i + 0.5) / n, b


def albedo(p, w, n=128):
    """E(w) = integral of luminance f(w, wo) cos(wo) dwo over the hemisphere.
    Three strategies (GGX half-vector at alpha_core, at alpha_haze, cosine) combined by the balance heuristic."""
    shape = p.shape[:-1]
    u1, u2 = _hammersley(n)
    a_c, a_h = alphas(p)
    wb = np.broadcast_to(w[..., None, :], shape + (n, 3))
    pb = np.broadcast_to(p[..., None, :], shape + (n, P))
    tot = np.zeros(shape)
    for a in (a_c, a_h, None):
        phi = 2 * np.pi * u2
        if a is None:
            ct, st = np.sqrt(1 - u1), np.sqrt(u1)
            wo = np.broadcast_to(np.stack([st * np.cos(phi), st * np.sin(phi), ct], -1), shape + (n, 3))
        else:
            aa = a[..., None]
            ct = np.sqrt((1 - u1) / (1 + (aa * aa - 1) * u1))
            st = np.sqrt(np.maximum(1 - ct * ct, 0))
            h = np.stack([st * np.cos(phi), st * np.sin(phi), ct], -1)
            wo = 2 * _dot(wb, h)[..., None] * h - wb
        f = lum(eval_f(pb, wb, wo))
        co = np.clip(wo[..., 2], 0, None)
        h = _norm(wb + wo)
        vh = np.maximum(_dot(wb, h), 1e-4)
        mix = (_ggx_D(h[..., 2], a_c[..., None]) * h[..., 2] / (4 * vh)
               + _ggx_D(h[..., 2], a_h[..., None]) * h[..., 2] / (4 * vh) + co / np.pi) / 3
        tot += np.mean(np.where(mix > 0, f * co / np.maximum(mix, EPS), 0.0), -1) / 3
    return tot


def lum(f):
    return f @ np.array([0.2126, 0.7152, 0.0722])


def evaluate(p, wi, wo, nq=96):
    """f [...,3], T [...], R [...] for parameter rows p (float64) and direction pairs (wi, wo)."""
    p, wi, wo = (np.asarray(x, np.float64) for x in (p, wi, wo))
    f = eval_f(p, wi, wo)
    return f, np.clip(1 - albedo(p, wi, nq), 0, 1), np.clip(albedo(p, wo, nq), 0, 1.5)


def furnace(p, wi, nq=256):
    """White-furnace energy E(wi); <= 1 + eps for a physically valid material."""
    return albedo(np.asarray(p, np.float64), np.asarray(wi, np.float64), nq)
