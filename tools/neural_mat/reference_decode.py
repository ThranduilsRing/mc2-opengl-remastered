"""NumPy reference decoder for a quantized network (the executable spec the N1 kernel must match).

Arithmetic contract: first/last layers (and any layer that failed the cosine gate) use FP16 weights with FP32
accumulate; INT8 layers quantize the FP32 activation with the per-tensor scale sx (round half to even, clamp +-127),
accumulate in INT32, then dequantize in FP32: y = acc * (sx * sw[out]) + b. Activations are never FP16.
"""
from __future__ import annotations
import numpy as np

FRAME_TILT = 0.25


def rot_to_z(n, v):
    nz = n[..., 2:3]
    ne = n.copy()
    ne[..., 2:3] = nz + 1
    dn = (v * n).sum(-1, keepdims=True)
    de = (v * ne).sum(-1, keepdims=True)
    out = v - de * ne / np.maximum(1 + nz, 1e-4)
    out = out + 2 * dn * np.array([0, 0, 1], np.float32)
    return out.astype(np.float32)


def tilt_normal(t):
    n = np.concatenate([t, np.ones_like(t[..., :1])], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)


def direction_inputs(wi, wo, ti, to):
    wi = rot_to_z(tilt_normal(ti), wi)
    wo = rot_to_z(tilt_normal(to), wo)
    h = wi + wo
    h = h / np.maximum(np.linalg.norm(h, axis=-1, keepdims=True), 1e-6)
    wd = rot_to_z(h, wi)
    return np.concatenate([h, wd], -1).astype(np.float32)


def run_layer(L, x):
    if L["kind"] == "int8":
        xq = np.clip(np.rint(x / L["sx"]), -127, 127).astype(np.int32)
        acc = xq @ L["W"].astype(np.int32).T
        return acc.astype(np.float32) * (np.float32(L["sx"]) * L["sw"]).astype(np.float32) + L["b"]
    return x @ L["W"].astype(np.float32).T + L["b"].astype(np.float32)


def trunk_input(q, u, wi, wo):
    u = u.astype(np.float32)
    t = FRAME_TILT * np.tanh(u @ q["frame_w"].astype(np.float32).T + q["frame_b"].astype(np.float32))
    return np.concatenate([u, direction_inputs(wi, wo, t[..., :2], t[..., 2:])], -1).astype(np.float32)


def trunk(q, x, taps=None):
    n = len(q["layers"])
    for i, L in enumerate(q["layers"]):
        if taps is not None:
            taps.append(x)
        x = run_layer(L, x)
        if i < n - 1:
            x = np.maximum(x, 0)
    return x


def decode(q, u, wi, wo):
    """(f [...,3], T [...], R [...]) for latents u [...,L] and unit directions wi, wo [...,3]."""
    x = trunk(q, trunk_input(q, u, wi, wo))
    f = np.exp(np.minimum(x[..., :3], 12))
    sig = lambda z: 1 / (1 + np.exp(-z))
    return f.astype(np.float32), sig(x[..., 3]).astype(np.float32), sig(x[..., 4]).astype(np.float32)


def proxy_params(q, u):
    h = np.maximum(u.astype(np.float32) @ q["proxy_w0"].astype(np.float32).T + q["proxy_b0"].astype(np.float32), 0)
    p = h @ q["proxy_w1"].astype(np.float32).T + q["proxy_b1"].astype(np.float32)
    a = 0.02 + 0.98 / (1 + np.exp(-p[..., :1]))
    return a, np.exp(np.clip(p[..., 1:], -8, 4))


def proxy_f(q, u, wi, wo):
    a, f0 = proxy_params(q, u)
    h = wi + wo
    h = h / np.maximum(np.linalg.norm(h, axis=-1, keepdims=True), 1e-6)
    ci, co, nh = np.maximum(wi[..., 2:3], 1e-3), np.maximum(wo[..., 2:3], 1e-3), h[..., 2:3]
    vh = np.clip((wi * h).sum(-1, keepdims=True), 0, 1)
    a2 = a * a
    d = nh * nh * (a2 - 1) + 1
    D = a2 / (np.pi * d * d + 1e-6)
    g1 = lambda c: 2 * c / (c + np.sqrt(a2 + (1 - a2) * c * c) + 1e-6)
    F = f0 + (1 - f0) * (1 - vh) ** 5
    return (D * g1(ci) * g1(co) / (4 * ci * co + 1e-6) * F).astype(np.float32)
