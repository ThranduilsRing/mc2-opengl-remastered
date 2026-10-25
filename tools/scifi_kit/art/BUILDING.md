# Rebuilding an intent asset

Builders copied from `claude/kit-intent` at `556288c2d42ae4630ba1e088ca6a30dcd784f440`.
No baked models or review sheets are required in Git.

## Prerequisites

Use Python 3 (`py -3`) with `numpy`, `Pillow`, `scipy`, and `psutil`
(`py -3 -m pip install numpy Pillow scipy psutil`). Blender supplies its own
`bpy`, `bmesh`, `mathutils`, and NumPy. The reference Blender version is pinned
once in `run.py:BLENDER_VERSION`; the launcher warns if the executable differs.
Set `BLENDER_EXECUTABLE` to choose an install; otherwise the existing
`tools/blender_pipeline/blender_runner.py` searches standard installs and PATH.
The launcher reads the sibling `mc2-ops/JOBS` thread limit when present;
else set `MC2_BLENDER_THREADS` (default 4).

These scripts still need the local stock/reference data: decoded stock JSON in
`G:/mc2-assets/out/scifi-kit/round2/stock`, catalog in
`E:/mc2-archive-2026-10/asset-modernize/docs/assets/catalog.json`, and inventory
in `E:/mc2-ref/scifi-kit/mapping/inventory.csv`. Footprint checks also use
`E:/mc2-archive-2026-10/proofs/placement-survey` (see `cells.py`).
`tgl_decode.py` can extract stock from a game install; see its `--help`.
Material grain comes from the optional CC0 files listed in
`material_sources.py` under its `ROOT`; missing files fall back to procedural
materials, so matching reference textures requires those same files.
These input paths are currently local constants, not a portable asset bundle.

## One command

```powershell
py -3 tools/scifi_kit/art/intent_run.py bake smallfueltank --output-root E:/mc2-ref/my-assets
```

Without `--output-root`, output goes under `E:/mc2-ref/scifi-kit/art/intent-tools`.
Each asset gets `<root>/<stock-name>/pbr/`: pristine LOD0/1/2 GLBs, PNG atlases,
asset sidecars, and bake/LOD JSON. Jobs and `build.log` stay in the asset folder.
Repeated builds overwrite that asset's outputs; choose a fresh root to preserve
an older bake. No review renders, sheets, game launch, or C++ build is involved.

## Four Python patterns (worked example: smallfueltank)

1. **Spec:** `intent.py:ASSETS['smallfueltank']` connects the builder function,
   `palette='intent_tank'`, size class/budget, written intent, and expected
   feature counts. New names need a matching stock reference for checks and
   pivots. Family files `intent_*.py` update this same registry.
2. **Parts:** `smallfueltank(k)` uses `ind_parts.lathe` for the pad,
   `hbeam`/`angle_brace` for eight supports, and `vessel` for the tank.
   Dimensions are metres; Z is up, front is -Y. Add pieces to the supplied
   `geo.Kit`; use `k.at(...)` for local placement and `k.rng` for seeded choices.
   Call `count(k, 'tanks')` / `count(k, 'legs_supports', 8)` to record features.
3. **Palette:** add/reuse an entry in `lang.json:palettes`; pieces name material
   slots such as `paint.main`, `paint.steel`, and `hazard.wrap`. Keep geometry
   independent of colour. Shared parts supply cheaper `geo.GAME_MESH` carriers
   during baking; do not hand-edit the resulting GLB.
4. **Checks:** run `intent_run.py check smallfueltank` before baking. This checks
   stock dimensions/mass, counts, lint, access clearance and footprint. LOD
   generation checks silhouettes and preserves animated stock nodes. Preserve
   `anim_nodes` pivot metadata for gates and rotating spotlights. Fit factors
   in `assets/fit_scales.json` are build inputs; keep them with the source.
