#!/usr/bin/env python3
# tools/terrain_gen/terrain_gen.py
"""
MC2 terrain generator -- Phase A visual output only.

Usage:
  python terrain_gen.py recipe.json --out outdir/

Outputs:
  {name}.burnin.tga    24-bit RGB TGA (MC2 colormap)
  {name}.recipe.json   resolved recipe with biome params applied
  {name}.preview.png   256x256 thumbnail
  contact_sheet.png    3x2 debug grid
  height.png           grayscale height
  slope.png            grayscale slope
  altitude.png         grayscale altitude
  valley.png           grayscale valley
  terrain_type.png     colorized terrain type map
"""
import sys
import os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import argparse
from pathlib import Path
import numpy as np
from PIL import Image

from terrain_gen.terrain_recipe import TerrainRecipe
from terrain_gen.biome_presets import BIOMES
from terrain_gen.height_generator import HeightGenerator
from terrain_gen.material_classifier import MaterialClassifier, TerrainType
from terrain_gen.burnin_renderer import BurninRenderer
from terrain_gen.preview import make_preview, make_contact_sheet

try:
    from terrain_gen.pak_exporter import PakExporter
    _PAK_AVAILABLE = True
except ImportError:
    _PAK_AVAILABLE = False


_TYPE_COLORS: dict = {
    TerrainType.BLUE_WATER:   (40,  80,  200),
    TerrainType.GREEN_WATER:  (60,  140, 80),
    TerrainType.MUD:          (100, 80,  60),
    TerrainType.MOSS:         (80,  110, 60),
    TerrainType.DIRT:         (160, 130, 90),
    TerrainType.ASH:          (150, 145, 140),
    TerrainType.MOUNTAIN:     (130, 120, 110),
    TerrainType.TUNDRA:       (200, 210, 220),
    TerrainType.FOREST_FLOOR: (50,  80,  40),
    TerrainType.GRASS:        (100, 150, 60),
    TerrainType.CONCRETE:     (160, 160, 155),
    TerrainType.CLIFF:        (90,  85,  80),
    TerrainType.NONE:         (30,  30,  30),
}


def _save_tga(img: Image.Image, path: Path) -> None:
    img.save(str(path), format='TGA')


def _save_jpg(img: Image.Image, path: Path, quality: int = 95) -> None:
    # terrtxm2 prefers <name>.burnin.jpg over .tga (UV-decoupled sampling, GPU-side).
    # q95 keeps colormap artifacts negligible at a fraction of TGA size.
    img.convert('RGB').save(str(path), format='JPEG', quality=quality, subsampling=0)


def _save_gray(arr: np.ndarray, path: Path) -> None:
    Image.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8), mode='L').save(str(path))


def _save_terrain_type(tt: np.ndarray, path: Path) -> None:
    rgb = np.zeros((*tt.shape, 3), dtype=np.uint8)
    for val, color in _TYPE_COLORS.items():
        rgb[tt == val] = color
    Image.fromarray(rgb, mode='RGB').save(str(path))


def _mission_skeleton_blocks() -> str:
    """Minimal GAME-LAUNCHABLE mission blocks (MISSION-FIT-MIN-BLOCKS-1).

    Mission::init (code/mission.cpp ~2640-2740) derives team/commander counts
    from [Parts] Part<N> TeamId/CommanderId; with no parts, maxCommanderID
    stays -1, Team::teams[0]/Commander::commanders[0] are never created, and
    `Commander::commanders[0]->setTeam(Team::home)` AVs (the gosASSERTs are
    no-ops in RelWithDebInfo). So an imported map that should be launchable
    in-game MUST carry at least one part with TeamID=0/CommanderID=0.

    Emits: 1 player part (Werewolf at map center -- cheapest known-good stock
    unit, cloned from stock mc2_01 Part1) + 1 warrior, empty [Teams], zeroed
    NavMarkers/Artillery/Objectives, a sane [MissionSettings] (scenarioTuneNum
    is read there), and [Script] ScenarioScript="m0101" (stock scenario ABL
    present in mission.fst; same default the editor writes for new maps --
    ABLi_preProcess STOPs on a missing/empty script name).
    """
    return (
        "\n"
        "[Warriors]\n"
        "ul NumWarriors = 1\n"
        "\n"
        "[Warrior1]\n"
        "st Profile = \"pmwflash\"\n"
        "st Brain = \"PBrain\"\n"
        "l NumCells = 0\n"
        "l NumStaticVars = 0\n"
        "\n"
        "[Parts]\n"
        "ul NumParts = 1\n"
        "\n"
        "[Part1]\n"
        "ul ObjectNumber = 1305\n"
        "ul ControlType = 2\n"
        "b PlayerPart = TRUE\n"
        "c MyIcon = 0\n"
        "c TeamID = 0\n"
        "c CommanderID = 0\n"
        "ul Pilot = 1\n"
        "f PositionX = 0.000000\n"
        "f PositionY = 0.000000\n"
        "f Rotation = 0.000000\n"
        "l Active = 1\n"
        "l Exists = 1\n"
        "f Damage = 0.000000\n"
        "ul BaseColor = -16711936\n"
        "ul HighlightColor1 = -16760832\n"
        "ul HighlightColor2 = -16760832\n"
        "ul SelfRepairBehavior = 1\n"
        "ul ControlDataType = 1\n"
        "st ObjectProfile = \"PM207300\"\n"
        "st CSVFile = \"Werewolf\"\n"
        "ul VariantNumber = 0\n"
        "ul SquadNum = 1\n"
        "ul NumAlternatives = 0\n"
        "ul AlternativeStartIndex = 0\n"
        "l[15] IndicesOfAlternatives = -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,\n"
        "\n"
        "[Teams]\n"
        "\n"
        "[Commander0Group:0]\n"
        "l[12] Mates = 1,0,0,0,0,0,0,0,0,0,0,0,\n"
        "\n"
        "[NavMarkers]\n"
        "ul NumNavMarkers = 0\n"
        "\n"
        "[Artillery]\n"
        "l NumLargeStrikes = 0\n"
        "l NumSmallStrikes = 0\n"
        "l NumSensorStrikes = 0\n"
        "l NumCameraStrikes = 0\n"
        "\n"
        "[MissionSettings]\n"
        "st MissionName = \"\"\n"
        "b MissionNameUseResourceString = FALSE\n"
        "ul MissionNameResourceStringID = 0\n"
        "st Author = \"terrain_gen import\"\n"
        "st Blurb = \"\"\n"
        "b BlurbUseResourceString = FALSE\n"
        "ul BlurbResourceStringID = 0\n"
        "st Blurb2 = \"\"\n"
        "b Blurb2UseResourceString = FALSE\n"
        "ul Blurb2ResourceStringID = 0\n"
        "f TimeLimit = -1.000000\n"
        "f DropWeightLimit = 300.000000\n"
        "l ResourcePoints = 100000\n"
        "ul IsSinglePlayer = 1\n"
        "ul MaximumNumberOfTeams = 2\n"
        "ul MaximumNumberOfPlayers = 2\n"
        "uc scenarioTuneNum = 0\n"
        "st AVIFilename = \"\"\n"
        "l AdditionalCBills = 0\n"
        "ul NumRandomRPbuildings = 0\n"
        "\n"
        "[Weather]\n"
        "ul MaxRainDrops = 0\n"
        "f StartingRainLevel = 0.000000\n"
        "l ChanceOfRain = 0\n"
        "f BaseLighteningChance = 0.000000\n"
        "\n"
        "[TheSky]\n"
        "l SkyNumber = 1\n"
        "\n"
        "[Objectives Version]\n"
        "ul Version = 3\n"
        "\n"
        "[Team0Objectives]\n"
        "ul NumObjectives = 0\n"
        "\n"
        "[Team1Objectives]\n"
        "ul NumObjectives = 0\n"
        "\n"
        "[Player0]\n"
        "ul DefaultTeam = 0\n"
        "\n"
        "[Player1]\n"
        "ul DefaultTeam = 1\n"
        "\n"
        "[Music]\n"
        "uc scenarioTuneNum = 0\n"
        "\n"
        "[Script]\n"
        "st ScenarioScript = \"m0101\"\n"
        "FITend \n"
    )


def _write_fit(path: Path, burnin_name: str, recipe: TerrainRecipe,
               mission_ready: bool = False) -> None:
    """Write a minimal FitIniFile that the MC2 editor can open alongside a .pak.

    The editor (EditorData::initTerrainFromPCV) requires:
      - [ColorMap] / ColorMapName  -> tells terrtxm2 which burnin TGA to load
      - [Terrain] / UserMin+UserMax -> elevation range displayed in editor sliders

    Everything else (camera, warriors, objects) the editor builds fresh on first save.
    burnin_name is the stem only (no extension), e.g. "my_map" -> loads "my_map.burnin.tga".

    mission_ready=True additionally appends the minimal GAME-launchable mission
    skeleton (player part/commander/team/script -- see _mission_skeleton_blocks);
    without it the .fit is editor-only and Mission::init crashes with no
    commanders. Default False keeps the editor-import path byte-identical.
    """
    world_half = recipe.size * 128 * 0.5   # worldUnitsPerVertex=128
    content = (
        "FITini \n"
        "FITini \n"
        "\n"
        "[ColorMap]\n"
        f"s ColorMapName = \"{burnin_name}\"\n"
        "\n"
        "[Terrain]\n"
        f"l UserMin = {int(recipe.height.min_elevation)}\n"
        f"l UserMax = {int(recipe.height.max_elevation)}\n"
        f"f TerrainMinX = {-world_half:.6f}\n"
        f"f TerrainMinY = {world_half:.6f}\n"
    )
    if mission_ready:
        content += _mission_skeleton_blocks()
    path.write_text(content, encoding='latin-1')


def progress(pct: int, stage: str, msg: str = "") -> None:
    """Emit progress line (editor can parse `PROGRESS pct stage msg`)."""
    print(f"PROGRESS {pct} {stage} {msg}", flush=True)


# Quality presets. These resolve to the legacy flags/caps so the no-quality CLI
# path stays byte-for-byte identical to before (editor compat). Only applied when
# --quality is explicitly passed.
#   work_size: height working res (0 = full-res banded), erosion_cap: max passes,
#   final_cap / working_cap: burnin caps, full_res: use banded generator.
QUALITY_PRESETS: dict = {
    "preview":     dict(work_size=256, erosion_cap=0, final_cap=256,  working_cap=256,  full_res=False, shrink=True),
    "interactive": dict(work_size=512, erosion_cap=1, final_cap=4096, working_cap=1024, full_res=False, shrink=False),
    "standard":    dict(work_size=768, erosion_cap=99, final_cap=4096, working_cap=2048, full_res=False, shrink=False),
    "final":       dict(work_size=0,   erosion_cap=99, final_cap=4096, working_cap=2048, full_res=True,  shrink=False),
}


def main() -> None:
    parser = argparse.ArgumentParser(description='MC2 terrain generator')
    parser.add_argument('recipe', help='Path to recipe JSON file')
    parser.add_argument('--out', default='terrain_out', help='Output directory')
    parser.add_argument('--template-pak', help='Template .pak for Packet 0 patching (Phase B)')
    parser.add_argument('--preview', action='store_true',
                        help='Fast preview: render a small thumbnail only (no elevation/extras)')
    parser.add_argument('--debug-assets', action='store_true',
                        help='Write contact sheet and diagnostic PNGs')
    parser.add_argument('--interactive', action='store_true',
                        help='Interactive mode: lower work resolution + reduced erosion for editor')
    parser.add_argument('--height-work-size', type=int, default=256,
                        help='Generate height at this resolution, then upscale (0 = full resolution, 256 default for interactive)')
    parser.add_argument('--full-res', action='store_true',
                        help='Full-resolution banded generation (no upscaling, progress per band)')
    parser.add_argument('--superchunk-chunks', type=int, default=3,
                        help='Rows per band: chunk_size (20) * this value. Default 3 = 60 rows/band')
    parser.add_argument('--quality', choices=list(QUALITY_PRESETS),
                        help='Quality preset: preview|interactive|standard|final. '
                             'Resolves work-size/erosion/burnin caps. Omit = legacy behavior.')
    parser.add_argument('--burnin-final-cap', type=int, default=4096,
                        help='Hard cap on final burnin colormap dimension (default 4096).')
    parser.add_argument('--burnin-working-cap', type=int, default=0,
                        help='Burnin shading working res cap (0 = preset/default).')
    parser.add_argument('--burnin-format', choices=('jpg', 'tga', 'both'), default='both',
                        help='Colormap output: jpg (engine-preferred), tga (editor staging), '
                             'or both (default; keeps editor compat).')
    parser.add_argument('--burnin-quality', type=int, default=95,
                        help='JPEG quality for .burnin.jpg (default 95).')
    parser.add_argument('--cancel-file',
                        help='If this path appears mid-generation, abort cleanly '
                             '(exit 2, prints CANCELLED). Editor cancel button writes it.')
    parser.add_argument('--generate-foliage', action='store_true',
                        help='Emit {name}.foliage.json from PCG rules (recipe.foliage '
                             'or --foliage-rules). No rules = no file.')
    parser.add_argument('--foliage-rules',
                        help='JSON file of foliage rules (list or {"foliage":[...]}). '
                             'Overrides recipe.foliage.')
    parser.add_argument('--foliage-only', action='store_true',
                        help='Regenerate ONLY the foliage sidecar against the recipe '
                             'terrain (height+masks), skipping the burnin/elev/preview '
                             'outputs. Fast path for iterative in-editor foliage authoring. '
                             'Implies --generate-foliage.')
    args = parser.parse_args()

    # Resolve quality preset -> legacy flags/caps. Only when --quality is passed,
    # so the no-quality path is unchanged (editor compat). Explicit flags still win
    # where they are non-default.
    q = QUALITY_PRESETS.get(args.quality) if args.quality else None
    if q:
        if q["shrink"]:
            args.preview = True
        if q["full_res"]:
            args.full_res = True
        # height work size: preset value unless user overrode --height-work-size
        if args.height_work_size == 256:  # 256 = argparse default (not user-set)
            args.height_work_size = q["work_size"]
        args.interactive = args.interactive or (args.quality == "interactive")
        if args.burnin_final_cap == 4096:
            args.burnin_final_cap = q["final_cap"]
        if args.burnin_working_cap == 0:
            args.burnin_working_cap = q["working_cap"]
        args._erosion_cap = q["erosion_cap"]

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)

    print(f"Loading recipe: {args.recipe}")
    recipe = TerrainRecipe.from_json(args.recipe)

    # Fast preview: shrink to a small grid (snapped to the block size) so the
    # height/material/burnin pipeline runs quickly; we only emit the thumbnail.
    if args.preview:
        prev = max(40, min(recipe.size, 100))
        prev = (prev // 20) * 20
        recipe.size = prev
        recipe._burnin_final_cap = 256
        recipe._burnin_working_cap = 256

    print(f"  size={recipe.size}  biome={recipe.biome}  seed={recipe.seed}  preview={args.preview}")
    recipe.apply_biome()

    # Set burnin resolution caps. Preview already pinned them to 256 above; only set
    # when not already set so preview wins. CLI --burnin-*-cap override the defaults.
    if not hasattr(recipe, "_burnin_final_cap"):
        recipe._burnin_final_cap = args.burnin_final_cap
    if not hasattr(recipe, "_burnin_working_cap"):
        recipe._burnin_working_cap = args.burnin_working_cap or 2048

    # Quality erosion cap (final/standard = uncapped 99, interactive = 1, preview = 0)
    erosion_cap = getattr(args, "_erosion_cap", None)
    if erosion_cap is not None:
        recipe.height.erosion_passes = min(recipe.height.erosion_passes, erosion_cap)

    # Interactive mode: lower work resolution + reduced erosion for editor responsiveness
    if args.interactive:
        recipe._interactive = True
        recipe._height_work_size = args.height_work_size or 768
        recipe.height.erosion_passes = min(recipe.height.erosion_passes, 1)
    elif args.height_work_size > 0:
        recipe._height_work_size = args.height_work_size

    if args.preview:
        progress(5, "height", "preview")
        height = HeightGenerator().generate(recipe, progress=progress)
        progress(70, "classify", "starting")
        masks  = MaterialClassifier().classify(height, recipe)
        progress(85, "burnin", "rendering")
        burnin = BurninRenderer().render(masks, recipe, BIOMES[recipe.biome])
        progress(95, "saving", "")
        make_preview(burnin).save(str(out / f"{recipe.name}.preview.png"))
        progress(100, "done", "")
        print(f"  {recipe.name}.preview.png (preview)")
        print("Done.")
        return

    progress(0, "height", "starting")
    gen = HeightGenerator()

    # Cancellation: editor writes the cancel-file to abort a long generate. We check
    # between bands/octaves/erosion passes and exit 2 cleanly (no half-written outputs).
    cancel_check = None
    if args.cancel_file:
        def cancel_check():
            if os.path.exists(args.cancel_file):
                print("CANCELLED", flush=True)
                sys.exit(2)

    if args.full_res:
        # Full-resolution banded generation (no upscaling)
        height = gen.generate_fullres_banded(recipe, progress=progress,
                                             superchunk_chunks=args.superchunk_chunks,
                                             cancel_check=cancel_check)
    else:
        # Interactive or standard generation (may upscale)
        height = gen.generate(recipe, progress=progress, cancel_check=cancel_check)

    progress(76, "height", "done")

    progress(70, "classify", "starting")
    masks = MaterialClassifier().classify(height, recipe)
    progress(80, "classify", "done")

    name = recipe.name

    # --foliage-only: skip the burnin/elev/preview/fit outputs and go straight to
    # the foliage sidecar (the editor already has the terrain; only the trees
    # change). Implies --generate-foliage.
    foliage_only = getattr(args, 'foliage_only', False)
    if foliage_only:
        args.generate_foliage = True

    if not foliage_only:
        progress(85, "burnin", "rendering")
        burnin = BurninRenderer().render(masks, recipe, BIOMES[recipe.biome])
        progress(92, "burnin", "done")

        print(f"Saving to {out}/")

        # Primary colormap: JPEG q95 (terrtxm2 prefers <name>.burnin.jpg). Also keep
        # .burnin.tga for the editor staging path (EditorData.cpp initTerrainFromTGA),
        # until that path is migrated to jpg. --burnin-format controls which are written.
        fmt = args.burnin_format
        if fmt in ("jpg", "both"):
            _save_jpg(burnin, out / f"{name}.burnin.jpg", quality=args.burnin_quality)
            jpg_kb = (out / f"{name}.burnin.jpg").stat().st_size // 1024
            print(f"  {name}.burnin.jpg  ({burnin.size[0]}x{burnin.size[1]}, q{args.burnin_quality}, {jpg_kb} KB)")
        if fmt in ("tga", "both"):
            _save_tga(burnin, out / f"{name}.burnin.tga")
            print(f"  {name}.burnin.tga  ({burnin.size[0]}x{burnin.size[1]})")

        # Editor (Path B) elevation export: raw float32 world-unit elevations,
        # row-major (y outer, x inner) matching PostcompVertex order. The editor
        # reads this straight into setVertexHeight; no pak round-trip needed.
        # elevation = height[0,1] * max_elevation + min_elevation (same as PakExporter).
        h_p = recipe.height
        elev = (height.astype(np.float32) * np.float32(h_p.max_elevation)
                + np.float32(h_p.min_elevation)).astype('<f4')
        elev.tofile(str(out / f"{name}.elev.r32"))
        print(f"  {name}.elev.r32  ({recipe.size}x{recipe.size} float32)")

        make_preview(burnin).save(str(out / f"{name}.preview.png"))
        print(f"  {name}.preview.png")

        if args.debug_assets:
            make_contact_sheet(height, masks, burnin).save(str(out / "contact_sheet.png"))
            print(f"  contact_sheet.png")

            _save_gray(height,         out / "height.png")
            _save_gray(masks.slope,    out / "slope.png")
            _save_gray(masks.altitude, out / "altitude.png")
            _save_gray(masks.valley,   out / "valley.png")
            _save_terrain_type(masks.terrain_type, out / "terrain_type.png")
            print("  height.png  slope.png  altitude.png  valley.png  terrain_type.png")

            # MAP-FEATURE-MASKS-1 (A1): semantic feature maps.
            _save_gray(masks.curvature,      out / "curvature.png")
            _save_gray(masks.flow,           out / "flow.png")
            _save_gray(masks.ridge,          out / "ridge.png")
            _save_gray(masks.wetness,        out / "wetness.png")
            _save_gray(masks.shoreline,      out / "shoreline.png")
            _save_gray(masks.traversability, out / "traversability.png")
            print("  curvature.png  flow.png  ridge.png  wetness.png  "
                  "shoreline.png  traversability.png")

        recipe.to_json(out / f"{name}.recipe.json")
        print(f"  {name}.recipe.json")

    # Phase 4: PCG foliage sidecar. Sources rules from --foliage-rules (file) or
    # recipe.foliage; no rules -> no file. Independent of the elev/burnin contract.
    if args.generate_foliage:
        from terrain_gen.foliage_generator import FoliageGenerator, load_rules, write_foliage
        rules = load_rules(args.foliage_rules) if args.foliage_rules else recipe.foliage
        if not rules:
            print("  (no foliage rules -> no .foliage.json)")
        else:
            progress(92, "foliage", "placing")
            result = FoliageGenerator().generate(height, masks, recipe, rules,
                                                 superchunk_chunks=args.superchunk_chunks,
                                                 progress=progress)
            write_foliage(result, out / f"{name}.foliage.json")
            print(f"  {name}.foliage.json  ({len(result['instances'])} instances)")

    if args.template_pak:
        if not _PAK_AVAILABLE:
            print("WARNING: --template-pak provided but pak_exporter not available (Phase B). Skipping.")
        else:
            print(f"Patching pak from template: {args.template_pak}")
            exp  = PakExporter()
            pkt0 = exp.build_packet0(height, masks, recipe)
            exp.patch_pak(args.template_pak, str(out / f"{name}.pak"), pkt0)
            print(f"  {name}.pak")
            # Write companion .fit so the editor can open the new mission directly
            _write_fit(out / f"{name}.fit", f"{name}.burnin", recipe)
            print(f"  {name}.fit")

    print("Done.")


if __name__ == '__main__':
    main()
