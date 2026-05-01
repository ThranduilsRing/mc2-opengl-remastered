# Indirect Terrain Draw — Recon Handoff

**Date:** 2026-04-30
**Predecessor:** `docs/superpowers/specs/2026-04-30-indirect-terrain-draw-design.md`
**Brainstorm:** `docs/superpowers/brainstorms/2026-04-30-indirect-terrain-draw-scope.md`
**Successor:** implementation-plan session (Stage 1 dense recipe + Stage 2 indirect draw, two-PR promotion)

## TL;DR

All 9 recon items resolved. **Item 1** surfaces a meaningful spec amendment: indirect-terrain hooks AT `Render.TerrainSolid` INSIDE `renderLists()` (replacing `TerrainPatchStream::flush()` at txmmgr.cpp:1330), NOT post-renderLists like water — terrain writes the depth buffer that everything later in renderLists depends on. **Item 3** collapses cleanly: `setOverlay`/`setOverlayTile` already route through `MapData::setTerrain` → `invalidateTerrainFaceCache()`; no new chokepoint needed. **Item 9** confirms a real parity gap — the existing M2 thin VS at `gos_terrain_thin.vert:137` is missing `+ TERRAIN_DEPTH_FUDGE` that the legacy emit applies at all 16 sites in `mclib/quad.cpp`; recommendation is to add it (precedent: water fast VS in commit `bc8c4f1`). Item 4 is partially-resolved (no on-disk mission file inspection possible — they're in FST archive); the spec's defensive-hooks-plus-parity-check posture handles the unknown.

## Item-by-item findings

### Item 1 — Render-order placement
**Status:** Resolved with spec amendment.
**Finding:** `renderLists()` is NOT a single bucket-flush. It interleaves render order:
1. `Render.3DObjects` (txmmgr.cpp:1102-1162) — mechs/vehicles via `masterHardwareVertexNodes`, BEFORE terrain.
2. Static + dynamic shadow passes (txmmgr.cpp:1180-1293).
3. **`Render.TerrainSolid`** (txmmgr.cpp:1297-1406) — current M2 fast path lives here: `if (TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) modernHandled = TerrainPatchStream::flush();`. The legacy DRAWSOLID loop at :1335-1397 follows, with the `if (modernHandled && MC2_ISTERRAIN) continue;` skip at :1340-1343.
4. `Render.GpuStaticProps` (txmmgr.cpp:1414-1421) — buildings via `GpuStaticPropBatcher::flush()`.
5. `Render.TerrainOverlays` + `Render.Decals` (txmmgr.cpp:1429-1438) — world-space cement/decals.
6. `Render.Overlays` (txmmgr.cpp:1442-1520) — DRAWALPHA water + waterDetail buckets (legacy non-water DRAWALPHA filtered out at :1471-1475).
7. `Render.NoUnderlayer` (txmmgr.cpp:1526-1583) — GPUOVERLAY terrain.
8. Shadow legacy buckets (txmmgr.cpp:1590-1644) — DRAWALPHA + ISSHADOWS.

**Implementation impact:** indirect-terrain hooks AT `Render.TerrainSolid` (txmmgr.cpp:1330), replacing or paralleling the `TerrainPatchStream::flush()` call. **Do NOT** copy renderWater's post-renderLists pattern — water can move out of `renderLists` because it reads depth and alpha-blends; terrain WRITES depth and everything later in renderLists (GpuStaticProps, decals, DRAWALPHA water-on-terrain) depth-tests against it. Spec line 142-156 ("the new path replaces the terrain main-emit buckets currently drained inside `renderLists()`") was correct; recon confirms the exact insertion site is inside `Render.TerrainSolid` zone, post-flush of `Render.3DObjects`. Bridge function follows the M2 flush pattern (`TerrainPatchStream::flush()` at gos_terrain_patch_stream.cpp).

**Spec amendment:** Add a paragraph clarifying "AT `Render.TerrainSolid`" (replacing `TerrainPatchStream::flush()`), distinct from water's post-renderLists hook. The brainstorm Q1 open follow-up is now resolved; the bullet at brainstorm.md:113-119 should be marked closed.

### Item 2 — Thin-record format coverage
**Status:** Resolved.
**Finding:** Audit of `TerrainQuad::setupTextures` body (quad.cpp:429-661) and the M2 fast-path emit (quad.cpp:1756-1849) against current `TerrainQuadRecipe` (gos_terrain_patch_stream.h:87-97, 144B) and `TerrainQuadThinRecord` (h:103-109, 32B):

| Per-frame state | Currently in | Indirect-terrain status |
|---|---|---|
| terrainHandle (slot index) | thin record (4B) — resolved per frame via `tex_resolve()` at flush | Keep in thin record |
| lightRGB[4] | thin record (16B) — selection + alphaOverride pre-merged at build | Keep in thin record |
| uvMode | thin record flags bit 0 | Keep in thin record |
| pzTri1Valid / pzTri2Valid | thin record flags bits 1-2 | Keep in thin record |
| 4 corner positions + normals + UV extents | recipe (128B + 16B) | Keep in recipe (mostly static) |
| 4 corner terrain types (material IDs) | recipe._wp0 packed | Keep in recipe |
| **overlayHandle** | NOT in thin record OR recipe; computed in setupTextures, used at draw | **GAP — needs to be added.** Recommend: add to recipe (mostly static; mutates only via setOverlay→setTerrain→invalidateRecipeFor). |
| **terrainDetailHandle** | NOT in thin record OR recipe; computed in setupTextures | **GAP — recommend recipe.** Mostly static (water-interest blend texture chosen by terrainTextures->setDetail(1,0); changes rarely). |
| **isCement** | NOT cached; looked up at thin-record build via `Terrain::terrainTextures->isCement(...)` | **GAP — recommend recipe bit.** Static per quad. |
| **alphaOverride** | NOT cached; computed at thin-record build (isCement + isAlpha + terrainTextures2 presence) | **Derivable from isCement bit + isAlpha bit + global** — no new field needed if isAlpha bit is added to recipe. |
| mineState / mineResult | per slice 2b: cached on the recipe entry | Keep cached on recipe; invalidated by setMine chokepoint |
| **selection state** | per-vertex `vertices[c]->pVertex->selected` — mutates per gameplay frame | Already inline-merged into `lightRGB` at thin-record build (quad.cpp:1793). Pattern works under indirect-terrain too. |
| waterHandle / waterDetailHandle | separate water path (renderWater Stage 2/3) | OUT OF SCOPE for indirect-terrain |

**Implementation impact:** Recipe SSBO grows by ~12-16 B/record to absorb overlayHandle (4B), terrainDetailHandle (4B), and a packed-flags DWORD (isCement, isAlpha, has-overlay, has-detail). Total recipe ~160 B at largest (was 144 B). At 256² stock map, ~10.5 MB; at 384² Wolfman max ~24 MB. Still well under any sensible budget. Thin record stays 32 B unchanged. Per-frame work in setupTextures collapses to: lookup recipe by `vertexNum`, compute lightRGB (with selection + alphaOverride merge), pack thin record, append.

### Item 3 — Bridge destruction path
**Status:** Resolved.
**Finding:** **No `Bridge` class exists in the source tree.** Greps for `class Bridge`, `Bridge::destroy`, `bridgeDestroy`, `pBridge` return only references in spec/recon docs, not code. Bridges in MC2 are encoded as map overlays (via `Overlays type, DWORD offset`) and route through:
- `Terrain::setOverlay` (terrain.cpp:869) → `MapData::setOverlay` (mapdata.cpp:1259) → modifies `blocks[].textureData` → calls `setTerrain(indexY, indexX, -1)` → `invalidateTerrainFaceCache()` at mapdata.cpp:1359.
- `Terrain::setOverlayTile` (terrain.cpp:863) → `MapData::setOverlayTile` (mapdata.cpp:1239) → same chokepoint chain.
- `MapData::setTerrain` (mapdata.cpp:1293) → `invalidateTerrainFaceCache()` at :1359 (single chokepoint).

| Mutation | Routes through | Strategy |
|---|---|---|
| `setTerrain` (terrain type change) | invalidateTerrainFaceCache | Convert to `invalidateRecipeFor(vertexNum)` for dense recipe |
| `setOverlay` / `setOverlayTile` | calls `setTerrain` → invalidateTerrainFaceCache | Same chokepoint — no new hook needed |
| `setMine` / blown / unburied | per slice 2b, all 19 callsites route through `GameMap::setMine` | Already cached on recipe per slice 2b; confirm chokepoint after dense migration |
| Bridge destruction | encoded as `setOverlay` / `setTerrain` events | Subsumed by setOverlay chokepoint — no new hook needed |
| Crater / scorch | calls `setTerrain(indexY, indexX, -1)` (mapdata.cpp:1255 indirectly) | Subsumed — no new hook needed |
| Editor edits | N/A — out of scope per spec | Skip |

**Implementation impact:** `invalidateTerrainFaceCache()` (mapdata.cpp:213) is the single chokepoint. For dense recipe, replace whole-array invalidate with per-entry `invalidateRecipeFor(vertexNum)` — call site stays the same (one location). Q6 mutation table can be simplified: every in-game mutation already routes through this chokepoint. No new defensive hooks needed beyond the existing one.

### Item 4 — Stock smoke missions: in-game terrain mutations
**Status:** Partially resolved (static analysis blocked).
**Finding:** No on-disk mission files under `data/missions/` for the smoke set; mission scripts are inside the FST archive (`mc2.fst`). Static analysis without a FST extractor / mission reader is not feasible in this recon. What we know from memory + code:
- **Mines:** mc2_24 has them (slice 2b memory). Mines mutate via `GameMap::setMine` → invalidates the recipe's mine cache (slice 2b infra).
- **Bridges:** Possibly mc2_10 / mc2_17 (per brainstorm Q6 line 412). Bridges = overlays = `setOverlay` chokepoint (Item 3).
- **Scorch / craters:** any mission can have these via weapons hits → `setTerrain(-1)` → invalidate.
- **Slice 2b finding:** "campaign-wide ~97% mine-free" — most quads on most missions never mutate.

**Implementation impact:** The spec's posture is correct: defensive hooks at all known chokepoints (one chokepoint exists per Item 3) + `MC2_TERRAIN_INDIRECT_PARITY_CHECK=1` validates the assumption holds. The Stage 1 parity-check zero-mismatch result IS the answer to "are there mutations the recipe drifts on" — if parity stays clean across tier1, the assumption holds. Don't enumerate missions; enumerate chokepoints (done) and trust the parity check to catch what static analysis would miss.

**Spec amendment (minor):** Add a note to Recon Item 4 that "static analysis of mission files is not feasible (FST archive); rely on parity check during Stage 1 to validate post-load immutability assumption." This is implicit in the spec but worth making explicit.

### Item 5 — drawMine on stock smoke missions
**Status:** Resolved.
**Finding:** `TerrainQuad::drawMine` (quad.cpp:4136) is **independent of the terrain main-emit path.** It:
- Reads `mineResult.getMine(cellPos)` (per-quad cache, set by setupTextures from `GameMap->getMine()`).
- Builds per-cell vertices via `eye->projectForScreenXY(thePoint, pos)` (CPU per-cell projection, NOT terrain quad reuse).
- Looks up terrain elevation via `land->getTerrainElevation(thePoint)`.
- Submits via `addVertices(mineTextureHandle/blownTextureHandle, MC2_DRAWALPHA)` to mcTextureManager (legacy contract).
- These flush at `Render.Overlays` zone (txmmgr.cpp:1442-1520), AFTER `Render.TerrainSolid` and `Render.GpuStaticProps`.

**Depth state inheritance:** drawMine runs at Render.Overlays, where the state cascade is `gos_State_AlphaMode = AlphaInvAlpha`, `TextureAddress = TextureWrap`. Depth state at that point: ZCompare=1 (set in renderLists header at :1042), ZWrite=1. drawMine's vertex stream depth-tests against terrain depth that indirect-terrain just wrote. Indirect-terrain just needs to write correct depth (with the fudge per Item 9), and drawMine inherits it cleanly.

**Implementation impact:** drawMine remains on legacy contract per Q5. No changes needed. Visual canary at a mine site (mc2_24) confirms via standard smoke run.

### Item 6 — Killswitch semantics
**Status:** Resolved.
**Finding:** `MC2_TERRAIN_INDIRECT=0` short-circuit branch points (4 specific sites):

1. **`Terrain::primeMissionTerrainCache`** (terrain.cpp:575). Guard the new dense-recipe build alongside the existing `WaterStream::Build()` call at :599. When env=0, skip recipe build entirely (no SSBO allocation, no walk of MapData::blocks).
2. **`Terrain::geometry quadSetupTextures` per-frame loop** (terrain.cpp:1681). Guard the *new* thin-record-direct emit. The existing M2 path (quad.cpp:1834 `appendThinRecordDirect`) is governed by `MC2_PATCHSTREAM_THIN_RECORD_FASTPATH` (independent env var, default-on per orchestrator). If `MC2_TERRAIN_INDIRECT=0`, the new dense-recipe-walk-and-emit path doesn't run; M2 path continues as today.
3. **`txmmgr.cpp Render.TerrainSolid`** (txmmgr.cpp:1330). The new bridge function entry: early-return when env=0; let M2 `TerrainPatchStream::flush()` continue as today. The existing `if (modernHandled && MC2_ISTERRAIN) continue;` skip at :1340 already handles the legacy fallback.
4. **New mutation invalidate hooks** (`invalidateRecipeFor(vertexNum)` calls at the existing setTerrain chokepoint). When env=0, hook is a no-op.

**Implementation impact:** When `MC2_TERRAIN_INDIRECT=0`: zero new SSBO allocation, zero new per-frame work, zero new draw calls. M2 thin path continues to be the production renderer. This is a true legacy fallback, mirrors Shape C's `MC2_MODERN_TERRAIN_PATCHES=0` killswitch shape. Convention: `=0` opts out, anything else (including unset, default-off → default-on at Stage 3 promotion) opts in.

### Item 7 — Tracy aggregator zone
**Status:** Resolved with recommendation.
**Finding:** Existing per-frame Tracy zones for terrain CPU work:
- `Terrain::geometry vertexProjectLoop` (terrain.cpp:1319, 1460 — duplicate? worth checking) — per-vertex projection.
- `Terrain::geometry quadSetupTextures` (terrain.cpp:1681) — per-quad admission. The targeted zone.
- `Terrain::geometry cloudUpdate` (terrain.cpp:1702) — sibling, not in scope.
- `Terrain::render drawPass` (terrain.cpp:936) — per-quad emit (1.46 ms after M2d).
- `Terrain::render minePass` (terrain.cpp:959) — per-quad mine emit. Out of scope.
- `Terrain::render debugOverlays` (terrain.cpp:970) — toggled.

**Recommendation:** ADD `Terrain::TotalCPU` aggregator. Place it wrapping the call sites of `Terrain::geometry` AND `Terrain::render` (both are called per-frame from gamecam.cpp / camera update — verify exact call order during Stage 2 implementation). Reason: indirect-terrain shifts work between zones — `quadSetupTextures` ↓, new `Terrain::IndirectThinRecordPack` ↑ (per-frame, subsumes some quadSetupTextures iteration), `Terrain::render drawPass` ↓ slightly (recipe lookup gets cheaper). Single-zone delta on `quadSetupTextures` may understate or overstate the win. Aggregator tells the truth.

**Implementation impact:** ~1 line of `ZoneScopedN("Terrain::TotalCPU")` in the appropriate per-frame caller scope. Tracy convention from `memory/tracy_profiler.md`. The aggregator delta is a STRETCH metric per the spec (Section "Validation gates" → B); single-zone `quadSetupTextures` delta remains the primary B-gate target (≥50% reduction).

### Item 8 — Two-PR promotion sequence
**Status:** Resolved.
**Finding:** Existing convention (from git log + commit message inspection):
- **Shape C precedent:** Stage 2/3 work shipped in earlier commits with default-off env gate. Final flip: `aee39cc` "feat(shape-c): flip MC2_MODERN_TERRAIN_PATCHES default-on" — single 2-file commit (quad.cpp + run_smoke.py env passthrough). Killswitch preserved.
- **renderWater precedent:** Stage 2 shipped in `bc8c4f1` "feat(water-fastpath): renderWater Stage 2 — static SSBO + thin record + GPU-direct draw" with default-off env gate `MC2_RENDER_WATER_FASTPATH`. Stage 3 (parity check) shipped separately. Default-on flip is **pending** per orchestrator status board.

**Convention encoded in commit-message templates:**

PR 1 (slice ships, default-off):
```
feat(terrain-indirect): dense recipe + indirect draw — Stage 1+2

Implements MC2_TERRAIN_INDIRECT env gate (default off). Adds dense
TerrainRecipe SSBO indexed by vertexNum, built at primeMissionTerrainCache.
Per-frame walks live quadList, packs thin record, builds indirect command
buffer, emits glMultiDrawElementsIndirect at Render.TerrainSolid (replaces
TerrainPatchStream::flush() when env=1; legacy M2 path still runs when env=0).

Adds MC2_TERRAIN_INDIRECT_PARITY_CHECK env gate (default off) for byte-
level recipe + thin-record validation against legacy quadSetupTextures.

Tier1 5/5 PASS triple (unset / INDIRECT=1 / INDIRECT=1+PARITY=1) with
+0 destroys delta. Tracy delta on Terrain::geometry quadSetupTextures:
[fill in actual delta]. Parity check: [fill in counts] checks across 5
missions, zero mismatches.

Killswitch: MC2_TERRAIN_INDIRECT=0 explicit opt-out.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

PR 2 (default-on flip, after soak):
```
feat(terrain-indirect): flip MC2_TERRAIN_INDIRECT default-on

Indirect-terrain dense-recipe + GPU-direct draw becomes the default for
Render.TerrainSolid main-emit. Tier1 parity validation: [N]M parity checks
across 5 missions, zero mismatches. Post-flip baseline: 5/5 tier1 PASS,
+0 destroys delta. Soak: [N] days real-world usage, no regressions reported.

Tracy delta on Terrain::geometry quadSetupTextures (mc2_01, max zoom):
- mean   3.01 -> [X.XX] ms ([-X.XX] ms,  [-X.X]%)
- median ...
[follow Shape-C aee39cc format]

Killswitch preserved: MC2_TERRAIN_INDIRECT=0 explicit opt-out.

Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
```

**Status board update protocol:**
- After PR 1 lands: move "Indirect terrain draw" from Queued to In progress; note "Stage 1+2 shipped, default off; awaiting soak."
- After PR 2 lands: move to Shipped row with perf delta + parity stats. Add Update log entry. Archive brainstorm + spec under their existing locations as the auditable scope record.

**Implementation impact:** Two distinct PRs. PR 1 is the substantive change; PR 2 is a 2-line flip + run_smoke.py env passthrough update. Mirrors Shape C exactly.

### Item 9 — Depth-fudge parity
**Status:** Resolved.
**Finding:** **Real parity gap exists.** Legacy CPU emit applies `+ TERRAIN_DEPTH_FUDGE` (`#define ... 0.001f` at quad.cpp:1618) to every terrain triangle z-coord at all 16 emit sites in `mclib/quad.cpp` (lines 1762, 1869, 2004/2014/2024, 2171, 2363/2373/2383, 2527, 2775/2785/2795, 2934, 3063/3073/3083, 3222 — `pz + FUDGE` for terrain, `wz + FUDGE` for water). The current M2 thin VS at `gos_terrain_thin.vert:137` reads `screen.z = clip.z * rhw;` — **no fudge.** Git log shows the thin VS has had this since `af93e46` (M1g) and was not touched in `b447ff9` (FogValue removal). Water fast VS DID add the fudge in commit `bc8c4f1` (Stage 2) at `gos_terrain_water_fast.vert:332` with the inline comment "2026-04-30 shoreline polish: bias water slightly farther in screen z to match legacy's TERRAIN_DEPTH_FUDGE=0.001 (quad.cpp:2775). Combined with the bridge's GL_LEQUAL depth test, water reliably loses depth ties to land at the coast — no z-fighting sparkle..."

**Why has the thin path's missing fudge not surfaced?** Hypothesis: in renderLists, `Render.3DObjects` runs at txmmgr.cpp:1102 (BEFORE `Render.TerrainSolid` at :1297). 3D mech/vehicle objects are written to depth before terrain is drawn — they don't depth-test against terrain's not-yet-written depth. So the legacy use case for the fudge (objects-vs-terrain z-tie at zoom levels where they coincide) doesn't apply in the M2 thin path's current draw order. Decals + GpuStaticProps + DRAWALPHA water DO run after terrain in renderLists; for those, missing fudge could in theory cause z-fighting at exactly-coincident depth. Smoke has not surfaced a regression — possibly because draw order resolves ties at LE, possibly because the sub-pixel difference is below visual detection threshold at smoke camera angles.

**Implementation impact for indirect-terrain:** **Add `+ 0.001` to `screen.z` in the thin VS** (or in a new VS variant, but reuse is cleaner). One-line change at `gos_terrain_thin.vert:137`:
```glsl
screen.z = clip.z * rhw + 0.001;  // TERRAIN_DEPTH_FUDGE — match legacy quad.cpp:1618
```

This is **load-bearing for parity** (legacy CPU emit applies it; thin VS / indirect-terrain VS must match for byte-equal outputs against `MC2_TERRAIN_INDIRECT_PARITY_CHECK`). It is **also a remediation** of an existing latent gap in the M2 thin path — adding the fudge brings the thin path's depth output in line with what the legacy emit would have written, reducing the chance of z-fighting at object/decal/static-prop interfaces that depth-test against terrain.

**Risk of NOT adding:** parity-check would need to skip post-projection z comparison (already does — sub-1-ULP drift). But the BEHAVIOR difference between legacy and indirect-terrain remains; if a stock mission has a camera angle where terrain z exactly equals decal/static-prop z, the legacy path lets the overlay win the LE tie (terrain z biased larger), but indirect-terrain leaves it ambiguous. Manifests as occasional sparkle / Z-fighting under the right zoom + angle.

**Recommendation:** Implementation-plan session adds the fudge to `gos_terrain_thin.vert:137` as a Stage 0.5 prerequisite (small change, validates against M2 path's existing tier1 5/5 PASS triple), THEN proceeds with Stage 1+2 of indirect-terrain. Alternative: bundle into Stage 2's bridge-function commit, with the inline comment matching the water VS's "match legacy's TERRAIN_DEPTH_FUDGE=0.001" precedent.

## New findings / spec amendments

1. **Render-order placement is INSIDE renderLists, not post.** Spec Section "Render-order hook" (lines 132-156) is mostly correct but should be sharpened: the hook is AT `Render.TerrainSolid` (txmmgr.cpp:1330), specifically replacing or paralleling the existing `TerrainPatchStream::flush()` call. Update the spec's "Decision (recon-pending — see Recon Items)" paragraph to "Decision (resolved 2026-04-30): replace `TerrainPatchStream::flush()` at txmmgr.cpp:1330 inside the `Render.TerrainSolid` zone." (See Item 1.)

2. **Item 3 mutation table simplifies.** The spec's Mutation Events table (Section "Mutation events") lists `setOverlay` and Bridge destruction as recon-pending. Recon resolves both: `setOverlay` calls `setTerrain` calls `invalidateTerrainFaceCache` — single chokepoint exists. No new defensive hook needed beyond converting the whole-array invalidate to per-entry `invalidateRecipeFor(vertexNum)`. Update the table's "Strategy" column for those rows to "Subsumed by setTerrain chokepoint."

3. **Thin-record format absorbs three fields** (overlayHandle, terrainDetailHandle, isCement bit) into the **recipe**, not the thin record. Recipe grows from 144 B to ~160 B. Thin record stays 32 B. Update spec's "Architecture" section data structure decision Q3 with this size budget.

4. **Item 4 is parity-check-driven, not static-analysis-driven.** Mission files are inside the FST archive; static analysis isn't feasible. The spec's defensive-hooks-plus-parity-check posture handles the unknown. Add an explicit note in Recon Item 4 that static analysis is blocked and parity-check during Stage 1 is the intended validation path.

5. **Item 9 surfaces a remediation opportunity.** The thin VS's missing fudge is an existing latent gap — fixing it for indirect-terrain also fixes a potential z-fighting bug nobody has reported in the M2 path. Spec's gotcha #9 paragraph (Section "Constraints" → 4 → #9) already flags this as recon-pending; recon resolves: ADD the fudge.

## Open follow-ups (not blocking implementation)

- **Verify duplicate `vertexProjectLoop` Tracy zone.** The grep showed `ZoneScopedN("Terrain::geometry vertexProjectLoop")` at BOTH terrain.cpp:1319 AND :1460. This may be the legacy + D1 (env-gated) variants both sharing the zone name — check before Stage 1 to avoid Tracy attribution confusion.
- **Confirm `Terrain::geometry` and `Terrain::render` per-frame caller scope.** Item 7's aggregator placement depends on whether both are called from a single per-frame site (gamecam.cpp likely) or interleaved with other work. Implementation-plan session verifies during Stage 2.
- **Decals/Render.Overlays depth-state under indirect-terrain.** Item 5 shows drawMine inherits cleanly. Worth confirming the same for `gos_DrawDecals()` (txmmgr.cpp:1437) and the `Render.Overlays` water-bucket loop at :1442-1520 — they all run AFTER indirect-terrain writes depth, and the bridge's depth-state save/restore must not corrupt their inherited state.
- **Tracy aggregator name.** "Terrain::TotalCPU" is the spec's suggested name; verify no conflict with existing zones before adding.
- **Smoke missions' actual mutation profile.** Once Stage 1 lands with `[TERRAIN_INDIRECT v1] event=invalidate` lifecycle prints (per debug-instrumentation rule), capture a tier1 trace and document the actual count per mission. This closes Item 4's static-analysis gap with runtime data.

## Recon time / token budget used

- **Files read:** spec, brainstorm, orchestrator, water_ssbo_pattern.md, gpu_direct_renderer_bringup_checklist.md (foundational); gamecam.cpp:220-280, txmmgr.cpp:1008-1644, terrain.cpp:560-995 + 1670-1719, quad.cpp:420-661 + 1700-1900 + 4136-4216, mapdata.cpp:1230-1430, gos_terrain_patch_stream.h (full), gos_terrain_thin.vert (full), gos_terrain_water_fast.vert:300-338.
- **Greps run:** TERRAIN_DEPTH_FUDGE (~50 hits), setOverlay/bridgeDestroy/Bridge::destroy/etc. (60-line cap, mostly mclib + docs), class Bridge / Bridge::destroy / destroyBridge / removeBridge (40-line cap, only spec/recon-prompt hits), ZoneScopedN("Terrain::|quadSetup (40-line cap, mclib).
- **Git operations:** log + show on `aee39cc` (Shape C flip) + `bc8c4f1` (renderWater Stage 2); blame on water VS lines 320-335; log on thin VS file (2 commits total).
- **Approx duration:** ~30-40 min equivalent. ~2 of 9 items needed deeper investigation (Items 1 + 9). Items 3, 5, 6, 7, 8 resolved cleanly. Item 4 is the only partial-resolution.
- **No code edits, no spec edits, no commits.** All deliverable in this handoff doc.
