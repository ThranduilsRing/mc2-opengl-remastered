#!/usr/bin/env python3
"""UI-CHROME-SPLIT-1: cut the packed UI sheets into per-widget PNG source files.

The legacy buttonLayout/mcl_*.fit blocks crop rects out of a few shared sheets
(MCUI_Universal*, MCL_MB_*, ...). The owner dislikes authored atlases, so this
tool turns every (sheet, rect) the stock FITs use into its own PNG under
data/art/ui/<sheet>/<widget>.png and writes data/art/ui/manifest.json
(widget -> source sheet rect, consumers, nine-slice insets when the widget is
exactly stretchable). The engine re-packs the widgets into the sheet at load
time (mclib/ui_chrome_sheets.cpp), so the on-screen result is pixel-identical.

Deterministic: same input -> byte-identical output.

Usage:
  py -3 tools/ui_art_split.py --install <install> [--out data/art/ui]   # write
  py -3 tools/ui_art_split.py --install <install> --check               # verify only
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
from collections import defaultdict

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: py -3 -m pip install Pillow")

STATES = [("normal", "UNormal", "VNormal"), ("pressed", "UPressed", "VPressed"),
          ("disabled", "UDisabled", "VDisabled"), ("highlight", "UHighlight", "VHighlight"),
          ("highlightpressed", "UHighlightPressed", "VHighlightPressed"),
          ("ambiguous", "UAmbiguous", "VAmbiguous")]
# Sheets that are not chrome atlases: cursor sheets (cursor code owns them) and mech icon registry art.
EXCLUDE_SHEETS = re.compile(r"^(cursors|editorcursors|mcui_gn_mechicons|mechparts|jump|run|walk|mcl_loadingscreen|"
                            r"mcl_loadscreen|mcl_splashscreen|mcl_pr_|mcl_gn_)", re.I)
FIT_DIRS = ("game", "shared")


def read_text(p):
    with open(p, "r", encoding="utf-8-sig", errors="replace") as f:
        return f.read()


def parse_blocks(text):
    """Yield (block_name, {lowerkey: raw}) for every [Block]."""
    out = []
    cur, kv = None, None
    for line in text.splitlines():
        m = re.match(r"^\[([^\]]+)\]", line)
        if m:
            if cur is not None:
                out.append((cur, kv))
            cur, kv = m.group(1), {}
            continue
        if cur is None:
            continue
        km = re.match(r"\s*(?:st|l|f|b|c|ul)\s+(\w+)\s*=\s*(.*?)\s*(?://.*)?$", line)
        if km:
            kv[km.group(1).lower()] = km.group(2).strip()
    if cur is not None:
        out.append((cur, kv))
    return out


def as_int(raw):
    m = re.match(r"[-+]?\d+", raw or "")
    return int(m.group(0)) if m else None


def sheet_key(name):
    base = re.sub(r"\.(tga|png|bmp)$", "", name.strip().strip('"'), flags=re.I)
    return base.replace("\\", "/").split("/")[-1].lower()


def find_sheet(art_dir, key):
    for fn in os.listdir(art_dir):
        stem, ext = os.path.splitext(fn)
        if stem.lower() == key and ext.lower() in (".png", ".tga"):
            return os.path.join(art_dir, fn)
    return None


def sanitize(s):
    return re.sub(r"[^a-z0-9_.\-]", "_", s.lower())


def collect(ui_root):
    """sheet -> {(x,y,w,h): [(fit_stem, block, state), ...]}"""
    uses = defaultdict(lambda: defaultdict(list))
    legacy = os.path.join(ui_root, "legacy")
    for d in FIT_DIRS:
        dd = os.path.join(legacy, d)
        if not os.path.isdir(dd):
            continue
        for fn in sorted(os.listdir(dd)):
            if not fn.lower().endswith(".fit"):
                continue
            stem = fn[:-4].lower()
            for block, kv in parse_blocks(read_text(os.path.join(dd, fn))):
                if "filename" not in kv:
                    continue
                w, h = as_int(kv.get("uwidth")), as_int(kv.get("vheight"))
                if not w or not h or w <= 0 or h <= 0:
                    continue
                key = sheet_key(kv["filename"])
                for state, uk, vk in STATES:
                    u, v = as_int(kv.get(uk.lower())), as_int(kv.get(vk.lower()))
                    if u is None or v is None or u < 0 or v < 0:
                        continue
                    uses[key][(u, v, w, h)].append((stem, block, state))
    return uses


def nine_insets(im):
    """Exact stretch invariance: the central band of columns (rows) all equal the middle one,
    so stretching/tiling that band changes no pixel. Returns [l,t,r,b] or None."""
    w, h = im.size
    if w < 12 or h < 12:
        return None
    px = im.load()

    def col(x):
        return tuple(px[x, y] for y in range(h))

    def row(y):
        return tuple(px[x, y] for x in range(w))

    mid = col(w // 2)
    lo = hi = w // 2
    while lo - 1 >= 0 and col(lo - 1) == mid:
        lo -= 1
    while hi + 1 < w and col(hi + 1) == mid:
        hi += 1
    midr = row(h // 2)
    tlo = thi = h // 2
    while tlo - 1 >= 0 and row(tlo - 1) == midr:
        tlo -= 1
    while thi + 1 < h and row(thi + 1) == midr:
        thi += 1
    l, r = lo, w - 1 - hi
    t, b = tlo, h - 1 - thi
    # sliceable only when a corner region survives on both axes and the stretch band is real
    if (hi - lo + 1) < 4 or (thi - tlo + 1) < 4:
        return None
    if l < 1 or r < 1 or t < 1 or b < 1:
        return None
    if l > w * 0.4 or r > w * 0.4 or t > h * 0.4 or b > h * 0.4:
        return None
    return [l, t, r, b]


def build(install, out_dir, write):
    data = os.path.join(install, "data")
    art_dir = os.path.join(data, "art", "gui")
    uses = collect(os.path.join(data, "defs", "ui"))
    manifest = {"version": 1, "generator": "tools/ui_art_split.py", "sheets": {}}
    files = {}
    skipped = []
    for key in sorted(uses):
        if EXCLUDE_SHEETS.match(key):
            skipped.append((key, "excluded"))
            continue
        rects = uses[key]
        if len(rects) < 2:
            skipped.append((key, "single rect"))
            continue
        path = find_sheet(art_dir, key)
        if not path:
            skipped.append((key, "sheet file not found"))
            continue
        im = Image.open(path)
        if im.mode not in ("RGB", "RGBA"):
            im = im.convert("RGBA" if "A" in im.mode else "RGB")
        W, H = im.size
        if any(x + w > W or y + h > H for (x, y, w, h) in rects):
            skipped.append((key, "a rect falls outside the sheet (another consumer owns it)"))
            continue
        widgets = {}
        used_names = set()
        screens = set()
        for rect in sorted(rects):
            x, y, w, h = rect
            cx1, cy1 = min(x + w, W), min(y + h, H)
            if x >= W or y >= H:
                skipped.append((key, f"rect {rect} outside sheet"))
                continue
            u = sorted(rects[rect])
            stem, block, state = u[0]
            name = sanitize(f"{stem}.{block}" + ("" if state == "normal" else "." + state))
            if name in used_names:
                name = f"{name}_{x}_{y}"
            used_names.add(name)
            crop = im.crop((x, y, cx1, cy1))
            rel = f"{key}/{name}.png"
            ent = {"rect": [x, y, cx1 - x, cy1 - y]}
            if (cx1 - x, cy1 - y) != (w, h):
                ent["clipped_from"] = [w, h]
            ni = nine_insets(crop)
            if ni:
                ent["nine"] = ni
            ent["uses"] = sorted({f"{s}:{b}" for s, b, _ in u})
            screens.update(s for s, _, _ in u)
            widgets[name] = ent
            files[rel] = crop
        manifest["sheets"][key] = {"file": os.path.basename(path), "size": [W, H], "mode": im.mode,
                                   "rects": len(widgets), "screens": sorted(screens), "widgets": widgets}
    if write:
        os.makedirs(out_dir, exist_ok=True)
        for rel, crop in files.items():
            p = os.path.join(out_dir, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            crop.save(p, format="PNG", optimize=False, compress_level=9)
        with open(os.path.join(out_dir, "manifest.json"), "w", newline="\n") as f:
            json.dump(manifest, f, indent=1, sort_keys=True)
            f.write("\n")
    return manifest, files, skipped


def verify(install, out_dir, manifest):
    """Re-pack every widget onto a magenta canvas and compare covered pixels with the source sheet."""
    art_dir = os.path.join(install, "data", "art", "gui")
    bad = 0
    for key, sh in manifest["sheets"].items():
        src = Image.open(find_sheet(art_dir, key)).convert("RGBA")
        canvas = Image.new("RGBA", src.size, (255, 0, 255, 255))
        for name, ent in sh["widgets"].items():
            w = Image.open(os.path.join(out_dir, key, name + ".png")).convert("RGBA")
            canvas.paste(w, (ent["rect"][0], ent["rect"][1]))
        cov = Image.new("L", src.size, 0)
        for ent in sh["widgets"].values():
            x, y, w, h = ent["rect"]
            cov.paste(255, (x, y, x + w, y + h))
        a, b, m = src.load(), canvas.load(), cov.load()
        for yy in range(src.size[1]):
            for xx in range(src.size[0]):
                if m[xx, yy] and a[xx, yy] != b[xx, yy]:
                    bad += 1
    return bad


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--install", required=True)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "data", "art", "ui"))
    ap.add_argument("--check", action="store_true", help="do not write; print the inventory")
    a = ap.parse_args()
    out = os.path.normpath(a.out)
    manifest, files, skipped = build(a.install, out, not a.check)
    tot = 0
    print(f"{'sheet':28} {'size':>9} {'rects':>5} {'nine':>4}  screens")
    for key, sh in manifest["sheets"].items():
        nn = sum(1 for e in sh["widgets"].values() if "nine" in e)
        tot += sh["rects"]
        print(f"{key:28} {sh['size'][0]}x{sh['size'][1]:<5} {sh['rects']:>5} {nn:>4}  {','.join(sh['screens'])[:100]}")
    print(f"sheets={len(manifest['sheets'])} widgets={tot} skipped={len(skipped)}")
    for k, why in skipped:
        print(f"  skipped {k}: {why}")
    if not a.check:
        bad = verify(a.install, out, manifest)
        print(f"verify: {bad} differing covered pixels")
        return 1 if bad else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
