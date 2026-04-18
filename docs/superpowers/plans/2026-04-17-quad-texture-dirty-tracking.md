# QuadSetupTextures Dirty-Tracking Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Eliminate the steady-state terrain selection cost in `TerrainQuad::setupTextures()` by caching the resolved terrain-selection handles on each `TerrainQuad` and skipping redundant `getTextureHandle` work when the selection inputs are unchanged.

**Architecture:** Cache the terrain-selection result for the lifetime of the mission. Keep the current-frame draw handles separate from the cached handles so offscreen quads can still clear their active handles to `0xffffffff` without invalidating the cached selection. Do **not** invalidate the cache when a quad goes offscreen. Only explicit reset paths such as quad init, mission teardown, or a future terrain-texture reset hook should clear it.

**Tech Stack:** C++, MC2 OpenGL port. Worktree: `A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/`. Build: `--config RelWithDebInfo` only. Deploy: `A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe`. Tracy Profiler for before/after perf verification. Git: `git -c safe.directory=A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev -C 'A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev'`.

---

## Pre-implementation Analysis (Read First)

Before touching any code, understand what determines texture selection per quad in the **active code path** (`Terrain::terrainTextures2` exists, which it always does in the nifty-mendeleev worktree):

**Terrain handle inputs (terrainTextures2 path):**
- `vertices[0]->pVertex->textureData & 0x0000ffff` — tile texture index; **static for mission lifetime** (set at map load)
- `isCement` / `isAlpha` — derived from `textureData`; equally static
- `vertices[{0,2}]->vx, vy` or `vertices[{1,3}]->vx, vy` — world positions; **static forever** (terrain geometry never moves)

**Visibility inputs:**
- `vertices[i]->clipInfo` — changes every frame as camera pans. When `clipped1 | clipped2 == 0`, quad is offscreen.

**Mine inputs:**
- `GameMap->getMine(row, col)` for 3×3 cells = 9 calls per visible quad per frame
- Mine state changes only when a mine detonates (rare gameplay event)
- Keep running the mine loop every frame for correctness — it's only 9 cheap array lookups

**Water inputs:**
- `Terrain::terrainTextures2->getWaterDetailHandle(sprayFrame)` — `sprayFrame` is an animated water frame index that changes every frame
- The water section MUST always re-run when water vertices are present
- Water is on at most a small fraction of quads; not the primary bottleneck

**Key insight:** Since `textureData` and vertex world positions are both static, the terrain-selection result is effectively **mission-lifetime static per quad**. It should be recomputed once, then reused until an explicit reset path clears it.

**What `addTriangle` does:** It registers the quad's triangles into the sorted render batch for this frame. It MUST be called every frame for every visible quad — it's not cached. Only the `getTextureHandle` sub-calls (which resolve handles and update `lastUsed`) can be skipped.

**Why `lastUsed` skip is safe:** All terrain textures are pinned with `neverFLUSH = 0x1` (`terrtxm.cpp`, `terrtxm2.cpp`). Pinned textures can never be cache-evicted regardless of `lastUsed`. Skipping the `get_gosTextureHandle` call (which updates `lastUsed`) is therefore safe.

**Vertex lighting (`calcThisFrame & 1`):** Already guarded per-vertex; not a target here. It runs once per vertex per frame and cannot be skipped. The dirty-tracking only targets the texture handle resolution at the top of `setupTextures`.

---

## File Map

| File | Change |
|---|---|
| `mclib/quad.h` | Add cached terrain-selection handles, `handlesValid`, `hasWater`, and an explicit invalidation helper |
| `mclib/quad.cpp` | Add cached-path vs resolve-path handling in `setupTextures()` and minimal Tracy instrumentation |

No other files need modification.

---

## Task 1: Understand the exact structure of setupTextures

**Files:**
- Read: `mclib/quad.cpp:120-1453`
- Read: `mclib/quad.h`

The function has this logical structure (all within `void TerrainQuad::setupTextures()`):

```
Lines 120-134:  Static texture lazy-loads (mine textures, one-time init)
Lines 136-496:  TERRAIN TEXTURE SECTION
                  if (!terrainTextures2)  -- old TXM path (never active in our worktree, skip)
                  else                    -- ACTIVE PATH: TerrainColorMap
                    if uvMode == BOTTOMRIGHT:
                      compute clipped1 = v0+v1+v2 clipInfo sum
                      compute clipped2 = v0+v2+v3 clipInfo sum
                      if (clipped1 || clipped2): VISIBLE → resolve handles + addTriangle + mine loop
                      else: INVISIBLE → clear all handles to 0xffffffff
                    else (BOTTOMLEFT):
                      compute clipped1 = v0+v1+v3 clipInfo sum
                      compute clipped2 = v1+v2+v3 clipInfo sum
                      same visible/invisible split
Lines 497-814:  WATER SECTION
                  if any vertex has water flag: resolve waterHandle + waterDetailHandle + addTriangle
                  else: clear waterHandle to 0xffffffff
Lines 816-1453: VERTEX LIGHTING SECTION
                  if (terrainHandle != 0xffffffff):
                    for each vertex: compute lightRGB (fog, normals, shadow) guarded by calcThisFrame & 1
```

The optimization only touches lines 136-496 (terrain texture section). Water and vertex lighting sections are untouched.

- [ ] **Step 1: Confirm the line ranges by reading quad.cpp:120-500**

Run: just read the file — no action needed in this task, it's a verification step.

- [ ] **Step 2: Verify `Terrain::terrainTextures2` is always non-null in nifty-mendeleev**

```bash
grep -n "terrainTextures2" A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/terrain.cpp | head -20
```

Expected: terrainTextures2 is always allocated at mission start. The `if (!Terrain::terrainTextures2)` branch in setupTextures is dead code in this worktree.

---

## Task 2: Add `handlesValid` to TerrainQuad

**Files:**
- Modify: `mclib/quad.h`

The `TerrainQuad` struct already has `bool isCement` at line 78. Add `handlesValid` alongside it, and `hasWater` as a pre-computed hint to avoid per-frame vertex water checks.

- [ ] **Step 1: Read `mclib/quad.h` to see the exact struct layout**

Read: `mclib/quad.h:59-141`

- [ ] **Step 2: Add the new fields after `isCement`**

In `mclib/quad.h`, after line 78 (`bool isCement;`), add:

```cpp
		bool				handlesValid;			//True when terrainHandle/Detail/overlayHandle are resolved and current.
		bool				hasWater;				//True if any vertex has water flag; pre-cached to avoid per-frame scan.
```

- [ ] **Step 3: Initialize both fields in `TerrainQuad::init()`**

In `mclib/quad.h`, in the `void init(void)` body (around line 94), after `isCement = false;` add:

```cpp
			handlesValid = false;
			hasWater = false;
```

- [ ] **Step 4: Verify no other init paths exist that set TerrainQuad state**

```bash
grep -n "terrainHandle = 0xffffffff\|handlesValid\|TerrainQuad.*init" A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/quad.cpp | head -20
```

Expected: `terrainHandle = 0xffffffff` only appears inside `setupTextures` and the struct `init()`. The struct `init()` is the only initialization site.

---

## Task 3: Add the cached-path split in setupTextures

**Files:**
- Modify: `mclib/quad.cpp:120-496`

This is the core change. The strategy:

1. Compute `clipVisible` before any terrain selection work.
2. If invisible: clear only the current-frame active handles so later draw and lighting work skips the quad, but leave the cached terrain-selection handles intact.
3. If visible AND `handlesValid`: copy the cached terrain-selection handles back into the active handles, re-add the triangles, and skip selection work.
4. If visible AND `!handlesValid`: run the full existing selection path, populate both the active handles and the cached handles, then set `handlesValid = true`.

The mine loop always runs for visible quads. Water stays dynamic and unchanged apart from an optional `hasWater` pre-check.

- [ ] **Step 1: Read the BOTTOMRIGHT visible branch (lines 285-392) in full**

Read: `mclib/quad.cpp:285-495`

This is the active code path. Note exactly which addTriangle calls happen for each sub-case:
- Non-cement: addTriangle(terrainHandle, SOLID) ×2, addTriangle(detailHandle, ALPHA) ×2
- Cement+isAlpha (alpha overlay): addTriangle(terrainHandle, SOLID) ×2, +detailHandle if set; overlayHandle → NOT addTriangle, used in draw()
- Cement+!isAlpha (solid cement): addTriangle(terrainHandle, SOLID) ×2; no detail

- [ ] **Step 2: Add the early-out at the top of the `else` (terrainTextures2) branch**

The `else` branch starts at line 283: `else // New single bitmap on the terrain.`

Inside that `else`, just before the `if (uvMode == BOTTOMRIGHT)` line (~line 285), insert the early-out block. The full replacement context:

```cpp
	else		//New single bitmap on the terrain.
	{
		// --- DIRTY-TRACKING EARLY-OUT ---
		// Texture handles for the terrainTextures2 path depend only on static data:
		// vertex world positions (vx/vy) and textureData (set at map load, never changes).
		// Skip all getTextureHandle calls when handles were already resolved this visibility window.
		auto computeClipVisible = [&]() -> long {
			if (uvMode == BOTTOMRIGHT) {
				long c1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[2]->clipInfo;
				long c2 = vertices[0]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
				return c1 | c2;
			} else {
				long c1 = vertices[0]->clipInfo + vertices[1]->clipInfo + vertices[3]->clipInfo;
				long c2 = vertices[1]->clipInfo + vertices[2]->clipInfo + vertices[3]->clipInfo;
				return c1 | c2;
			}
		};

		long clipVisible = computeClipVisible();

		if (!clipVisible) {
			// Quad is offscreen — clear all handles and mark dirty for next visibility window.
			overlayHandle = 0xffffffff;
			terrainHandle = 0xffffffff;
			waterHandle = 0xffffffff;
			waterDetailHandle = 0xffffffff;
			terrainDetailHandle = 0xffffffff;
			handlesValid = false;
			// Skip to water section — hasWater scan happens below.
			goto water_section;
		}

		if (handlesValid) {
			// Handles are valid from a prior frame in this visibility window — re-add triangles directly.
			if (terrainHandle != 0xffffffff && terrainHandle != 0) {
				mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
				mcTextureManager->addTriangle(terrainHandle, MC2_ISTERRAIN | MC2_DRAWSOLID);
				if (terrainDetailHandle != 0xffffffff) {
					mcTextureManager->addTriangle(terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
					mcTextureManager->addTriangle(terrainDetailHandle, MC2_ISTERRAIN | MC2_DRAWALPHA);
				}
			}
			// overlayHandle is consumed in draw() via get_gosTextureHandle — no addTriangle needed here.
			// Mine loop: always re-run for correctness (mine detonation invalidates cached mineResult).
			{
				long rowCol = vertices[0]->posTile;
				long tileR = rowCol >> 16;
				long tileC = rowCol & 0x0000ffff;
				if (GameMap) {
					mineResult.init();
					long cellPos = 0;
					for (long cellR = 0; cellR < MAPCELL_DIM; cellR++) {
						for (long cellC = 0; cellC < MAPCELL_DIM; cellC++, cellPos++) {
							long ar = tileR * MAPCELL_DIM + cellR;
							long ac = tileC * MAPCELL_DIM + cellC;
							DWORD localResult = 0;
							if (GameMap->inBounds(ar, ac))
								localResult = GameMap->getMine(ar, ac);
							if (localResult == 1) {
								mcTextureManager->get_gosTextureHandle(mineTextureHandle);
								mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								mcTextureManager->addTriangle(mineTextureHandle, MC2_DRAWALPHA);
								mineResult.setMine(cellPos, localResult);
							} else if (localResult == 2) {
								mcTextureManager->get_gosTextureHandle(blownTextureHandle);
								mcTextureManager->addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								mcTextureManager->addTriangle(blownTextureHandle, MC2_DRAWALPHA);
								mineResult.setMine(cellPos, localResult);
							}
						}
					}
				}
			}
			goto water_section;
		}

		if (uvMode == BOTTOMRIGHT)
		// ... rest of existing BOTTOMRIGHT block unchanged ...
```

And at the very end of the existing else-branch (after both uvMode blocks, just before the water section that starts with `if ((vertices[0]->pVertex->water & 1) ||`), add:

```cpp
		handlesValid = true;
	water_section:;
	}  // end else (terrainTextures2)
```

Then label the start of the water block to make the goto target valid.

**IMPORTANT — goto label placement:** The `goto water_section` skips to AFTER the entire terrain texture section. The water block (lines 497+) is currently outside the `else` braces — it needs to remain accessible. The label `water_section:` goes immediately after the closing `}` of the `else` block, before the existing water check.

- [ ] **Step 3: Verify `goto` is not crossing any variable initializations**

The `goto water_section` skips the following variable declarations in the normal path:
- `isCement` (bool, local to the if-block)
- `isAlpha` (bool, local to the if-block)
- Various `clipped1`, `clipped2` locals inside each uvMode block

These are all **inside** the if-blocks we're skipping, so the goto doesn't cross any declarations — it jumps over the entire block body. This is valid C++.

- [ ] **Step 4: Add `hasWater` pre-scan at mission load (optional fast path)**

`hasWater` can be set once when the quad is initialized from map data:

In `mclib/quad.cpp`, in `TerrainQuad::init(VertexPtr v0, VertexPtr v1, VertexPtr v2, VertexPtr v3)` at line 90, after calling the other init, add:

```cpp
	hasWater = (v0->pVertex->water & 1) ||
	           (v1->pVertex->water & 1) ||
	           (v2->pVertex->water & 1) ||
	           (v3->pVertex->water & 1);
```

This lets the water section check `if (hasWater)` instead of re-scanning 4 vertices each frame. Since water flags don't change during gameplay, this is safe.

**Note:** Find the existing `long TerrainQuad::init(VertexPtr v0, ...)` at line 90 of quad.cpp. It sets up the pointer-based init. Add `hasWater` there.

- [ ] **Step 5: Replace the water section's `if ((vertices[0]->pVertex->water & 1) || ...)` with `if (hasWater)`**

In `mclib/quad.cpp` at line 500 (approximately), replace:

```cpp
	if ((vertices[0]->pVertex->water & 1) ||
		(vertices[1]->pVertex->water & 1) ||
		(vertices[2]->pVertex->water & 1) ||
		(vertices[3]->pVertex->water & 1))
```

with:

```cpp
	if (hasWater)
```

---

## Task 4: Verify the goto approach compiles cleanly

There is an alternative to `goto` if it causes issues: restructure using a helper lambda or a local function. The goto is cleaner here because both exit points (invisible path and cached path) both need to jump to the water section.

**Alternative to goto (if needed):** Extract the full else-branch into a helper:

```cpp
// In quad.h, add private:
void resolveTerrainTextureHandles();  // called only when !handlesValid
```

And call it from `setupTextures` conditionally. But this adds a function-call overhead and the goto is cleaner in this specific case.

- [ ] **Step 1: Build RelWithDebInfo to check for compile errors**

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" \
  --config RelWithDebInfo \
  --target mc2 \
  2>&1 | tail -30
```

Expected: `Build succeeded.` If any `goto crosses initialization` error appears, remove the goto and use the lambda/helper approach instead.

- [ ] **Step 2: Deploy the exe**

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

Expected: `diff -q` prints nothing (files are identical).

- [ ] **Step 3: Commit the change**

```bash
git -c safe.directory=A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev \
  -C 'A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev' \
  add mclib/quad.h mclib/quad.cpp

git -c safe.directory=A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev \
  -C 'A:\Games\mc2-opengl-src\.claude\worktrees\nifty-mendeleev' \
  commit -m "$(cat <<'EOF'
perf: skip redundant getTextureHandle calls via handlesValid dirty flag

Terrain texture handles in the terrainTextures2 path are determined
solely by static data (vertex world positions, textureData at map load).
Cache them on first setup; re-add triangles with cached handles on
subsequent frames without calling getTextureHandle or get_gosTextureHandle.

Clear handlesValid when quad leaves screen so first-visible frame
always does a fresh setup. Mine loop still runs every frame for
correctness when mines are present.

Expected: quadSetupTextures ~20ms -> ~3ms at full zoom-out.
EOF
)"
```

---

## Task 5: Profile and verify correctness

**Goal:** Confirm correctness visually AND confirm the performance improvement in Tracy.

- [ ] **Step 1: Launch the game and load a mission with cement tiles (e.g., mission with airfield/runway)**

Load the same mission used in previous profiling sessions so the before/after numbers are comparable.

- [ ] **Step 2: Verify visual correctness**

Check in-game at both normal zoom and full zoom-out:
1. Terrain textures render correctly (no missing or glitched tiles)
2. Cement/runway tiles look the same as before
3. Alpha overlay tiles (road edges) look the same
4. Water renders correctly (animated ripples still animate)
5. Mine textures appear if any mines are present on the map

If anything looks wrong, the most likely cause is the `handlesValid` flag being set `true` too aggressively. Check whether `overlayHandle` needs to call `get_gosTextureHandle` explicitly in the skip path (it may be needed if the texture is not guaranteed pinned).

- [ ] **Step 3: Connect Tracy and measure quadSetupTextures at full zoom-out**

In Tracy, look for the `Terrain::geometry quadSetupTextures` zone at full zoom-out (the zoom level where the bottleneck was 20ms).

Expected: zone drops from ~20ms to ~2-4ms. The remaining time is the mine loop (9 getMine calls × 5000 quads = cheap) and the hasWater check overhead.

- [ ] **Step 4: Check for `DrawScreen` overall improvement**

The 20ms savings in quadSetupTextures should translate directly to overall `DrawScreen` dropping from ~4ms (at normal zoom) and higher at full zoom-out. Verify the full-zoom-out frame time improves.

- [ ] **Step 5: If water animation breaks (no ripple changes), fix the water section**

If `sprayFrame` doesn't animate correctly because we're hitting `goto water_section` and bypassing the frame-by-frame water handle refresh — check that the `water_section:` label is correctly placed BEFORE the `if (hasWater)` block so the water section always runs regardless of the early-out path.

---

## Task 6: (Optional) Cache UVData for colormap quads

**Goal:** Eliminate the `uvData` recomputation inside `TerrainColorMap::getTextureHandle` for quads that were already cached.

**Note:** This is only worth doing if Task 5's profiling shows remaining time is dominated by `uvData` updates rather than the mine loop. Skip unless profiling shows it's needed.

The UVData (`minU`, `minV`, `maxU`, `maxV`) is computed from static vertex world positions. It never changes. After Task 3, the first-frame setup computes and stores `uvData` in `TerrainQuad::uvData` (it already exists in the struct). Subsequent frames skip `getTextureHandle` entirely so `uvData` is never re-written — this is already correct behavior after Task 3.

No additional code needed — this optimization falls out naturally from Task 3.

---

## Correctness Edge Cases

**Q: What if a tile's textureData changes mid-mission?**

It doesn't. `PostcompVertex::textureData` is set from map data at load and never modified during gameplay in this codebase.

**Q: What happens when the shadow map updates?**

Shadow map updates affect vertex lighting (via `vertices[i]->pVertex->shadow`) but NOT texture handle selection. The vertex lighting section (lines 816+) is not touched by this change and is guarded by `calcThisFrame & 1` per vertex.

**Q: What about the `isCement` member variable?**

`isCement` is set inside the full setup block and used in `draw()`. After Task 3, `isCement` is only written on the first-frame setup. Since it derives from static `textureData`, its value never changes — this is correct.

**Q: What if `get_gosTextureHandle(overlayHandle)` in `draw()` encounters a non-resident texture?**

All cement overlay textures are loaded via `TerrainTextures::getTextureHandle` which calls `get_gosTextureHandle` during setup and then caches the node index in `overlayHandle`. Since cement textures are pinned (`neverFLUSH=0x1`), they never get evicted. The `get_gosTextureHandle(overlayHandle)` in `draw()` will always find them resident.

**Q: Can `handlesValid` ever be true while `terrainHandle == 0xffffffff`?**

Yes — for offscreen or edge quads that ended up with no valid terrain texture. The skip-path code in Task 3 guards with `if (terrainHandle != 0xffffffff && terrainHandle != 0)` before calling `addTriangle`, so this is safe.

---

## Before/After Perf Summary

| Metric | Before | Expected After |
|---|---|---|
| `quadSetupTextures` at full zoom-out | ~20ms | ~2-4ms |
| `getTextureHandle` calls/frame | ~21K | ~0 (steady state after first frame) |
| Full zoom-out frame time | 40-50ms | 25-35ms |
| Normal zoom frame time | ~16ms (60fps stable) | unchanged |

The only frames that pay the full cost are: first frame after map load, and frames where the camera pans far enough to bring new quads into view for the first time that session.
