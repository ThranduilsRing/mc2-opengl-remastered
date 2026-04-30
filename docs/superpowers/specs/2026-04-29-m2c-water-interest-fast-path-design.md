# M2c: Water-Interest Fast-Path Extension (Design)

> Status: design sketch — not yet implemented. Sibling to M2b (pure-water early-exit, deployed).

## Goal

Allow water-interest tiles (quads with `useWaterInterestTexture && terrainDetailHandle != 0xffffffff`) to be emitted from the M2 thin-record fast path instead of falling through to the legacy `TerrainQuad::draw()` path. On water-heavy maps (e.g. mc2_01 at max zoom) these are **~71% of all quads** (~10,000 per frame) — the dominant remaining cost in the `Terrain::render drawPass` Tracy zone after M2b.

## Why this is the right next slice

After M2b's pure-water early-exit, the per-quad cost distribution at max zoom is:

| Bucket                  | Quads/frame | Path           |
|-------------------------|-------------|----------------|
| Fast (M2)               | ~80–160     | fast emit only |
| Pure water (skipped M2b)| ~3,000      | early-return   |
| Water-interest detail   | **~10,000** | **legacy**     |
| Overlay                 | ~120        | legacy         |
| (Other terrain)         | ~few hundred| legacy         |

Of the legacy work for water-interest quads, what actually emits is **the thin record** (via `appendThinRecord` at quad.cpp:2219) plus **the detail-texture draw** (`addVertices(terrainDetailHandle, sVertex, MC2_DRAWALPHA)`). Everything else — `gVertex[3]` setup for both triangles, `isCement`/`isAlpha` lookups consumed only for the ARGB override, the `pzTri` re-check from `gVertex[].z`, the `appendQuad` and `appendQuadRecord` calls (both no-ops with thin records on per gos_terrain_patch_stream.cpp:712 and the `s_quadRecordsOn` gate) — is wasted.

The fast path already emits the thin record. We just need it to also emit the detail draw for these quads.

## Architecture

Two changes that compose:

1. **Drop the `g_noWater` gate in `fastPathEligible`** (quad.cpp ~1685). Water-interest quads now enter the M2 branch.
2. **Inline a streamlined detail-quad emit** inside the fast path, after `appendThinRecordDirect`, only for quads with `useWaterInterestTexture && terrainDetailHandle != 0xffffffff`. Builds `sVertex[3]` directly from `vertices[c]` — no intermediate `gVertex` setup.

Quads with `useOverlayTexture && overlayHandle != 0xffffffff` continue to fall through to legacy (overlay path is small, ~1% of quads, and shares more state with the legacy alpha pass than is worth replicating right now).

## Streamlined Detail Emit

Per corner the legacy `sVertex[]` is built by copying `gVertex` and then overwriting most fields. The actually-needed inputs from `vertices[c]` are:

| sVertex field | Source                                                                 |
|---------------|------------------------------------------------------------------------|
| .x            | `vertices[c]->px`                                                      |
| .y            | `vertices[c]->py`                                                      |
| .z            | `vertices[c]->pz + TERRAIN_DEPTH_FUDGE`                                |
| .rhw          | `vertices[c]->pw`                                                      |
| .u            | `(vertices[c]->vx - Terrain::mapTopLeft3d.x) * oneOverTf`              |
| .v            | `(Terrain::mapTopLeft3d.y - vertices[c]->vy) * oneOverTf`              |
| .argb         | `Terrain::terrainTextures2 ? 0xFFFFFFFFu : lightRGBc(c)`               |
| .frgb         | `(vertices[c]->fogRGB & 0xFFFFFF00) | terrainTypeToMaterial(...)`      |

`oneOverTf` is computed once per quad:
```cpp
float tilingFactor = Terrain::terrainTextures2
    ? Terrain::terrainTextures2->getDetailTilingFactor()
    : Terrain::terrainTextures->getDetailTilingFactor(1);
float oneOverTf = tilingFactor / Terrain::worldUnitsMapSide;
```

UV out-of-range adjustment (preserves existing behavior at quad.cpp:1882-1903) is applied per triangle, not per quad.

## Pseudocode

Insert this block in quad.cpp at the end of the fast-path branch, after `appendThinRecordDirect(tr);` and before the `return;`:

```cpp
// M2c: water-interest detail emit. Replicates legacy 1862-1914 / equivalent
// BOTTOMLEFT block, but builds sVertex directly from vertices[c] instead of
// memcpy'ing gVertex and overwriting most fields.
if (useWaterInterestTexture && terrainDetailHandle != 0xffffffff)
{
    float tilingFactor = Terrain::terrainTextures2
        ? Terrain::terrainTextures2->getDetailTilingFactor()
        : Terrain::terrainTextures->getDetailTilingFactor(1);
    float oneOverTf = tilingFactor / Terrain::worldUnitsMapSide;

    // Build all 4 corners once (shared between tri1 and tri2).
    gos_VERTEX corner[4];
    for (int c = 0; c < 4; c++) {
        corner[c].x    = vertices[c]->px;
        corner[c].y    = vertices[c]->py;
        corner[c].z    = vertices[c]->pz + TERRAIN_DEPTH_FUDGE;
        corner[c].rhw  = vertices[c]->pw;
        corner[c].u    = (vertices[c]->vx - Terrain::mapTopLeft3d.x) * oneOverTf;
        corner[c].v    = (Terrain::mapTopLeft3d.y - vertices[c]->vy) * oneOverTf;
        corner[c].argb = Terrain::terrainTextures2
                       ? 0xFFFFFFFFu
                       : lightRGBc(c);
        corner[c].frgb = (vertices[c]->fogRGB & 0xFFFFFF00u)
                       | terrainTypeToMaterial(vertices[c]->pVertex->terrainType);
    }

    auto clampUVs = [](gos_VERTEX* tri) {
        if (tri[0].u > MaxMinUV || tri[0].v > MaxMinUV ||
            tri[1].u > MaxMinUV || tri[1].v > MaxMinUV ||
            tri[2].u > MaxMinUV || tri[2].v > MaxMinUV) {
            float maxU = fmax(tri[0].u, fmax(tri[1].u, tri[2].u));
            maxU = floor(maxU - (MaxMinUV - 1.0f));
            float maxV = fmax(tri[0].v, fmax(tri[1].v, tri[2].v));
            maxV = floor(maxV - (MaxMinUV - 1.0f));
            tri[0].u -= maxU; tri[1].u -= maxU; tri[2].u -= maxU;
            tri[0].v -= maxV; tri[1].v -= maxV; tri[2].v -= maxV;
        }
    };

    // Triangle assembly mirrors the thin-record uvMode/pzTri rules already used
    // by the fast path's flag bits (and by gos_terrain_thin.vert's cornerIdx table).
    if (uvMode == BOTTOMLEFT) {
        if (pzTri1) {
            gos_VERTEX tri[3] = { corner[0], corner[1], corner[3] };
            clampUVs(tri);
            mcTextureManager->addVertices(terrainDetailHandle, tri,
                                          MC2_ISTERRAIN | MC2_DRAWALPHA);
        }
        if (pzTri2) {
            gos_VERTEX tri[3] = { corner[1], corner[2], corner[3] };
            clampUVs(tri);
            mcTextureManager->addVertices(terrainDetailHandle, tri,
                                          MC2_ISTERRAIN | MC2_DRAWALPHA);
        }
    } else {
        // BOTTOMRIGHT (= TOPRIGHT diagonal)
        if (pzTri1) {
            gos_VERTEX tri[3] = { corner[0], corner[1], corner[2] };
            clampUVs(tri);
            mcTextureManager->addVertices(terrainDetailHandle, tri,
                                          MC2_ISTERRAIN | MC2_DRAWALPHA);
        }
        if (pzTri2) {
            gos_VERTEX tri[3] = { corner[0], corner[2], corner[3] };
            clampUVs(tri);
            mcTextureManager->addVertices(terrainDetailHandle, tri,
                                          MC2_ISTERRAIN | MC2_DRAWALPHA);
        }
    }
}
```

## What this saves

For ~10,000 water-interest quads per frame at max zoom:

- Eliminates ~60-store `gVertex[3]` setup × 2 triangles (currently in legacy 1755-1916 and equivalent BOTTOMLEFT block)
- Eliminates duplicated `isCement`/`isAlpha` reads (already happens in fast path's `g_handle` decision; legacy reads them again)
- Eliminates redundant `pzTri` re-derivation from `gVertex[].z` (fast path already computed `pzTri1`/`pzTri2` from `vertices[c]->pz` directly)
- Eliminates the legacy thin-record recipe rebuild at quad.cpp:2219 — fast path's lazy recipe cache is reused
- Eliminates the no-op `appendQuad` and `appendQuadRecord` Tracy-zone overhead (~14 ns/call × 10,000 = 140 μs)

Estimated drawPass improvement at max zoom: **~10-12 ms** (drop from ~20 ms post-M2b to ~10 ms). Not asymptotic — there's still per-quad work for ~13,000 active quads, mostly in `addVertices` and the per-quad memory chases — but a meaningful step.

## Risks & open questions

1. **Visual parity for water-interest tiles.** The legacy path's `gVertex[].argb` for the detail draw is `0xFFFFFFFFu` if `terrainTextures2`, else uses the corner's `lightRGB` (without the `selected` override). The sketch above uses `lightRGBc(c)` which DOES apply `selected → SELECTION_COLOR`. Need to decide:
   - **Option A:** match legacy exactly (no `selected` override on detail tiles). Visual canary: select a unit on a water-interest tile, see if its tile pulses red in legacy vs M2c.
   - **Option B:** apply `selected` override on detail too (small visual change, arguably more correct).
   - Recommend Option A for parity-first verification, switch to B later if desired.

2. **`isCement`/`isAlpha` not consulted by the new path.** Legacy reads them for the solid pass's ARGB override only, which is skipped under fast-path-active anyway. Confirmed not needed for the detail emit.

3. **`MC2_DRAWALPHA` flag.** The legacy detail call uses `MC2_ISTERRAIN | MC2_DRAWALPHA` (line 1913). Mirrored in the sketch. Verify there's no flag drift from the BOTTOMLEFT block — they should be identical.

4. **Tile clamp behavior.** Legacy applies the UV clamp inside the per-triangle `if (...UV > MaxMinUV)` block. The sketch applies it the same way per triangle. Should match exactly.

5. **Performance of `mcTextureManager->addVertices` itself.** This call is still made — the saving is in pre-call setup, not in the call itself. If `addVertices` is the dominant cost (e.g. virtual dispatch + `std::vector` push_back), this fix alone won't close the gap and we'd need to attack `addVertices` next.

## Validation gates

After implementation:

**Gate A — visual parity.** Screenshot mc2_01 at max zoom in two configurations:
- Pre-M2c: current build (fast path with `g_noWater` gate intact)
- Post-M2c: with the gate removed and detail emit inlined

Diff the screenshots. Water-interest tile borders are the canary — if UV clamp is wrong they'll show wrap seams.

**Gate B — Tracy timing.** `Terrain::render drawPass` at max zoom. Expected:
- Before: ~20 ms (post-M2b, 25 ms originally)
- After: ~10-12 ms

If the drop is less than ~5 ms, the cost is in `addVertices` itself (see Risk 5).

**Gate C — path_mix counter.** Post-warmup `path_mix` log line should show:
- `fast` count jumped by ~10,000 (from ~80 to ~10,080)
- `fail_water` dropped to ~0
- `fail_handle0` and `fail_overlay` unchanged

**Gate D — parity log.** `[PATCH_STREAM v1] event=thin_record_parity match=1` must still hold. If `match=0` appears, the new fast-path emits don't agree with the legacy emit count for the same quads — revert and investigate.

**Gate E — tier1 smoke.** Standard regression gate.

## What this does NOT do

- Does **not** address `addVertices` overhead (still called per-quad for the detail texture). If that's the next bottleneck, M2d would be a per-bucket batched detail-quad emit (similar pattern to thin records: an SSBO of detail records the GPU expands).
- Does **not** address the `fail_overlay` ~120 quads. They stay legacy; the savings there would be small.
- Does **not** change the M2b pure-water early-exit (those quads still return immediately at the top of `TerrainQuad::draw()`).
- Does **not** introduce a new env var. M2c is unconditionally on once implemented; the existing `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH` gate covers all M2 family.

## Implementation order

1. Apply the `g_noWater` drop and the inline detail emit (one focused commit).
2. Run Gates A → B → C → D → E in order.
3. If all pass, demote the `MC2_THIN_DEBUG` path_mix counter to silent (still gated, just set a flag default for it).
4. If Gate B falls short, file an M2d ticket to batch the detail emits (the same SSBO trick we used for the thin record).
