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


def hex_rgb(value: str) -> tuple[int, int, int]:
    return tuple(int(value[i:i+2], 16) for i in (1, 3, 5))


def ramp_colour(ramp: list[str], t: float) -> tuple[int, int, int]:
    """Linear interpolation along a dark-to-light list of palette swatches."""
    t = min(max(t, 0.0), 1.0) * (len(ramp) - 1)
    i = min(int(t), len(ramp) - 2)
    a, b = hex_rgb(ramp[i]), hex_rgb(ramp[i + 1])
    f = t - i
    return tuple(int(round(a[k] + (b[k] - a[k]) * f)) for k in range(3))


def load_palette(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def procedural_woody_cards(source: Path, sheet: Path, palette: dict) -> None:
    """Original seeded branch/leaf silhouettes; no source photography or licences.

    Four species share the existing eight-layer BC7 array with the grass. Leaves
    are painted from the mission palette's per-species ramps (dark at the inner
    and lower crown, light at the sunlit top) so each species keeps its own hue
    and value; the runtime only sets exposure from the ground.
    """
    import random
    import math
    from PIL import ImageDraw

    source.mkdir(parents=True, exist_ok=True)
    samples = []
    bark = hex_rgb(palette["bark"])
    for species, name in enumerate(CARDS[4:]):
        rng = random.Random(20261108 + species)
        ramp = palette["species"][name]
        image = Image.new("RGBA", (512, 512))
        draw = ImageDraw.Draw(image)
        tree = species >= 2
        base = (256, 504)
        top = (250, 50 if tree else 230)
        draw.line([base, top], fill=bark + (255,), width=9 if tree else 5)
        for branch in range(18 if tree else 28):
            t = 0.28 + 0.70*(branch + 1) / (19 if tree else 29)
            y = base[1] + (top[1] - base[1])*t
            side = -1 if branch % 2 else 1
            # Ragged tiers instead of a clean cone for the conifer.
            span = ((1-t)*250+15)*(0.6+0.55*rng.random()) if species == 3 else 190*math.sin(t*math.pi)
            if not tree:
                span = 65 + rng.random()*155
                y = 455 - rng.random()*155
            end = (256+side*span, y-25-rng.random()*60)
            draw.line([(256,y), end], fill=bark + (255,), width=4 if tree else 3)
            for leaf in range(100 if tree else 110):
                u = 0.20 + rng.random()*0.8
                px = 256+(end[0]-256)*u+rng.gauss(0,20)
                py = y+(end[1]-y)*u+rng.gauss(0,14 if species == 3 else 25)
                r = rng.uniform(5,12) if species != 1 else rng.uniform(3,8)
                light = 0.55*(1.0 - py/512.0) + 0.35*u + rng.uniform(-0.25, 0.25)
                draw.ellipse((px-r,py-r*0.65,px+r,py+r*0.65), fill=ramp_colour(ramp, light)+(255,))
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


def mix(a: tuple, b: tuple, t: float) -> tuple[int, int, int]:
    return tuple(int(round(a[k] + (b[k] - a[k]) * t)) for k in range(3))


def procedural_grass_cards(source: Path, palette: dict) -> None:
    """Original seeded blade fields (owner grass-field reference, 2026-11-10).

    Long thin tapering blades, each with its own tone: olive at the root, the
    blade's straw/green/rust colour up the stem and a light backlit tip; some
    blades carry seed heads. Per-card weights come from the palette's
    `grass_cards`; tones come from `grass_tones`. Drawn at 2x and downsampled.
    """
    import math
    import random
    from PIL import ImageDraw

    tones = {k: [hex_rgb(c) for c in v] if isinstance(v, list) else hex_rgb(v) for k, v in palette["grass_tones"].items()}
    for index, name in enumerate(CARDS[:4]):
        spec = palette["grass_cards"][name]
        rng = random.Random(20261110 + index)
        size = 1024
        image = Image.new("RGBA", (size, size))
        draw = ImageDraw.Draw(image)
        kinds, weights = zip(*spec["mix"].items())
        for _ in range(spec["blades"]):
            kind = rng.choices(kinds, weights)[0]
            colour = rng.choice(tones[kind])
            colour = mix(colour, (0, 0, 0), rng.uniform(0.0, 0.25))
            x = size / 2 + rng.gauss(0, size * 0.20)
            length = size * rng.uniform(0.45, 0.97)
            lean = rng.gauss(0, 0.35) + spec.get("lean", 0.0)
            width = rng.uniform(5, 11)
            steps = 14
            points = []
            for s in range(steps + 1):
                t = s / steps
                points.append((x + lean * length * t * t * 0.6 + math.sin(t * 3 + x) * 4, size - 2 - length * t * (1 - 0.15 * abs(lean) * t)))
            for s in range(steps):
                t = (s + 0.5) / steps
                c = mix(tones["root"], colour, min(1.0, t / 0.35))
                if t > 0.8:
                    c = mix(c, tones["tip"], (t - 0.8) / 0.2 * 0.7)
                draw.line([points[s], points[s + 1]], fill=c + (255,), width=max(2, int(round(width * (1 - t) + 2))))
            if rng.random() < spec["seed_heads"]:
                head = rng.choice(tones["seed"])
                tx, ty = points[-1]
                for k in range(7):
                    hx = tx + rng.gauss(0, 5) + lean * k * 3
                    hy = ty + k * 9
                    draw.ellipse((hx - 4, hy - 7, hx + 4, hy + 7), fill=head + (255,))
        image.resize((512, 512), Image.Resampling.LANCZOS).save(source / f"{name}.png")


def paint_cards(source: Path, palette_path: Path, sheet: Path) -> None:
    """Repaint all eight shipped cards into a mission palette (grass blades and woody cards regenerated)."""
    palette = load_palette(palette_path)
    procedural_grass_cards(source, palette)
    procedural_woody_cards(source, sheet, palette)


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
    parser.add_argument("--paint", type=Path, help="repaint the source cards into this palette JSON first")
    parser.add_argument("--sheet", type=Path, help="woody contact sheet written by --paint")
    args = parser.parse_args()
    if args.paint:
        paint_cards(args.source, args.paint, args.sheet or Path(tempfile.gettempdir()) / "veg_woody_sheet.png")
    cook(args.source, args.output, args.ktx, args.trim_output)
