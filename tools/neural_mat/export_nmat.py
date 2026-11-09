"""Export: .nmat weights container, latent KTX2 textures (texcook role neural_latent) and the white-furnace energy gate.

.nmat layout (little-endian): b"NMAT", u32 version, u32 header_len, header JSON (utf-8, sorted keys, space padded to a
multiple of 16), then a raw tensor blob. Tensors are listed in the header (name, dtype, shape, offset, nbytes). Linear
layers are row-major W[out][in] then bias[out] (the slang.neural FFLayer LinearLayout the kernel reads).
"""
from __future__ import annotations
import json
import struct
import hashlib
from pathlib import Path
import numpy as np

from . import reference_decode as rd

MAGIC = b"NMAT"
VERSION = 1
FURNACE_EPS = 0.05
ARITHMETIC = ("fp16 first/last layers (fp32 accumulate); int8 layers: x -> clamp(rint(x/sx),+-127), int32 accumulate, "
              "y = acc*(sx*sw[out]) + b in fp32; activations never fp16; outputs f=exp(x0..2), T,R=sigmoid(x3,x4)")


class EnergyGainError(ValueError):
    pass


# ------------------------------------------------------------------ .nmat

def _flatten(q):
    t = {k: q[k] for k in ("frame_w", "frame_b", "proxy_w0", "proxy_b0", "proxy_w1", "proxy_b1")}
    layers = []
    for i, L in enumerate(q["layers"]):
        t[f"l{i}.W"], t[f"l{i}.b"] = L["W"], L["b"]
        if L["kind"] == "int8":
            t[f"l{i}.sw"], t[f"l{i}.sx"] = L["sw"], np.asarray([L["sx"]], np.float32)
        layers.append(dict(kind=L["kind"], out=int(L["W"].shape[0]), inp=int(L["W"].shape[1])))
    return t, layers


def write_nmat(path, q, meta=None) -> str:
    tensors, layers = _flatten(q)
    entries, blob = [], bytearray()
    for name in sorted(tensors):
        a = np.ascontiguousarray(tensors[name])
        blob += b"\0" * (-len(blob) % 16)
        entries.append(dict(name=name, dtype=str(a.dtype), shape=list(a.shape), offset=len(blob), nbytes=a.nbytes))
        blob += a.tobytes()
    head = dict(format_version=VERSION, spec=q["spec"], layers=layers, tensors=entries, arithmetic=ARITHMETIC,
                report=q.get("report", []), meta=meta or {})
    hb = json.dumps(head, sort_keys=True, separators=(",", ":")).encode()
    hb += b" " * (-len(hb) % 16)
    data = MAGIC + struct.pack("<II", VERSION, len(hb)) + hb + bytes(blob)
    Path(path).write_bytes(data)
    return hashlib.sha256(data).hexdigest()


def read_nmat(path):
    data = Path(path).read_bytes()
    if data[:4] != MAGIC:
        raise ValueError("not an nmat file")
    ver, hl = struct.unpack_from("<II", data, 4)
    if ver != VERSION:
        raise ValueError(f"unsupported nmat version {ver}")
    head = json.loads(data[12:12 + hl])
    base = 12 + hl
    t = {}
    for e in head["tensors"]:
        a = np.frombuffer(data, np.dtype(e["dtype"]), count=int(np.prod(e["shape"])) if e["shape"] else 1,
                          offset=base + e["offset"]).reshape(e["shape"])
        t[e["name"]] = a.copy()
    q = {k: t[k] for k in ("frame_w", "frame_b", "proxy_w0", "proxy_b0", "proxy_w1", "proxy_b1")}
    q["spec"], q["report"], q["layers"] = head["spec"], head["report"], []
    for i, l in enumerate(head["layers"]):
        L = dict(kind=l["kind"], W=t[f"l{i}.W"], b=t[f"l{i}.b"])
        if l["kind"] == "int8":
            L["sw"], L["sx"] = t[f"l{i}.sw"], np.float32(t[f"l{i}.sx"][0])
        q["layers"].append(L)
    return q, head


# ------------------------------------------------------------------ energy gate

def furnace(q, u, n_wi=8, n_wo=96):
    """White-furnace per texel: E(wi) = pi * mean lum(f) over cosine-weighted wo, plus T(wi) for a white base.
    Returns max over wi directions of E_spec + T for each latent row of u [N,L]."""
    from .enhance import _hammersley, lum
    u1, u2 = _hammersley(n_wo)
    ct, st = np.sqrt(1 - u1), np.sqrt(u1)
    wo = np.stack([st * np.cos(2 * np.pi * u2), st * np.sin(2 * np.pi * u2), ct], -1).astype(np.float32)
    worst = np.zeros(len(u))
    for k in range(n_wi):
        c = 0.05 + 0.95 * k / (n_wi - 1)
        s = np.sqrt(1 - c * c)
        wi = np.array([s, 0, c], np.float32)
        UU = np.repeat(u[:, None, :], n_wo, 1)
        f, T, _ = rd.decode(q, UU, np.broadcast_to(wi, UU.shape[:-1] + (3,)), np.broadcast_to(wo, UU.shape[:-1] + (3,)))
        E = np.pi * lum(f).mean(1) + T[:, 0]
        worst = np.maximum(worst, E)
    return worst


def check_energy(q, u, eps=FURNACE_EPS):
    w = furnace(q, u)
    if w.max() > 1 + eps:
        raise EnergyGainError(f"white furnace gain {w.max():.3f} > 1+{eps}")
    return float(w.max())


# ------------------------------------------------------------------ latent textures

def latent_scale_bias(u):
    lo, hi = u.reshape(-1, u.shape[-1]).min(0), u.reshape(-1, u.shape[-1]).max(0)
    return (np.maximum(hi - lo, 1e-6)).astype(np.float32), lo.astype(np.float32)


def latent_to_rgba(u, scale, bias):
    """u [H,W,8] -> two RGBA8 images (channels 0..3, 4..7), normalised per channel to the full 8-bit range."""
    n = np.clip(np.rint((u - bias) / scale * 255), 0, 255).astype(np.uint8)
    return n[..., :4].copy(), n[..., 4:8].copy()


def rgba_to_latent(a, b, scale, bias):
    n = np.concatenate([a, b], -1).astype(np.float32) / 255.0
    return n * scale + bias


def _dds_bc7(blocks: bytes, w: int, h: int) -> bytes:
    hdr = struct.pack("<4sIIIIIII11I", b"DDS ", 124, 0x1007, h, w, 0, 0, 1, *([0] * 11))
    pf = struct.pack("<II4sIIIII", 32, 4, b"DX10", 0, 0, 0, 0, 0)
    caps = struct.pack("<IIIII", 0x1000, 0, 0, 0, 0)
    dx10 = struct.pack("<IIIII", 98, 3, 0, 1, 0)
    return hdr + pf + caps + dx10 + blocks


def bc7_roundtrip(rgba8: np.ndarray) -> np.ndarray:
    """Encode one level with the pipeline's texconv BC7 encoder (-nogpu) and decode it again (Pillow)."""
    import io
    from PIL import Image
    from tools.content_pipeline import texcook
    import tempfile
    h, w = rgba8.shape[:2]
    with tempfile.TemporaryDirectory(prefix="nmat-bc7-") as d:
        blocks = texcook._texconv_level(rgba8, "BC7_UNORM", Path(d))
    pw, ph = max(4, w), max(4, h)
    im = Image.open(io.BytesIO(_dds_bc7(blocks, pw if pw == w else w, ph if ph == h else h))).convert("RGBA")
    return np.asarray(im, np.uint8)[:h, :w].copy()


def write_latent_textures(out_dir, stem, u, compress=True):
    """Cook the two latent textures through texcook (role neural_latent). Returns the manifest dict."""
    from tools.content_pipeline import texcook
    out = Path(out_dir)
    out.mkdir(parents=True, exist_ok=True)
    scale, bias = latent_scale_bias(u)
    a, b = latent_to_rgba(u, scale, bias)
    files = []
    for i, img in enumerate((a, b)):
        data = texcook.cook_ktx2(img, "neural_latent", compress=compress, cache_dir=None)
        p = out / f"{stem}_latent{i}.ktx2"
        p.write_bytes(data)
        texcook.record_cook(p, hashlib.sha256(img.tobytes()).hexdigest(),
                            {"role": "neural_latent", "channels": f"{4 * i}..{4 * i + 3}"}, compress=compress)
        files.append(p.name)
    return dict(textures=files, scale=[float(x) for x in scale], bias=[float(x) for x in bias], channels=int(u.shape[-1]))
