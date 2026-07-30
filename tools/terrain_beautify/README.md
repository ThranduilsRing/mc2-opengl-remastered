# Terrain Beautify — Arc B (stock-mission beautification)

Non-destructive analysis + (future) beautification of **authored** stock missions.
Sibling to `tools/terrain_gen/` (Arc A: new-map procedural generation). Both share the
same feature-mask language (slope / curvature / cliff / flat / shoreline).

## Slices

| Slice | Status | What |
|---|---|---|
| **B1 MISSION-TERRAIN-ANALYZER-1** | ✅ DONE | read-only `.pak`+`.fit` → debug masks + report. No writes. |
| B4 TERRAIN-SLOPE-BIAS-VISUAL-1 | next | `gos_terrain.frag` slopeZ rock/cliff bias + veg suppress, default-OFF gate |
| B2 SIDECAR-FORMAT-1 | gated on B1 trust | baseHash + reversible delta package |
| B3 SMOOTH-PASS-1 | gated on B2 | protection-weighted height edit, applied via `pak_exporter.patch_pak` |

## B1 — mission_terrain_analyzer.py

```
python tools/terrain_beautify/mission_terrain_analyzer.py mc2_01 mc2_24 \
    [--missions-dir DIR] [--out tests/terrain/beautify]
```

Read-only. Emits per-mission `report.json` + `masks/*.png`:
height, slope, curvature, blockiness, water, shoreline, islands, pyramid_island_candidates,
cliff_candidates, flat_playable, roads_overlays, building_footprints, protected_hard, protected_soft.

### Verified format facts (the load-bearing recon — reuse for B2/B3)

- **PacketFile** (`mclib/packet.h`): magic `0xFEEDFACE`; entry = `type<<29 | offset`.
  Stock tier1 paks are all `RAW` (no LZD/ZLIB), but decoder handles both.
- **Heightfield** = packet 0, `side²·32` bytes, side ∈ {60,80,100,120}, 128 wu/vertex.
- **PostcompVertex** (`mclib/vertex.h`, 32 B): normal[0..11], **elevation@12 (float)**,
  **textureData@16** (hi16 = overlay handle, lo16 = base TXM), localRGBLight@20,
  terrainType@24, selected@28, water@29, shadow@30, highlighted@31.
- ⚠️ **`PostcompVertex.water` byte is NOT a clean bool** — it's a packed alpha
  ({0,1,64,65,128,129,192,193} on mc2_01). Treating nonzero as water gave a false
  92% / 78%. **Authoritative water = `elevation ≤ [Water].Elevation` from mission `.fit`**
  (`mclib/terrain.cpp:3740`). mc2_01 Elevation=350, mc2_24 Elevation=0.
- ⚠️ Per-vertex `terrainType` is dominated by a single code (2 on mc2_01) — not a usable
  water/material signal at the vertex level. Use elevation + overlay + slope instead.
- **Terrain objects packet** (`code/objmgr.cpp:1090` countTerrainObjects): `1 int count`
  then `count × 40 B` records = `objTypeNum(i), x,y,z(f), rotation(f), damage(i),
  teamId(i), parentId(i), 2 pad(i)`. mc2_01 → 1000 objs, mc2_24 → 2677.
- **World→grid** (`mclib/terrain.h:446`): `mapTopLeft = (-side·128/2, +side·128/2)`;
  `col=(x-tlx)/128`, `row=(tly-y)/128` (note Y flip).
- **Overlay enum** (`mclib/mapdata.h`): DIRT_ROAD=0 … RUNWAY=7 … OBRIDGE=14 …
  DAMAGED_BRIDGE=16. Detected via textureData hi16 ≠ {0, 0xFFFF}.

### B1 acceptance (all met)

- mc2_01 (100²) + mc2_24 (120²) load; objs 1000 / 2677 match registry counts.
- Waterline correct via `.fit` (not the broken vertex byte).
- Roads/buildings/bridges land on real authored features (road grid + city cluster visible
  in masks); pyramid detector flags 2 small isolated islands on mc2_01, 0 on land-map mc2_24.
- Read-only: only outputs under `--out` written. Deterministic (no RNG/timestamp in masks).

## Tunables (top of analyzer)
`CLIFF_SLOPE_DEG=35`, `FLAT_SLOPE_DEG=10`, `PYRAMID_MAX_AREA=600`, `BUILDING_FOOTPRINT_RADIUS=1`.

---

## Installing a cook into a deploy tree (READ THIS — cooks do not ship themselves)

A cook under `tests/terrain/beautify/` is **not** live. The engine reads sidecars
from `<deploy>/data/missions/<stem>.beauty/`, and `scripts/deploy_payload.py`
**deliberately never walks `data/missions/*.beauty/`** — it is a build-output
deployer, not a mission-content deployer. Installing is a separate, explicit act.

Every loader fails **silently** when its sidecar is absent: it `fopen()`s, misses,
prints one diagnostic line and carries on. `MC2_WATER_SHORE_MASK` is **default-ON**
and degrades to a complete no-op. Nothing crashes, nothing warns, and the feature
you just cooked simply is not there. This has already shipped once (v0.5.2 carried
zero sidecars).

### Do it

```
# audit every known lane
py -3 scripts/check-beauty-sidecars.py --all-lanes

# install everything cooked-and-committed but missing, into one tree
py -3 scripts/check-beauty-sidecars.py --target "A:/Games/mc2-opengl/mc2-win64-v0.4" --install

# after adding or re-cooking a sidecar, refresh the committed inventory
py -3 scripts/check-beauty-sidecars.py --write
```

Per-asset installers also exist (one file per call, useful when you want the
tool's own validation): `cook_shoreline.py`, `control_map_tool.py`,
`overlay_extract.py`, `cook_markings.py` — all take
`install --png <file> --deploy <dir> --mission <stem>`.

**`visual_height_4x.r32`, `visual_height_mips.r32`, `visual_damp.r32`,
`height_delta.r32` and `protected.r8` have no per-tool installer**
(`visual_heightfield.py` only writes to `--out`). Use
`check-beauty-sidecars.py --install`, or copy them by hand.

The last two are read by the **editor** (`editor/BeautySidecarPreview.cpp`),
which falls back to `tests/terrain/beautify/<stem>.beauty/` when the deploy copy
is absent. That fallback hides the gap in a dev worktree and then dies in a
shipped install, where no repo dir exists — so install them anyway.

### Gates

`scripts/check-beauty-sidecars.py` fails when a git-tracked cook is not installed
in an enforced lane; it is wired into `scripts/check-contracts.sh` (repo-only +
self-test halves — the live lane audit is machine-specific and run deliberately).
`scripts/build_release.sh` runs the same gate against `$DEPLOY` **before** writing
a byte, so a fresh release lane cannot ship empty again.

Its mirror image is `scripts/check-deploy-hygiene.py`, which fails on cook
*byproducts* (`*_preview.png`, `*_delta_heatmap.png`, `*_report.json`) that leaked
**into** a tree. Do not `cp -r` a whole cook dir into a deploy lane — that is how
diagnostics ship. Install the named runtime files only.

### Only what is COMMITTED counts

The install gate reads git-tracked cooks. A bake sitting untracked in a scratch
worktree is work in progress, not a shipping obligation — commit it first, or it
will never be installed and nothing will tell you.
