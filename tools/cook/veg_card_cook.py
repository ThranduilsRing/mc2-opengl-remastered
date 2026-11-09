"""Cook vegetation cards to BC7 with stable cutout coverage and trimmed strips.

The eight slots retain their existing order. Each KTX stores authored mip levels;
the runtime interleaves their blocks into a texture array, without atlas borders.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))
import console_child

CARDS = [f"grass_{i:02d}" for i in range(1, 5)] + ["bush_round", "bush_heath", "sapling_leaf", "sapling_pine"]


def procedural_woody_cards(source: Path, sheet: Path) -> None:
    """Original seeded branch/leaf silhouettes; no source photography or licences.

    Four species share the existing eight-layer BC7 array with the grass.
    Runtime ground colour supplies biome/season hues without extra textures.
    """
    import random
    import math
    from PIL import ImageDraw

    source.mkdir(parents=True, exist_ok=True)
    samples = []
    for species, name in enumerate(CARDS[4:]):
        rng = random.Random(20261108 + species)
        image = Image.new("RGBA", (512, 512))
        draw = ImageDraw.Draw(image)
        tree = species >= 2
        base = (256, 504)
        top = (250, 50 if tree else 230)
        draw.line([base, top], fill=(90, 72, 43, 255), width=9 if tree else 5)
        for branch in range(18 if tree else 28):
            t = 0.28 + 0.70*(branch + 1) / (19 if tree else 29)
            y = base[1] + (top[1] - base[1])*t
            side = -1 if branch % 2 else 1
            span = (1-t)*250+15 if species == 3 else 190*math.sin(t*math.pi)
            if not tree:
                span = 65 + rng.random()*155
                y = 455 - rng.random()*155
            end = (256+side*span, y-25-rng.random()*60)
            draw.line([(256,y), end], fill=(94,80,48,255), width=4 if tree else 3)
            for leaf in range(100 if tree else 110):
                u = 0.20 + rng.random()*0.8
                px = 256+(end[0]-256)*u+rng.gauss(0,20)
                py = y+(end[1]-y)*u+rng.gauss(0,14 if species == 3 else 25)
                r = rng.uniform(5,12) if species != 1 else rng.uniform(3,8)
                tone = rng.uniform(0.7,1.25)
                colour = tuple(int(c*tone) for c in ((69,105,43) if species != 1 else (102,111,62)))+(255,)
                draw.ellipse((px-r,py-r*0.65,px+r,py+r*0.65), fill=colour)
        # Scale shrubs to the full card height while preserving the root.
        if not tree:
            bounds = image.getbbox()
            image = image.crop((0,bounds[1],512,512)).resize((512,512), Image.Resampling.LANCZOS)
        image.save(source / f"{name}.png")
        preview = Image.new("RGB", (512, 552), (48, 53, 44))
        preview.paste(image, (0,0), image)
        ImageDraw.Draw(preview).text((15,525), name, fill=(240,240,220))
        samples.append(preview)
    result = Image.new("RGB", (1024,1104))
    for i, sample in enumerate(samples):
        result.paste(sample, ((i%2)*512,(i//2)*552))
    sheet.parent.mkdir(parents=True, exist_ok=True)
    result.save(sheet)


def preserve_coverage(alpha: np.ndarray, target: float) -> np.ndarray:
    """Choose the alpha scale with closest representable coverage at cutoff 128.

    >>> a = np.array([[64, 96], [32, 0]], dtype=np.uint8)
    >>> int((preserve_coverage(a, 0.5) >= 128).sum())
    2
    >>> int(preserve_coverage(np.zeros((2, 2), dtype=np.uint8), 0.5).sum())
    0
    """
    lo, hi = 0.0, 256.0
    best = alpha.copy()
    error = abs(float((best >= 128).mean()) - target)
    for _ in range(24):
        scale = (lo + hi) * 0.5
        candidate = np.clip(np.rint(alpha.astype(np.float32) * scale), 0, 255).astype(np.uint8)
        coverage = float((candidate >= 128).mean())
        if abs(coverage - target) < error:
            best, error = candidate, abs(coverage - target)
        if coverage < target:
            lo = scale
        else:
            hi = scale
    return best


def dilate_rgb(rgba: np.ndarray, steps: int = 8) -> np.ndarray:
    """Extend edge colours into transparent texels, keeping coverage unchanged."""
    result = rgba.copy()
    known = rgba[:, :, 3] > 0
    for _ in range(steps):
        colours = np.zeros_like(result[:, :, :3], dtype=np.uint32)
        count = np.zeros_like(known, dtype=np.uint32)
        padded = np.pad(result[:, :, :3], ((1, 1), (1, 1), (0, 0)))
        mask = np.pad(known, 1)
        h, w = known.shape
        for dy, dx in ((0, 1), (1, 0), (1, 2), (2, 1)):
            neighbour = mask[dy:dy+h, dx:dx+w]
            count += neighbour
            colours += padded[dy:dy+h, dx:dx+w].astype(np.uint32) * neighbour[:, :, None]
        fill = (~known) & (count > 0)
        result[fill, :3] = (colours[fill] // count[fill, None]).astype(np.uint8)
        known |= fill
    return result


def mip_chain(image: Image.Image) -> list[Image.Image]:
    rgba = np.array(image.convert("RGBA"))
    target = float((rgba[:, :, 3] >= 128).mean())
    chain = [Image.fromarray(dilate_rgb(rgba))]
    while chain[-1].width > 1 or chain[-1].height > 1:
        size = (max(1, chain[-1].width // 2), max(1, chain[-1].height // 2))
        # Keep RGB unassociated: Pillow's RGBA resize premultiplies and loses
        # the dilated edge colours needed by anisotropic filtering.
        channels = [ch.resize(size, Image.Resampling.BOX) for ch in chain[-1].split()]
        rgba = np.array(Image.merge("RGBA", channels))
        rgba[:, :, 3] = preserve_coverage(rgba[:, :, 3], target)
        chain.append(Image.fromarray(dilate_rgb(rgba)))
    return chain


def trim_strip(image: Image.Image) -> list[list[float]]:
    """Conservative 8-vertex strip: each row bounds both neighbouring bands."""
    alpha = np.array(image.getchannel("A"))
    h, w = alpha.shape
    rows = []
    for row in range(4):
        y0 = max(0, int((row-1) * h / 3) - 2)
        y1 = min(h, int((row+1) * h / 3) + 3)
        _, xs = np.where(alpha[y0:y1] >= 128)
        left = max(0, int(xs.min()) - 2) / w if xs.size else 0.5
        right = min(w, int(xs.max()) + 3) / w if xs.size else 0.5
        rows.extend([[left, row / 3], [right, row / 3]])
    return rows


def cook(source: Path, output: Path, ktx: Path, trim_output: Path | None = None) -> None:
    output.mkdir(parents=True, exist_ok=True)
    manifest = []
    with tempfile.TemporaryDirectory(prefix="veg-card-") as tmp:
        tmp = Path(tmp)
        for name in CARDS:
            image = Image.open(source / f"{name}.png").convert("RGBA").resize((512, 512), Image.Resampling.LANCZOS)
            chain = mip_chain(image)
            inputs = []
            for level, mip in enumerate(chain):
                path = tmp / f"{name}-{level}.png"
                mip.save(path)
                inputs.append(str(path))
            intermediate = tmp / f"{name}.ktx2"
            commands = [
                [str(ktx), "create", "--format", "R8G8B8A8_UNORM", "--assign-tf", "linear",
                 "--encode", "uastc", "--levels", str(len(chain)), *inputs, str(intermediate)],
                [str(ktx), "transcode", "--target", "bc7", str(intermediate), str(output / f"{name}.ktx2")],
            ]
            for command in commands:
                subprocess.run(command, check=True, capture_output=True, **console_child.child_kwargs(redirected=True))
            manifest.append({"name": name, "strip": trim_strip(image),
                             "coverage": [float((np.array(m.getchannel("A")) >= 128).mean()) for m in chain]})
    (output / "cards.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    trim_output = trim_output or output / "vegetation_card_trim.glsl"
    vertices = [uv for card in manifest for uv in card["strip"]]
    text = "// Generated by tools/cook/veg_card_cook.py; top-to-bottom texture coordinates.\n"
    text += "const vec2 kVegCardTrim[64] = vec2[](\n"
    text += ",\n".join(f"    vec2({u:.8f}, {v:.8f})" for u, v in vertices) + ");\n"
    trim_output.parent.mkdir(parents=True, exist_ok=True)
    trim_output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT / "data/vegetation/cards")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--ktx", type=Path, default=Path("A:/Games/mc2-tools/ktx/ktx.exe"))
    parser.add_argument("--trim-output", type=Path)
    args = parser.parse_args()
    cook(args.source, args.output, args.ktx, args.trim_output)
