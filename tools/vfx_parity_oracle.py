"""Report DIAGNOSTIC repeatability or proven four-family PARITY.

Manifest: {"effects": [{"class": "missile", "rect": [x0,y0,x1,y1],
"images": {"mlr": ["a.png","b.png"], "gpu": ["c.png","d.png"]}}]}.
Paths are relative to the manifest. Rectangles are half-open pixel bounds.
This tool does not launch an engine or infer that an effect is present.
PARITY additionally requires an evidence object documented in the Q2 report.
Exit zero is reserved for PARITY across missile/LRM trail, PPC, explosion, smoke.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct

from PIL import Image, ImageChops, ImageDraw


FAMILIES = {"missile", "ppc", "explosion", "smoke"}
PIXEL_THRESHOLD = 8
MIN_CHANGED_PIXELS = 16


def region(image: Image.Image, rect: list[int]) -> Image.Image:
    if (len(rect) != 4 or any(type(v) is not int for v in rect)
            or not 0 <= rect[0] < rect[2] <= image.width
            or not 0 <= rect[1] < rect[3] <= image.height):
        raise ValueError(f"invalid crop {rect} for {image.size}")
    return image.convert("RGBA").crop(tuple(rect))


def digest(image: Image.Image) -> str:
    rgba = image.convert("RGBA")
    return hashlib.sha256(struct.pack("<II", *rgba.size) + rgba.tobytes()).hexdigest()


def source_paths(base: Path, images: dict) -> list[Path]:
    paths = []
    for arm in ("mlr", "gpu"):
        if len(images[arm]) != 2:
            raise ValueError(f"{arm}: two repeats required")
        paths.extend((base / path).resolve() for path in images[arm])
    if len(set(paths)) != 4:
        raise ValueError("four distinct capture paths required; an image cannot be its own repeat control")
    return paths


def compare(crops: list[Image.Image]) -> dict:
    if len(crops) != 4 or len({im.size for im in crops}) != 1:
        raise ValueError("four equal-sized crops required (MLR1, MLR2, GPU1, GPU2)")
    hashes = [digest(im) for im in crops]
    return {"mlr": hashes[:2], "gpu": hashes[2:],
            "repeatable": hashes[0] == hashes[1] and hashes[2] == hashes[3],
            "cross_arm_equal": hashes[0] == hashes[2] and hashes[1] == hashes[3]}


def changed_pixels(image: Image.Image, control: Image.Image) -> int:
    if image.size != control.size:
        raise ValueError("effect and control resolutions differ")
    difference = ImageChops.difference(image.convert("RGB"), control.convert("RGB"))
    channels = [channel.tobytes() for channel in difference.split()]
    return sum(max(rgb) > PIXEL_THRESHOLD for rgb in zip(*channels))


def provenance(record: dict) -> tuple:
    if not isinstance(record, dict):
        raise ValueError("capture provenance must be an object")
    fingerprint = record.get("fingerprint", "")
    bookmark, frame = record.get("bookmark"), record.get("frame")
    if (not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9a-fA-F]{12,64}", fingerprint)
            or not isinstance(bookmark, str) or not bookmark.strip()
            or type(frame) is not int or frame < 0):
        raise ValueError("capture provenance requires fingerprint, bookmark and frame")
    return fingerprint.lower(), bookmark, frame


def acceptance(effect: dict, crops: list[Image.Image], base: Path,
               source_size: tuple[int, int]) -> dict:
    comparison = compare(crops)
    failures = []
    if not comparison["repeatable"]:
        failures.append("same-arm repeats differ")
    if not comparison["cross_arm_equal"]:
        failures.append("MLR/GPU pixels differ")
    evidence = effect.get("evidence")
    checks = {}
    if evidence is None:
        failures.append("family demonstration, provenance, effect-free and suppressed controls absent")
    else:
        try:
            paths = source_paths(base, effect["images"])
            stamps = []
            primary_counts, negative_counts = {}, {}
            for arm, offset in (("mlr", 0), ("gpu", 2)):
                records = evidence["captures"][arm]
                if len(records) != 2:
                    raise ValueError(f"{arm}: provenance for two repeats required")
                for record in records:
                    stamps.append(provenance(record))
                    demonstration = record.get("demonstration")
                    if not isinstance(demonstration, str) or not demonstration.strip():
                        raise ValueError(f"{arm}: visible {effect['class']} demonstration required")
                controls = []
                for kind in ("effect_free", "suppressed"):
                    record = evidence[kind][arm]
                    stamps.append(provenance(record))
                    path = (base / record["image"]).resolve()
                    if path in paths:
                        raise ValueError("effect-free and suppressed controls require distinct capture paths")
                    paths.append(path)
                    with Image.open(path) as im:
                        if im.size != source_size:
                            raise ValueError("control capture resolutions differ")
                        controls.append(region(im, effect["rect"]))
                if evidence["suppressed"][arm].get("effect_suppressed") is not True:
                    raise ValueError(f"{arm}: negative control must document effect suppression")
                primary_counts[arm] = [changed_pixels(im, controls[0])
                                       for im in crops[offset:offset + 2]]
                negative_counts[arm] = changed_pixels(controls[1], controls[0])
            if len(set(stamps)) != 1:
                raise ValueError("capture/control fingerprint, bookmark or frame differs")
            checks["effect_region_changed_pixels"] = primary_counts
            checks["negative_control"] = {
                "changed_pixels": negative_counts,
                "verdict": "FAIL" if all(n <= MIN_CHANGED_PIXELS for n in negative_counts.values()) else "PASS",
            }
            if any(n <= MIN_CHANGED_PIXELS for counts in primary_counts.values() for n in counts):
                failures.append("empty effect region: effect-free difference below presence threshold")
            if checks["negative_control"]["verdict"] != "FAIL":
                failures.append("suppressed negative control did not FAIL effect presence")
        except (KeyError, TypeError, ValueError, OSError) as error:
            failures.append(f"invalid parity evidence: {error}")
    return {**comparison, "status": "DIAGNOSTIC" if failures else "PARITY",
            "parity_accepted": not failures, "parity_failures": failures, **checks}


def summarize(rows: list[dict]) -> dict:
    missing = sorted(FAMILIES - {row["class"] for row in rows})
    accepted = not missing and bool(rows) and all(row["parity_accepted"] for row in rows)
    return {"status": "PARITY" if accepted else "DIAGNOSTIC",
            "parity_accepted": accepted, "missing_families": missing}


def montage(crops: list[Image.Image]) -> Image.Image:
    compare(crops)
    width, height = crops[0].size
    panels = crops + [ImageChops.difference(crops[0].convert("RGB"),
                                           crops[2].convert("RGB"))]
    labels = ["GL MLR repeat 1", "GL MLR repeat 2", "GL GPU repeat 1",
              "GL GPU repeat 2", "Absolute RGB difference MLR1 / GPU1"]
    sheet = Image.new("RGB", (width * 3, (height + 24) * 2), "#202020")
    draw = ImageDraw.Draw(sheet)
    for i, (panel, label) in enumerate(zip(panels, labels)):
        x, y = (i % 3) * width, (i // 3) * (height + 24)
        draw.text((x + 5, y + 5), label, fill="white")
        sheet.paste(panel.convert("RGB"), (x, y + 24))
    return sheet


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    doc = json.loads(args.manifest.read_text(encoding="utf-8"))
    args.out.mkdir(parents=True, exist_ok=True)
    rows = []
    seen = set()
    for effect in doc["effects"]:
        name = effect["class"]
        if name not in FAMILIES or name in seen:
            raise ValueError(f"unknown or duplicate effect class {name!r}")
        seen.add(name)
        crops = []
        source_size = None
        for path in source_paths(args.manifest.parent, effect["images"]):
            with Image.open(path) as im:
                if source_size is not None and source_size != im.size:
                    raise ValueError("capture resolutions differ")
                source_size = im.size
                crops.append(region(im, effect["rect"]))
        row = {"class": name, "rect": effect["rect"],
               **acceptance(effect, crops, args.manifest.parent, source_size),
               "images": effect["images"], "montage": f"{name}.png"}
        if "evidence" in effect:
            row["evidence"] = effect["evidence"]
        montage(crops).save(args.out / row["montage"])
        rows.append(row)
        print(json.dumps(row))
    if not rows:
        raise ValueError("no effect regions")
    result = {"digest_method": "SHA256(LE u32 width, LE u32 height, row-major RGBA8)",
              **summarize(rows), "pixel_threshold": PIXEL_THRESHOLD,
              "min_changed_pixels": MIN_CHANGED_PIXELS,
              "manifest": str(args.manifest.resolve()), "effects": rows}
    (args.out / "digests.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"{result['status']}: parity_accepted={result['parity_accepted']}; "
          f"missing_families={result['missing_families']}")
    return 0 if result["parity_accepted"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
