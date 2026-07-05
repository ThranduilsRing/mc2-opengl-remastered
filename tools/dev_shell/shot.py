#!/usr/bin/env python3
"""shot.py — screenshot post-processing CLI for dev-shell captures.

Replaces the inline PIL one-liners used during UI work:

  py -3 tools/dev_shell/shot.py png  shot.tga [-o out.png] [--thumb 800]
  py -3 tools/dev_shell/shot.py crop shot.tga X Y W H [-o out.png]
  py -3 tools/dev_shell/shot.py diff a.tga b.tga [-o diff.png] [--thresh 8]

png   : convert TGA (or any PIL-readable) to PNG, optional max-width thumbnail.
crop  : crop region, save PNG.
diff  : per-pixel abs diff of two same-size images; prints changed-pixel count,
        percentage, and bounding box of changes; optionally writes a diff
        visualization PNG (changes in red over dimmed base).

Exit codes: diff exits 1 if any pixel differs above threshold (usable as a gate).
"""
import argparse
import os
import sys

try:
    from PIL import Image, ImageChops
except ImportError:
    sys.exit("shot.py needs Pillow: py -3 -m pip install Pillow")


def load(path):
    if not os.path.isfile(path):
        sys.exit(f"no such file: {path}")
    return Image.open(path).convert("RGB")


def out_path(inp, override, suffix):
    if override:
        return override
    base, _ = os.path.splitext(inp)
    return base + suffix


def cmd_png(a):
    img = load(a.image)
    if a.thumb and img.width > a.thumb:
        h = round(img.height * a.thumb / img.width)
        img = img.resize((a.thumb, h), Image.LANCZOS)
    dst = out_path(a.image, a.out, ".png")
    img.save(dst)
    img.close()
    print(f"{dst} {img.width}x{img.height}")
    if not a.keep and a.image.lower().endswith(".tga") \
            and os.path.abspath(dst) != os.path.abspath(a.image):
        os.remove(a.image)
        print(f"removed {a.image}")


def cmd_crop(a):
    img = load(a.image)
    box = (a.x, a.y, a.x + a.w, a.y + a.h)
    if box[2] > img.width or box[3] > img.height:
        sys.exit(f"crop {box} exceeds image {img.width}x{img.height}")
    dst = out_path(a.image, a.out, f"_crop_{a.x}_{a.y}_{a.w}x{a.h}.png")
    img.crop(box).save(dst)
    print(f"{dst} {a.w}x{a.h}")


def cmd_diff(a):
    ia, ib = load(a.a), load(a.b)
    if ia.size != ib.size:
        sys.exit(f"size mismatch: {ia.size} vs {ib.size}")
    d = ImageChops.difference(ia, ib)
    # changed = any channel diff > thresh
    mask = d.convert("L").point(lambda v: 255 if v > a.thresh else 0)
    bbox = mask.getbbox()
    changed = sum(1 for v in mask.getdata() if v)
    total = ia.width * ia.height
    pct = 100.0 * changed / total
    print(f"changed={changed}/{total} ({pct:.4f}%) thresh={a.thresh} bbox={bbox}")
    if a.out:
        vis = Image.blend(ia, Image.new("RGB", ia.size), 0.6)
        red = Image.new("RGB", ia.size, (255, 0, 0))
        vis.paste(red, mask=mask)
        vis.save(a.out)
        print(f"wrote {a.out}")
    sys.exit(1 if changed else 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("png", help="TGA -> PNG (+ optional thumbnail)")
    p.add_argument("image")
    p.add_argument("-o", "--out")
    p.add_argument("--thumb", type=int, default=0, help="max width; 0 = full size")
    p.add_argument("--keep", action="store_true",
                   help="keep the source .tga (default: delete after convert)")
    p.set_defaults(fn=cmd_png)

    p = sub.add_parser("crop", help="crop region -> PNG")
    p.add_argument("image")
    p.add_argument("x", type=int)
    p.add_argument("y", type=int)
    p.add_argument("w", type=int)
    p.add_argument("h", type=int)
    p.add_argument("-o", "--out")
    p.set_defaults(fn=cmd_crop)

    p = sub.add_parser("diff", help="pixel diff + bbox (exit 1 if differs)")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("-o", "--out", help="write red-overlay diff PNG")
    p.add_argument("--thresh", type=int, default=8, help="per-channel threshold (default 8)")
    p.set_defaults(fn=cmd_diff)

    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
