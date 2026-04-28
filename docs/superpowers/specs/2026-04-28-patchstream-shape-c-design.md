# Shape C PatchTable — Design Spec

Status: spec only. No source modifications yet.
Worktree: `.claude/worktrees/nifty-mendeleev/`
Branch: `claude/nifty-mendeleev`
Author: planning session 2026-04-28, post Shape B M0b (persistent-mapped VBO, default-ON).

---

## 0. Thesis

Shape C reuses `MapData::WorldQuadTerrainCacheEntry` as the PatchTable for stable
solid-terrain recipe data. The per-frame quad loop keeps `projectZ` admission, cull
gates, lighting, fog, water, mine, and selection logic live, but reads stable
handles/UV/flags from the cache when `MC2_MODERN_TERRAIN_PATCHES=1`. If the cache
is missing or invalid (`getTerrainFaceCacheEntry()` returns NULL or `!isValid()`),
the loop falls back to existing inline computation.

The key insight from the exploration: Shape C is **not** a new data-model project.
`WorldQuadTerrainCacheEntry` already IS most of PatchTable. The work is wiring and
cache invalidation correctness, not invention.

---

## 1. Background

Shape A (TexResolveTable) memoized texture-handle resolution per frame, collapsing
repeated `get_gosTextureHandle` calls. Shape B (M0b, persistent-mapped VBO) reduced
per-frame CPU→GPU transfer for the terrain solid path. Shape C is the next layer:
eliminate the per-quad texture lookup and UV recomputation that happens inside the
solid-terrain emit loop in `quad.cpp` four times per mission frame.

---

## 2. Verified facts from code reads (2026-04-28)

All three claims flagged as load-bearing by the advisor were verified directly before
writing this spec.

### 2.1 No lazy rebuild — invalidation is whole-array free

`MapData::invalidateTerrainFaceCache()` (`mapdata.cpp:213`):

```cpp
void MapData::invalidateTerrainFaceCache (void)
{
    if (terrainFaceCache)
    {
        Terrain::terrainHeap->Free(terrainFaceCache);
        terrainFaceCache = NULL;
    }
```

`getTerrainFaceCacheEntry()` returns `NULL` when `terrainFaceCache == NULL`.
There is no per-entry lazy rebuild. After invalidation the cache is completely
unavailable until `buildTerrainFaceCache()` is called again explicitly.

**Shape C consequence:** `getTerrainFaceCacheEntry()` returning NULL is a valid
runtime state (mission is loading, terrain was just modified). The emit loop
**must** fall back to inline computation in that case, not crash or skip.

### 2.2 `setOverlay()` always funnels through `setTerrain()`

`mapdata.cpp:1259-1267`:

```cpp
void MapData::setOverlay( long indexY, long indexX, Overlays type, DWORD offset )
{
    ...
    ourBlock->textureData |= Terrain::terrainTextures->getOverlayHandle( type, offset );
    setTerrain(indexY,indexX,-1);   // ← always called
}
```

Adding `invalidateTerrainFaceCache()` in `setTerrain()` is sufficient. No
separate call is needed in `setOverlay()`.

### 2.3 `uvData` is atlas/colormap UV only — not world-space detail tiling

`buildTerrainFaceCache()` stores `uvData` from:

```cpp
entry.terrainHandle = Terrain::terrainTextures2->resolveTextureHandle(
    vMin, vMax, &entry.uvData, NULL, false);
```

This is the colormap/atlas sub-tile UV (minU, minV, maxU, maxV within the
atlas page). World-space detail tiling UV is computed per-vertex from world
position in `quad.cpp` and is **not** in the cache. Shape C must continue to
compute detail tiling UVs exactly as today.

---

## 3. Field classification

```
WorldQuadTerrainCacheEntry fields (stable, safe to read from cache):
  terrainHandle         — colormap / base texture handle
  terrainDetailHandle   — detail texture handle
  overlayHandle         — overlay texture handle (cement-alpha path)
  uvData.{minU/minV/maxU/maxV}  — atlas/colormap tile UV
  flags:
    TERRAIN_CACHE_VALID   — entry was built
    TERRAIN_CACHE_CEMENT  — isCement()
    TERRAIN_CACHE_ALPHA   — isAlpha()
    TERRAIN_CACHE_COLORMAP — usesColorMap()

Per-frame (must remain computed live):
  projectZ / pz admission         — screen-space cull, changes every camera move
  projected screen x/y/z/rhw      — vertex positions, changes every camera move
  diffuse lighting                 — Terrain::recalcLight, changes on time-of-day
  fog color / fog density          — changes with camera altitude/position
  waterDepth / WaterTXMData flags  — water animation + alpha varies per-frame
  mines / scorch overlay state     — scripted runtime mutations
  selection / highlight state      — per-object, changes on player input
  world-space detail tiling UV     — computed from vertex world position
```

---

## 4. Prerequisite: cache invalidation fix (standalone commit)

Before Shape C emits from the cache, `setTerrain()` must invalidate it when
mission-load terrain mutations occur. Without this, the cache could silently
serve stale recipe data.

**Change:** in `mapdata.cpp`, at the end of `MapData::setTerrain()` (line ~1354),
add:

```cpp
// Invalidate terrain face cache so next buildTerrainFaceCache() reflects
// the mutation. Shape C reads only from a valid, post-mutation cache.
invalidateTerrainFaceCache();
```

**Why `setTerrain()` only:** `setOverlay()` always calls `setTerrain()` (verified
§2.2). All other terrain mutation paths also flow through `setTerrain()`. The
function already touches `blocks[index]` which is the data source for
`buildTerrainFaceCache()`.

**Cost analysis:** `invalidateTerrainFaceCache()` is a single heap-free. The
cache is rebuilt by `buildTerrainFaceCache()` which is called at mission load
by `primeMissionTerrainCache` — after all `setTerrain()` mutations are complete.
In-gameplay `setTerrain()` calls (scripted terrain changes, mines, scorch) invalidate
the **whole** terrain face cache. Shape C then falls back to inline computation for
all cache reads until `buildTerrainFaceCache()` is explicitly run again. This is safe
but may reduce Shape C's performance benefit for the rest of that mission. In practice
these runtime mutations are rare.

**Land this standalone** — it is a correctness fix independent of Shape C and
carries zero risk by itself.

---

## 5. Pre-implementation verification task (field parity proof)

Before switching the emit path, add a `MC2_SHAPE_C_PARITY_CHECK=1` debug mode
that runs both paths side-by-side for one frame and asserts equality on the
stable fields. This should be a debug-only code path, not a performance path.

For each visible quad where `getTerrainFaceCacheEntry()` returns a valid entry,
compare cached vs inline for the four emit-site branches. Each site must be
checked independently because they use different vertex combinations and diagonal
modes — a cache value can be correct for one branch and mapped incorrectly in
another:

```
// Check per branch: tag each log line with which site / diagonal
// Site tags: BOTTOMRIGHT_TOP, BOTTOMRIGHT_BOTTOM, TOPLEFT_TOP, TOPLEFT_BOTTOM

assert(cached->terrainHandle            == inline_terrainHandle)
assert(cached->terrainDetailHandle      == inline_detailHandle)
assert(cached->overlayHandle            == inline_overlayHandle)
assert(cached->isCement()               == inline_isCement)
assert(cached->isAlpha()                == inline_isAlpha)
// uvData: compare within float epsilon
assert(fabsf(cached->uvData.minU - inline_uvData.minU) < 1e-5f)
assert(fabsf(cached->uvData.minV - inline_uvData.minV) < 1e-5f)
assert(fabsf(cached->uvData.maxU - inline_uvData.maxU) < 1e-5f)
assert(fabsf(cached->uvData.maxV - inline_uvData.maxV) < 1e-5f)
```

Instrument using the env-gated `[SHAPE_C]` pattern:

```cpp
static const bool s_shapeCParityCheck = (getenv("MC2_SHAPE_C_PARITY_CHECK") != nullptr);
```

Log first mismatch per `(tileR, tileC, siteTag)` with quad coordinates, site
name, and both values. Run tier1 smoke with `MC2_SHAPE_C_PARITY_CHECK=1` and
confirm zero mismatches on all four site tags before enabling the cache path.

---

## 6. Shape C change — `TerrainQuad::setupTextures()`

**Target:** `mclib/quad.cpp`, `TerrainQuad::setupTextures()` (line ~271), specifically
the `else { terrainTextures2 }` branch (lines ~446–565), zone `"resolveFallback"`.

**Not** `draw()` (lines ~1536+). `draw()` uses member vars set by `setupTextures()` and
must not be touched.

The current `resolveFallback` zone computes `terrainHandle`, `overlayHandle`, `uvData`,
`isCement` inline via `getTextureHandle()` calls across 6 branches (uvMode × cement/alpha).
Shape C replaces that computation with a cache read, keeping `addTriangle` and
`pz_emit_terrain_tris` calls at their original callsite locations in `setupTextures()`.

**Helper functions** (static, file-scope in `quad.cpp`):

```cpp
struct TerrainRecipe {
    DWORD terrainHandle       = 0xffffffff;
    DWORD terrainDetailHandle = 0xffffffff;
    DWORD overlayHandle       = 0xffffffff;
    TerrainUVData uvData      = {};
    bool isCement             = false;
    bool isAlpha              = false;
};

// Fills recipe from inline getTextureHandle() calls — always correct, never stale.
static void buildTerrainRecipeInline(VertexPtr* vertices, long uvMode, TerrainRecipe& r);

// Fills recipe from cache when s_shapeCEnabled and entry is valid. Returns false
// and leaves r unmodified if cache unavailable.
static bool tryGetCachedTerrainRecipe(
    const MapData::WorldQuadTerrainCacheEntry* entry, TerrainRecipe& r);

// Calls mcTextureManager->addTriangle() for the appropriate handles.
// Does NOT call pz_emit_terrain_tris — that stays at the setupTextures() callsite
// to preserve projectZ callsite identity.
static void addTerrainTriangles(const TerrainRecipe& r);
```

**Shape C call pattern in `setupTextures()` resolveFallback zone:**

```cpp
TerrainRecipe recipe;
TerrainRecipe inlineRecipe;

// Always build inline recipe when parity check is on — keeps comparison meaningful
// even after the cache path is enabled.
if (s_shapeCParityCheck)
    buildTerrainRecipeInline(vertices, uvMode, inlineRecipe);

if (!tryGetCachedTerrainRecipe(cachedEntry, recipe))
    buildTerrainRecipeInline(vertices, uvMode, recipe);

// Parity check: compare inline vs cache entry directly.
if (s_shapeCParityCheck && cachedEntry && cachedEntry->isValid())
    shapeC_checkParity(inlineRecipe, *cachedEntry, tileR, tileC, uvMode);

// Assign member vars in the member function (not in the free helper).
isCement            = recipe.isCement;
terrainHandle       = recipe.terrainHandle;
terrainDetailHandle = recipe.terrainDetailHandle;
overlayHandle       = recipe.overlayHandle;
uvData              = recipe.uvData;

// addTriangle calls (no pz_emit — that stays below at original callsite).
addTerrainTriangles(recipe);

// pz_emit stays at original callsite in setupTextures() for callsite identity.
if (!recipe.isCement && recipe.terrainHandle != 0)
    pz_emit_terrain_tris(vertices, uvMode,
        (uvMode == BOTTOMRIGHT) ? "terrain_quad_cluster_a" : "terrain_quad_cluster_c",
        __FILE__, __LINE__);
```

The cache path replaces **only** atlas/colormap UV lookup (`uvData`) and stable
handle/flag reads. It does **not** cache world-space detail tiling UV math —
that is computed per-vertex in `draw()` from world position, unchanged in both paths.

---

## 7. Killswitch

`MC2_MODERN_TERRAIN_PATCHES` environment variable.

- Default: **OFF** (0). Shape C lands behind opt-in.
- Opt-in: `MC2_MODERN_TERRAIN_PATCHES=1`
- Opt-out: `MC2_MODERN_TERRAIN_PATCHES=0` (also the default)

Shape C lands **default-off**. The pattern that worked for Shape A and PatchStream:
land default-off → validate → profile → visual check → bake → decide default-on.

Conditions before considering default-on (separate follow-up decision):
- Parity check passes on tier1 (§5) with all four site tags clean
- Smoke gate passes with `MC2_MODERN_TERRAIN_PATCHES=1`
- Visual confirmation: no UV seams, no wrong textures, no cement-overlay scrambling
- CPU profiling baseline recorded (confirm benefit before baking)

---

## 8. Implementation sequence

```
Step 1: Land invalidation fix in setTerrain() — standalone commit.
         Verify: buildTerrainFaceCache() still called at mission load after setTerrain() mutations.
         Cost: ~3 lines.

Step 2: Add MC2_SHAPE_C_PARITY_CHECK=1 parity logging — debug commit behind env gate.
         Run tier1 with parity check on. Fix any mismatches.

Step 3: Add MC2_MODERN_TERRAIN_PATCHES killswitch + cache-read branch at 4 emit sites.
         Keep legacy path 100% intact. Default off.

Step 4: Run parity check with killswitch on. Assert zero mismatches.

Step 5: Keep MC2_MODERN_TERRAIN_PATCHES default-off. Record parity/smoke/visual/
         profile results. Decide default-on in a follow-up after a bake period.
```

---

## 9. Non-goals

- Shape C does NOT change the VBO upload path (that is Shape B / M0b).
- Shape C does NOT cache world-space UV tiling or vertex screen positions.
- Shape C does NOT change the projectZ or cull admission logic.
- Shape C does NOT modify `buildTerrainFaceCache()` or `WorldQuadTerrainCacheEntry`
  struct layout (existing fields are sufficient).
- Shape C does NOT affect the shadow terrain draw path (that path does not use
  the `quad.cpp` emit sites targeted here).

---

## 10. Risk table

| Risk | Likelihood | Mitigation |
|------|------------|------------|
| Cache NULL during gameplay (scripted terrain change) | Low but real | emit-loop NULL check + fallback (§6) |
| uvData mismatch on atlas boundary quads | Unknown until parity check | parity check (§5) catches it |
| cement/overlay scrambling if `setOverlay()` mutation not re-cached | Low; rebuild at next load | invalidation fix (§4) + NULL fallback |
| Detail UV behavior change | None | detail UV path unchanged (§2.3) |
| Shape C emit speedup smaller than expected | Possible | not the primary goal; correctness + CPU profiling baseline first |
