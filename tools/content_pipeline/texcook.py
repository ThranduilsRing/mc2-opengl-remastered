"""The texture cook contract: one table of roles -> colour space, block format,
mip filter and tiers, one KTX2 writer, one cache.

Role contract (CONTRACT_VERSION bumps whenever an output byte could change):
  base_color  sRGB   BC7 (145)       sRGB-encoded bytes tagged UNORM: the mech and
                                     legacy shaders read albedo as raw sRGB bytes at
                                     gamma 1.0, so a sampler-side sRGB decode darkens
                                     it. Linear-light, alpha-premultiplied box mips;
                                     MASK materials keep level-0 alpha-test coverage
                                     per mip. Runtime tiers data/tgl/{32..1024} are
                                     REAL downscales (never enlarged, never copies of
                                     the full-res image) as TGA + KTX2 sidecar.
  normal      linear BC5 (141)       tangent space, OpenGL +Y; mips average the
                                     decoded vectors and renormalise; Z is rebuilt
                                     at runtime from RG.
  orm         linear BC7 (145)       R=AO, G=roughness, B=metallic, A=authored
                                     coverage (0 = keep the legacy response).
  emissive    sRGB   BC7_SRGB (146)  as base_color, opaque.
  neural_latent linear BC7 (145)     neural-material latent texels (8 channels as two RGBA
                                     textures); raw values, plain box mips, no coverage rule.
                                     Written with -nogpu (deterministic) and cook.json provenance.
Stock pixels are never re-encoded: a passthrough TextureRef is copied byte-for-byte.
Block encoding uses the installed DirectXTex texconv (CPU codec, -nogpu, so the
bytes are deterministic); the KTX2 container is written here. No downloads.
"""
from __future__ import annotations

import io
import logging
import os
import struct
import subprocess
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path

import numpy as np
from PIL import Image, __version__ as PILLOW_VERSION

from . import cache, provenance

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import console_child  # noqa: E402  (windowless console children; scripts/console_child.py)

CONTRACT_VERSION = "mc2-texcook/2"   # /2: base_color tagged UNORM (145/37), not sRGB
TEXCONV = Path(os.environ.get("MC2_TEXCONV", "A:/Games/mc2-tools/texconv/texconv.exe"))
TIERS = (32, 64, 128, 256, 512, 1024)

VK_RGBA8_UNORM, VK_RGBA8_SRGB = 37, 43
VK_BC5_UNORM, VK_BC7_UNORM, VK_BC7_SRGB = 141, 145, 146


@dataclass(frozen=True)
class Role:
    srgb: bool
    vk_format: int
    texconv_format: str
    mip: str            # "color" | "normal" | "linear"


ROLES = {
    "base_color": Role(True, VK_BC7_UNORM, "BC7_UNORM", "color"),
    "emissive": Role(True, VK_BC7_SRGB, "BC7_UNORM", "color"),
    "normal": Role(False, VK_BC5_UNORM, "BC5_UNORM", "normal"),
    "orm": Role(False, VK_BC7_UNORM, "BC7_UNORM", "linear"),
    "neural_latent": Role(False, VK_BC7_UNORM, "BC7_UNORM", "linear"),
}
KTX_KIND = cache.CacheKind("tex-", ".ktx2")


# --------------------------------------------------------------------------- pixels

def load_rgba(path_or_bytes) -> np.ndarray:
    data = path_or_bytes if isinstance(path_or_bytes, (bytes, bytearray)) else Path(path_or_bytes).read_bytes()
    if data[:4] == b"\xabKTX":
        return read_ktx2_rgba8(data)
    with Image.open(io.BytesIO(data)) as im:
        return np.array(im.convert("RGBA"), dtype=np.uint8)


def srgb_to_linear(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def linear_to_srgb(c):
    c = np.maximum(c, 0)
    return np.where(c <= 0.0031308, c * 12.92, 1.055 * c ** (1 / 2.4) - 0.055)


def _box(a):
    h, w = a.shape[:2]
    if h > 1:
        a = (a[0::2] + a[1::2]) / 2 if h % 2 == 0 else a
    if w > 1:
        a = (a[:, 0::2] + a[:, 1::2]) / 2 if w % 2 == 0 else a
    return a


def _coverage(alpha, cutoff):
    return float(np.mean(alpha >= cutoff))


def _keep_coverage(alpha, target, cutoff):
    """Scale alpha so its alpha-test coverage matches the base level (binary search)."""
    if target <= 0 or target >= 1:
        return alpha
    lo, hi = 0.0, 4.0
    for _ in range(24):
        mid = (lo + hi) / 2
        if _coverage(np.clip(alpha * mid, 0, 1), cutoff) < target:
            lo = mid
        else:
            hi = mid
    return np.clip(alpha * hi, 0, 1)


def mip_chain(rgba: np.ndarray, role: str, alpha_cutoff: float | None = None) -> list[np.ndarray]:
    """Full chain to 1x1 (power-of-two edges required)."""
    h, w = rgba.shape[:2]
    if w & (w - 1) or h & (h - 1):
        raise ValueError(f"{role}: texture edges must be powers of two ({w}x{h})")
    kind = ROLES[role].mip
    levels = [rgba]
    f = rgba.astype(np.float64) / 255.0
    if kind == "color":
        lin = srgb_to_linear(f[..., :3]) * f[..., 3:4]
        cur = np.concatenate([lin, f[..., 3:4]], axis=2)
        base_cov = _coverage(f[..., 3], alpha_cutoff) if alpha_cutoff is not None else None
    elif kind == "normal":
        cur = f[..., :3] * 2 - 1
    else:
        cur = f
    while cur.shape[0] > 1 or cur.shape[1] > 1:
        cur = _box(cur)
        if kind == "color":
            a = cur[..., 3]
            rgb = cur[..., :3] / np.maximum(a[..., None], 1e-8)
            if base_cov is not None:
                a = _keep_coverage(a, base_cov, alpha_cutoff)
            out = np.concatenate([linear_to_srgb(np.clip(rgb, 0, 1)), a[..., None]], axis=2)
        elif kind == "normal":
            n = cur / np.maximum(np.linalg.norm(cur, axis=2, keepdims=True), 1e-8)
            flat = np.linalg.norm(cur, axis=2) < 1e-4
            n[flat] = (0, 0, 1)
            out = np.concatenate([n * 0.5 + 0.5, np.ones(n.shape[:2] + (1,))], axis=2)
        else:
            out = cur
        levels.append(np.clip(np.rint(out * 255), 0, 255).astype(np.uint8))
    return levels


def resize_pow2(rgba: np.ndarray, edge: int) -> np.ndarray:
    """Downscale (never enlarge) so the longest edge is <= edge and both edges are powers of two."""
    h, w = rgba.shape[:2]
    def p2(v):
        return 1 << max(0, int(v).bit_length() - 1)
    scale = min(1.0, edge / max(w, h))
    tw, th = max(1, p2(w * scale)), max(1, p2(h * scale))
    if (tw, th) == (w, h):
        return rgba
    im = Image.fromarray(rgba, "RGBA")
    return np.array(im.resize((tw, th), Image.Resampling.LANCZOS), dtype=np.uint8)


# --------------------------------------------------------------------------- encode

def _texconv_level(level: np.ndarray, fmt: str, workdir: Path) -> bytes:
    h, w = level.shape[:2]
    pw, ph = max(4, w), max(4, h)
    if (pw, ph) != (w, h):     # a 1x1 / 2x2 level is one block: pad by edge replication
        level = np.pad(level, ((0, ph - h), (0, pw - w), (0, 0)), mode="edge")
    src = workdir / "level.png"
    Image.fromarray(level, "RGBA").save(src)
    out = workdir / "level.dds"
    out.unlink(missing_ok=True)
    r = subprocess.run([str(TEXCONV), "-nologo", "-nogpu", "-y", "-dx10", "--ignore-srgb", "-m", "1",
                        "-f", fmt, "-ft", "dds", "-o", str(workdir), str(src)],
                       capture_output=True, text=True, env=dict(os.environ, OMP_NUM_THREADS="1"),
                       **console_child.child_kwargs(redirected=True))
    if r.returncode or not out.is_file():
        raise RuntimeError(f"texconv {fmt} failed: {r.stdout[-400:]} {r.stderr[-400:]}")
    data = out.read_bytes()
    if data[:4] != b"DDS " or data[84:88] != b"DX10":
        raise RuntimeError("texconv did not write a DX10 DDS")
    blocks = data[148:]
    bw, bh = (w + 3) // 4, (h + 3) // 4
    per_block = 16
    if (pw, ph) != (w, h):     # keep only the blocks that cover the real level
        row = (pw // 4) * per_block
        blocks = b"".join(blocks[r * row:r * row + bw * per_block] for r in range(bh))
    if len(blocks) != bw * bh * per_block:
        raise RuntimeError(f"unexpected block payload {len(blocks)} for {w}x{h}")
    return blocks


def encode_levels(levels: list[np.ndarray], role: str, compress=True) -> tuple[int, list[bytes]]:
    r = ROLES[role]
    if not compress:
        return (VK_RGBA8_SRGB if r.vk_format == VK_BC7_SRGB else VK_RGBA8_UNORM), [lv.tobytes() for lv in levels]
    if not TEXCONV.is_file():
        raise RuntimeError(f"texconv not found at {TEXCONV} (set MC2_TEXCONV); refusing to download one")
    with tempfile.TemporaryDirectory(prefix="mc2tex-") as d:
        return r.vk_format, [_texconv_level(lv, r.texconv_format, Path(d)) for lv in levels]


# --------------------------------------------------------------------------- KTX2

KTX2_MAGIC = b"\xabKTX 20\xbb\r\n\x1a\n"
_MODEL = {VK_RGBA8_UNORM: 1, VK_RGBA8_SRGB: 1, VK_BC5_UNORM: 132, VK_BC7_UNORM: 134, VK_BC7_SRGB: 134}


def _dfd(vk_format: int) -> bytes:
    srgb = vk_format in (VK_RGBA8_SRGB, VK_BC7_SRGB)
    if vk_format in (VK_RGBA8_UNORM, VK_RGBA8_SRGB):
        dims, plane = (0, 0, 0, 0), 4
        samples = [(i * 8, 7, ch | (0x10 if (ch == 15 and srgb) else 0), 0, 255) for i, ch in enumerate((0, 1, 2, 15))]
    elif vk_format == VK_BC5_UNORM:
        dims, plane = (3, 3, 0, 0), 16
        samples = [(0, 63, 0, 0, 0xFFFFFFFF), (64, 63, 1, 0, 0xFFFFFFFF)]
    else:
        dims, plane = (3, 3, 0, 0), 16
        samples = [(0, 127, 0, 0, 0xFFFFFFFF)]
    block = struct.pack("<IHHBBBB4B8B", 0, 2, 24 + 16 * len(samples), _MODEL[vk_format], 1, 2 if srgb else 1, 0,
                        *dims, plane, 0, 0, 0, 0, 0, 0, 0)
    for off, length, ch, lower, upper in samples:
        block += struct.pack("<HBB4BII", off, length, ch, 0, 0, 0, 0, lower, upper)
    return struct.pack("<I", 4 + len(block)) + block


def write_ktx2(vk_format: int, width: int, height: int, levels: list[bytes]) -> bytes:
    n = len(levels)
    dfd = _dfd(vk_format)
    header = KTX2_MAGIC + struct.pack("<9I", vk_format, 1, width, height, 0, 0, 1, n, 0)
    index_end = len(header) + 32 + 24 * n
    dfd_off = index_end
    data_start = dfd_off + len(dfd)
    align = 16 if vk_format in (VK_BC5_UNORM, VK_BC7_UNORM, VK_BC7_SRGB) else 4
    body = bytearray()
    offsets = [0] * n
    pos = data_start
    for lvl in reversed(range(n)):          # KTX2 stores the smallest level first
        pad = -pos % align
        body += b"\0" * pad
        pos += pad
        offsets[lvl] = pos
        body += levels[lvl]
        pos += len(levels[lvl])
    index = struct.pack("<IIIIQQ", dfd_off, len(dfd), 0, 0, 0, 0)
    for lvl in range(n):
        index += struct.pack("<QQQ", offsets[lvl], len(levels[lvl]), len(levels[lvl]))
    return header + index + dfd + bytes(body)


def read_ktx2(data: bytes):
    """(vk_format, width, height, [level bytes, level 0 first]) -- tests and tooling."""
    if data[:12] != KTX2_MAGIC:
        raise ValueError("not KTX2")
    vk, _, w, h, _, _, _, n, sc = struct.unpack_from("<9I", data, 12)
    if sc:
        raise ValueError("supercompressed KTX2 unsupported")
    levels = []
    for lvl in range(n):
        off, length, _ = struct.unpack_from("<QQQ", data, 80 + 24 * lvl)
        levels.append(data[off:off + length])
    return vk, w, h, levels


def read_ktx2_rgba8(data: bytes) -> np.ndarray:
    vk, w, h, levels = read_ktx2(data)
    if vk not in (VK_RGBA8_UNORM, VK_RGBA8_SRGB):
        raise ValueError("only uncompressed RGBA8 KTX2 decodes in Python; BC sources are not re-encoded")
    return np.frombuffer(levels[0], np.uint8).reshape(h, w, 4).copy()


# --------------------------------------------------------------------------- cook

def cook_ktx2(rgba: np.ndarray, role: str, *, alpha_cutoff=None, compress=True,
              source_sha256: str | None = None, cache_dir="default") -> bytes:
    """Cache-backed: same resolved bytes + contract + params -> the same KTX2 bytes."""
    if role not in ROLES:
        raise ValueError(f"unknown texture role {role!r}")
    h, w = rgba.shape[:2]
    params = {"role": role, "w": w, "h": h, "cutoff": alpha_cutoff, "compress": compress,
              "encoder": "texconv-cpu" if compress else "rgba8",
              "toolVersion": texture_tool_version(compress),
              "pillow": PILLOW_VERSION, "numpy": np.__version__}
    key = cache.cache_key(CONTRACT_VERSION, params, [source_sha256 or rgba.tobytes()])

    def produce():
        vk, blocks = encode_levels(mip_chain(rgba, role, alpha_cutoff), role, compress)
        return write_ktx2(vk, w, h, blocks)
    if cache_dir is None:
        return produce()
    if cache_dir == "default":
        cache_dir = cache.DEFAULT_DIR
    return cache.cached(cache_dir, KTX_KIND, key, produce)


def texture_tool_version(compress=True):
    return provenance.tool_version(__file__, Path(__file__).with_name("package.py"),
                                   provenance.__file__, *([TEXCONV] if compress else []))


def record_cook(output, source_hash, params, compress=True):
    provenance.record(output, source_hash=source_hash, tool="mc2-content-texcook",
                      version=texture_tool_version(compress),
                      params=dict(params, contract=CONTRACT_VERSION, compress=compress,
                                  pillow=PILLOW_VERSION, numpy=np.__version__))


def write_tga(path, rgba: np.ndarray):
    """Uncompressed TGA (32-bit when any texel is translucent, else 24-bit)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    has_alpha = bool((rgba[..., 3] < 255).any())
    Image.fromarray(rgba if has_alpha else rgba[..., :3], "RGBA" if has_alpha else "RGB").save(path, compression=None)


def base_color_tiers(rgba: np.ndarray, tiers=TIERS, texel_scale: int = 1):
    """{tier: image} real downscales; a tier never exceeds the source edge.

    ObjectTextureRes names a quality level sized for stock atlases. texel_scale states how
    much more texture an authored asset needs for the same on-screen texel density (a unique
    bake atlas spends ~4x the texels of a tiled stock atlas): tier t then holds an edge of
    t * texel_scale, still a real downscale per tier and never above the source."""
    if texel_scale not in (1, 2, 4, 8, 16):
        raise ValueError("texel_scale must be 1, 2, 4, 8 or 16")
    src = resize_pow2(rgba, max(rgba.shape[:2]))
    result = {t: resize_pow2(src, t * texel_scale) for t in tiers}
    shapes = [image.shape[:2] for image in result.values()]
    if len(set(shapes)) < len(shapes):
        logging.getLogger(__name__).warning(
            "texture tiers collapse at source %dx%d (texel_scale=%d): %s",
            src.shape[1], src.shape[0], texel_scale,
            ", ".join(f"{tier}={image.shape[1]}x{image.shape[0]}" for tier, image in result.items()))
    return result
