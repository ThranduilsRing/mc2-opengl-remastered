#!/usr/bin/env python3
"""VK before/after A/B: capture two builds on the Vulkan backend, then montage (A|B with a FLIP error map below) + crops + diff heatmap + FLIP/MAD table.

A is the left/"before" image, B the right/"after" one. Pick the two builds one way:
  --install-a DIR --install-b DIR     two installs (each has its own mc2.exe + data)
  --exe-a EXE --exe-b EXE [--install DIR]   two exes run against one install's data (staged by hard links; DLLs come from --install)
Usage:
  py -3 tools/visual_verify/ab.py --bookmarks a,b (--install-a DIR --install-b DIR | --exe-a EXE --exe-b EXE) [--env K=V ...]
        [--crop x0,y0,x1,y1 ...] [--out-root DIR]
  py -3 tools/visual_verify/ab.py --compare-only DIR_A DIR_B [--crop ...] [--out-root DIR]
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
# OWNER RULE: no window for any child process (hidden_kwargs: CREATE_NO_WINDOW + SW_HIDE).
from smoke_lib import nofocus  # noqa: E402
import mc2_env_pin as env_pin  # noqa: E402

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


def flip_map(a: Image.Image, b: Image.Image):
    """NVIDIA FLIP (LDR) error map, HxW float in [0,1]; None when flip-evaluator is not installed (pip install flip-evaluator==1.7)."""
    try:
        import flip_evaluator
    except ImportError:
        print("[ab] flip-evaluator not installed: FLIP columns are n/a", flush=True)
        return None
    f32 = lambda im: np.asarray(im, dtype=np.float32) / 255.0
    return flip_evaluator.evaluate(f32(a), f32(b), "LDR", inputsRGB=True, applyMagma=False)[0][:, :, 0]


def flip_image(fm: np.ndarray) -> Image.Image:
    """Magma-like ramp (black, purple, orange, pale yellow) of the FLIP map; FLIP 1.0 = full scale, no gain."""
    v = np.clip(fm, 0, 1)
    r = np.clip(v * 3.0, 0, 1)
    g = np.clip(v * 3.0 - 1.2, 0, 1)
    bl = np.clip(np.where(v < 0.5, v * 2.0, 1.2 - v * 2.0), 0, 1) * 0.8 + np.clip(v * 3.0 - 2.2, 0, 1) * 0.6
    return Image.fromarray((np.stack([r, g, np.clip(bl, 0, 1)], -1) * 255).astype(np.uint8))


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
            print(f"[ab] {bm}: missing png (a={pg} b={pv})")
            continue
        a, b = load(pg), load(pv)
        size_mismatch = None
        if a.size != b.size:
            size_mismatch = {}
            print(f"[ab] WARNING {bm}: image size mismatch A={a.size} B={b.size}; resizing B (metrics unreliable)", flush=True)
            b = b.resize(a.size, Image.LANCZOS)
        A, B = np.asarray(a).astype(np.int16), np.asarray(b).astype(np.int16)
        D = np.abs(A - B)
        fm = flip_map(a, b)
        row = {"bookmark": bm, "size": list(a.size),
               "flip_mean": float(fm.mean()) if fm is not None else None,
               "flip_p99": float(np.percentile(fm, 99)) if fm is not None else None,
               "mad": float(D.mean()),
               "pct_diff_gt24": float((D.max(axis=2) > THRESH).mean() * 100),
               "size_mismatch": size_mismatch,
               "crops": []}
        bd = out / bm
        bd.mkdir(exist_ok=True)
        m = side(a, b, 1800)
        if fm is not None:  # FLIP error map under the A|B pair, scaled to the montage width
            fi = flip_image(fm)
            fi = fi.resize((m.width, max(1, int(fi.height * m.width / fi.width))), Image.BILINEAR)
            mm = Image.new("RGB", (m.width, m.height + fi.height))
            mm.paste(m, (0, 0))
            mm.paste(fi, (0, m.height))
            m = mm
            flip_image(fm).save(bd / "flip.jpg", quality=85)
        m.save(bd / "montage.jpg", quality=85)
        h = heat(D.astype(np.uint8))
        if h.width > 1200:
            h = h.resize((1200, int(h.height * 1200 / h.width)), Image.BILINEAR)
        h.save(bd / "diff.jpg", quality=85)
        for i, c in enumerate(crops):
            box = crop_box(a, c)
            ca, cb = a.crop(box), b.crop(box)
            m = float(D[box[1]:box[3], box[0]:box[2]].mean())
            row["crops"].append({"crop": list(c), "mad": m,
                                 "flip_mean": float(fm[box[1]:box[3], box[0]:box[2]].mean()) if fm is not None else None})
            side(ca, cb, 1600).save(bd / f"crop{i}.jpg", quality=85)
        rows.append(row)
    # FLIP leads (perceptual: 0 identical, 1 worst); MAD and %>24 stay for continuity. No pass threshold here.
    hdr = "bookmark | FLIP mean | FLIP p99 | MAD | %>24" + "".join(f" | crop{i} (MAD/FLIP)" for i in range(len(crops)))
    print(hdr)
    for r in rows:
        fmean = "n/a" if r["flip_mean"] is None else f"{r['flip_mean']:.4f}"
        fp99 = "n/a" if r["flip_p99"] is None else f"{r['flip_p99']:.4f}"
        print(f"{r['bookmark']} | {fmean} | {fp99} | {r['mad']:.4f} | {r['pct_diff_gt24']:.2f}"
              + "".join(f" | {c['mad']:.2f}/" + ("n/a" if c["flip_mean"] is None else f"{c['flip_mean']:.4f}") for c in r["crops"]))
    (out / "summary.json").write_text(json.dumps(
        {"gl": str(gl_dir), "vk": str(vk_dir), "threshold": THRESH, "rows": rows}, indent=2))
    print(f"[ab] wrote {out}")
    return rows




def stage_install(install: Path, exe: Path, dest: Path) -> Path:
    """An install clone whose mc2.exe is `exe`: data and DLLs are hard-linked from `install` (copied across volumes)."""
    def link(src, dst):
        try:
            os.link(src, dst)
        except OSError:
            shutil.copy2(src, dst)
    if dest.exists():
        shutil.rmtree(dest)
    shutil.copytree(install, dest, copy_function=link)
    target = dest / "mc2.exe"
    if target.exists():
        target.unlink()
    shutil.copy2(exe, target)
    return dest


def seed_install(install, bookmarks, reference=DEFAULT_INSTALL):
    """A fresh lane install lacks saved_bookmarks/<bm>.json and data/textures/*.burnin.*: copy what is missing from the reference
    install (as the gate's stage step does); exit naming what is still missing so a capture never runs on the wrong data."""
    install, reference = Path(install), Path(reference)
    missing = []
    for bm in [b.strip() for b in bookmarks.split(",") if b.strip()]:
        dst = install / "saved_bookmarks" / (bm + ".json")
        src = reference / "saved_bookmarks" / (bm + ".json")
        if not dst.is_file() and src.is_file():
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dst)
        if not dst.is_file():
            missing.append(f"{dst} (not in reference {reference} either)")
    textures = install / "data" / "textures"
    for src in (reference / "data" / "textures").glob("*.burnin.*"):
        if not (textures / src.name).exists():
            textures.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, textures / src.name)
    if not any(textures.glob("*.burnin.*")):
        missing.append(f"{textures}/*.burnin.* (reference {reference} has none)")
    if missing:
        sys.exit("ab.py: install is missing required capture data:\n  " + "\n  ".join(missing)
                 + "\n  fix: use --install DIR to name a reference install that has them, or copy them in.")


def resolve_installs(a, out: Path):
    """-> (install_a, install_b) from --install-a/--install-b or --exe-a/--exe-b (+ --install); exits on a bad combination."""
    if a.install_a or a.install_b:
        if not (a.install_a and a.install_b) or a.exe_a or a.exe_b:
            sys.exit("use --install-a with --install-b, or --exe-a with --exe-b, not a mix")
        for d in (a.install_a, a.install_b):
            seed_install(d, a.bookmarks or "", a.install)
        return a.install_a, a.install_b
    if a.exe_a and a.exe_b:
        for e in (a.exe_a, a.exe_b):
            if not Path(e).is_file():
                sys.exit(f"exe not found: {e}")
        return (str(stage_install(Path(a.install), Path(a.exe_a), out / "install_a")),
                str(stage_install(Path(a.install), Path(a.exe_b), out / "install_b")))
    sys.exit("need --install-a/--install-b or --exe-a/--exe-b (or --compare-only DIR_A DIR_B)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("--bookmarks")
    ap.add_argument("--install-a", metavar="DIR")
    ap.add_argument("--install-b", metavar="DIR")
    ap.add_argument("--exe-a", metavar="EXE")
    ap.add_argument("--exe-b", metavar="EXE")
    ap.add_argument("--install", default=DEFAULT_INSTALL, help="data/DLL source for --exe-a/--exe-b")
    ap.add_argument("--focus", metavar="mech[:N]", default=None,
                    help="MC2_CAPTURE_FOCUS: move every bookmark pose onto the Nth live "
                         "player-team mech (default 0); the bookmark still supplies rot/proj.")
    ap.add_argument("--focus-zoom", type=float, default=None, metavar="ALT",
                    help="camera altitude with --focus (default 200 = mech at about a fifth of the frame)")
    ap.add_argument("--env", action="append", default=[], metavar="K=V", help="extra env for both captures")
    ap.add_argument("--crop", action="append", default=[], help="x0,y0,x1,y1 fractions")
    ap.add_argument("--out-root")
    ap.add_argument("--compare-only", nargs=2, metavar=("DIR_A", "DIR_B"))
    a = ap.parse_args(argv)
    os.environ["MC2_CURSOR_NO_PARK"] = "1"
    crops = [tuple(float(x) for x in c.split(",")) for c in a.crop]
    for c in crops:
        if len(c) != 4:
            sys.exit("--crop needs 4 comma-separated fractions")
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = Path(a.out_root) if a.out_root else ROOT / "tests/visual/captures" / f"ab_{stamp}"
    if a.compare_only:
        compare(Path(a.compare_only[0]), Path(a.compare_only[1]), out, crops)
        return
    if not a.bookmarks:
        sys.exit("--bookmarks required")
    out.mkdir(parents=True, exist_ok=True)
    ia, ib = resolve_installs(a, out)
    env = ["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"] + a.env + env_pin.focus_declarations(a.focus, a.focus_zoom)
    run_capture(ia, a.bookmarks, out / "a", env)
    run_capture(ib, a.bookmarks, out / "b", env)
    compare(out / "a", out / "b", out / "cmp", crops, [s.strip() for s in a.bookmarks.split(",")])


if __name__ == "__main__":
    main()
