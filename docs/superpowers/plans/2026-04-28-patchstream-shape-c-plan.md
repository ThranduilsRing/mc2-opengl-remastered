# Shape C PatchTable Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace per-frame `getTextureHandle()` calls in `TerrainQuad::setupTextures()` with reads from the existing `MapData::WorldQuadTerrainCacheEntry`, reducing per-quad texture resolution overhead in the solid terrain path.

**Architecture:** The existing `WorldQuadTerrainCacheEntry` already stores all stable recipe data (handles, UV, flags). Three static helpers (`buildTerrainRecipeInline`, `tryGetCachedTerrainRecipe`, `addTerrainTriangles`) decouple handle-resolution from `addTriangle` calls. `pz_emit_terrain_tris` stays at its original callsite in `setupTextures()` to preserve projectZ callsite identity. Member var assignments stay inside the member function. A parity check always builds an inline recipe when active, keeping the comparison meaningful even after the cache path is enabled.

**Tech Stack:** C++ (MSVC/CMake, RelWithDebInfo), `mclib/quad.cpp`, `mclib/mapdata.cpp`, env-gated killswitches.

**Spec:** `docs/superpowers/specs/2026-04-28-patchstream-shape-c-design.md`

---

## Context for implementors

`TerrainQuad::setupTextures()` runs every frame for each visible terrain quad. Its `else { terrainTextures2 }` branch (lines ~446–565 of `quad.cpp`) does two things:

1. **Residency** (lines ~451–456): calls `ensureTerrainFaceCacheEntryResident()` using the terrain face cache — already wired up, **untouched by Shape C**.
2. **Recipe resolution** (lines ~458–562, zone `"resolveFallback"`): calls `terrainTextures2->getTextureHandle()` / `terrainTextures->getTextureHandle()` inline across 6 branches (uvMode × cement/alpha). Sets member vars (`terrainHandle`, `overlayHandle`, `uvData`, `isCement`) then calls `addTriangle` / `pz_emit_terrain_tris`.

Shape C replaces step 2's `getTextureHandle()` calls with cache reads. `draw()` uses the member vars set by step 2 and must not be touched. The `if (!Terrain::terrainTextures2)` branch (lines ~287–445) is the legacy path and must not be touched at all.

The `cachedEntry` pointer is already computed at line ~451 (for residency). It is in scope for the parity check and cache-read path.

---

## Task 1: Standalone cache-invalidation fix in `setTerrain()`

**Files:**
- Modify: `mclib/mapdata.cpp` (end of `MapData::setTerrain()`, line ~1354)

This is a prerequisite correctness fix independent of Shape C. `setTerrain()` mutates `blocks[index].textureData`, which is the source of truth for `buildTerrainFaceCache()`. Without invalidation, a runtime mutation leaves stale cache values. `setOverlay()` always calls `setTerrain()` (verified: `mapdata.cpp:1267`), so one invalidation site is sufficient.

- [ ] **Step 1: Read the end of `setTerrain()`**

Read `mclib/mapdata.cpp` lines 1293–1354 to confirm the for-loop closing brace location.

- [ ] **Step 2: Add `invalidateTerrainFaceCache()` at the end of the function**

In `mclib/mapdata.cpp`, at the end of `MapData::setTerrain()` — after the for-loop's closing brace, before the function's closing brace — add:

```cpp
    } // end for-loop

    // Mutations to blocks[].textureData invalidate the terrain face cache. Shape C
    // falls back to inline for ALL cache reads until buildTerrainFaceCache() is
    // explicitly called again (at next primeMissionTerrainCache). In-gameplay
    // setTerrain() calls (mines, scorch) leave cache NULL for the rest of that
    // mission — acceptable since these events are rare.
    invalidateTerrainFaceCache();
}
```

- [ ] **Step 3: Build and run menu canary**

```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --menu-canary --kill-existing
```

Expected: exit 0, clean boot.

- [ ] **Step 4: Run one tier1 mission to confirm cache still rebuilt at mission load**

```
MC2_SMOKE_MODE=1 "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe" --profile stock --mission mc2_03 --duration 15
```

Expected: mission loads, terrain renders, no crash.

- [ ] **Step 5: Commit**

```bash
git add mclib/mapdata.cpp
git commit -m "fix: invalidate terrain face cache in setTerrain() for Shape C safety

setTerrain() mutates blocks[].textureData — the source data for
buildTerrainFaceCache(). Without invalidation, in-gameplay setTerrain()
calls leave WorldQuadTerrainCacheEntry values stale. After invalidation,
all cache reads fall back to inline until buildTerrainFaceCache() runs
again at next primeMissionTerrainCache(). setOverlay() always calls
setTerrain() (mapdata.cpp:1267), so one site suffices."
```

---

## Task 2: Extract `TerrainRecipe` helpers — pure refactor, no behavior change

**Files:**
- Modify: `mclib/quad.cpp` (add 3 helper functions before `setupTextures()`, refactor `resolveFallback` zone)

This task extracts the 6-branch inline recipe logic into helpers. The `resolveFallback` zone body shrinks to ~15 lines. No handles change, no `addTriangle` calls change, `pz_emit_terrain_tris` stays at its original callsite in `setupTextures()`. Member var assignments stay inside the member function. Smoke run verifies identical behavior.

- [ ] **Step 1: Read the target zone**

Read `mclib/quad.cpp` lines 446–565. Confirm the 6 branches and that `tileR`/`tileC` are in scope (from lines ~448–450).

- [ ] **Step 2: Add `TerrainRecipe` struct and 3 helpers before `setupTextures()`**

In `mclib/quad.cpp`, find `void TerrainQuad::setupTextures (void)` (line ~271). Insert the following **before** that function:

```cpp
struct TerrainRecipe {
    DWORD terrainHandle       = 0xffffffff;
    DWORD terrainDetailHandle = 0xffffffff;
    DWORD overlayHandle       = 0xffffffff;
    TerrainUVData uvData      = {};
    bool isCement             = false;
    bool isAlpha              = false;
};

// Fills recipe from inline getTextureHandle() calls. Always correct; never stale.
static void buildTerrainRecipeInline(VertexPtr* vertices, long uvMode, TerrainRecipe& r)
{
    r.isCement = Terrain::terrainTextures->isCement(vertices[0]->pVertex->textureData & 0x0000ffff);
    r.isAlpha  = Terrain::terrainTextures->isAlpha (vertices[0]->pVertex->textureData & 0x0000ffff);

    if (!r.isCement)
    {
        VertexPtr vA = (uvMode == BOTTOMRIGHT) ? vertices[0] : vertices[1];
        VertexPtr vB = (uvMode == BOTTOMRIGHT) ? vertices[2] : vertices[3];
        r.terrainHandle       = Terrain::terrainTextures2->getTextureHandle(vA, vB, &r.uvData);
        r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
        r.overlayHandle       = 0xffffffff;
    }
    else if (r.isAlpha)
    {
        r.overlayHandle       = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
        r.terrainHandle       = Terrain::terrainTextures2->getTextureHandle(vertices[1], vertices[3], &r.uvData);
        r.terrainDetailHandle = Terrain::terrainTextures2->getDetailHandle();
    }
    else // pure cement
    {
        r.terrainHandle       = Terrain::terrainTextures->getTextureHandle(vertices[0]->pVertex->textureData & 0x0000ffff);
        r.terrainDetailHandle = 0xffffffff;
        r.overlayHandle       = 0xffffffff;
    }
}

// Calls mcTextureManager->addTriangle() for the recipe's handles.
// Does NOT call pz_emit_terrain_tris — that stays at the setupTextures() callsite
// to preserve projectZ callsite identity.
static void addTerrainTriangles(const TerrainRecipe& r)
{
    if (!r.isCement)
    {
        if (r.terrainHandle != 0)
        {
            mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
            mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
            if (r.terrainDetailHandle != 0xffffffff)
            {
                mcTextureManager->addTriangle(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
                mcTextureManager->addTriangle(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
            }
        }
    }
    else if (r.isAlpha)
    {
        if (r.terrainHandle != 0)
        {
            mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
            mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
        }
        if (r.terrainDetailHandle != 0xffffffff)
        {
            mcTextureManager->addTriangle(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
            mcTextureManager->addTriangle(r.terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
        }
    }
    else // pure cement
    {
        if (r.terrainHandle != 0)
        {
            mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
            mcTextureManager->addTriangle(r.terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
        }
    }
}
```

- [ ] **Step 3: Replace the `resolveFallback` zone body in `setupTextures()`**

Find the `ZoneScopedN("TerrainQuad::setupTextures resolveFallback")` block (lines ~458–562) inside the `else { terrainTextures2 }` branch. Replace its contents:

```cpp
// Inside the resolveFallback zone braces:
{
    ZoneScopedN("TerrainQuad::setupTextures resolveFallback");

    TerrainRecipe recipe;
    buildTerrainRecipeInline(vertices, uvMode, recipe);

    // Assign member vars inside the member function.
    isCement            = recipe.isCement;
    terrainHandle       = recipe.terrainHandle;
    terrainDetailHandle = recipe.terrainDetailHandle;
    overlayHandle       = recipe.overlayHandle;
    uvData              = recipe.uvData;

    // Register triangle batches (no pz_emit here).
    addTerrainTriangles(recipe);

    // pz_emit stays at this callsite to preserve projectZ callsite identity.
    if (!recipe.isCement && recipe.terrainHandle != 0)
        pz_emit_terrain_tris(vertices, uvMode,
            (uvMode == BOTTOMRIGHT) ? "terrain_quad_cluster_a" : "terrain_quad_cluster_c",
            __FILE__, __LINE__);
}
```

- [ ] **Step 4: Build and run full tier1 smoke**

```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0. Visually inspect mc2_03 and mc2_17 — no UV seams, no wrong textures, cement tiles intact.

- [ ] **Step 5: Commit**

```bash
git add mclib/quad.cpp
git commit -m "refactor: extract TerrainRecipe + buildTerrainRecipeInline/addTerrainTriangles

Separates handle/UV resolution from addTriangle calls in setupTextures()
resolveFallback zone. pz_emit_terrain_tris stays at original callsite in
setupTextures() to preserve projectZ callsite identity. Member var
assignments remain in the member function. Behavior identical — this is
the pure-refactor prerequisite for the Shape C cache-read path."
```

---

## Task 3: Add `MC2_SHAPE_C_PARITY_CHECK=1` env gate + throttled site-tagged comparison

**Files:**
- Modify: `mclib/quad.cpp` (add env parse + `inlineRecipe` var + parity function + comparison block)

Adds a debug cross-check that compares `buildTerrainRecipeInline()` output against the terrain face cache entry field-by-field. The parity check fires only when `MC2_SHAPE_C_PARITY_CHECK=1`. Mismatches are throttled to the first 10 per session plus a closing summary to avoid per-frame log floods.

- [ ] **Step 1: Add env var parse near top of `quad.cpp`**

After the existing `#include` block and alongside other `static` globals in `quad.cpp`, add:

```cpp
static const bool s_shapeCParityCheck = (getenv("MC2_SHAPE_C_PARITY_CHECK") != nullptr);
```

- [ ] **Step 2: Add `shapeC_checkParity()` helper before `buildTerrainRecipeInline()`**

```cpp
static void shapeC_checkParity(
    const TerrainRecipe& inlineR,
    const MapData::WorldQuadTerrainCacheEntry& e,
    long tileR, long tileC, long uvMode)
{
    static int s_mismatches = 0;
    static int s_checks     = 0;
    ++s_checks;

    const char* siteTag =
        !inlineR.isCement ? (uvMode == BOTTOMRIGHT ? "BR_no_cement" : "TL_no_cement")
      : inlineR.isAlpha   ? (uvMode == BOTTOMRIGHT ? "BR_alpha"     : "TL_alpha")
                          : (uvMode == BOTTOMRIGHT ? "BR_cement"    : "TL_cement");

    bool ok = true;

#define SHAPE_C_CHECK_DWORD(field) \
    if (inlineR.field != e.field) { \
        if (s_mismatches < 10) \
            printf("[SHAPE_C] MISMATCH " #field " site=%s tileR=%ld tileC=%ld" \
                   " inline=%u cached=%u\n", \
                   siteTag, tileR, tileC, inlineR.field, e.field); \
        ok = false; ++s_mismatches; }

#define SHAPE_C_CHECK_BOOL(method) \
    if (inlineR.method != e.method()) { \
        if (s_mismatches < 10) \
            printf("[SHAPE_C] MISMATCH " #method " site=%s tileR=%ld tileC=%ld" \
                   " inline=%d cached=%d\n", \
                   siteTag, tileR, tileC, (int)inlineR.method, (int)e.method()); \
        ok = false; ++s_mismatches; }

    SHAPE_C_CHECK_DWORD(terrainHandle)
    SHAPE_C_CHECK_DWORD(terrainDetailHandle)
    SHAPE_C_CHECK_DWORD(overlayHandle)
    SHAPE_C_CHECK_BOOL(isCement)
    SHAPE_C_CHECK_BOOL(isAlpha)

#undef SHAPE_C_CHECK_DWORD
#undef SHAPE_C_CHECK_BOOL

    if (fabsf(inlineR.uvData.minU - e.uvData.minU) > 1e-5f ||
        fabsf(inlineR.uvData.minV - e.uvData.minV) > 1e-5f ||
        fabsf(inlineR.uvData.maxU - e.uvData.maxU) > 1e-5f ||
        fabsf(inlineR.uvData.maxV - e.uvData.maxV) > 1e-5f)
    {
        if (s_mismatches < 10)
            printf("[SHAPE_C] MISMATCH uvData site=%s tileR=%ld tileC=%ld"
                   " inline=(%.5f,%.5f,%.5f,%.5f) cached=(%.5f,%.5f,%.5f,%.5f)\n",
                   siteTag, tileR, tileC,
                   inlineR.uvData.minU, inlineR.uvData.minV,
                   inlineR.uvData.maxU, inlineR.uvData.maxV,
                   e.uvData.minU, e.uvData.minV, e.uvData.maxU, e.uvData.maxV);
        ok = false; ++s_mismatches;
    }

    // Summary every 10000 checks and on first check of session.
    if (s_checks == 1 || s_checks % 10000 == 0)
        printf("[SHAPE_C] parity checks=%d mismatches=%d\n", s_checks, s_mismatches);
    (void)ok;
}
```

- [ ] **Step 3: Wire parity check into `setupTextures()` resolveFallback zone**

In the `resolveFallback` zone body from Task 2, add the `inlineRecipe` variable and parity call. The zone body now reads:

```cpp
{
    ZoneScopedN("TerrainQuad::setupTextures resolveFallback");

    TerrainRecipe recipe;
    TerrainRecipe inlineRecipe;

    // Always build inline recipe when parity check is on — keeps comparison
    // meaningful even after the cache path is enabled in Task 5.
    if (s_shapeCParityCheck)
        buildTerrainRecipeInline(vertices, uvMode, inlineRecipe);

    buildTerrainRecipeInline(vertices, uvMode, recipe);  // will be replaced in Task 5

    if (s_shapeCParityCheck && cachedEntry && cachedEntry->isValid())
        shapeC_checkParity(inlineRecipe, *cachedEntry, tileR, tileC, uvMode);

    isCement            = recipe.isCement;
    terrainHandle       = recipe.terrainHandle;
    terrainDetailHandle = recipe.terrainDetailHandle;
    overlayHandle       = recipe.overlayHandle;
    uvData              = recipe.uvData;

    addTerrainTriangles(recipe);

    if (!recipe.isCement && recipe.terrainHandle != 0)
        pz_emit_terrain_tris(vertices, uvMode,
            (uvMode == BOTTOMRIGHT) ? "terrain_quad_cluster_a" : "terrain_quad_cluster_c",
            __FILE__, __LINE__);
}
```

- [ ] **Step 4: Build — confirm parity check is off by default**

```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0. No `[SHAPE_C]` output (env var not set).

- [ ] **Step 5: Commit**

```bash
git add mclib/quad.cpp
git commit -m "debug: add MC2_SHAPE_C_PARITY_CHECK env gate for Shape C cache validation

shapeC_checkParity() compares buildTerrainRecipeInline() output field-by-field
against WorldQuadTerrainCacheEntry values. Always uses a separately-built
inlineRecipe (not the cache-sourced recipe) so the check remains meaningful
after the cache path is enabled. Site tags: BR/TL x no_cement/alpha/cement.
Throttled to first 10 mismatches + summary every 10k checks. Default off."
```

---

## Task 4: Run parity check on tier1 — fix any mismatches

**Files:**
- Possibly modify: `mclib/quad.cpp` (only if mismatches found)

- [ ] **Step 1: Deploy current build**

```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --kill-existing
```

- [ ] **Step 2: Run parity check on cement-heavy missions**

```bash
MC2_SMOKE_MODE=1 MC2_SHAPE_C_PARITY_CHECK=1 \
  "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe" \
  --profile stock --mission mc2_03 --duration 25 2>&1 | grep "\[SHAPE_C\]"
```

Repeat for `mc2_10` and `mc2_17`.

Expected: only `[SHAPE_C] parity checks=N mismatches=0` summary lines. No `MISMATCH` lines.

- [ ] **Step 3: If mismatches found — diagnose by site tag**

Common root causes:

**`uvData` mismatch on `BR_no_cement` or `TL_no_cement`:** `buildTerrainFaceCache()` uses `resolveTextureHandle(vMin, vMax)` where `vMin`/`vMax` are world-space ordered; `buildTerrainRecipeInline()` uses `getTextureHandle(vA, vB)` with index-order vertices. Check `terrtxm2.cpp` to verify both use the same tile selection logic. If different, fix `buildTerrainRecipeInline()` to match.

**`terrainHandle` mismatch:** `resolveTextureHandle` may realize (stream in) a texture while `getTextureHandle` does not. Both should return the same handle value regardless. If not, check whether `getTextureHandle` has a fallback path that `resolveTextureHandle` bypasses.

**`overlayHandle` mismatch on `BR_alpha`/`TL_alpha`:** Verify `buildTerrainFaceCache()` and `buildTerrainRecipeInline()` call the same `terrainTextures->getOverlayHandle()` / `getTextureHandle()` overloads for the cement-alpha case.

- [ ] **Step 4: After any fix, re-run parity and confirm zero mismatches**

```bash
MC2_SMOKE_MODE=1 MC2_SHAPE_C_PARITY_CHECK=1 \
  "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe" \
  --profile stock --mission mc2_03 --duration 25 2>&1 | grep "MISMATCH"
```

Expected: zero lines.

- [ ] **Step 5: Commit fixes (skip if no fixes needed)**

```bash
git add mclib/quad.cpp
git commit -m "fix: align buildTerrainRecipeInline() with buildTerrainFaceCache() vertex selection

Parity check (MC2_SHAPE_C_PARITY_CHECK=1) revealed [describe mismatch here].
[Describe fix]. Zero MISMATCH lines on mc2_03, mc2_10, mc2_17."
```

---

## Task 5: Add `MC2_MODERN_TERRAIN_PATCHES=1` cache-read path

**Files:**
- Modify: `mclib/quad.cpp` (add `s_shapeCEnabled` + `tryGetCachedTerrainRecipe()` + wire into resolveFallback zone)

With parity confirmed, replace the single `buildTerrainRecipeInline` call with the cache-or-inline pattern. The parity check continues to use its own `inlineRecipe` (always computed when active), keeping validation meaningful.

- [ ] **Step 1: Add `s_shapeCEnabled` env parse near `s_shapeCParityCheck`**

```cpp
static const bool s_shapeCEnabled = ([] {
    const char* env = getenv("MC2_MODERN_TERRAIN_PATCHES");
    return (env != nullptr) && (env[0] == '1');
})();
```

- [ ] **Step 2: Add `tryGetCachedTerrainRecipe()` alongside the other helpers**

```cpp
// Fills recipe from cache when s_shapeCEnabled and entry is valid.
// Returns false if cache unavailable; recipe is unmodified.
static bool tryGetCachedTerrainRecipe(
    const MapData::WorldQuadTerrainCacheEntry* entry, TerrainRecipe& r)
{
    if (!s_shapeCEnabled || !entry || !entry->isValid())
        return false;
    r.isCement            = entry->isCement();
    r.isAlpha             = entry->isAlpha();
    r.terrainHandle       = entry->terrainHandle;
    r.terrainDetailHandle = entry->terrainDetailHandle;
    r.overlayHandle       = entry->overlayHandle;
    r.uvData              = entry->uvData;
    return true;
}
```

- [ ] **Step 3: Update the resolveFallback zone to use cache-or-inline**

Replace the single `buildTerrainRecipeInline(vertices, uvMode, recipe)` line with the cache-first pattern. The full zone body:

```cpp
{
    ZoneScopedN("TerrainQuad::setupTextures resolveFallback");

    TerrainRecipe recipe;
    TerrainRecipe inlineRecipe;

    // Always build inline recipe when parity check is on — keeps comparison
    // meaningful even now that the cache path is active.
    if (s_shapeCParityCheck)
        buildTerrainRecipeInline(vertices, uvMode, inlineRecipe);

    if (!tryGetCachedTerrainRecipe(cachedEntry, recipe))
        buildTerrainRecipeInline(vertices, uvMode, recipe);

    // Compare inline vs cache entry (not recipe — recipe may now be cache-sourced).
    if (s_shapeCParityCheck && cachedEntry && cachedEntry->isValid())
        shapeC_checkParity(inlineRecipe, *cachedEntry, tileR, tileC, uvMode);

    isCement            = recipe.isCement;
    terrainHandle       = recipe.terrainHandle;
    terrainDetailHandle = recipe.terrainDetailHandle;
    overlayHandle       = recipe.overlayHandle;
    uvData              = recipe.uvData;

    addTerrainTriangles(recipe);

    if (!recipe.isCement && recipe.terrainHandle != 0)
        pz_emit_terrain_tris(vertices, uvMode,
            (uvMode == BOTTOMRIGHT) ? "terrain_quad_cluster_a" : "terrain_quad_cluster_c",
            __FILE__, __LINE__);
}
```

- [ ] **Step 4: Run parity check WITH cache path enabled — confirm still valid**

```bash
MC2_SMOKE_MODE=1 MC2_SHAPE_C_PARITY_CHECK=1 MC2_MODERN_TERRAIN_PATCHES=1 \
  "A:/Games/mc2-opengl/mc2-win64-v0.2/mc2.exe" \
  --profile stock --mission mc2_03 --duration 25 2>&1 | grep "\[SHAPE_C\]"
```

Expected: `mismatches=0` summary. (If there are mismatches here that did not appear in Task 4, the bug is in `tryGetCachedTerrainRecipe()` — check the copy-from-entry logic.)

Repeat for `mc2_10`, `mc2_17`.

- [ ] **Step 5: Full tier1 smoke with cache path on**

```
MC2_MODERN_TERRAIN_PATCHES=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0.

- [ ] **Step 6: Commit**

```bash
git add mclib/quad.cpp
git commit -m "feat: Shape C cache-read path behind MC2_MODERN_TERRAIN_PATCHES=1

tryGetCachedTerrainRecipe() reads stable handles/UV/flags from
WorldQuadTerrainCacheEntry when MC2_MODERN_TERRAIN_PATCHES=1 (default off).
Falls back to buildTerrainRecipeInline() when cache is NULL/invalid.
Parity check continues comparing a separately-built inlineRecipe against
the cache entry — remains meaningful with cache path active.
detail UV wrapping remains per-vertex in pz_emit / draw() paths.
Parity passes on mc2_03, mc2_10, mc2_17."
```

---

## Task 6: Validation — smoke + visual + Tracy baseline

**Files:**
- Read-only

- [ ] **Step 1: Full tier1 smoke with cache path on**

```
MC2_MODERN_TERRAIN_PATCHES=1 py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0.

- [ ] **Step 2: Visual confirmation**

Launch mc2_03, mc2_10, mc2_17 manually with `MC2_MODERN_TERRAIN_PATCHES=1`. Inspect:
- No UV seams at tile boundaries
- Cement tiles still render as cement, overlay alpha correct
- No texture flicker during camera pan across large terrain areas
- Pan into previously-unseen terrain: textures appear correctly

- [ ] **Step 3: Tracy CPU baseline (recommended)**

With Tracy GUI attached, run mc2_03 for 60 seconds twice:
1. `MC2_MODERN_TERRAIN_PATCHES=0` (default — inline path)
2. `MC2_MODERN_TERRAIN_PATCHES=1` (cache path)

Record `TerrainQuad::setupTextures` zone average in each run. Note the delta. If Tracy GUI is unavailable, skip and record "Tracy baseline skipped" in Task 7.

- [ ] **Step 4: Regression check — full tier1 with cache path OFF**

```
py -3 A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev\scripts\run_smoke.py --tier tier1 --with-menu-canary --kill-existing
```

Expected: exit 0.

---

## Task 7: Closing report and memory update

**Files:**
- Create or update: `C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\patchstream_shape_c.md`
- Modify: `C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\MEMORY.md`

Note: memory files live outside the repo. Write them directly using the Write/Edit tools. Do **not** `git add` memory file paths.

- [ ] **Step 1: Write memory file**

Create `C:\Users\Joe\.claude\projects\A--Games-mc2-opengl-src\memory\patchstream_shape_c.md`:

```markdown
---
name: PatchStream Shape C — terrain cache-read path
description: Shape C status, parity check results, killswitch state, and any mismatches found
type: project
---

Shape C landed on branch `claude/nifty-mendeleev`. Spec at
`docs/superpowers/specs/2026-04-28-patchstream-shape-c-design.md`. Plan at
`docs/superpowers/plans/2026-04-28-patchstream-shape-c-plan.md`.

**Current status:** [fill in: parity pass / mismatches found + fixed / visual clean / default-off]

**Parity check results (MC2_SHAPE_C_PARITY_CHECK=1):**
- mc2_03: mismatches=0 (or describe fix)
- mc2_10: mismatches=0
- mc2_17: mismatches=0

**Tracy baseline:**
- setupTextures avg inline: [Xms]
- setupTextures avg cache:  [Xms]
- Delta: [Xms] ([Y]%)
(or: "Tracy baseline skipped")

**Killswitch:** MC2_MODERN_TERRAIN_PATCHES default-off. Decide default-on separately
after bake period.

**Key design decisions:**
- pz_emit_terrain_tris stays at original callsite in setupTextures() — projectZ
  callsite identity preserved
- Member var assignments inside member function, not in free helper
- inlineRecipe always computed when parity check on — comparison stays valid
  even with cache path enabled
- invalidateTerrainFaceCache() in setTerrain() invalidates WHOLE cache array,
  not individual entries
```

- [ ] **Step 2: Update MEMORY.md index**

Add under `## Rendering / shaders`:

```
- [PatchStream Shape C — terrain cache-read path](patchstream_shape_c.md) — cache-read in setupTextures(), default-off, parity-validated
```

- [ ] **Step 3: Commit docs only (not memory files)**

```bash
git add docs/superpowers/specs/2026-04-28-patchstream-shape-c-design.md
git add docs/superpowers/plans/2026-04-28-patchstream-shape-c-plan.md
git commit -m "docs: Shape C implementation complete — parity passed, default-off

MC2_MODERN_TERRAIN_PATCHES=1 passes parity check + tier1 smoke.
Default-off pending bake period. Tracy baseline: [delta or skipped]."
```
