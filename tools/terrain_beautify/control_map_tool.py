#!/usr/bin/env python3
"""TERRAIN-CONTROLMAP-SAMPLE-1: offline authoring/validation harness for the
authored terrain control-map sidecar (`data/missions/<stem>.beauty/control_map.png`).

Engine contract (mclib/terrain.cpp, gated MC2_TERRAIN_CONTROLMAP, search
"TERRAIN_CONTROLMAP v1" / gos_TerrainLodChunk_UploadControlMap):
  - RGBA8 PNG, size side x side where side == Terrain::realVerticesMapSide
    (one texel per terrain VERTEX; side in {60,80,100,120}).
  - R=rock, G=grass, B=dirt, A=concrete weight, normalized in-shader (GL_LINEAR
    sampling gives bilinear cross-cell blend, matching how the colormap atlas
    is sampled).
  - Loader decode: mclib/control_map_png_decode.cpp ControlMapPng_DecodeRGBA()
    -> stb_image `stbi_load_from_memory(..., 4)`. stb_image returns rows in
    on-disk PNG top-to-bottom order (row 0 of the decoded buffer = the TOP row
    of the authored image, standard image convention, NOT flipped).
  - terrain.cpp:897 requires `pw == mapSide && ph == mapSide` (exact match,
    no rescale) then passes the flat decoded buffer straight to
    gos_TerrainLodChunk_UploadControlMap() -> glTexImage2D(..., rgba) with NO
    row reversal anywhere in that call chain (gos_terrain_lod_chunk.cpp:1466).
  - The vertex-side arrays this must line up with (elev[], ttype[] uploaded a
    few lines above at terrain.cpp:817/821, and mission_terrain_analyzer.py's
    `extract_layers()` reshape) are flat row-major `blks[i]`, i = row*side+col,
    with NO flip applied either.
  - CONCLUSION (row-order finding): the control-map PNG's pixel row 0 (top of
    the image as opened in any editor) corresponds DIRECTLY to vertex row 0
    (i = 0..side-1) with no vertical flip anywhere in the pipeline. Authors
    can treat the PNG like a plain top-down 2D array matching `blks[]`/
    `extract_layers()` output index-for-index. This tool follows that
    convention throughout (numpy array row 0 == PNG row 0 == vertex row 0).

Subcommands:
  generate  --pak <mission.pak> --out <png>
      v2-authoring-seed classification: reuses mission_terrain_analyzer's
      PostcompVertex.terrtype extraction (NOT a copy) and maps the raw
      terrainType index -> material bucket with the SAME mapping the engine
      uses (mclib/terrain.cpp terrainTypeToMaterial lambda), then writes
      RGBA = one-hot(material) * 255. Classifier PARITY with the live
      colormap-colour classifier (chunkColorWeights) is explicitly NOT
      required here -- this is an authoring seed (v1(a)-adjacent), not the
      runtime classifier twin.
  pattern   --side N --out <png> --kind quadrant|gradient|checker
      Synthetic deterministic test maps for in-game visual verification.
  validate  --png <file> --pak <mission.pak>
      Size match (against the mission's realVerticesMapSide), RGBA8 format,
      weight-sum sanity. Prints PASS/FAIL, exit 0/1.
  install   --png <file> --deploy <dir> --mission <stem>
      Copies to <deploy>/data/missions/<stem>.beauty/control_map.png.
  sheet     --png <file> --out <file>
      Per-channel grayscale + composite RGB contact sheet (reuses
      terrain_workbench's panel/resize convention).

PIL + numpy only, offline, read-only w.r.t. .pak files (generate/validate never
write pak/fit). No engine launch, no gameos import.
"""
from __future__ import annotations

import argparse
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from mission_terrain_analyzer import (  # noqa: E402
    read_packets, locate_mapdata, extract_layers, read_water_elevation,
    VALID_GRID_SIDES, WORLD_UNITS_PER_VERTEX,
)

# Mirrors mclib/terrain.cpp terrainTypeToMaterial lambda EXACTLY (search
# "MUST mirror gos_terrain_indirect.cpp terrainTypeToMaterialLocal" in
# terrain.cpp ~line 806). Material index: 0=Rock 1=Grass 2=Dirt 3=Concrete.
_GRASS = {3, 8, 9, 12}
_DIRT = {2, 4}
_CONCRETE = {10, 13, 14, 15, 16, 17, 18, 19, 20}


def terrain_type_to_material(raw: np.ndarray) -> np.ndarray:
    """raw: uint32 array of PostcompVertex.terrainType indices (any shape).
    Returns uint8 array of material index 0..3, same shape."""
    out = np.zeros(raw.shape, dtype=np.uint8)  # default 0 = Rock
    for v in _GRASS:
        out[raw == v] = 1
    for v in _DIRT:
        out[raw == v] = 2
    for v in _CONCRETE:
        out[raw == v] = 3
    return out


def material_to_rgba(material: np.ndarray) -> np.ndarray:
    """One-hot material index (0..3) -> RGBA8 weight image.
    R=rock(0) G=grass(1) B=dirt(2) A=concrete(3), 0 or 255 per channel."""
    h, w = material.shape
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    for idx in range(4):
        rgba[..., idx] = np.where(material == idx, 255, 0).astype(np.uint8)
    return rgba


# --- generate-from-height (TERRAIN-CONTROLMAP-GENERATE-1) ---------------------
# Hi-res heightfield classifier: slope + altitude bands -> soft rock/grass/dirt
# weights, optionally blended with authored Gaea deposition masks, smoothed,
# normalized, and area-averaged down to the mission's vertex-resolution grid.
# A (concrete) is left at 0 here -- concrete/cement is a separate authored/runtime
# axis (see TERRAIN-CONTROLMAP-GENERATE-1-RECON.md landmine #3), this classifier
# only ever proposes R/G/B.

def _read_r32(path: Path, res: int) -> np.ndarray:
    """Read a single-channel float32 Gaea raw export, res x res, row-major.
    Falls back to PIL for image-container masks (PNG/TIFF/EXR)."""
    suffix = path.suffix.lower()
    if suffix in (".png", ".tif", ".tiff", ".exr", ".jpg", ".jpeg"):
        arr = np.asarray(Image.open(path)).astype(np.float64)
        if arr.ndim == 3:
            arr = arr[..., :3].mean(axis=-1)
        if arr.max() > 1.0:
            arr = arr / 255.0 if arr.max() <= 255.0 else arr / arr.max()
        if arr.shape[0] != res or arr.shape[1] != res:
            arr = np.asarray(Image.fromarray(arr.astype(np.float32), "F")
                              .resize((res, res), Image.BILINEAR), dtype=np.float64)
        return arr
    raw = np.fromfile(path, dtype="<f4")
    if raw.size != res * res:
        raise ValueError(f"{path}: {raw.size} floats != res^2 ({res * res}); wrong --res?")
    return raw.reshape(res, res).astype(np.float64)


def _gaussian_kernel1d(sigma: float) -> np.ndarray:
    radius = max(1, int(round(3.0 * sigma)))
    x = np.arange(-radius, radius + 1, dtype=np.float64)
    k = np.exp(-(x ** 2) / (2.0 * sigma * sigma))
    return k / k.sum()


def _gaussian_blur(a: np.ndarray, sigma: float) -> np.ndarray:
    """Separable gaussian blur, edge-replicated padding, numpy/PIL only (no scipy)."""
    if sigma <= 0.0:
        return a
    k = _gaussian_kernel1d(sigma)
    radius = (len(k) - 1) // 2
    padded = np.pad(a, radius, mode="edge")
    # horizontal pass
    tmp = np.zeros_like(a)
    for i, w in enumerate(k):
        tmp += w * padded[radius:radius + a.shape[0], i:i + a.shape[1]]
    padded2 = np.pad(tmp, radius, mode="edge")
    out = np.zeros_like(a)
    for i, w in enumerate(k):
        out += w * padded2[i:i + a.shape[0], radius:radius + a.shape[1]]
    return out


def compute_slope_deg(height_wu: np.ndarray, size_wu: float) -> np.ndarray:
    """Slope in degrees from a world-unit heightfield, wu-correct: the gradient
    step in world units is size_wu / (res - 1) (res-1 cells span the full
    world-unit extent, matching mission_terrain_analyzer's worldToTile
    convention where extent = (side) * WORLD_UNITS_PER_VERTEX)."""
    res = height_wu.shape[0]
    step_wu = size_wu / max(1, res - 1)
    gy, gx = np.gradient(height_wu, step_wu)
    grad = np.sqrt(gx ** 2 + gy ** 2)
    return np.degrees(np.arctan(grad))


def classify_weights(height01: np.ndarray, size_wu: float, max_elev_wu: float,
                      steep_deg: float = 35.0, flat_deg: float = 10.0,
                      high_frac: float = 0.65, low_frac: float = 0.35,
                      smooth_sigma: float = 2.0,
                      rock_mask: np.ndarray = None,
                      sediment_mask: np.ndarray = None) -> dict:
    """v0 classifier: slope + altitude bands -> soft rock/grass/dirt weights.

    height01: [0,1]-normalized heightfield (as Gaea exports it).
    size_wu: world-unit extent of the heightfield (edge to edge) for wu-correct
             slope math.
    max_elev_wu: converts height01 -> world-unit elevation for the altitude
             bands (elevation_wu = height01 * max_elev_wu).
    Rules (soft, blended via smoothstep-like weights, then gaussian-smoothed):
      - steep OR high altitude       -> rock
      - low-slope AND low altitude   -> grass
      - everything else (mid-slope / mid-altitude / deposition) -> dirt
      - rock_mask / sediment_mask (0..1, same res) additively boost rock / dirt
        respectively before smoothing+normalize (authored Gaea masks override
        the coarse slope/altitude heuristic where present).
    Returns dict of raw (pre-smooth) and final normalized weight arrays plus
    stats, all shape (res, res) float64 in [0,1], not yet re-summed to 255.
    """
    height_wu = height01 * max_elev_wu
    slope_deg = compute_slope_deg(height_wu, size_wu)

    elev_norm = np.clip(height01, 0.0, 1.0)

    def smoothstep(edge0, edge1, x):
        t = np.clip((x - edge0) / max(1e-9, (edge1 - edge0)), 0.0, 1.0)
        return t * t * (3.0 - 2.0 * t)

    steep_w = smoothstep(flat_deg, steep_deg, slope_deg)
    high_w = smoothstep(low_frac, high_frac, elev_norm)
    low_alt_w = 1.0 - high_w
    flat_w = 1.0 - steep_w

    rock = np.maximum(steep_w, high_w)
    grass = flat_w * low_alt_w
    dirt = np.clip(1.0 - np.maximum(rock, grass), 0.0, None)

    if rock_mask is not None:
        rock = np.clip(rock + rock_mask, 0.0, 1.0)
    if sediment_mask is not None:
        dirt = np.clip(dirt + sediment_mask, 0.0, 1.0)

    rock_s = _gaussian_blur(rock, smooth_sigma)
    grass_s = _gaussian_blur(grass, smooth_sigma)
    dirt_s = _gaussian_blur(dirt, smooth_sigma)

    total = rock_s + grass_s + dirt_s
    total_safe = np.where(total <= 1e-9, 1.0, total)
    rock_n = np.where(total <= 1e-9, 1.0 / 3.0, rock_s / total_safe)
    grass_n = np.where(total <= 1e-9, 1.0 / 3.0, grass_s / total_safe)
    dirt_n = np.where(total <= 1e-9, 1.0 / 3.0, dirt_s / total_safe)

    return {
        "slope_deg": slope_deg, "elev_norm": elev_norm,
        "rock": rock_n, "grass": grass_n, "dirt": dirt_n,
    }


def area_average_downsample(a: np.ndarray, side: int) -> np.ndarray:
    """Downsample a (res,res) float array to (side,side) by area-averaging
    (box filter over each output cell's footprint), preferred over point
    sampling because it preserves mask/detail energy instead of aliasing it."""
    res = a.shape[0]
    if res == side:
        return a.astype(np.float64)
    img = Image.fromarray(a.astype(np.float32), "F")
    # PIL's BOX resize is a true area-average for downsampling.
    resized = img.resize((side, side), Image.BOX if res > side else Image.BILINEAR)
    return np.asarray(resized, dtype=np.float64)


def weights_to_rgba(rock: np.ndarray, grass: np.ndarray, dirt: np.ndarray) -> np.ndarray:
    """Normalize (rock,grass,dirt) to sum<=255 and pack RGBA8 (A=concrete=0)."""
    total = rock + grass + dirt
    total_safe = np.where(total <= 1e-9, 1.0, total)
    r = np.clip(rock / total_safe * 255.0, 0, 255)
    g = np.clip(grass / total_safe * 255.0, 0, 255)
    b = np.clip(dirt / total_safe * 255.0, 0, 255)
    h, w = rock.shape
    rgba = np.zeros((h, w, 4), dtype=np.uint8)
    rgba[..., 0] = np.round(r).astype(np.uint8)
    rgba[..., 1] = np.round(g).astype(np.uint8)
    rgba[..., 2] = np.round(b).astype(np.uint8)
    rgba[..., 3] = 0
    return rgba


# --- generate -----------------------------------------------------------------
def cmd_generate(args) -> int:
    pak = Path(args.pak)
    if not pak.is_file():
        print(f"[generate] ERROR pak not found: {pak}", file=sys.stderr)
        return 4
    packets = read_packets(pak)
    md = locate_mapdata(packets)
    if md is None:
        print(f"[generate] ERROR no MapData packet matched signature in {pak}", file=sys.stderr)
        return 4
    pkt_idx, side, blocks = md

    mission = pak.stem
    fit_path = pak.with_suffix(".fit")
    water_elev = read_water_elevation(fit_path)
    layers = extract_layers(side, blocks, water_elev)  # terrtype shape (side, side), row-major, no flip
    material = terrain_type_to_material(layers["terrtype"])
    rgba = material_to_rgba(material)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(out_path)

    counts = {name: int((material == idx).sum())
              for idx, name in enumerate(("rock", "grass", "dirt", "concrete"))}
    print(f"[generate] {mission}: side={side} -> {out_path} counts={counts}")
    return 0


# --- generate-from-height ---------------------------------------------------
def cmd_generate_from_height(args) -> int:
    side = args.side
    if side <= 1:
        print(f"[generate-from-height] ERROR --side must be > 1, got {side}", file=sys.stderr)
        return 4

    if args.r32:
        r32_path = Path(args.r32)
        if not r32_path.is_file():
            print(f"[generate-from-height] ERROR r32 not found: {r32_path}", file=sys.stderr)
            return 4
        res = args.res
        if not res:
            print("[generate-from-height] ERROR --res required with --r32", file=sys.stderr)
            return 4
        height01 = _read_r32(r32_path, res)
        source = f"r32:{r32_path.name}"
    elif args.pak:
        pak = Path(args.pak)
        if not pak.is_file():
            print(f"[generate-from-height] ERROR pak not found: {pak}", file=sys.stderr)
            return 4
        packets = read_packets(pak)
        md = locate_mapdata(packets)
        if md is None:
            print(f"[generate-from-height] ERROR no MapData packet in {pak}", file=sys.stderr)
            return 4
        _, pak_side, blocks = md
        water_elev = read_water_elevation(pak.with_suffix(".fit"))
        layers = extract_layers(pak_side, blocks, water_elev)
        elev = layers["elev"]
        emin, emax = float(elev.min()), float(elev.max())
        rng = (emax - emin) if (emax - emin) > 1e-9 else 1.0
        height01 = (elev - emin) / rng
        res = pak_side
        source = f"pak:{pak.name} (fallback float-elev, no hi-res source)"
    else:
        print("[generate-from-height] ERROR one of --r32 or --pak is required", file=sys.stderr)
        return 4

    size_wu = args.size if args.size else float(side) * WORLD_UNITS_PER_VERTEX
    max_elev_wu = args.max_elev

    rock_mask = None
    if args.rock_mask:
        rmp = Path(args.rock_mask)
        if not rmp.is_file():
            print(f"[generate-from-height] ERROR --rock-mask not found: {rmp}", file=sys.stderr)
            return 4
        rock_mask = np.clip(_read_r32(rmp, res), 0.0, 1.0)

    sediment_mask = None
    if args.sediment_mask:
        smp = Path(args.sediment_mask)
        if not smp.is_file():
            print(f"[generate-from-height] ERROR --sediment-mask not found: {smp}", file=sys.stderr)
            return 4
        sediment_mask = np.clip(_read_r32(smp, res), 0.0, 1.0)

    weights = classify_weights(
        height01, size_wu=size_wu, max_elev_wu=max_elev_wu,
        steep_deg=args.steep_deg, flat_deg=args.flat_deg,
        high_frac=args.high_frac, low_frac=args.low_frac,
        smooth_sigma=args.smooth_sigma,
        rock_mask=rock_mask, sediment_mask=sediment_mask,
    )

    rock_ds = area_average_downsample(weights["rock"], side)
    grass_ds = area_average_downsample(weights["grass"], side)
    dirt_ds = area_average_downsample(weights["dirt"], side)

    rgba = weights_to_rgba(rock_ds, grass_ds, dirt_ds)

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(out_path)

    total = rgba[..., :3].astype(np.float64).sum(axis=-1)
    total_safe = np.where(total <= 0, 1.0, total)
    pct_rock = float((rgba[..., 0] / total_safe * 100.0).mean())
    pct_grass = float((rgba[..., 1] / total_safe * 100.0).mean())
    pct_dirt = float((rgba[..., 2] / total_safe * 100.0).mean())

    print(f"[generate-from-height] source={source} res={res} -> side={side} "
          f"size_wu={size_wu:.1f} max_elev_wu={max_elev_wu:.1f} -> {out_path} "
          f"pct_rock={pct_rock:.1f} pct_grass={pct_grass:.1f} pct_dirt={pct_dirt:.1f}")
    return 0


# --- pattern --------------------------------------------------------------
def cmd_pattern(args) -> int:
    side = args.side
    if side <= 1:
        print(f"[pattern] ERROR --side must be > 1, got {side}", file=sys.stderr)
        return 4
    rgba = np.zeros((side, side, 4), dtype=np.uint8)
    kind = args.kind

    if kind == "quadrant":
        # 4 pure corners: TL=rock(R) TR=grass(G) BL=dirt(B) BR=concrete(A).
        # Row 0 = top per the PNG/vertex-row-0 convention documented above.
        half = side // 2
        rgba[:half, :half, 0] = 255            # top-left    -> rock
        rgba[:half, half:, 1] = 255            # top-right   -> grass
        rgba[half:, :half, 2] = 255            # bottom-left -> dirt
        rgba[half:, half:, 3] = 255            # bottom-right-> concrete
    elif kind == "gradient":
        # Horizontal rock->grass gradient (R falls, G rises left->right);
        # dirt/concrete held at 0 so weights always sum to 255 (R+G=255).
        ramp = np.linspace(0, 255, side, dtype=np.uint8)
        rgba[:, :, 0] = 255 - ramp[np.newaxis, :]
        rgba[:, :, 1] = ramp[np.newaxis, :]
    elif kind == "checker":
        cell = max(1, side // 8)
        rr, cc = np.indices((side, side))
        is_a = ((rr // cell) + (cc // cell)) % 2 == 0
        rgba[is_a, 0] = 255     # rock squares
        rgba[~is_a, 1] = 255    # grass squares
    else:
        print(f"[pattern] ERROR unknown --kind {kind}", file=sys.stderr)
        return 4

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    Image.fromarray(rgba, mode="RGBA").save(out_path)
    print(f"[pattern] kind={kind} side={side} -> {out_path}")
    return 0


# --- validate -------------------------------------------------------------
def cmd_validate(args) -> int:
    png_path = Path(args.png)
    ok = True

    if not png_path.is_file():
        print(f"[validate] FAIL png not found: {png_path}")
        return 1

    img = Image.open(png_path)
    if img.mode != "RGBA":
        print(f"[validate] FAIL mode={img.mode}, want RGBA (8-bit/channel)")
        ok = False
    w, h = img.size
    if w != h:
        print(f"[validate] FAIL non-square image {w}x{h}")
        ok = False

    expected_side = None
    if args.pak:
        pak = Path(args.pak)
        if not pak.is_file():
            print(f"[validate] FAIL pak not found: {pak}")
            return 1
        packets = read_packets(pak)
        md = locate_mapdata(packets)
        if md is None:
            print(f"[validate] FAIL no MapData packet in {pak}")
            return 1
        _, expected_side, _ = md
    elif args.side:
        expected_side = args.side

    if expected_side is not None:
        if w != expected_side or h != expected_side:
            print(f"[validate] FAIL size {w}x{h} != expected {expected_side}x{expected_side} "
                  "(engine requires EXACT match, terrain.cpp:897)")
            ok = False
        else:
            print(f"[validate] size {w}x{h} == expected {expected_side}x{expected_side}: PASS")
    else:
        if w not in VALID_GRID_SIDES:
            print(f"[validate] WARN side={w} not in known VALID_GRID_SIDES {VALID_GRID_SIDES} "
                  "(no --pak/--side given to confirm against mission)")

    arr = np.array(img.convert("RGBA"), dtype=np.uint16)
    total = arr[..., 0].astype(np.uint16) + arr[..., 1] + arr[..., 2] + arr[..., 3]
    zero_weight = int((total == 0).sum())
    overflow = int((total > 255 * 4).sum())  # structurally impossible for uint8 channels, sanity only
    if zero_weight > 0:
        print(f"[validate] WARN {zero_weight} texel(s) have all-zero RGBA weight "
              "(engine normalizes in-shader -> undefined/zero material there)")
    if overflow > 0:
        print(f"[validate] FAIL {overflow} texel(s) with impossible channel overflow")
        ok = False
    print(f"[validate] weight-sum stats: min={int(total.min())} max={int(total.max())} "
          f"mean={float(total.mean()):.1f} (255 = single-material texel, engine normalizes any sum)")

    print("[validate] PASS" if ok else "[validate] FAIL")
    return 0 if ok else 1


# --- install ----------------------------------------------------------------
def cmd_install(args) -> int:
    src = Path(args.png)
    if not src.is_file():
        print(f"[install] ERROR source png not found: {src}", file=sys.stderr)
        return 4
    dest_dir = Path(args.deploy) / "data" / "missions" / f"{args.mission}.beauty"
    dest_dir.mkdir(parents=True, exist_ok=True)
    dest = dest_dir / "control_map.png"
    shutil.copyfile(src, dest)
    print(f"[install] {src} -> {dest}")
    return 0


# --- sheet ------------------------------------------------------------------
PANEL = 220  # matches terrain_workbench.py's panel size convention


def _resize(rgb: np.ndarray, n: int = PANEL) -> Image.Image:
    return Image.fromarray(rgb, "RGB").resize((n, n), Image.NEAREST)


def cmd_sheet(args) -> int:
    png_path = Path(args.png)
    if not png_path.is_file():
        print(f"[sheet] ERROR png not found: {png_path}", file=sys.stderr)
        return 4
    arr = np.array(Image.open(png_path).convert("RGBA"))
    names = ("rock(R)", "grass(G)", "dirt(B)", "concrete(A)")

    panels = []
    labels = []
    for idx, name in enumerate(names):
        chan = arr[..., idx]
        gray = np.stack([chan, chan, chan], axis=-1)
        panels.append(_resize(gray))
        labels.append(name)
    composite = arr[..., :3].copy()
    panels.append(_resize(composite))
    labels.append("composite RGB")

    cols = len(panels)
    sheet = Image.new("RGB", (PANEL * cols, PANEL + 24), (30, 30, 30))
    from PIL import ImageDraw
    draw = ImageDraw.Draw(sheet)
    for i, (panel, label) in enumerate(zip(panels, labels)):
        sheet.paste(panel, (i * PANEL, 24))
        draw.text((i * PANEL + 4, 4), label, fill=(255, 255, 255))

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(out_path)
    print(f"[sheet] {png_path} -> {out_path}")
    return 0


# --- CLI ----------------------------------------------------------------------
def main() -> int:
    ap = argparse.ArgumentParser(description="control_map_tool: offline authoring/validation for terrain control maps")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("generate", help="classify stock mission terrainType -> RGBA control map (authoring seed)")
    p.add_argument("--pak", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_generate)

    p = sub.add_parser("generate-from-height",
                        help="classify a hi-res heightfield (Gaea r32, or --pak fallback) "
                             "into an RGBA control map (slope+altitude, area-avg downsample)")
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument("--r32", help="single-channel float32 Gaea raw export (or PNG/TIFF/EXR)")
    src.add_argument("--pak", help="mission .pak fallback (uses coarse float elevation, no hi-res source)")
    p.add_argument("--res", type=int, help="r32 resolution (required with --r32, e.g. 512)")
    p.add_argument("--side", type=int, required=True, help="output vertex resolution, e.g. 120")
    p.add_argument("--size", type=float, default=0.0,
                   help="world-unit extent of the heightfield (edge-to-edge), for wu-correct "
                        "slope math; default = --side * 128 (WORLD_UNITS_PER_VERTEX)")
    p.add_argument("--max-elev", dest="max_elev", type=float, default=1200.0,
                   help="world-unit elevation the height01=1.0 texel represents (altitude bands)")
    p.add_argument("--rock-mask", dest="rock_mask",
                   help="optional Gaea rock/sandstone mask r32 (0..1), same --res, additively boosts rock")
    p.add_argument("--sediment-mask", dest="sediment_mask",
                   help="optional Gaea deposition/sediment mask r32 (0..1), same --res, additively boosts dirt")
    p.add_argument("--steep-deg", dest="steep_deg", type=float, default=35.0,
                   help="slope (deg) at/above which a texel is fully classified steep->rock")
    p.add_argument("--flat-deg", dest="flat_deg", type=float, default=10.0,
                   help="slope (deg) at/below which a texel is fully classified flat")
    p.add_argument("--high-frac", dest="high_frac", type=float, default=0.65,
                   help="normalized height (0..1) at/above which a texel is fully 'high altitude'")
    p.add_argument("--low-frac", dest="low_frac", type=float, default=0.35,
                   help="normalized height (0..1) at/below which a texel is fully 'low altitude'")
    p.add_argument("--smooth-sigma", dest="smooth_sigma", type=float, default=2.0,
                   help="gaussian blur sigma (in hi-res texels) applied to weights before normalize")
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_generate_from_height)

    p = sub.add_parser("pattern", help="synthetic test control map")
    p.add_argument("--side", type=int, required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--kind", choices=("quadrant", "gradient", "checker"), required=True)
    p.set_defaults(func=cmd_pattern)

    p = sub.add_parser("validate", help="size/format/weight-sum checks")
    p.add_argument("--png", required=True)
    p.add_argument("--pak", help="mission pak to confirm expected side against")
    p.add_argument("--side", type=int, help="expected side if no --pak available")
    p.set_defaults(func=cmd_validate)

    p = sub.add_parser("install", help="copy png into <deploy>/data/missions/<stem>.beauty/control_map.png")
    p.add_argument("--png", required=True)
    p.add_argument("--deploy", required=True)
    p.add_argument("--mission", required=True, help="mission stem, e.g. mc2_01")
    p.set_defaults(func=cmd_install)

    p = sub.add_parser("sheet", help="per-channel + composite contact sheet")
    p.add_argument("--png", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_sheet)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
