"""BT2018 environment materials -> one baked texture set per prop LOD (albedo, normal, ORM).

BT2018 structure materials (envMatStct_*) are not baked albedos: a tiling clean layer (_MainTex with its
own _ST tiling) is blended per vertex (vertex colour, _VERTEX_BLEND) with a tiling weathered layer
(_BlendMainTex / _BlendNormalMap / _BlendMetallicGlossMap), tinted by _Color. A single _MainTex per
slot renders as a flat colour slab. This module decodes each material and composites
clean x tint lerp weathered (by vertex colour) into a per-prop atlas laid out from the mesh's own UV0
(each submesh gets its own region, same orientation and texel density, so tangent space is unchanged).

UV conventions: Unity UV0 has v up with images stored bottom-up; UnityPy returns images top-down, so a
Unity (u, v) samples image row (1 - v) * H. Atlas texels keep that orientation (u right, v up = row up),
and the baked glTF UV is the texel position / size (glTF v down).
"""
from __future__ import annotations

import json
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

MAP_SLOTS = ("_MainTex", "_BumpMap", "_MetallicGlossMap", "_OcclusionMap",
             "_BlendMainTex", "_BlendNormalMap", "_BlendMetallicGlossMap")
PAD = 4


class MaterialIndex:
    """Material name -> decoded properties, over resources.assets and every sharedassets file (read only)."""

    def __init__(self, inst, cache: Path | None = None):
        self.inst, self.cache, self._idx, self._dec = inst, cache, None, {}

    def index(self):
        if self._idx is None:
            if self.cache and Path(self.cache).is_file():
                self._idx = json.loads(Path(self.cache).read_text(encoding="utf-8"))
            else:
                self._idx = {}
                for f in ["resources.assets"] + [f"sharedassets{i}.assets" for i in range(100)]:
                    if not (self.inst.root / f).exists():
                        continue
                    for o in self.inst.env(f).objects:
                        if o.type.name == "Material":
                            self._idx.setdefault(o.read().m_Name, [f, o.path_id])
                    self.inst._env.pop(f, None)
                if self.cache:
                    Path(self.cache).write_text(json.dumps(self._idx), encoding="utf-8")
        return self._idx

    def decode(self, name):
        """{"maps": {slot: (texture, (sx, sy), (ox, oy))}, "color": (r, g, b), "floats": {...}} or None."""
        if name not in self._dec:
            hit = self.index().get(name)
            dec = None
            if hit:
                f, pid = hit
                m = next(o for o in self.inst.env(f).objects if o.path_id == pid).read()
                sp = m.m_SavedProperties
                maps = {}
                for k, v in sp.m_TexEnvs:
                    if k in MAP_SLOTS and v.m_Texture.path_id:
                        try:
                            maps[k] = (v.m_Texture.read().m_Name, (float(v.m_Scale.x), float(v.m_Scale.y)),
                                       (float(v.m_Offset.x), float(v.m_Offset.y)))
                        except Exception:
                            pass
                cols = {k: (c.r, c.g, c.b) for k, c in sp.m_Colors}
                dec = {"maps": maps, "color": cols.get("_Color", (1.0, 1.0, 1.0)), "floats": dict(sp.m_Floats)}
            self._dec[name] = dec
        return self._dec[name]


def pack_regions(exts, size):
    """Shelf-pack submesh UV extents (w, h) at one shared scale s (px per UV unit). -> (s, [(x0, y0)])."""
    def fit(s):
        x = y = row = 0
        out = []
        for w, h in exts:
            pw, ph = int(np.ceil(w * s)) + 2 * PAD, int(np.ceil(h * s)) + 2 * PAD
            if pw > size or ph > size:
                return None
            if x + pw > size:
                x, y, row = 0, y + row, 0
            if y + ph > size:
                return None
            out.append((x + PAD, y + PAD))
            x, row = x + pw, max(row, ph)
        return out
    lo, hi = 1.0, 1e6
    for _ in range(40):
        mid = (lo + hi) / 2
        lo, hi = (mid, hi) if fit(mid) is not None else (lo, mid)
    return lo, fit(lo)


class TexCache:
    """Texture name -> float RGBA array, prefiltered (box-down-sampled) to a requested max width."""

    def __init__(self, inst):
        self.inst, self.full, self.small = inst, {}, {}

    def get(self, name, width):
        width = int(max(4, min(width, 4096)))
        if name not in self.full:
            try:
                self.full[name] = self.inst.texture(name)
            except (KeyError, StopIteration) as exc:
                raise ValueError(f"missing texture {name}") from exc
        im = self.full[name]
        width = min(width, im.size[0])
        key = (name, width)
        if key not in self.small:
            h = max(4, round(im.size[1] * width / im.size[0]))
            self.small[key] = np.asarray(im if width == im.size[0] else im.resize((width, h), Image.BOX), np.uint8)
        return self.small[key]


def sample(tc, spec, uv, s):
    """Sample one material map at Unity uv (N, 2) with its tiling; s = atlas px per UV unit (prefilter)."""
    name, (sx, sy), (ox, oy) = spec
    img = tc.get(name, s / max(abs(sx), abs(sy), 1e-3))
    h, w = img.shape[:2]
    t = uv * np.array([sx, sy], np.float32) + np.array([ox, oy], np.float32)
    x = np.floor((t[:, 0] % 1.0) * w).astype(np.int64) % w
    y = np.floor(((1.0 - t[:, 1]) % 1.0) * h).astype(np.int64) % h
    return img[y, x].astype(np.float32) / 255.0


def decode_normal(px):
    """Unity normal map texels (N, 4) in 0..1 -> tangent normals (N, 3); handles DXT5nm (x in A)."""
    dxt5 = px[:, 0].mean() > 0.94
    x = (px[:, 3] if dxt5 else px[:, 0]) * 2 - 1
    y = px[:, 1] * 2 - 1
    return np.stack([x, y, np.sqrt(np.clip(1 - x * x - y * y, 0, 1))], -1)


def shade_texels(tc, mat, uv, vc, s):
    """Composite one material at texels: -> albedo (N,3), normal (N,3), ao (N,), smoothness (N,)."""
    mp, fl = mat["maps"], mat["floats"]
    n = len(uv)
    flat = np.tile(np.array([0.0, 0.0, 1.0], np.float32), (n, 1))

    def layer(alb, nrm, spc):
        a = sample(tc, mp[alb], uv, s)[:, :3] if alb in mp else np.full((n, 3), 0.7, np.float32)
        nm = decode_normal(sample(tc, mp[nrm], uv, s)) if nrm in mp else flat
        sm = (sample(tc, mp[spc], uv, s)[:, 3] * float(fl.get("_GlossMapScale", 1.0)) if spc in mp
              else np.full(n, float(fl.get("_Glossiness", 0.5)), np.float32))
        return a, nm, sm
    a, nm, sm = layer("_MainTex", "_BumpMap", "_MetallicGlossMap")
    if "_BlendMainTex" in mp and float(fl.get("_UseBlend", 1.0)) > 0:
        ba, bn, bs = layer("_BlendMainTex", "_BlendNormalMap", "_BlendMetallicGlossMap")
        w = (1.0 - vc)[:, None]                # vertex colour 1 = clean layer, 0 = weathered layer
        a, sm = a * (1 - w) + ba * w, sm * (1 - w[:, 0]) + bs * w[:, 0]
        nm = nm * (1 - w) + bn * w
    a = a * np.asarray(mat["color"], np.float32)[None, :3]
    bump = float(fl.get("_BumpScale", 1.0))
    nm = nm * np.array([bump, bump, 1.0], np.float32)
    nm /= np.linalg.norm(nm, axis=1, keepdims=True) + 1e-6
    ao = (sample(tc, mp["_OcclusionMap"], uv, s)[:, 1] if "_OcclusionMap" in mp else np.ones(n, np.float32))
    return a, nm, ao, sm


def bake(uv, colors, subs, mats, tc, size=2048):
    """uv: Unity UV0 (V, 2); colors: (V,) 0..1 or None; subs: [tri index arrays (T, 3)]; mats: decoded material
    per submesh (None = neutral). -> (albedo RGBA u8, normal RGB u8, orm RGBA u8, [glTF uv (V, 2) per submesh])."""
    vc = np.ones(len(uv), np.float32) if colors is None else np.asarray(colors, np.float32)
    boxes = []
    for t in subs:
        used = uv[np.unique(t)] if len(t) else np.zeros((1, 2), np.float32)
        lo, hi = used.min(0), used.max(0)
        boxes.append((lo, hi))
    s, origins = pack_regions([tuple(np.maximum(hi - lo, 1e-4)) for lo, hi in boxes], size)
    alb = np.zeros((size, size, 3), np.float32)
    nrm = np.tile(np.array([0.5, 0.5, 1.0], np.float32), (size, size, 1))
    orm = np.zeros((size, size, 3), np.float32)
    covered = np.zeros((size, size), bool)
    new_uvs = []
    for (lo, hi), (x0, y0), t, mat in zip(boxes, origins, subs, mats):
        px = np.stack([x0 + (uv[:, 0] - lo[0]) * s, y0 + (hi[1] - uv[:, 1]) * s], -1)
        new_uvs.append((px / size).astype(np.float32))
        if not len(t):
            continue
        ids = Image.new("I", (size, size), 0)
        d = ImageDraw.Draw(ids)
        for k, tri in enumerate(t):
            d.polygon([tuple(px[i]) for i in tri], fill=k + 1)
        ids = np.asarray(ids, np.int64)
        yy, xx = np.nonzero(ids)
        if not len(yy):
            continue
        tri = t[ids[yy, xx] - 1]
        p = np.stack([xx + 0.5, yy + 0.5], -1)
        a, b, c = px[tri[:, 0]], px[tri[:, 1]], px[tri[:, 2]]
        den = (b[:, 1] - c[:, 1]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 1] - c[:, 1])
        den = np.where(np.abs(den) < 1e-9, 1e-9, den)
        w0 = ((b[:, 1] - c[:, 1]) * (p[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (p[:, 1] - c[:, 1])) / den
        w1 = ((c[:, 1] - a[:, 1]) * (p[:, 0] - c[:, 0]) + (a[:, 0] - c[:, 0]) * (p[:, 1] - c[:, 1])) / den
        w = np.clip(np.stack([w0, w1, 1 - w0 - w1], -1), 0, 1)
        w /= w.sum(1, keepdims=True) + 1e-9
        tuv = np.stack([lo[0] + (p[:, 0] - x0) / s, hi[1] - (p[:, 1] - y0) / s], -1).astype(np.float32)
        tvc = (vc[tri] * w).sum(1)
        if mat is None:
            ca, cn, cao, csm = np.full((len(p), 3), 0.6, np.float32), np.tile([0.0, 0.0, 1.0], (len(p), 1)), np.ones(len(p)), np.full(len(p), 0.3)
        else:
            ca, cn, cao, csm = shade_texels(tc, mat, tuv, tvc, s)
        alb[yy, xx] = ca
        nrm[yy, xx] = cn * 0.5 + 0.5
        orm[yy, xx] = np.stack([cao, 1.0 - np.clip(csm, 0, 1), np.zeros(len(p))], -1)
        covered[yy, xx] = True
    alb, nrm, orm = (dilate(x, covered) for x in (alb, nrm, orm))
    u8 = lambda x: (np.clip(x, 0, 1) * 255 + 0.5).astype(np.uint8)
    return (np.dstack([u8(alb), covered.astype(np.uint8) * 255]), u8(nrm),
            np.dstack([u8(orm), np.full((size, size), 255, np.uint8)]), new_uvs)


def dilate(img, mask, steps=PAD):
    """Grow covered texels outward so bilinear/mip sampling at island edges never reads background."""
    img, mask = img.copy(), mask.copy()
    for _ in range(steps):
        grow = np.zeros_like(img)
        cnt = np.zeros(mask.shape, np.float32)
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            m = np.roll(mask, (dy, dx), (0, 1))
            grow += np.roll(img, (dy, dx), (0, 1)) * m[..., None]
            cnt += m
        new = (~mask) & (cnt > 0)
        img[new] = grow[new] / cnt[new][:, None]
        mask |= new
    return img
