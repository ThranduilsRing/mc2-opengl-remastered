# M2d: Overlay Fast-Path Extension (Design + Handoff)

> Status: **SHIPPED 2026-04-29.** Result exceeded predictions: `Terrain::render drawPass` dropped from ~5-6 ms (post-M2c) to **1.46 ms** at max zoom out on mc2_01 vs. the spec's "~3-4 ms" target. `fast=14000 legacy=0` every frame post-warmup; all in-frustum quads now enter the fast path. Tier1 smoke 5/5 PASS, +0 destroys delta. See `memory/m2_thin_record_cpu_reduction_results.md` for the consolidated record.
>
> The remainder of this document is the original design sketch, kept for historical reference.
>
> **Implementation correction:** spec sketch's `wov_corner[c].wz = vertices[c]->pVertex->elevation` is wrong — must add `+ OVERLAY_ELEV_OFFSET` (0.15f) to match `setOverlayWorldCoords`. Without the offset, overlays z-fight the base terrain.
>
> **Parity-log gotcha:** Gate C's `event=thin_record_parity match=1` is **silent on success** — `gos_terrain_patch_stream.cpp:1461-1462` only prints when parity fails. Validation rule: absence of `event=thin_record_parity` lines = pass.

## Goal

Allow quads with `overlayHandle != 0xffffffff` (MC2's per-quad **base-texture overlay**, NOT the GPU shader's detail-normal layer) to enter the M2 thin-record fast path instead of falling through to legacy. After M2b/c/c-ext, ~4,700 quads/frame still take legacy because the `g_noOverlay` gate excludes them. They're the last big population that can be migrated without restructuring.

## Background — WHAT `overlayHandle` ACTUALLY IS

This was a source of confusion in the M2c session. Definitions:

| Concept | Where it lives | What it is |
|---|---|---|
| **MC2 `overlayHandle`** (THIS spec's target) | `TerrainQuad::overlayHandle` (per-quad data) | A SECOND terrain texture handle used for splat-blending (cement/transition/special tiles). Set from `Terrain::terrainTextures->getTextureHandle(...)` at quad.cpp:363. Rendered via `gos_PushTerrainOverlay` in legacy. |
| **MC2 `terrainDetailHandle`** (M2c's target) | `TerrainQuad::terrainDetailHandle` (per-quad data) | The water-interest detail texture (shorelines, beach foam). Rendered via `mcTextureManager->addVertices(handle, ..., MC2_DRAWALPHA)` in legacy. |
| **GPU detail-normal** (NOT a CPU concern) | Fragment shader, `matNormal0..3` uniforms | A universal high-frequency surface bump/normal map applied to ALL terrain in the splatting fragment shader. Has nothing to do with MC2's per-quad fields. |

When the user says "the terrain has a detail overlay", they usually mean the **GPU detail-normal** (universal). The 4,700 legacy quads have **MC2 `overlayHandle`** (per-quad).

## Architecture

Mirror of M2c's structure, swapping `addVertices(MC2_DRAWALPHA)` for `gos_PushTerrainOverlay`:

1. **Drop `g_noOverlay` from `fastPathEligible`** in quad.cpp.
2. **Inside the fast-path branch, after the M2c detail emit, add an inline overlay emit** if `useOverlayTexture && overlayHandle != 0xffffffff`. Build `WorldOverlayVert wov[3]` per triangle directly from `vertices[c]` (no intermediate `gVertex`).
3. **Quads with neither base nor detail nor overlay** are still loop-hoisted in `Terrain::render` — no change needed there.

## Reference: legacy overlay emit

Currently at `quad.cpp:1820-1858` (BOTTOMRIGHT tri1) and three sibling locations (BOTTOMRIGHT tri2, BOTTOMLEFT tri1, BOTTOMLEFT tri2). The legacy code:

```cpp
if (useOverlayTexture && (overlayHandle != 0xffffffff))
{
    gos_VERTEX oVertex[3];
    memcpy(oVertex, gVertex, sizeof(gos_VERTEX) * 3);
    oVertex[0].u = oldminU; oVertex[0].v = oldminV;
    oVertex[1].u = oldmaxU; oVertex[1].v = oldminV;
    oVertex[2].u = oldmaxU; oVertex[2].v = oldmaxV;
    oVertex[0].argb = vertices[0]->lightRGB;
    oVertex[1].argb = vertices[1]->lightRGB;
    oVertex[2].argb = vertices[2]->lightRGB;
    setOverlayWorldCoords(oVertex[0], vertices[0]);
    setOverlayWorldCoords(oVertex[1], vertices[1]);
    setOverlayWorldCoords(oVertex[2], vertices[2]);

    WorldOverlayVert wov[3];
    for (int _k = 0; _k < 3; ++_k) {
        wov[_k].wx = oVertex[_k].x;
        wov[_k].wy = oVertex[_k].y;
        wov[_k].wz = oVertex[_k].z;
        wov[_k].u  = oVertex[_k].u;
        wov[_k].v  = oVertex[_k].v;
        wov[_k].fog  = (float)((oVertex[_k].frgb >> 24) & 0xFF) / 255.0f;
        wov[_k].argb = oVertex[_k].argb;
    }
    const DWORD overlayTexId = tex_resolve(overlayHandle);
    if (overlayTexId != 0)
        gos_PushTerrainOverlay(wov, overlayTexId);
}
```

`setOverlayWorldCoords` rewrites `oVertex.x/y/z` from `vertices[c]` world-space (since `gos_PushTerrainOverlay` takes world coords, not screen). So we don't actually need `gVertex` for this — we can build `wov` directly.

## Fast-path inline (sketch)

Insert after the M2c detail emit block, before `return;`:

```cpp
// M2d: overlay emit (mirrors legacy 1820-1858 / siblings).
if (useOverlayTexture && overlayHandle != 0xffffffff)
{
    const DWORD overlayTexId = tex_resolve(overlayHandle);
    if (overlayTexId != 0)
    {
        // Build per-corner world-space overlay vertex data directly from vertices[c].
        WorldOverlayVert wov_corner[4];
        for (int c = 0; c < 4; c++) {
            // setOverlayWorldCoords inlined — see its definition for the world-coord mapping.
            // Likely just: vertices[c]->vx/vy + elevation + axis swap. Verify against
            // setOverlayWorldCoords body before implementing.
            wov_corner[c].wx = /* world x for vertices[c] */;
            wov_corner[c].wy = /* world y for vertices[c] */;
            wov_corner[c].wz = vertices[c]->pVertex->elevation;
            // UV is fixed per-corner — same oldminU/oldmaxU/oldminV/oldmaxV pattern as legacy.
            wov_corner[c].fog  = (float)((vertices[c]->fogRGB >> 24) & 0xFF) / 255.0f;
            wov_corner[c].argb = vertices[c]->lightRGB;
            // u/v assigned per triangle below using oldminU/maxU pattern.
        }

        // Triangle assembly: TOPRIGHT tri1=[0,1,2] tri2=[0,2,3] (UVs minmin, maxmin, maxmax, minmax)
        // BOTTOMLEFT tri1=[0,1,3] tri2=[1,2,3] (UVs same corner→UV mapping)
        if (uvMode == BOTTOMLEFT) {
            if (pzTri1) {
                WorldOverlayVert wov[3] = { wov_corner[0], wov_corner[1], wov_corner[3] };
                wov[0].u=oldminU; wov[0].v=oldminV;
                wov[1].u=oldmaxU; wov[1].v=oldminV;
                wov[2].u=oldminU; wov[2].v=oldmaxV;
                gos_PushTerrainOverlay(wov, overlayTexId);
            }
            if (pzTri2) {
                WorldOverlayVert wov[3] = { wov_corner[1], wov_corner[2], wov_corner[3] };
                wov[0].u=oldmaxU; wov[0].v=oldminV;
                wov[1].u=oldmaxU; wov[1].v=oldmaxV;
                wov[2].u=oldminU; wov[2].v=oldmaxV;
                gos_PushTerrainOverlay(wov, overlayTexId);
            }
        } else {
            // BOTTOMRIGHT (TOPRIGHT diagonal)
            if (pzTri1) {
                WorldOverlayVert wov[3] = { wov_corner[0], wov_corner[1], wov_corner[2] };
                wov[0].u=oldminU; wov[0].v=oldminV;
                wov[1].u=oldmaxU; wov[1].v=oldminV;
                wov[2].u=oldmaxU; wov[2].v=oldmaxV;
                gos_PushTerrainOverlay(wov, overlayTexId);
            }
            if (pzTri2) {
                WorldOverlayVert wov[3] = { wov_corner[0], wov_corner[2], wov_corner[3] };
                wov[0].u=oldminU; wov[0].v=oldminV;
                wov[1].u=oldmaxU; wov[1].v=oldmaxV;
                wov[2].u=oldminU; wov[2].v=oldmaxV;
                gos_PushTerrainOverlay(wov, overlayTexId);
            }
        }
    }
}
```

## Open questions to resolve in implementation

1. **`setOverlayWorldCoords` body.** Read this function before writing the inline. The world-coord mapping needs to match exactly. Likely just an axis swap + elevation but need to verify.
2. **UV mapping per uvMode.** Legacy assigns UVs to gVertex BEFORE the overlay block, so the gVertex.u/v at the overlay block are the PRIMARY-texture UVs (which the overlay then OVERWRITES with oldminU/oldmaxU/etc.). Trace that the corner→UV mapping in my sketch matches what gVertex held at the overlay-block entry for each tri/uvMode combination. Risk: easy to get the corner-3 vs corner-2 swap wrong, especially for BOTTOMLEFT.
3. **Does overlay always need both triangles, or can it skip pz-culled ones?** Legacy gates the overlay block on `pzTri1` for tri1 and `pzTri2` for tri2 (inside the per-tri code blocks). Mirror that.
4. **Fast path eligibility once g_noOverlay is dropped.** Will become: `g_active && g_thin && g_ready && g_notOver && (g_handle || !g_noWater || hasOverlay)`. The `(g_handle || !g_noWater)` from M2c-ext extends to `(g_handle || !g_noWater || !g_noOverlay)`. Or simpler: drop the entire `g_noOverlay` exclusion since "has overlay" is now a valid fast-path case.

## Validation gates

Mirror M2c's gates:
- **A: Visual parity** — screenshot mc2_01 transition zones (cement/concrete edges) before/after. Overlay misalignment shows as wrong-blend at tile borders.
- **B: Tracy timing** — `Terrain::render drawPass` should drop from ~5-6 ms to ~3-4 ms. `Quad.legacy` count should fall from ~4,700 to a few hundred (pz-culled edge cases only).
- **C: Console parity** — `[PATCH_STREAM v1] event=thin_record_parity match=1` must hold.
- **D: tier1 smoke** — standard regression gate.

## Estimated win

~1.5-2 ms additional drawPass reduction. Smaller than M2c (which moved 5,800 quads with TWO emits each — thin record + detail) because overlay quads only do ONE emit (no thin record needed when terrainHandle==0). But meaningful because we collapse the legacy gVertex setup entirely for these quads.

## What this does NOT do

- Does NOT touch `Terrain::renderWater()` — separate work, large architectural change (water-to-GPU).
- Does NOT touch `quadSetupTextures` — separate per-quad pass, same cost class, same playbook applies but is its own session.
- Does NOT introduce new env vars. M2d is unconditionally on once shipped, gated under existing `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH`.

---

# Handoff Prompt for Fresh Session

```
Continue M2 thin-record CPU reduction work. M2b/c/c-ext shipped, dropping
Terrain::render drawPass from ~25ms to ~5-6ms on water-heavy maps (mc2_01,
80-90 fps). The remaining ~4,700 legacy quads/frame are quads with
`overlayHandle != 0xffffffff` — MC2's per-quad base-texture overlay
(NOT the GPU shader's detail-normal — those are different concepts).

Implement M2d-overlay per spec at:
  docs/superpowers/specs/2026-04-29-m2d-overlay-fast-path-design.md

Background context lives at:
  memory/m2_thin_record_cpu_reduction_results.md

Before you start, READ these to confirm conventions:
  - mclib/quad.cpp:1820-1858 (legacy overlay emit, BOTTOMRIGHT tri1) and the
    3 sibling locations (BOTTOMRIGHT tri2, BOTTOMLEFT tri1+tri2)
  - The body of `setOverlayWorldCoords` (search the codebase)
  - mclib/quad.cpp around line 1810 (current fast-path eligibility + thin
    record + M2c detail emit) — this is where M2d lives
  - The M2c spec at docs/superpowers/specs/2026-04-29-m2c-water-interest-fast-path-design.md
    (sibling pattern; M2d is a near-mirror)

Build/deploy uses the standard worktree CMake pattern; see CLAUDE.md.
Run with these env vars to activate the fast path:
  set MC2_PATCHSTREAM_THIN_RECORDS=1& set MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1& set MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1& "A:\Games\mc2-opengl\mc2-win64-v0.2\mc2.exe"

Add MC2_THIN_DEBUG=1 to see path_mix counter output (silent by default,
prints 5 frames after warmup).

Validation gates A-D in the spec. Visual parity is the canary — overlay
misalignment shows at tile borders (cement/concrete transitions).

Expected win: ~1.5-2ms drawPass drop. If much less, investigate whether
WorldOverlayVert UV mapping matches legacy exactly per uvMode.
```
