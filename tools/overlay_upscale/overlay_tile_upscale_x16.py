#!/usr/bin/env python3
"""RUNWAY-OVERLAY-QUALITY-1 cont1: cook <512|1024>Overlays/ and <512|1024>/cement_*.tga from the shipped 256 art.

256 tiles (already the 64px stock art through the house upscale) -> realesrgan-x4plus (ncnn) once = 1024; 512 = Lanczos
downscale of that 1024. Alpha (the road shape) stays Lanczos, never AI. Engine ladder picks the folders up (mclib/terrtxm.cpp).
  py -3 overlay_tile_upscale_x16.py --tex <data/textures> --out <dir> [--ncnn <exe>]
"""
import argparse, os, shutil, struct, subprocess, sys, tempfile
from pathlib import Path
import numpy as np
from PIL import Image

def rd(p):
    b = p.read_bytes(); w, h = struct.unpack_from("<HH", b, 12); d, desc = b[16], b[17]; n = d // 8
    return np.frombuffer(b, np.uint8, w * h * n, 18 + b[0]).reshape(h, w, n), desc

def wr(p, bgra, desc):
    h, w, n = bgra.shape; p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(bytes([0, 0, 2]) + bytes(9) + struct.pack("<HH", w, h) + bytes([n * 8, desc]) + bgra.astype(np.uint8).tobytes())

def main():
    ap = argparse.ArgumentParser(); ap.add_argument("--tex", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--ncnn", default=os.environ.get("MC2_REALESRGAN_EXE", "A:/Games/mc2-opengl-src/realesrgan-ncnn-vulkan/realesrgan-ncnn-vulkan.exe"))  # drive-path: ok maintainer-only offline art cook, env override
    a = ap.parse_args(); tex, out = Path(a.tex), Path(a.out)
    jobs = [(src, f"{{}}Overlays") for src in sorted((tex / "256Overlays").glob("*.tga"))] + \
           [(src, "{}") for src in sorted((tex / "256").glob("*.tga")) if src.name.split("_")[0] in ("cement", "undefined", "water")]
    tmp = Path(tempfile.mkdtemp(dir=out.parent if out.parent.exists() else None)); tin, tout = tmp / "in", tmp / "out"
    tin.mkdir(); tout.mkdir(); meta = {}
    for i, (src, sub) in enumerate(jobs):
        px, desc = rd(src); meta[i] = (src, sub, px[..., 3] if px.shape[2] == 4 else None, desc)
        Image.fromarray(px[..., [2, 1, 0]]).save(tin / f"{i}.png")
    subprocess.run([a.ncnn, "-i", str(tin), "-o", str(tout), "-n", "realesrgan-x4plus", "-s", "4", "-f", "png"], check=True,
                   cwd=Path(a.ncnn).parent)  # background-launch: allow offline realesrgan upscaler, never the game
    for i, (src, sub, alpha, desc) in meta.items():
        big = Image.open(tout / f"{i}.png").convert("RGB")
        for edge in (1024, 512):
            rgb = np.asarray(big if edge == 1024 else big.resize((edge, edge), Image.LANCZOS))
            o = np.empty((edge, edge, 3 + (alpha is not None)), np.uint8); o[..., :3] = rgb[..., ::-1]
            if alpha is not None: o[..., 3] = np.asarray(Image.fromarray(alpha).resize((edge, edge), Image.LANCZOS))
            bad = (o[..., 2] > 150) & (o[..., 0] > 150) & (o[..., 2] > o[..., 1].astype(int) + 40) & (o[..., 0] > o[..., 1].astype(int) + 40)
            o[..., 1][bad] = np.maximum(o[..., 1][bad], np.minimum(o[..., 0], o[..., 2])[bad] - 40)   # engine colorkey safety
            wr(out / sub.format(edge) / src.name, o, desc)
    shutil.rmtree(tmp); print(f"[x16] {len(meta)} files -> {out}")

if __name__ == "__main__":
    sys.exit(main())
