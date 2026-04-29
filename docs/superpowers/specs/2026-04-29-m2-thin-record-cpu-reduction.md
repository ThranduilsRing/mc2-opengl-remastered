# M2: Thin-Record CPU Reduction

## Goal

Eliminate the per-quad CPU overhead introduced by M1d–M1g while keeping the thin-record GPU draw path intact. After M1g, `quadSetupTextures` scales from ~3.5 ms zoomed in to ~9 ms zoomed out. The root cause: `quad.cpp` still builds `gos_VERTEX[6]` structs per quad in the fast path — a legacy carrier object the thin path doesn't need — and pays a per-frame hash lookup to resolve the recipe index. This spec removes both costs.

## Architecture

Two independent changes that compose:

**Struct compaction:** `TerrainQuadThinRecord` shrinks from 48 B to 32 B by dropping `fogRGBs` entirely. `TerrainType` (previously `fogRGBs` bits 0–7) migrates to the recipe's unused `_wp0` padding slot as a packed uint (4 corner material values). `FogValue` (previously `fogRGBs` bits 24–31) is confirmed dead in the fragment shader and removed from all shaders and the CPU path. Specular glow bytes deferred to Phase B (GPU lighting).

**Fast-path emit branch:** When both `isThinRecordsActive()` and `isFastPathActive()` are true, `quad.cpp` enters a new branch that never constructs `gos_VERTEX`. It evaluates projectZ validity for 4 corners only (not 6 triangle vertices), reads `lightRGB` per corner via `effectiveLightRGB()`, ensures a recipe exists via `ensureRecipeForQuad()`, looks up `recipeIdx` from a flat grid array (no hash), and writes the 32 B thin record directly. The legacy path (either gate off) has unchanged render behavior.

## Tech Stack

OpenGL 4.3 / GLSL 430, C++17. Files touched: `gos_terrain_patch_stream.h/.cpp`, `shaders/gos_terrain_thin.vert`, `shaders/gos_terrain.frag`, `shaders/gos_terrain.vert`, `shaders/gos_terrain.tesc`, `shaders/gos_terrain.tese`, `mclib/quad.cpp`.

---

## Data Structure Changes

### TerrainQuadRecipe — pack TerrainType into `_wp0`

`_wp0` (the padding float in `worldPos0`) stores all 4 corner material values as a packed uint:

```cpp
// CPU (in appendThinRecord on first recipe allocation):
uint32_t packed = m0 | (m1 << 8) | (m2 << 16) | (m3 << 24);
// where mN = terrainTypeToMaterial(vertices[N]->pVertex->terrainType)
memcpy(&recipe._wp0, &packed, 4);  // bit-preserving write, not numeric
```

```glsl
// GLSL (in gos_terrain_thin.vert):
uint terrainTypes = floatBitsToUint(rec.worldPos0.w);
TerrainType = float((terrainTypes >> (cornerIdx * 8u)) & 0xFFu);
```

Corner order: `m0` = corner 0 in bits 0–7, `m1` = corner 1 in bits 8–15, `m2` in 16–23, `m3` in 24–31. Matches `cornerIdx` used throughout the thin VS. Post-`terrainTypeToMaterial()` values stored (not raw `terrainType`), preserving current shader semantics.

Struct size stays 144 B. No std430 layout change.

### TerrainQuadThinRecord — drop `fogRGBs`

```cpp
struct alignas(16) TerrainQuadThinRecord {
    uint32_t recipeIdx;
    uint32_t terrainHandle;
    uint32_t flags;       // bit0: uvMode, bit1: pzTri1Valid, bit2: pzTri2Valid
    uint32_t _pad0;
    uint32_t lightRGB0, lightRGB1, lightRGB2, lightRGB3;
    // fogRGBs removed: TerrainType → recipe, FogValue → dead, specular → Phase B
};
static_assert(sizeof(TerrainQuadThinRecord) == 32, "...");
```

48 B → 32 B. `kPatchStreamThinRecordBytesPerSlot` updates accordingly.

### Recipe index flat cache

Replace the per-frame `s_recipeIndex.find(key)` hash lookup with a flat array:

```cpp
// In gos_terrain_patch_stream.cpp:
static uint32_t s_recipeIdxByGrid[kMaxTerrainGridW * kMaxTerrainGridH];
// Initialized to UINT32_MAX at init()/destroy().
// Key: terrain grid cell index (gridX * kMaxTerrainGridH + gridY)
```

**Key derivation — prefer grid indices over float division.** During implementation, check whether MC2's `TerrainVertex` or its parent quad structure carries explicit grid `(x, y)` cell indices (likely as integer fields or derivable from the vertex list position in the traversal). If available, use those directly — they are exact and carry no floating-point rounding risk. Fall back to `(int)(wx0 / kTerrainCellSize)` only if no integer grid index is accessible.

`s_recipeIndex` (the `unordered_map`) is retained as the allocator (source of truth for miss-path). On first encounter: hash lookup allocates slot, result stored in flat array. Subsequent frames: flat array hit, no hash touched.

Add a debug/parity check on flat-array hit: if cached slot's recipe data differs from current quad's positions/normals/UVs, log `[PATCH_STREAM v1] event=recipe_grid_collision` and fall back to hash lookup. Guards against bad grid key derivation.

`kMaxTerrainGridW / kMaxTerrainGridH` must be determined from MC2's terrain constants during implementation. If the grid dimensions are not statically knowable, use a small open-addressing flat hash table instead (same O(1) guarantee, better cache locality than `unordered_map`).

---

## quad.cpp Fast-Path Emit Branch

### Structure

```cpp
if (TerrainPatchStream::isFastPathActive() && TerrainPatchStream::isThinRecordsActive()
        && TerrainPatchStream::isReady() && !TerrainPatchStream::isOverflowed()) {
    // M2 direct thin-record path — no gos_VERTEX construction
    emitThinRecordDirect(terrainHandle, vertices, uvMode, shadow);
    return;
}
// Legacy path: build gos_VERTEX[6], appendQuad, appendThinRecord, etc.
```

This branch fires only when both conditions are set. The legacy path below it has unchanged render behavior. (Shader changes remove `FogValue` from all paths including the legacy passthrough, but this is dead-code removal — it was never read in the fragment shader.)

### effectiveLightRGB helper

```cpp
static inline DWORD effectiveLightRGB(const TerrainVertex* v, bool alphaOverride) {
    if (v->pVertex->selected) return SELECTION_COLOR;
    if (Terrain::terrainTextures2 && alphaOverride) return 0xFFFFFFFFu;
    return v->lightRGB;
}
```

Called once per corner (4 calls per quad). `selected` is read from `v->pVertex->selected` — the same per-vertex field the legacy path uses, not a quad-global flag. `alphaOverride` encodes the `(!isCement || isAlpha)` condition the legacy path computed inline. Preserves all three existing override cases.

### 4-corner projectZ validity

The fast path must produce the same per-triangle pz validity result as the legacy path. During implementation, read quad.cpp to determine whether `vertices[c]` already carries a pre-projected depth value (e.g. `pVertex->px/py/pz/pw` filled by an earlier camera-transform pass) or whether `projectZ()` must be called explicitly. Use whichever mechanism the legacy path uses — do not invent a new projection here.

```cpp
bool pz[4];
for (int c = 0; c < 4; c++)
    pz[c] = /* same validity test as legacy path for this vertex */;
bool pzTri1 = pz[t1c0] && pz[t1c1] && pz[t1c2];
bool pzTri2 = pz[t2c0] && pz[t2c1] && pz[t2c2];
// t1cN / t2cN are the corner indices for each triangle per uvMode:
// TOPRIGHT  (uvMode=0): tri1=[0,1,2], tri2=[0,2,3]
// BOTTOMLEFT(uvMode=1): tri1=[0,1,3], tri2=[1,2,3]
```

6 validity evaluations → 4 (2 corners shared between the two triangles). Saves ~2 × ~50 ns per quad.

If both triangles are pz-culled (both false), skip emitting the thin record entirely — zero bytes written, zero bucket touched.

### Recipe ensure + direct thin record write

Recipe allocation is separated from thin record emit via a new entry point:

```cpp
// Ensure recipe exists for this quad (allocates on first encounter, no-op thereafter).
// Returns UINT32_MAX on overflow; caller skips emit in that case.
uint32_t recipeIdx = TerrainPatchStream::ensureRecipeForQuad(
    gridKey,       // flat-array key (preferred) or (wx0,wy0) float key (fallback)
    recipe);       // TerrainQuadRecipe filled from vertices[0..3] + packed terrainTypes

if (recipeIdx == UINT32_MAX) return;  // SSBO full, graceful skip

TerrainQuadThinRecord tr;
tr.recipeIdx     = recipeIdx;
tr.terrainHandle = terrainHandle;
tr.flags         = (uvMode ? 1u : 0u)
                 | (pzTri1 ? 2u : 0u)
                 | (pzTri2 ? 4u : 0u);
tr._pad0         = 0;
tr.lightRGB0     = effectiveLightRGB(vertices[0], alphaOverride0);
tr.lightRGB1     = effectiveLightRGB(vertices[1], alphaOverride1);
tr.lightRGB2     = effectiveLightRGB(vertices[2], alphaOverride2);
tr.lightRGB3     = effectiveLightRGB(vertices[3], alphaOverride3);
TerrainPatchStream::appendThinRecordDirect(terrainHandle, tr);
```

`ensureRecipeForQuad`: checks flat array by `gridKey`; on hit returns cached index; on miss allocates a new recipe slot in the SSBO (same logic as current `appendThinRecord` allocation path), stores in flat array, returns index.

`appendThinRecordDirect`: takes a pre-built `TerrainQuadThinRecord`, writes it to the CPU shadow array, increments `s_thinRecordCount`, and calls `addThinRecordVertParity`. No recipe lookup — recipe already guaranteed present by `ensureRecipeForQuad`.

---

## Shader Changes

### gos_terrain_thin.vert

- Remove `out float FogValue`
- Remove all `fogRGBs` decoding
- Replace `TerrainType` assignment: `TerrainType = float((floatBitsToUint(rec.worldPos0.w) >> (cornerIdx * 8u)) & 0xFFu);`
- `lightRGBs` decode unchanged (still `uvec4Idx(tr.lightRGBs, cornerIdx)`)

### gos_terrain.frag

- Remove `in PREC float FogValue` declaration (confirmed dead — never read)
- No other changes. `TerrainType` still arrives as a float varying; shader behavior identical.

### gos_terrain.vert (legacy expanded path)

- Remove `vs_FogValue` output computation and declaration
- Legacy path still reads `frgb` for TerrainType (bits 0–7) — unchanged

### gos_terrain.tesc

- Remove `in float vs_FogValue[]` / `out float tcs_FogValue[]` passthrough in all three branches (passthrough, fat-record, now-removed thin-record)

### gos_terrain.tese

- Remove `in float tcs_FogValue[]` / `out float FogValue` passthrough and assignment

---

## Validation Gates

Run in this order after deploy:

**Gate 1 — thin draw without fast path (existing parity)**
```
MC2_PATCHSTREAM_THIN_RECORDS=1
MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1
```
Load mc2_01. Check console: `[PATCH_STREAM v1] event=thin_record_parity ... match=1`. Confirms thin record emit still correct before enabling direct branch.

**Gate 2 — full fast path (new direct emit branch)**
```
MC2_PATCHSTREAM_THIN_RECORDS=1
MC2_PATCHSTREAM_THIN_RECORDS_DRAW=1
MC2_PATCHSTREAM_THIN_RECORD_FASTPATH=1
```
Same parity check must still show `match=1`. This validates the new `emitThinRecordDirect` branch produces the same vertex counts as the old path.

**Gate 3 — visual diff at zoom levels**
Screenshot mc2_01 at standard zoom and at max Wolfman zoom. Compare against pre-M2 reference. Cement/concrete tiles are the canary: if TerrainType packing or `floatBitsToUint` read is wrong, cement material blending shifts noticeably. Grass/rock transitions also checked.

**Gate 4 — recipe collision log**
Run with `MC2_PATCHSTREAM_THIN_RECORDS=1` and scan console for any `[PATCH_STREAM v1] event=recipe_grid_collision`. Zero collisions expected on standard missions. A collision indicates a grid key derivation bug.

**Gate 5 — Tracy timing**
Connect Tracy, load mc2_01, zoom to max. Compare `quadSetupTextures` before and after. Expected reduction: 9 ms → 4–7 ms at max zoom. If the drop is less than ~1 ms, re-investigate whether the flat array cache is actually being hit (add a hit/miss counter gated on env var).

**Gate 6 — tier1 smoke**
```
py -3 scripts/run_smoke.py --tier tier1 --kill-existing
```
Exit 0, all 5 missions pass. Run without thin-record env vars (standard path must be unaffected).

---

## What This Does NOT Change

- Legacy expanded-vertex path (`!isFastPathActive()`) — render behavior unchanged
- Fat-record path (MC2_PATCHSTREAM_QUAD_RECORDS) — unchanged
- `TerrainQuadRecord` (fat record struct) — unchanged
- TCS/TES fat-record branch — reads `fogRGBs` from fat record, not thin record; unchanged
- Dynamic point light / lightning glow on terrain — deferred to Phase B (GPU lighting)
- Fog computation — deferred; GPU-side fog reconstruction is a Phase B concern

---

## Phase B Preview (GPU Lighting)

After M2 validates, Phase B removes `lightRGBs` from the thin record entirely by moving sun+shadow diffuse computation to the vertex shader (`WorldNorm × terrainLightDir` → diffuse) and point lights to a small per-frame SSBO (positions + colors, written only when lights exist). The thin record shrinks to 16 B: recipeIdx + handle + flags + pad. This is the pattern that transfers to objects (mechs/vehicles pay the same per-vertex CPU lighting cost today).
