#!/usr/bin/env python3
"""GAEA-HEIGHT-IMPORT-1: import a Gaea-authored terrain into the MC2 pipeline.

Reads a Gaea raw export, normalizes + downsamples to the MC2 coarse gameplay grid,
classifies materials, and emits a minimal MC2 map (<name>.pak / .elev.r32 / .fit /
.burnin.tga) by reusing the existing generator (PakExporter.build_packet0 +
patch_pak — which PATCHES a stock template .pak, so the target grid must match the
template grid). The full-res normalized height is also saved as the render-only
visual-height layer (feeds the visual_height_4x arc).

HEIGHT SOURCE: a single-channel float .r32 (size = res*res*4) is the real height.
If only a 3-channel COLOR .r32 is available (size = res*res*3*4, e.g. Gaea's Combine
output), this uses its LUMINANCE as a PLACEHOLDER height + the RGB as the colormap —
clearly flagged. Re-export a dedicated Height .r32 (Mountain node) for real terrain.

Run:
  python import_gaea_height.py --recipe recipes/gaea_test_01.json
  python import_gaea_height.py --height-file X.r32 --res 512 --size 120 \
        --template-pak <stock 120-grid .pak> --name gaea_test_01 [--max-elev 280] ...
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from terrain_gen.terrain_recipe import TerrainRecipe, HeightParams, MaterialParams  # noqa: E402
from terrain_gen.material_classifier import MaterialClassifier                       # noqa: E402
from terrain_gen.pak_exporter import PakExporter                                     # noqa: E402
from terrain_gen.terrain_gen import _write_fit                                       # noqa: E402


def _read_image_height(height_file: Path):
    """Decode a heightmap stored as an IMAGE (EXR/PNG/TIFF — Gaea or Blender export)
    rather than a raw .r32. Returns (height[H,W] float64, color[H,W,3] or None) or
    None if PIL cannot open it (caller falls back to raw .r32 parsing). Greyscale ->
    height; RGB/RGBA -> luminance height + RGB colormap placeholder. 16-bit PNG and
    32-bit float EXR/TIFF are read at full precision; res is inferred from the image."""
    try:
        img = Image.open(height_file)
    except Exception:
        return None
    arr = np.asarray(img).astype(np.float64)
    color = None
    if arr.ndim == 3:
        rgb = arr[..., :3]
        h = rgb @ np.array([0.299, 0.587, 0.114])
        # only treat as a colormap if it carries real chroma (not a grey-packed height)
        if np.ptp(rgb, axis=2).max() > 1e-6:
            cmax = rgb.max() if rgb.max() > 0 else 1.0
            color = np.clip(rgb / cmax, 0, 1)
    else:
        h = arr
    return h, color


def read_gaea(height_file: Path, res: int):
    """Return (height01[res,res], color01[res,res,3] or None, source_label).

    Accepts EITHER a raw single-/three-channel float .r32 (Gaea raw export; --res
    must match) OR an image heightfield (EXR/PNG/TIFF — Gaea OR Blender export), in
    which case the resolution is taken from the image and --res is ignored."""
    if height_file.suffix.lower() in (".exr", ".png", ".tif", ".tiff", ".jpg", ".jpeg"):
        img = _read_image_height(height_file)
        if img is not None:
            h, color = img
            label = "image heightfield (%dx%d %s)" % (h.shape[1], h.shape[0],
                                                       height_file.suffix.lower())
            return h.astype(np.float64), color, label
    raw = np.fromfile(height_file, dtype="<f4")
    if raw.size == res * res:
        h = raw.reshape(res, res).astype(np.float64)
        return h, None, "height (1-channel .r32)"
    if raw.size == res * res * 3:
        rgb = raw.reshape(res, res, 3).astype(np.float64)
        lum = rgb @ np.array([0.299, 0.587, 0.114])
        return lum, np.clip(rgb, 0, 1), "COLOR-LUMINANCE PLACEHOLDER (re-export a Height .r32!)"
    raise ValueError(f"{height_file}: {raw.size} floats != res^2 ({res*res}) or res^2*3 "
                     f"({res*res*3}); wrong --res or not a Gaea raw export")


def _resize01(a: np.ndarray, n: int) -> np.ndarray:
    return np.clip(np.asarray(Image.fromarray(a.astype(np.float32), "F").resize((n, n), Image.BILINEAR)), 0, 1)


def write_clean_pak(template: str, out_pak: Path, pkt0: bytes, clear_objects: bool = True) -> int:
    """Rewrite a .pak: packet 0 = our terrain, packet 1 (terrain objects) emptied to
    count=0 so the template's buildings/links do NOT come along, and the seek table
    rebuilt for the resized packet. (patch_pak only swaps same-size packet 0, which
    inherits ALL the template's object/move packets -> a gaea map wearing the
    template's city.) Returns the new object count (0 when cleared)."""
    import struct as _s
    raw = Path(template).read_bytes()
    magic, first = _s.unpack_from('<II', raw, 0)
    n = first // 4 - 2
    ent = [_s.unpack_from('<I', raw, 8 + 4 * i)[0] for i in range(n)]
    packets = []
    for i in range(n):
        t = (ent[i] >> 29) & 7
        off = ent[i] & ((1 << 29) - 1)
        end = (ent[i + 1] & ((1 << 29) - 1)) if i + 1 < n else len(raw)
        packets.append([t, raw[off:end]])
    packets[0] = [0, pkt0]                                  # our gaea terrain (RAW -- the
        # template's packet 0 is typically STORAGE_TYPE_ZLIB (t=4); pkt0 here is raw
        # uncompressed PostcompVertex bytes from build_packet0(), so the type tag MUST
        # be forced to RAW (0) or the engine (packet.cpp STORAGE_TYPE_ZLIB branch) and
        # any offline tooling (mission_terrain_analyzer.decode_packet) will try to
        # zlib-decompress raw terrain data -- silent corruption / "no MapData packet
        # matched signature" downstream (TERRAIN-GAEA-RELIEF-1).
    if clear_objects and n > 1:
        packets[1] = [0, _s.pack('<i', 0)]                # terrain objects: count=0 (RAW)
    # re-serialize with a fresh seek table (offsets shift because packet 1 resized).
    new_first = 8 + n * 4
    entries, body, off = [], bytearray(), new_first
    for (t, d) in packets:
        entries.append(((t & 7) << 29) | (off & ((1 << 29) - 1)))
        body += d
        off += len(d)
    hdr = _s.pack('<II', magic, new_first) + b''.join(_s.pack('<I', e) for e in entries)
    out_pak.write_bytes(hdr + bytes(body))
    return 0 if clear_objects else -1


def run(cfg: dict, out_root: Path) -> dict:
    res = int(cfg["height_resolution"])
    size = int(cfg["size"])
    name = cfg["name"]
    h_raw, color, src = read_gaea(Path(cfg["height_file"]), res)
    # Optional separate Gaea COLOR export (e.g. Combine_Out.r32) for the colormap.
    if cfg.get("color_file") and color is None:
        craw = np.fromfile(Path(cfg["color_file"]), dtype="<f4")
        if craw.size == res * res * 3:
            color = np.clip(craw.reshape(res, res, 3).astype(np.float64), 0, 1)
        else:
            print(f"[gaea-import] WARN color_file is {craw.size} floats != res^2*3; ignoring")

    # normalize -> [0,1], orientation guard (Gaea image-Y-down vs MC2 row-major).
    hn = (h_raw - h_raw.min()) / (h_raw.max() - h_raw.min() + 1e-9)
    if cfg.get("flip_y", True):
        hn = np.flipud(hn)
        if color is not None:
            color = np.flipud(color)

    coarse = _resize01(hn, size)                      # gameplay grid
    water_level = float(np.percentile(coarse, cfg.get("water_percentile", 30)))

    recipe = TerrainRecipe(
        version=1, name=name, size=size, biome=cfg.get("biome", "temperate_hills"), seed=1,
        height=HeightParams(min_elevation=float(cfg["height"]["min_elevation"]),
                            max_elevation=float(cfg["height"]["max_elevation"])),
        materials=MaterialParams(water_level=water_level),
    )

    masks = MaterialClassifier().classify(coarse.astype(np.float32), recipe)

    # By default each import gets its own <out_root>/<name>/ subdir (Gaea CLI flow).
    # flat_out=True writes the artifacts DIRECTLY into out_root with no <name> subdir
    # — this is what the editor "Import from..." button uses so the files land at
    # terrain_gen_out/<name>.elev.r32 etc., exactly where the editor apply path reads.
    out = out_root if cfg.get("flat_out") else out_root / name
    out.mkdir(parents=True, exist_ok=True)
    exp = PakExporter()

    # The .pak + .fit are only emitted when a stock template .pak is supplied
    # (build_packet0 produces the terrain packet, but write_clean_pak PATCHES a
    # template). Without a template (e.g. the editor "Import from..." button) we
    # still emit everything the editor apply path consumes — .elev.r32,
    # .burnin.tga, .preview.png — but SKIP the .pak/.fit step.
    have_template = bool(cfg.get("template_pak"))
    if have_template:
        pkt0 = exp.build_packet0(coarse.astype(np.float32), masks, recipe)
        # Clean map by default: empty the template's terrain-objects packet so the gaea
        # terrain doesn't inherit the template's buildings/links (patch_pak would).
        write_clean_pak(cfg["template_pak"], out / f"{name}.pak", pkt0,
                        clear_objects=not cfg.get("keep_objects", False))

    # .elev.r32 grid is size x size (= recipe.size). The editor reads it as
    # MapSizeToVertexSide(mapSizeIndex), so the dialog MUST pass
    # --size = the vertex-side for the selected map size (cellSide + 1).
    elev = (coarse * recipe.height.max_elevation + recipe.height.min_elevation).astype("<f4")
    elev.tofile(str(out / f"{name}.elev.r32"))
    if have_template:
        # mission_ready: the template-pak path emits a directly game-launchable
        # map, so the .fit MUST carry the minimal player/commander skeleton --
        # a bare [ColorMap]/[Terrain] fit AVs Mission::init (no commanders
        # created; see _mission_skeleton_blocks in terrain_gen.py).
        _write_fit(out / f"{name}.fit", f"{name}.burnin", recipe, mission_ready=True)

    # colormap (burnin): use the Gaea color directly if present, else hillshade of height.
    burnin_res = recipe.burnin_resolution()
    if color is not None:
        Image.fromarray((color * 255).astype(np.uint8), "RGB").resize(
            (burnin_res, burnin_res), Image.LANCZOS).save(str(out / f"{name}.burnin.tga"))
        cmap_src = "gaea color"
    else:
        g = (_resize01(hn, burnin_res) * 255).astype(np.uint8)
        Image.fromarray(np.stack([g, g, g], -1), "RGB").save(str(out / f"{name}.burnin.tga"))
        cmap_src = "height greyscale"

    # full-res visual-height layer (render-only; feeds visual_height arc).
    vis = (_resize01(hn, res) * recipe.height.max_elevation + recipe.height.min_elevation).astype("<f4")
    vis.tofile(str(out / f"{name}.visual_height.r32"))
    # preview
    pg = (_resize01(hn, 256) * 255).astype(np.uint8)
    Image.fromarray(np.stack([pg, pg, pg], -1), "RGB").save(str(out / f"{name}.preview.png"))

    outputs = [f"{name}.elev.r32", f"{name}.burnin.tga",
               f"{name}.visual_height.r32", f"{name}.preview.png"]
    if have_template:
        outputs = [f"{name}.pak", f"{name}.fit"] + outputs
    rep = {
        "name": name, "height_source": src, "colormap_source": cmap_src,
        "gaea_res": res, "mc2_grid": size, "template_pak": cfg.get("template_pak"),
        "elevation_wu": {"min": recipe.height.min_elevation, "max": recipe.height.max_elevation},
        "water_level_norm": water_level,
        "water_elevation_wu": water_level * recipe.height.max_elevation + recipe.height.min_elevation,
        "flip_y": cfg.get("flip_y", True),
        "outputs": outputs,
        "out_dir": str(out),
    }
    (out / f"{name}.import_report.json").write_text(json.dumps(rep, indent=2), encoding="utf-8")
    return rep


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--recipe", help="gaea recipe json (overrides individual flags)")
    ap.add_argument("--height-file"); ap.add_argument("--color-file")
    ap.add_argument("--res", type=int, default=512)
    ap.add_argument("--size", type=int, default=120)
    ap.add_argument("--template-pak"); ap.add_argument("--name", default="gaea_test_01")
    ap.add_argument("--biome", default="temperate_hills")
    ap.add_argument("--max-elev", type=float, default=280.0)
    ap.add_argument("--min-elev", type=float, default=0.0)
    ap.add_argument("--water-pct", type=float, default=30.0)
    ap.add_argument("--no-flip", action="store_true")
    ap.add_argument("--keep-objects", action="store_true")
    ap.add_argument("--out", default="tests/terrain/gaea")
    ap.add_argument("--flat-out", action="store_true",
                    help="write artifacts directly into --out (no <name>/ subdir); "
                         "used by the editor 'Import from...' button")
    a = ap.parse_args()

    if a.recipe:
        cfg = json.loads(Path(a.recipe).read_text())
    else:
        # Only --height-file is mandatory. --template-pak is OPTIONAL: when given,
        # a .pak/.fit are patched from the template; when absent (the editor
        # "Import from..." path) only the heightfield/colormap/preview are emitted.
        if not a.height_file:
            ap.error("need --recipe OR --height-file (--template-pak is optional)")
        cfg = {"name": a.name, "height_file": a.height_file, "color_file": a.color_file,
               "height_resolution": a.res, "size": a.size, "template_pak": a.template_pak,
               "biome": a.biome, "flip_y": not a.no_flip, "water_percentile": a.water_pct,
               "height": {"min_elevation": a.min_elev, "max_elevation": a.max_elev},
               "keep_objects": a.keep_objects, "flat_out": a.flat_out}
    rep = run(cfg, Path(a.out))
    print(f"[gaea-import] {rep['name']}: src={rep['height_source']} "
          f"gaea{rep['gaea_res']}->mc2 {rep['mc2_grid']}^2  water={rep['water_elevation_wu']:.0f}wu "
          f"cmap={rep['colormap_source']} -> {rep['out_dir']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
