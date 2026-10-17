#!/usr/bin/env python3
"""GL-vs-VK parity A/B: capture both backends, then montage + diff metrics.

Usage:
  py -3 tools/visual_verify/ab.py --bookmarks a,b [--install DIR] [--env K=V ...]
        [--crop x0,y0,x1,y1 ...] [--out-root DIR] [--skip-gl GL_DIR]
  py -3 tools/visual_verify/ab.py --compare-only GL_DIR VK_DIR [--crop ...] [--out-root DIR]
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_INSTALL = "A:/Games/mc2-opengl/mc2-win64-v0.4"
THRESH = 24


def run_capture(install, bookmarks, out, env_extra):
    cmd = [sys.executable, str(HERE / "capture.py"), "--install", install,
           "--bookmarks", bookmarks, "--out", str(out), "--clean"]
    for e in env_extra:
        cmd += ["--env", e]
    print("[ab] " + " ".join(cmd), flush=True)
    # background-launch: allow streams capture.py progress to the caller's console; inherits it (no new window when the parent has one)
    rc = subprocess.call(cmd, env=dict(os.environ, MC2_CURSOR_NO_PARK="1"))
    if rc != 0:
        sys.exit(f"[ab] capture failed rc={rc}: {out}")


def find_png(d: Path, bm: str):
    p = d / bm / f"{bm}.png"
    if p.exists():
        return p
    c = [x for x in sorted((d / bm).glob("*.png")) if not x.stem.endswith("_dup")]
    return c[0] if c else None


def load(p):
    return Image.open(p).convert("RGB")


def side(a: Image.Image, b: Image.Image, maxw: int) -> Image.Image:
    w = a.width + b.width
    if w > maxw:
        s = maxw / w
        a = a.resize((max(1, int(a.width * s)), max(1, int(a.height * s))), Image.LANCZOS)
        b = b.resize((max(1, int(b.width * s)), max(1, int(b.height * s))), Image.LANCZOS)
    m = Image.new("RGB", (a.width + b.width, max(a.height, b.height)))
    m.paste(a, (0, 0))
    m.paste(b, (a.width, 0))
    return m


def heat(d: np.ndarray) -> Image.Image:
    v = np.clip(d.max(axis=2) * 4.0, 0, 255).astype(np.uint8)  # 4x gain
    r = v
    g = np.clip(v.astype(int) * 2 - 255, 0, 255).astype(np.uint8)
    b = np.zeros_like(v)
    return Image.fromarray(np.stack([r, g, b], -1))


def crop_box(img, c):
    w, h = img.size
    return (int(c[0] * w), int(c[1] * h), max(int(c[2] * w), int(c[0] * w) + 1),
            max(int(c[3] * h), int(c[1] * h) + 1))


def compare(gl_dir: Path, vk_dir: Path, out: Path, crops, bookmarks=None):
    if not bookmarks:
        bookmarks = sorted(p.name for p in gl_dir.iterdir()
                           if p.is_dir() and (vk_dir / p.name).is_dir())
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    for bm in bookmarks:
        pg, pv = find_png(gl_dir, bm), find_png(vk_dir, bm)
        if not pg or not pv:
            print(f"[ab] {bm}: missing png (gl={pg} vk={pv})")
            continue
        a, b = load(pg), load(pv)
        size_mismatch = None
        if a.size != b.size:
            size_mismatch = {"gl": list(a.size), "vk": list(b.size)}
            print(f"[ab] WARNING {bm}: image size mismatch GL={a.size} VK={b.size}; resizing VK (metrics unreliable)", flush=True)
            b = b.resize(a.size, Image.LANCZOS)
        A, B = np.asarray(a).astype(np.int16), np.asarray(b).astype(np.int16)
        D = np.abs(A - B)
        row = {"bookmark": bm, "size": list(a.size),
               "mad": float(D.mean()),
               "pct_diff_gt24": float((D.max(axis=2) > THRESH).mean() * 100),
               "size_mismatch": size_mismatch,
               "crops": []}
        bd = out / bm
        bd.mkdir(exist_ok=True)
        side(a, b, 1800).save(bd / "montage.jpg", quality=85)
        h = heat(D.astype(np.uint8))
        if h.width > 1200:
            h = h.resize((1200, int(h.height * 1200 / h.width)), Image.BILINEAR)
        h.save(bd / "diff.jpg", quality=85)
        for i, c in enumerate(crops):
            box = crop_box(a, c)
            ca, cb = a.crop(box), b.crop(box)
            m = float(D[box[1]:box[3], box[0]:box[2]].mean())
            row["crops"].append({"crop": list(c), "mad": m})
            side(ca, cb, 1600).save(bd / f"crop{i}.jpg", quality=85)
        rows.append(row)
    hdr = "bookmark | MAD | %>24" + "".join(f" | crop{i}" for i in range(len(crops)))
    print(hdr)
    for r in rows:
        print(f"{r['bookmark']} | {r['mad']:.2f} | {r['pct_diff_gt24']:.2f}"
              + "".join(f" | {c['mad']:.2f}" for c in r["crops"]))
    (out / "summary.json").write_text(json.dumps(
        {"gl": str(gl_dir), "vk": str(vk_dir), "threshold": THRESH, "rows": rows}, indent=2))
    print(f"[ab] wrote {out}")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--bookmarks")
    ap.add_argument("--install", default=DEFAULT_INSTALL)
    ap.add_argument("--env", action="append", default=[], metavar="K=V", help="VK-only extras")
    ap.add_argument("--crop", action="append", default=[], help="x0,y0,x1,y1 fractions")
    ap.add_argument("--out-root")
    ap.add_argument("--skip-gl", metavar="DIR")
    ap.add_argument("--compare-only", nargs=2, metavar=("GL_DIR", "VK_DIR"))
    a = ap.parse_args()
    os.environ["MC2_CURSOR_NO_PARK"] = "1"
    crops = [tuple(float(x) for x in c.split(",")) for c in a.crop]
    for c in crops:
        if len(c) != 4:
            sys.exit("--crop needs 4 comma-separated fractions")
    stamp = time.strftime("%Y%m%d_%H%M%S")
    if a.compare_only:
        gl, vk = Path(a.compare_only[0]), Path(a.compare_only[1])
        out = Path(a.out_root) if a.out_root else ROOT / "tests/visual/captures" / f"ab_{stamp}"
        compare(gl, vk, out, crops)
        return
    if not a.bookmarks:
        sys.exit("--bookmarks required")
    out = Path(a.out_root) if a.out_root else ROOT / "tests/visual/captures" / f"ab_{stamp}"
    out.mkdir(parents=True, exist_ok=True)
    if a.skip_gl:
        gl = Path(a.skip_gl)
    else:
        gl = out / "gl"
        run_capture(a.install, a.bookmarks, gl, ["MC2_RENDER_BACKEND=gl"])  # unset means Vulkan: pin the GL arm
    vk = out / "vk"
    run_capture(a.install, a.bookmarks, vk,
                ["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"] + a.env)
    compare(gl, vk, out / "cmp", crops, [s.strip() for s in a.bookmarks.split(",")])


if __name__ == "__main__":
    main()
