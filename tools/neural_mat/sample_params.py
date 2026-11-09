"""Procedural enhanced-BRDF parameter sampler (study 1.1 full paper): four themed classes drawn from a physically
informed distribution plus a 20 % uniform infill class. Seeded; numpy Generator only."""
from __future__ import annotations
import numpy as np
from .enhance import P

CLASSES = ("haze", "dust", "clearcoat", "scatter", "infill")
INFILL_FRACTION = 0.20
# Dirichlet priors over the 8 lobe weights per themed class
_PRIOR = {
    0: [2, 4, 2, 4, .2, .2, .2, .2],     # haze: heavy haze pairs
    1: [2, 1, 2, 1, 4, .2, .2, .2],      # dust: sheen
    2: [2, .5, 2, .5, .2, 4, .2, .2],    # clearcoat
    3: [1, .5, 1, .5, .2, .2, 4, 4],     # inner / subcutaneous scattering
}


def sample(n: int, seed: int):
    """(params [n, P] float32, class index [n]); 20 % infill, 20 % per themed class."""
    rng = np.random.default_rng(seed)
    cls = rng.choice(5, size=n, p=[0.2] * 5)
    p = np.zeros((n, P))
    p[:, 0] = np.exp(rng.uniform(np.log(0.08), 0.0, n))
    p[:, 1] = rng.uniform(2.05, 3.5, n)
    p[:, 2] = rng.uniform(0.5, 1.5, n)
    p[:, 3] = rng.uniform(1.0, 4.0, n)
    p[:, 4:7] = rng.uniform(0.6, 1.0, (n, 3)) * rng.uniform(0.85, 1.0, (n, 1))
    p[:, 7] = rng.uniform(0.02, 0.08, n)
    w = np.zeros((n, 8))
    for c, prior in _PRIOR.items():
        m = cls == c
        g = rng.gamma(np.array(prior), size=(int(m.sum()), 8))
        w[m] = g / g.sum(-1, keepdims=True)
    p[:, 8:16] = w * rng.uniform(0.3, 1.0, (n, 1))
    p[:, 16] = rng.uniform(0.2, 0.9, n)
    p[:, 17] = rng.uniform(0.2, 1.0, n)
    p[:, 18] = rng.uniform(0.05, 0.5, n)
    p[:, 19] = rng.uniform(2.0, 30.0, n)
    p[cls == 1, 16] = rng.uniform(0.5, 0.9, int((cls == 1).sum()))
    p[cls == 2, 18] = rng.uniform(0.05, 0.25, int((cls == 2).sum()))
    # infill: every parameter uniform over its full range (weights uniform, then clamped to a unit sum)
    m = cls == 4
    k = int(m.sum())
    p[m, 0] = rng.uniform(0.08, 1.0, k)
    p[m, 1] = rng.uniform(2.0, 4.0, k)
    p[m, 2] = rng.uniform(0.3, 2.0, k)
    p[m, 3] = rng.uniform(1.0, 6.0, k)
    p[m, 4:7] = rng.uniform(0, 1, (k, 3))
    p[m, 7] = rng.uniform(0.0, 0.1, k)
    wu = rng.uniform(0, 1, (k, 8))
    p[m, 8:16] = wu / np.maximum(wu.sum(-1, keepdims=True), 1.0)
    return p.astype(np.float32), cls.astype(np.int8)


def sample_dirs(n: int, seed: int):
    """Random (wi, wo) pairs on the upper hemisphere (cos^2 theta uniform in [0.0025, 1])."""
    rng = np.random.default_rng(seed)

    def hemi():
        z = np.sqrt(rng.uniform(0.0025, 1.0, n))
        phi = rng.uniform(0, 2 * np.pi, n)
        s = np.sqrt(1 - z * z)
        return np.stack([s * np.cos(phi), s * np.sin(phi), z], -1)
    return hemi().astype(np.float32), hemi().astype(np.float32)
