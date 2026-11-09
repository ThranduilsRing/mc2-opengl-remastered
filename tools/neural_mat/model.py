"""Universal hard-surface decoder (study 1.1 / 1.8 hybrid; direction parameterisation from the s26 course's
Rusinkiewicz half/difference inputs).

  latent u (L channels in [0,1], BC7 texel values)
    -> frame head: two shading-frame tilts (linear, FP16 weights) rotate wi and wo
    -> inputs: u (L) + half vector wh (3) + difference direction wd (3)
    -> MLP (High 4 x 64, Low 2 x 32), ReLU, linear last layer
    -> BRDF f = exp(x0..x2), transmission albedo T = sigmoid(x3), luminance reflectance R = sigmoid(x4)
  proxy head (z only, no directions): GGX parameters (alpha, F0 rgb) = the analytic stand-in for local night lights.
"""
from __future__ import annotations
import torch
from torch import nn

CONFIGS = {"high": dict(hidden=4, width=64), "low": dict(hidden=2, width=32), "tiny": dict(hidden=2, width=16)}
DIR_IN = 6
N_OUT = 5
FRAME_TILT = 0.25


def rot_to_z(n, v):
    """Minimal rotation taking unit n to +z, applied to v: v - (v.(n+e))(n+e)/(1+nz) + 2 (v.n) e (two reflections)."""
    nz = n[..., 2:3]
    ne = n.clone()
    ne[..., 2:3] = nz + 1
    dn = (v * n).sum(-1, keepdim=True)
    de = (v * ne).sum(-1, keepdim=True)
    out = v - de * ne / (1 + nz).clamp_min(1e-4)
    out = out + 2 * dn * torch.tensor([0.0, 0.0, 1.0], dtype=v.dtype)
    return out


def tilt_normal(t):
    n = torch.cat([t, torch.ones_like(t[..., :1])], -1)
    return n / n.norm(dim=-1, keepdim=True)


def direction_inputs(wi, wo, tilt_i, tilt_o):
    """6 direction inputs from (wi, wo) rotated into the two learned frames: half vector wh and difference wd."""
    wi = rot_to_z(tilt_normal(tilt_i), wi)
    wo = rot_to_z(tilt_normal(tilt_o), wo)
    h = wi + wo
    h = h / h.norm(dim=-1, keepdim=True).clamp_min(1e-6)
    wd = rot_to_z(h, wi)
    return torch.cat([h, wd], -1)


def ggx_proxy(alpha, f0, wi, wo):
    """GGX lobe (D F G / 4 ci co, Schlick F); alpha [...,1], f0 [...,3]; torch twin of enhance._ggx_lobe."""
    h = wi + wo
    h = h / h.norm(dim=-1, keepdim=True).clamp_min(1e-6)
    ci, co, nh = wi[..., 2:3].clamp_min(1e-3), wo[..., 2:3].clamp_min(1e-3), h[..., 2:3]
    vh = (wi * h).sum(-1, keepdim=True).clamp(0, 1)
    a2 = alpha * alpha
    d = nh * nh * (a2 - 1) + 1
    D = a2 / (3.14159265 * d * d + 1e-6)
    g1 = lambda c: 2 * c / (c + torch.sqrt(a2 + (1 - a2) * c * c) + 1e-6)
    F = f0 + (1 - f0) * (1 - vh) ** 5
    return D * g1(ci) * g1(co) / (4 * ci * co + 1e-6) * F


class Decoder(nn.Module):
    def __init__(self, name="high", latents=8):
        super().__init__()
        c = CONFIGS[name]
        self.name, self.L, self.hidden, self.width = name, latents, c["hidden"], c["width"]
        dims = [latents + DIR_IN] + [self.width] * self.hidden + [N_OUT]
        self.layers = nn.ModuleList(nn.Linear(a, b) for a, b in zip(dims[:-1], dims[1:]))
        self.frame = nn.Linear(latents, 4)
        nn.init.zeros_(self.frame.weight)
        nn.init.zeros_(self.frame.bias)
        self.proxy = nn.Sequential(nn.Linear(latents, 32), nn.ReLU(), nn.Linear(32, 4))

    def frames(self, u):
        t = FRAME_TILT * torch.tanh(self.frame(u))
        return t[..., :2], t[..., 2:]

    def trunk_input(self, u, wi, wo):
        ti, to = self.frames(u)
        return torch.cat([u, direction_inputs(wi, wo, ti, to)], -1)

    def raw(self, u, wi, wo):
        a = self.trunk_input(u, wi, wo)
        for i, lin in enumerate(self.layers):
            a = lin(a)
            if i < len(self.layers) - 1:
                a = torch.relu(a)
        return a

    def forward(self, u, wi, wo):
        x = self.raw(u, wi, wo)
        return torch.exp(x[..., :3].clamp(max=12)), torch.sigmoid(x[..., 3]), torch.sigmoid(x[..., 4])

    def proxy_params(self, u):
        p = self.proxy(u)
        return 0.02 + 0.98 * torch.sigmoid(p[..., :1]), torch.exp(p[..., 1:].clamp(-8, 4))

    def proxy_f(self, u, wi, wo):
        a, f0 = self.proxy_params(u)
        return ggx_proxy(a, f0, wi, wo)

    def spec(self):
        return dict(name=self.name, latents=self.L, hidden=self.hidden, width=self.width)
