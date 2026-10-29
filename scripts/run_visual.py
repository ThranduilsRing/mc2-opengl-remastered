#!/usr/bin/env python3
"""Phase A visual regression. Goldens and reports live outside git.

Create a new golden: --install DIR --record-golden
Capture and compare: --install DIR --out DIR
Reuse captures: --compare-only DIR --out DIR
Exit 0 = no changed regions, 1 = changed regions, 2 = invalid/missing evidence.
The schema contract never captures or compares. No merge-gate integration.
"""
import argparse
import hashlib
import html
import json
import math
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CONFIG = Path(__file__).with_name("visual_regions.json")
GOLDEN = Path("E:/mc2-archive-2026-10/golden/visual-a")


def validate_schema(doc):
    if doc.get("schema_v") != 1:
        raise ValueError("schema_v must be 1")
    tile = doc.get("tile_px")
    if type(tile) is not int or not 1 <= tile <= 256:
        raise ValueError("tile_px must be 1..256")
    regions = doc.get("regions", [])
    if not regions or len({r["name"] for r in regions}) != len(regions):
        raise ValueError("regions need unique names")
    for region in regions:
        rect = region["rect"]
        if (len(rect) != 4 or any(type(v) not in (int, float) or not math.isfinite(v) for v in rect)
                or not 0 <= rect[0] < rect[2] <= 1 or not 0 <= rect[1] < rect[3] <= 1):
            raise ValueError("invalid normalized region rect")
        for key, upper in (("channel_delta", 255), ("changed_fraction", 1)):
            value = region[key]
            if type(value) not in (int, float) or not math.isfinite(value) or not 0 <= value < upper:
                raise ValueError(f"invalid {key}")
    # The tolerance map must cover the whole frame, without ignored gaps/overlaps.
    xs = sorted({0, 1, *(r["rect"][i] for r in regions for i in (0, 2))})
    ys = sorted({0, 1, *(r["rect"][i] for r in regions for i in (1, 3))})
    for x0, x1 in zip(xs, xs[1:]):
        for y0, y1 in zip(ys, ys[1:]):
            x, y = (x0 + x1) / 2, (y0 + y1) / 2
            if sum(r["rect"][0] <= x < r["rect"][2] and r["rect"][1] <= y < r["rect"][3]
                   for r in regions) != 1:
                raise ValueError("regions must partition the frame")
    scenes = doc.get("scenes", [])
    if {s["name"] for s in scenes} != {"day", "night", "base", "battle", "mech"} or len(scenes) != 5:
        raise ValueError("five fixed scenes required")
    for scene in scenes:
        path = ROOT / "tests/visual/bookmarks" / scene["bookmarks"]
        if path.parent != ROOT / "tests/visual/bookmarks":
            raise ValueError("bookmark must be a filename")
        bookmark = json.loads(path.read_text(encoding="utf-8"))
        if bookmark["mission"] != scene["mission"] or scene["pose"] not in {b["name"] for b in bookmark["bookmarks"]}:
            raise ValueError("scene mission/pose missing from bookmark")


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(install, out, doc):
    out.mkdir(parents=True, exist_ok=False)
    for scene in doc["scenes"]:
        dest = out / scene["name"]
        dest.mkdir()
        source = ROOT / "tests/visual/bookmarks" / scene["bookmarks"]
        bookmark = json.loads(source.read_text(encoding="utf-8"))
        pose = next(b for b in bookmark["bookmarks"] if b["name"] == scene["pose"])
        # The engine requires two poses; duplicate the fixed pose, not the entire sweep.
        bookmark["bookmarks"] = [pose, dict(pose, name=pose["name"] + "_dup")]
        fixed = dest / "bookmark.json"
        fixed.write_text(json.dumps(bookmark, indent=2) + "\n", encoding="utf-8")
        command = [sys.executable, str(ROOT / "scripts/run_visual_capture.py"),
                   "--exe", str(install / "mc2.exe"), "--mission", scene["mission"],
                   "--bookmarks", str(fixed), "--out-dir", str(dest),
                   "--trigger-frame", "240", "--settle", "30", "--no-kill"]
        with (dest / "runner.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            raise ValueError(f"capture failed: {dest / 'runner.log'} (rc={result.returncode})")
        print(f"captured {scene['name']}", flush=True)
    (out / "set.json").write_text(json.dumps({"schema_v": 1, "config": doc,
        "exe_sha256": digest(install / "mc2.exe")}, indent=2) + "\n", encoding="utf-8")


def evidence(directory, scene):
    path = directory / scene["name"] / f"{scene['mission']}_{scene['pose']}.png"
    side = json.loads(path.with_suffix(".json").read_text(encoding="utf-8"))
    if side.get("deterministic") is not True or side.get("backend") != "vulkan":
        raise ValueError(f"non-deterministic/non-Vulkan evidence: {path}")
    return path, side


def changed_boxes(a, b, doc):
    """Local tile occupancy catches small missing objects hidden by whole-frame MAD."""
    import numpy as np
    if a.size != b.size:
        raise ValueError(f"capture dimensions differ: {a.size} vs {b.size}")
    delta = np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16)).max(axis=2)
    width, height = a.size
    boxes = []
    for region in doc["regions"]:
        x0, y0, x1, y1 = [round(v * (width if i % 2 == 0 else height)) for i, v in enumerate(region["rect"])]
        step = doc["tile_px"]
        for y in range(y0, y1, step):
            for x in range(x0, x1, step):
                rect = [x, y, min(x + step, x1), min(y + step, y1)]
                fraction = float((delta[y:rect[3], x:rect[2]] > region["channel_delta"]).mean())
                if fraction > region["changed_fraction"]:
                    boxes.append({"region": region["name"], "rect": rect, "changed_fraction": fraction})
    return boxes


def compare(golden, candidate, out, doc):
    from PIL import Image, ImageDraw
    import numpy as np
    # Both arms must use the same capture recipe and tolerance map.
    for directory in (golden, candidate):
        if json.loads((directory / "set.json").read_text(encoding="utf-8"))["config"] != doc:
            raise ValueError(f"capture recipe differs: {directory}")
    out.mkdir(parents=True, exist_ok=True)
    rows, cards = [], []
    for scene in doc["scenes"]:
        ap, aside = evidence(golden, scene)
        bp, bside = evidence(candidate, scene)
        for key in ("cap_pos", "cap_rot", "cap_proj", "cap_alt", "width", "height",
                    "trigger_frame", "seed", "render_options"):
            if aside.get(key) != bside.get(key):
                raise ValueError(f"incompatible {key} in {scene['name']} sidecars")
        with Image.open(ap) as src:
            a = src.convert("RGB")
        with Image.open(bp) as src:
            b = src.convert("RGB")
        boxes = changed_boxes(a, b, doc)
        mad = float(np.abs(np.asarray(a, dtype=np.int16) - np.asarray(b, dtype=np.int16)).mean())
        rows.append({"scene": scene["name"], "mad": mad, "boxes": boxes,
                     "golden_build": aside.get("build"), "candidate_build": bside.get("build"),
                     "golden_sim_frame": aside.get("sim_frame"), "candidate_sim_frame": bside.get("sim_frame"),
                     "golden_sha256": digest(ap), "candidate_sha256": digest(bp)})
        draw = ImageDraw.Draw(b)
        for box in boxes:
            draw.rectangle(box["rect"], outline="#ff3030", width=3)
        montage = Image.new("RGB", (a.width * 2, a.height))
        montage.paste(a, (0, 0))
        montage.paste(b, (a.width, 0))
        name = scene["name"] + ".png"
        montage.save(out / name)
        cards.append(f'<article><h2>{html.escape(scene["name"])}: {len(boxes)} changed regions</h2>'
                     f'<p>Golden (left) / candidate (right, red boxes). MAD {mad:.4f} (informational).</p>'
                     f'<a href="{name}"><img src="{name}" alt="{scene["name"]} comparison"></a></article>')
    changed = any(row["boxes"] for row in rows)
    report = {"schema_v": 1, "status": "CHANGED" if changed else "CLEAN", "scenes": rows,
              "golden": str(golden), "candidate": str(candidate), "tolerances": doc["regions"]}
    (out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (out / "index.html").write_text('<!doctype html><html lang="en"><meta charset="utf-8">'
        '<meta name="viewport" content="width=device-width,initial-scale=1"><title>MC2 visual regression</title>'
        '<style>body{margin:auto;padding:12px;max-width:1100px;background:#181a20;color:#eee;'
        'font:16px system-ui}img{width:100%;height:auto}article{margin:24px 0}a{color:#9cf}</style>'
        f'<h1>Visual Phase A: {report["status"]}</h1><p>Local region tolerances decide the result. '
        'Tap a montage to inspect at full resolution. <a href="report.json">Box coordinates and tolerances</a>.</p>'
        + ''.join(cards) + '</html>', encoding="utf-8")
    print(f"{report['status']}: {sum(len(r['boxes']) for r in rows)} changed regions; {out / 'index.html'}")
    return int(changed)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate-schema", action="store_true")
    parser.add_argument("--config", type=Path, default=CONFIG)
    parser.add_argument("--golden", type=Path, default=GOLDEN)
    source = parser.add_mutually_exclusive_group()
    source.add_argument("--install", type=Path)
    source.add_argument("--compare-only", type=Path)
    parser.add_argument("--record-golden", action="store_true")
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    try:
        doc = json.loads(args.config.read_text(encoding="utf-8"))
        validate_schema(doc)
        if args.validate_schema:
            print("visual regression schema OK (no capture/comparison)")
            return 0
        if args.record_golden:
            if not args.install or args.compare_only or args.golden.exists():
                raise ValueError("record needs --install and a NEW golden directory")
            capture(args.install.resolve(), args.golden.resolve(), doc)
            for scene in doc["scenes"]:
                evidence(args.golden, scene)
            return 0
        if not args.out or not (args.install or args.compare_only):
            raise ValueError("comparison needs --out and --install or --compare-only")
        candidate = args.compare_only
        if args.install:
            candidate = args.out.resolve() / "capture"
            capture(args.install.resolve(), candidate, doc)
        return compare(args.golden.resolve(), candidate.resolve(), args.out.resolve(), doc)
    except (ValueError, OSError, KeyError, TypeError) as exc:
        print(f"visual regression ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
