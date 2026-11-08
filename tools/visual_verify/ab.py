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
import csv
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
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pixel_hist  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEFAULT_INSTALL = "A:/Games/mc2-opengl/mc2-win64-v0.4"
THRESH = 24


MANIFEST = ".deployed_manifest.csv"


def read_manifest(install):
    """{relpath: sha256} from <install>/.deployed_manifest.csv, plus the src_commit recorded for mc2.exe (or None); ({}, None) when absent."""
    files, exe_commit = {}, None
    try:
        with open(Path(install) / MANIFEST, newline="", encoding="utf-8") as f:
            for row in csv.reader(f):
                if len(row) >= 4 and row[0] not in ("manifest_version", "relpath"):
                    files[row[0]] = row[1]
                    if row[0] == "mc2.exe":
                        exe_commit = row[3] or None
    except OSError:
        pass
    return files, exe_commit


def exe_commit(exe):
    """Commit sha embedded in an exe (build fingerprint), or None."""
    try:
        from exe_fingerprint import extract
        fp = extract(exe)
        sha = fp.get("sha") if fp.get("present") else None
        return sha if sha and sha != "unknown" else None
    except Exception:  # noqa: BLE001 -- an unreadable exe is "unknown", reported by the caller
        return None


def install_commit(install):
    """(commit, how) of an install: its mc2.exe fingerprint, else the manifest's mc2.exe row; (None, why) when unknown."""
    c = exe_commit(Path(install) / "mc2.exe")
    if c:
        return c, "exe fingerprint"
    c = read_manifest(install)[1]
    return (c, MANIFEST) if c else (None, f"no fingerprint in {install}/mc2.exe and no mc2.exe row in {MANIFEST}")


def git_ok(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True).returncode == 0


def commit_order_problem(older, newer, what_older, what_newer):
    """None when `older` is an ancestor of (or equal to) `newer`; else the exact mismatch."""
    if not older or not newer:
        return f"{what_older if not older else what_newer}: commit unknown"
    for c, w in ((older, what_older), (newer, what_newer)):
        if not git_ok("cat-file", "-e", c + "^{commit}"):
            return f"{w} commit {c[:12]} is not in this repository"
    if not git_ok("merge-base", "--is-ancestor", older, newer):
        return f"{what_older} {older[:12]} is not an ancestor of {what_newer} {newer[:12]}"
    return None


def manifest_mismatches(ma, mb):
    """Data/shader manifest differences ignoring mc2.exe and shaders/vulkan/*.spv (those are the build under test)."""
    skip = lambda p: p == "mc2.exe" or (p.startswith("shaders/vulkan/") and p.endswith(".spv"))
    ma = {k: v for k, v in ma.items() if not skip(k)}
    mb = {k: v for k, v in mb.items() if not skip(k)}
    out = [f"only in A: {k}" for k in sorted(ma.keys() - mb.keys())] + [f"only in B: {k}" for k in sorted(mb.keys() - ma.keys())]
    out += [f"differs: {k} (A {ma[k][:12]} vs B {mb[k][:12]})" for k in sorted(ma.keys() & mb.keys()) if ma[k] != mb[k]]
    return out


def base_problems(install_a=None, install_b=None, exe_a=None, exe_b=None, install=None):
    """Reasons the two sides are not a same-base A/B. Install mode: A's commit must be an ancestor of B's and the data/shader
    manifests must match. Exe-swap mode: each exe's commit must descend from the install's deployed commit."""
    problems = []
    if install_a and install_b:
        ca, how_a = install_commit(install_a)
        cb, how_b = install_commit(install_b)
        p = commit_order_problem(ca, cb, f"A ({install_a}; {how_a})", f"B ({install_b}; {how_b})")
        if p:
            problems.append(p)
        (ma, _), (mb, _) = read_manifest(install_a), read_manifest(install_b)
        if not ma or not mb:
            problems.append(f"{MANIFEST} missing in " + " and ".join(d for d, m in ((install_a, ma), (install_b, mb)) if not m))
        else:
            diffs = manifest_mismatches(ma, mb)
            if diffs:
                problems.append(f"{len(diffs)} data/shader manifest mismatch(es) besides mc2.exe and shaders/vulkan/*.spv: "
                                + "; ".join(diffs[:20]) + (" ..." if len(diffs) > 20 else ""))
    elif exe_a and exe_b:
        base, how = install_commit(install)
        for name, exe in (("A", exe_a), ("B", exe_b)):
            p = commit_order_problem(base, exe_commit(exe), f"install {install} ({how})", f"exe {name} {exe}")
            if p:
                problems.append(p)
    return problems


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


def capture_gpus(cap_dir: Path) -> list:
    """The GPU device names a capture dir's manifests logged (run_visual_capture records them), sorted."""
    names = set()
    for mf in Path(cap_dir).glob("*_capture_manifest.json"):
        try:
            m = json.loads(mf.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue
        rec = m.get("gpu") or (m.get("extra") or {}).get("gpu") or {}
        names.update(rec.get("logged") or [])
    return sorted(names)


def usable_png(p) -> bool:
    """True for an existing, non-empty PNG that PIL can decode (a 0-byte or truncated capture is not evidence)."""
    try:
        if not p or Path(p).stat().st_size == 0:
            return False
        with Image.open(p) as im:
            im.load()
        return True
    except (OSError, ValueError):
        return False


def evidence_problems(a_dir: Path, b_dir: Path, bookmarks) -> list:
    """One line per bookmark whose A or B image is missing, empty or undecodable. An empty bookmark list is itself a
    problem: a comparison of nothing must never exit 0."""
    if not bookmarks:
        return ["no bookmarks were compared"]
    return [f"{bm}: side {name} has no usable png" for bm in bookmarks
            for name, d in (("A", a_dir), ("B", b_dir)) if not usable_png(find_png(d, bm))]


def discover_bookmarks(a_dir: Path, b_dir: Path) -> list:
    """Union of the capture folders on either side, so a bookmark one side lacks is reported instead of silently skipped."""
    names = set()
    for d in (a_dir, b_dir):
        if d.is_dir():
            names |= {p.name for p in d.iterdir() if p.is_dir()}
    return sorted(names)


def stamp_cross_base(img: Image.Image) -> Image.Image:
    """Burn a red CROSS-BASE banner into the top-left of a montage (the two sides are not a same-base A/B)."""
    from PIL import ImageDraw, ImageFont
    d = ImageDraw.Draw(img)
    try:
        font = ImageFont.load_default(size=max(16, img.width // 40))
    except TypeError:
        font = ImageFont.load_default()
    box = d.textbbox((8, 8), "CROSS-BASE", font=font)
    d.rectangle((box[0] - 4, box[1] - 4, box[2] + 4, box[3] + 4), fill=(200, 0, 0))
    d.text((8, 8), "CROSS-BASE", fill=(255, 255, 255), font=font)
    return img


def compare(gl_dir: Path, vk_dir: Path, out: Path, crops, bookmarks=None, cross_base=False):
    """Returns the compared rows; compare.problems lists missing/empty evidence (callers exit nonzero on it)."""
    if not bookmarks:
        bookmarks = discover_bookmarks(gl_dir, vk_dir)
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    gpu_a, gpu_b = capture_gpus(gl_dir), capture_gpus(vk_dir)
    ph_cfg = pixel_hist.load_thresholds()
    for bm in bookmarks:
        pg, pv = find_png(gl_dir, bm), find_png(vk_dir, bm)
        if not usable_png(pg) or not usable_png(pv):
            print(f"[ab] {bm}: missing or empty png (a={pg} b={pv})")
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
        # pixel-count verdict next to MAD (reported only, never a gate input yet)
        ph_res = pixel_hist.evaluate(np.asarray(a), np.asarray(b), pixel_hist.resolve_thresholds(ph_cfg, bm, gpu_b))
        row = {"bookmark": bm, "size": list(a.size), "pixel_hist": ph_res,
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
        if cross_base:
            m = stamp_cross_base(m)
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
    for r in rows:  # separate lines (no '|'): the gate parses the table above by columns
        print(f"[pixel_hist] {r['bookmark']}: {pixel_hist.summary_line(r['pixel_hist'])}")
    if gpu_a and gpu_b and gpu_a != gpu_b:
        print(f"[ab] WARNING: the two sides ran on different GPUs ({gpu_a} vs {gpu_b}); the diff is not a clean A/B")
    problems = evidence_problems(gl_dir, vk_dir, bookmarks)
    if problems:
        print("[ab] FAIL: " + "; ".join(problems))
    (out / "summary.json").write_text(json.dumps(
        {"gl": str(gl_dir), "vk": str(vk_dir), "threshold": THRESH, "cross_base": cross_base, "rows": rows, "problems": problems,
         "gpu": {"a": gpu_a, "b": gpu_b, "same": gpu_a == gpu_b}}, indent=2))
    print(f"[ab] wrote {out}")
    compare.problems = problems
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
    ap.add_argument("--allow-base-mismatch", action="store_true",
                    help="deliberate cross-base comparison: print the base/data mismatch, stamp CROSS-BASE on the montage, go on")
    a = ap.parse_args(argv)
    os.environ["MC2_CURSOR_NO_PARK"] = "1"
    crops = [tuple(float(x) for x in c.split(",")) for c in a.crop]
    for c in crops:
        if len(c) != 4:
            sys.exit("--crop needs 4 comma-separated fractions")
    stamp = time.strftime("%Y%m%d_%H%M%S")
    out = Path(a.out_root) if a.out_root else ROOT / "tests/visual/captures" / f"ab_{stamp}"
    if a.compare_only:
        compare(Path(a.compare_only[0]), Path(a.compare_only[1]), out, crops, cross_base=a.allow_base_mismatch)  # capture dirs carry no install identity
        sys.exit(1 if getattr(compare, "problems", None) else 0)
    if not a.bookmarks:
        sys.exit("--bookmarks required")
    bad = base_problems(a.install_a, a.install_b, a.exe_a, a.exe_b, a.install)
    if bad and not a.allow_base_mismatch:
        sys.exit("ab.py: refusing an A/B across different bases:\n  " + "\n  ".join(bad)
                 + "\n  (deliberate cross-base comparison: --allow-base-mismatch; the montage is then stamped CROSS-BASE)")
    for b in bad:
        print("[ab] CROSS-BASE (allowed): " + b, flush=True)
    out.mkdir(parents=True, exist_ok=True)
    ia, ib = resolve_installs(a, out)
    env = ["MC2_RENDER_BACKEND=vulkan", "MC2_VK_SHARED_GBUFFER=1"] + a.env + env_pin.focus_declarations(a.focus, a.focus_zoom)
    run_capture(ia, a.bookmarks, out / "a", env)
    run_capture(ib, a.bookmarks, out / "b", env)
    compare(out / "a", out / "b", out / "cmp", crops, [s.strip() for s in a.bookmarks.split(",") if s.strip()], cross_base=bool(bad))
    sys.exit(1 if getattr(compare, "problems", None) else 0)


if __name__ == "__main__":
    main()
