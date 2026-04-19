# Render Contract and Cleanup Implementation Plan

## Outcome (added 2026-04-18, post-execution)

- **Task 1 (D1a — remove pz gate):** ATTEMPTED then REVERTED.
  - Commit: `ddc173f` (D1a removal), `6c6e872` (revert).
  - In-game result: giant triangles + atrocious framerate the moment the gate was bypassed under tessellation.
  - **The plan's premise was empirically falsified.** "GPU owns depth clipping for tessellated geometry; the CPU pz gate is redundant" is wrong — the gate is load-bearing in some way the design analysis missed. Suspected mechanisms (unconfirmed): `gVertex.xyz` flowing into the vertex shader as `gl_Position` even when the TES recomputes geometry from the extras VBO; or the TES using `gl_in[i].gl_Position` for tessellation-level decisions, producing tessellation explosions on garbage screen-space input.
  - D1a is **NOT resolved.** It needs an investigation pass before any second attempt: what does the vertex/TES actually consume from `gVertex.xyz`, and what does it do with the values from rejected (off-screen / behind-near-plane) terrain corners?
  - Fallback A (per-vertex `clipInfo > 0` gate) was not tried — the regression severity argued for revert + investigate, not narrow-and-retry blind.
- **Task 2 (Phase 4a — forced initial shadow pass):** SHIPPED.
  - Commit: `f28a1f5`.
  - In-game result: works. Plan deviation noted in commit (API in `gameos.hpp` + `gameos_graphics.cpp` instead of `gos_postprocess.h/.cpp`; file-static instead of class member; matches the existing `gos_StaticLightMatrixBuilt`/etc pattern).
- **Task 3 (docs update):** NOT DONE in the form the plan specified. The plan assumed both code tasks would land cleanly — D1a being reverted means `render-contract.md` should NOT mark D1a resolved. Phase 4a-only doc updates are a smaller, separate change.

**Lessons for future render-contract changes:**
- Removing CPU-side gates in this codebase needs an empirical safety check, not just architectural reasoning. The gates were written for DX7 software rasterization, but the OpenGL/tessellation pipeline still consumes the same `gVertex` struct. A gate that "should be redundant" may be feeding load-bearing geometry data downstream.
- Plan-time premise checks ("GPU clips this naturally") should include a one-shot diagnostic build that *logs* what the GPU sees with the gate bypassed (e.g., count triangles submitted with `pz < 0`), before the gate is actually removed.

---

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Finish the terrain/world-space render contract cleanup by (1) removing the DX7-era projected-depth gate (D1a) from the tessellated terrain DRAWSOLID path, (2) guaranteeing the initial static shadow pass runs at mission load (Phase 4a), and (3) updating render-contract docs to reflect the resulting state.

**Architecture:** Three independent work streams: removing the per-triangle pz gate from four exact line clusters in `mclib/quad.cpp`; adding a one-shot forced-shadow flag consumed by the `Shadow.StaticAccum` block in `mclib/txmmgr.cpp`, latched from the same file when terrain geometry first reaches `masterVertexNodes[]`; and a documentation pass after both code changes ship.

**Tech Stack:** C++ (mclib/quad.cpp, mclib/txmmgr.cpp, GameOS/gameos/gos_postprocess.cpp/.h), CMake, MSYS2/bash. Build: `cmake --build build64 --config RelWithDebInfo`. Deploy: per-file `cp -f` to `A:/Games/mc2-opengl/mc2-win64-v0.1.1/`.

**Execution order:** Task 1 (D1a code) → Task 2 (Phase 4a code) → Task 3 (docs). Docs land last so commit dates and "RESOLVED as of <date>" claims match reality.

---

## What This Plan Closes — and What It Does Not

**Closes:**
- The tessellated per-triangle depth gate (D1a): the `pz ∈ [0,1)` acceptance check inside the four terrain-base DRAWSOLID clusters in `quad.cpp` is removed for the tessellated render path.
- The first-frame shadow gap: the static shadow pass is forced to run once immediately after terrain geometry reaches `masterVertexNodes[]` for the first time, regardless of camera movement.
- Stale documentation: `docs/render-contract.md` and the roadmap still describe D2 and Phase 2 as active/unfinished. This plan corrects that *after* the code changes are shipped and validated.

**Does not close:**
- D1 overall: the terrain visibility producer (`onScreen` / `clipInfo` in `terrain.cpp`) still derives from a camera-angle/FOV test that is not a clean world-space bound check. D1 remains open until that producer is decoupled. This plan removes the per-triangle gate (D1a); the visibility producer cleanup is D1b (future work).
- Phase 4 overall: Phase 4a delivers one forced visible-set pass at terrain init. Full-map static shadow coverage independent of camera travel (Phase 4b) is deferred.
- Terrain visibility contract: this plan does not change `terrain.cpp`'s `projectZ()` call chain or the `onScreen`/`clipInfo` production logic.

A later worker reading this plan should understand: after all tasks here are done, the biggest open items are (a) `terrain.cpp` visibility producer cleanup (D1b) and (b) full-map shadow bake (Phase 4b).

---

## Context: What Is Already Done (nifty-mendeleev current state)

Phase 2 from the roadmap is **complete**. Do not re-implement these:

- `WorldOverlayVert` typed batch exists in `gameos_graphics.cpp` with `gos_PushTerrainOverlay()` / `gos_PushDecal()`
- `shaders/terrain_overlay.vert` + `shaders/terrain_overlay.frag` handle cement perimeter tiles with world-space shadows, cloud noise, and `GBuffer1.alpha=1` (prevents double-shadow)
- `shaders/decal.frag` handles bomb craters with world-space shadows and `GBuffer1.alpha=1`
- `mclib/quad.cpp` routes alpha cement → `gos_PushTerrainOverlay()`, `mclib/crater.cpp` routes craters → `gos_PushDecal()`
- `shaders/gos_tex_vertex.frag` is clean — no IS_OVERLAY logic, no shadow sampling
- The `Overlay.SplitDraw` path has been removed from `gameos_graphics.cpp`

Remaining debt relevant to this plan:

- **D1a (active):** `mclib/quad.cpp` — four terrain-base DRAWSOLID clusters gate triangle submission on `pz + 0.001 ∈ [0, 1)`. For the tessellated path the GPU handles depth clipping natively; this CPU gate is redundant and creates silent mismatch risk at oblique angles. See Task 1.
- **D1b (deferred):** `mclib/terrain.cpp` — `clipInfo` is set from `onScreen`, which is a camera-angle/FOV coarse test, not a geometric world-space bound check. `projectZ()` still runs immediately and populates `pz`. The visibility producer is not yet decoupled from the projected/camera contract. This is future work, not in this plan.
- **Phase 4a (not done):** Static terrain shadow is still camera-motion-triggered accumulation (100-unit threshold). Coverage at mission start depends on camera travel. See Task 2.
- **Docs stale:** `docs/render-contract.md` still lists D2 as "active bridge" and A3 as "target state, not fully implemented".

---

## Pre-Resolved Header / Symbol Locations

These were checked at plan-write time so the implementer does not have to hunt:

- `gos_IsTerrainTessellationActive` is declared in `GameOS/include/gameos.hpp` (already included transitively by both `mclib/quad.cpp` and `mclib/txmmgr.cpp`). No header edit needed.
- The four `pz`-based DRAWSOLID assignment lines in `mclib/quad.cpp` are at approximately lines 1495, 1660, 1799, 1961 (`gVertex[i].z = vertices[i]->pz + TERRAIN_DEPTH_FUDGE`). The corresponding six-condition `if` test sits ~25–35 lines below each assignment (~1525, ~1690, ~1830, ~1990 — confirm by sed).
- The `wz`-based clusters at ~2151, ~2310, ~2439, ~2598 are the projected-exception (Bucket B) paths. Do not touch.
- The `Shadow.StaticAccum` block in `mclib/txmmgr.cpp` is at lines ~1148–1204. The static `lastShadowCamX/Y/Z` declaration is at line 1156.

---

## File Map

| File | Change |
|------|--------|
| `mclib/quad.cpp` | Task 1: remove pz depth gate from four terrain-base DRAWSOLID clusters only |
| `GameOS/gameos/gos_postprocess.h` | Task 2: declare `gos_RequestFullShadowRebuild()`, `gos_ShadowRebuildPending()`, `gos_ClearShadowRebuildPending()` |
| `GameOS/gameos/gos_postprocess.cpp` | Task 2: implement the three functions and `shadowBuildPending_` member |
| `mclib/txmmgr.cpp` | Task 2: latch the forced flag on first terrain frame; respect it in `Shadow.StaticAccum` |
| `docs/render-contract.md` | Task 3: close D2, update A3, split D1 into D1a (resolved) / D1b (open) |
| `docs/plans/render-contract-and-shadows-roadmap.md` | Task 3: mark Phase 1+2 complete, Phase 3 partial, Phase 4 split into 4a/4b |

---

## Task 1: Remove pz Depth Gate from Tessellated Terrain DRAWSOLID Path (D1a)

**Files:**
- Modify: `mclib/quad.cpp` — four exact clusters only

### Scope contract — read before touching any code

This task touches **only** the four terrain-base DRAWSOLID clusters in `quad.cpp` that submit geometry under `MC2_ISTERRAIN | MC2_DRAWSOLID`. Their `gVertex[i].z = vertices[i]->pz + TERRAIN_DEPTH_FUDGE` assignment lines are at approximately 1495, 1660, 1799, 1961.

**Do not touch any of the following:**
- Any cluster that uses `wx`/`wy`/`wz` vertex fields (clusters at ~2151, ~2310, ~2439, ~2598). These are distinct projected-style paths and belong in Bucket B (projected exceptions) per `render-contract.md`.
- Any path using `MC2_ISWATER`, `MC2_ISWATERDETAIL`, `MC2_ISCRATERS`, `MC2_GPUOVERLAY`, or `MC2_ISSHADOWS` flags.
- Any cluster near line 3621.
- Any path that does not set `MC2_ISTERRAIN | MC2_DRAWSOLID` as its draw flags.

If a grep returns sites outside the four expected clusters, stop and confirm with the user before proceeding.

### Rationale (do not misstate this when writing code comments)

The pz gate is removed from the tessellated path because **the GPU owns depth clipping for tessellated geometry**. The TES pipeline naturally clips vertices behind the near plane and beyond the far plane. The CPU-side `pz ∈ [0,1)` check was a DX7-era precaution for software rasterization that is redundant — and potentially harmful — when a hardware tessellation pipeline is active.

This removal is **not** justified by `clipInfo` being a clean world-space visibility signal. `clipInfo` is set from `onScreen` in `terrain.cpp`, which is a camera-angle/FOV coarse test. `clipInfo` still depends on the projected/camera contract (D1b). The comment in code should say "GPU clips naturally" — not "clipInfo covers this."

- [ ] **Step 1: Locate the four terrain-base DRAWSOLID clusters**

Use two greps and cross-check that they identify the same four clusters:

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
grep -n "gVertex\[0\]\.z >= 0" mclib/quad.cpp
grep -n "pz + TERRAIN_DEPTH_FUDGE" mclib/quad.cpp
```

Expected: the first grep returns the four target if-tests plus additional sites near 2199, 2338, 2487, 2626, 3621 (do NOT touch). The second grep returns the four `pz`-based assignment lines (target clusters) plus four `wz`-based assignments (do NOT touch). If counts disagree with the pre-resolved expectations above, stop.

- [ ] **Step 2: Read each of the four target clusters in context**

For each of the four assignment line numbers from Step 1's `pz` grep, read ~50 lines starting ~5 lines above:

```bash
sed -n '1490,1545p' mclib/quad.cpp
sed -n '1655,1710p' mclib/quad.cpp
sed -n '1795,1850p' mclib/quad.cpp
sed -n '1955,2010p' mclib/quad.cpp
```

Confirm each cluster:
- Sets `gVertex[i].z = vertices[i]->pz + TERRAIN_DEPTH_FUDGE` for three vertices
- Has the six-condition pz range check (`gVertex[0].z >= 0.0f && gVertex[0].z < 1.0f && ...`)
- Calls `mcTextureManager->addVertices(terrainHandle, gVertex, MC2_ISTERRAIN | MC2_DRAWSOLID)` inside the if-block
- Calls `fillTerrainExtra(...)` inside the if-block

Do not proceed to Step 3 if a cluster does not match this signature.

- [ ] **Step 3: Apply the tessellation-aware guard to each of the four clusters**

For each of the four clusters, replace the six-condition pz gate:

**Before** (exact pattern at each site):
```cpp
if ((gVertex[0].z >= 0.0f) &&
    (gVertex[0].z < 1.0f) &&
    (gVertex[1].z >= 0.0f) &&
    (gVertex[1].z < 1.0f) &&
    (gVertex[2].z >= 0.0f) &&
    (gVertex[2].z < 1.0f))
{
```

**After**:
```cpp
// For tessellated terrain the GPU owns depth clipping; the CPU pz gate is
// redundant and risks silent triangle drops when projectZ() and TES disagree.
// Legacy sw-renderer (no tessellation) retains the pz gate for screen-space
// correctness. Note: clipInfo is NOT a clean world-space alternative — it
// derives from the same projected/camera contract (D1b, still open).
bool depthGatePass = gos_IsTerrainTessellationActive() ||
                     ((gVertex[0].z >= 0.0f) && (gVertex[0].z < 1.0f) &&
                      (gVertex[1].z >= 0.0f) && (gVertex[1].z < 1.0f) &&
                      (gVertex[2].z >= 0.0f) && (gVertex[2].z < 1.0f));
if (depthGatePass)
{
```

The body of the if-block is unchanged.

Apply this to all four clusters. Do not modify any other site in the file.

- [ ] **Step 4: Verify symbol scope**

`gos_IsTerrainTessellationActive` is declared in `GameOS/include/gameos.hpp`, which `mclib/quad.cpp` already includes transitively. Confirm with:

```bash
grep -n "gos_IsTerrainTessellationActive\|gameos\.hpp" mclib/quad.cpp | head -5
```

If the symbol resolves at compile time (Step 5 succeeds), no header edit is required. Do **not** add a bare `extern` declaration inside `quad.cpp`.

- [ ] **Step 5: Build**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build build64 --config RelWithDebInfo 2>&1 | tail -20
```

Expected: zero errors. If `gos_IsTerrainTessellationActive` is reported undeclared, add `#include "gameos.hpp"` at the top of `quad.cpp` (matching the style of other GameOS includes in the file) and rebuild.

- [ ] **Step 6: Deploy exe**

Close the game first (linker cannot write `mc2.pdb` while the process holds it).

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

Expected: no diff output (files identical).

- [ ] **Step 7: In-game validation**

Launch and check in order:

1. **Normal terrain at default zoom** — no holes, no missing tiles
2. **Pan to map edge** — terrain renders to the edge with no black gaps
3. **Wolfman zoom (altitude ~6000)** — no terrain disappearance at the horizon (this is the primary regression site: oblique angles where projectZ/TES disagreement was most likely to trigger the old pz gate)
4. **Near-camera zoom-in** — no giant triangles bleeding in from behind the near plane
5. **Cement overlays still visible** — runway/apron perimeter tiles still render
6. **Water still renders** — water uses a separate path and must be unaffected

### Fallback ladder (if regressions appear)

Apply in order; pick the first that fixes the regression:

**Fallback A — narrow the bypass with `clipInfo`:** if horizon holes appear at Wolfman zoom (case 3), the pz gate was providing a useful near-plane-behind rejection that `clipInfo` already covers. Change the guard to:
```cpp
bool tessClipOk = gos_IsTerrainTessellationActive() &&
                  (vertices[0]->clipInfo > 0) &&
                  (vertices[1]->clipInfo > 0) &&
                  (vertices[2]->clipInfo > 0);
bool depthGatePass = tessClipOk || (/* original six-condition pz check */);
```
Document this adjustment in the commit message and re-run validation.

**Fallback B — full revert:** if Fallback A still leaves visible holes or introduces new artifacts, revert the change with `git checkout mclib/quad.cpp`, document in CLAUDE.md what failed, and open a separate D1a investigation. Do not ship a half-fix.

- [ ] **Step 8: Commit**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add mclib/quad.cpp
git commit -m "$(cat <<'EOF'
fix: remove tessellated per-triangle pz depth gate from DRAWSOLID path (D1a)

GPU owns depth clipping for tessellated geometry; the CPU-side pz range check
was redundant and caused silent triangle drops when projectZ() and TES disagreed
at oblique angles. Legacy sw-renderer retains the original gate.

Only the four MC2_ISTERRAIN|MC2_DRAWSOLID clusters using vertices[i]->pz
(~1495, ~1660, ~1799, ~1961) are modified. Water, cloud, selection, and
projected-exception paths using vertices[i]->wz (~2151, ~2310, ~2439, ~2598)
are untouched.

D1a resolved. D1b (clipInfo visibility producer) remains open.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Phase 4a — Guarantee Initial Visible-Set Static Shadow Pass

**Files:**
- Modify: `GameOS/gameos/gos_postprocess.h`
- Modify: `GameOS/gameos/gos_postprocess.cpp`
- Modify: `mclib/txmmgr.cpp`

### Scope contract — read before touching any code

This task delivers **one guaranteed shadow pass for the initially visible tile set at terrain init**. It does not deliver full-map shadow coverage. After this task:

- The initial camera view is shadowed from frame 1 (specifically: the first `renderLists()` call where `masterVertexNodes[]` contains terrain).
- Camera-motion-triggered accumulation continues normally for subsequent frames.
- Terrain tiles outside the initial camera view are still only shadowed as the camera reaches them (Phase 4b, deferred).

Do not describe this as "Phase 4 complete" in any comment or commit message. Use "Phase 4a".

### Why latch from `txmmgr.cpp`, not `terrain.cpp`

The naive design — call `gos_RequestFullShadowRebuild()` from `Terrain::init()` — has a failure mode: terrain init runs *before* the first `renderLists()` call that populates `masterVertexNodes[]`. If the forced pass fires on a frame where `masterVertexNodes[]` is still empty for terrain, the shadow pass renders nothing, the flag clears, and the bug is silently masked because the *next* camera-distance pass (within 100 units of `1e9f` initial — i.e., always) also runs.

To guarantee the forced pass actually has terrain to render, latch the flag inside `txmmgr.cpp::renderLists()` itself, on the first frame where `masterVertexNodes[]` contains an `MC2_ISTERRAIN | MC2_DRAWSOLID` node with `currentVertex != vertices` (i.e., real terrain submitted). Use a static bool to fire exactly once.

### Key context — current accumulation logic (txmmgr.cpp:1148-1204)

```cpp
static float lastShadowCamX = 1e9f, lastShadowCamY = 1e9f, lastShadowCamZ = 1e9f;
float shadowCamDist = (squared distance from last shadow render position);
float shadowCacheThreshold = 100.0f;

if (gos_IsTerrainTessellationActive() &&
    shadowCamDist > shadowCacheThreshold * shadowCacheThreshold) {
    lastShadowCamX = cp.x; lastShadowCamY = cp.y; lastShadowCamZ = cp.z;
    bool firstFrame = !gos_StaticLightMatrixBuilt();
    if (firstFrame) { gos_BuildStaticLightMatrix(); gos_MarkStaticLightMatrixBuilt(); }
    gos_BeginShadowPrePass(firstFrame);
    // draw visible terrain nodes from masterVertexNodes[]
    gos_EndShadowPrePass();
}
```

- [ ] **Step 1: Add declarations to gos_postprocess.h**

Open `GameOS/gameos/gos_postprocess.h`. Near the existing shadow-related declarations add:

```cpp
// Phase 4a: force the static terrain shadow pass to run on the next
// renderLists() call regardless of camera movement.
// Latched automatically inside renderLists() on the first frame that submits
// real terrain geometry; can also be called externally if needed.
void __stdcall gos_RequestFullShadowRebuild();
bool __stdcall gos_ShadowRebuildPending();
void __stdcall gos_ClearShadowRebuildPending();
```

- [ ] **Step 2: Add member and implement functions in gos_postprocess.cpp**

Find the `gosPostProcess` class definition. Add a member:

```cpp
bool shadowBuildPending_ = false;
```

Add the three free functions near the other `gos_*` shadow helpers in the file:

```cpp
void __stdcall gos_RequestFullShadowRebuild() {
    if (g_postProcess) g_postProcess->shadowBuildPending_ = true;
}
bool __stdcall gos_ShadowRebuildPending() {
    return g_postProcess && g_postProcess->shadowBuildPending_;
}
void __stdcall gos_ClearShadowRebuildPending() {
    if (g_postProcess) g_postProcess->shadowBuildPending_ = false;
}
```

`g_postProcess` is the existing global `gosPostProcess*` used by other `gos_*` free functions in this file. Use the same access pattern they use.

- [ ] **Step 3: Latch the forced flag from `txmmgr.cpp` on first terrain frame**

In `mclib/txmmgr.cpp::renderLists()`, immediately *before* the `Shadow.StaticAccum` block (before line ~1155, before the `static float lastShadowCamX...` declaration), add:

```cpp
// Phase 4a: on the first frame that submits real terrain into
// masterVertexNodes[], force one static shadow pass regardless of camera
// position history. This guarantees the initial camera view is shadowed
// from frame 1 instead of waiting for a >100-unit camera move.
{
    static bool s_terrainShadowPrimed = false;
    if (!s_terrainShadowPrimed) {
        for (long si = 0; si < nextAvailableVertexNode; si++) {
            if ((masterVertexNodes[si].flags & MC2_DRAWSOLID) &&
                (masterVertexNodes[si].flags & MC2_ISTERRAIN) &&
                masterVertexNodes[si].vertices &&
                masterVertexNodes[si].currentVertex != masterVertexNodes[si].vertices) {
                gos_RequestFullShadowRebuild();
                s_terrainShadowPrimed = true;
                break;
            }
        }
    }
}
```

This guarantees the request fires on a frame where the terrain submission loop will actually find work to do.

- [ ] **Step 4: Modify Shadow.StaticAccum to respect the flag**

Find the block at txmmgr.cpp ~1163. Change the outer condition:

**Before:**
```cpp
if (gos_IsTerrainTessellationActive() && shadowCamDist > shadowCacheThreshold * shadowCacheThreshold) {
    ZoneScopedN("Shadow.StaticAccum");
    TracyGpuZone("Shadow.StaticAccum");

    lastShadowCamX = cp.x; lastShadowCamY = cp.y; lastShadowCamZ = cp.z;
    bool firstFrame = !gos_StaticLightMatrixBuilt();
```

**After:**
```cpp
bool shadowRebuildForced = gos_ShadowRebuildPending();
if (gos_IsTerrainTessellationActive() &&
    (shadowRebuildForced || shadowCamDist > shadowCacheThreshold * shadowCacheThreshold)) {
    ZoneScopedN("Shadow.StaticAccum");
    TracyGpuZone("Shadow.StaticAccum");

    // Only advance the camera position tracker on non-forced passes.
    // Forced passes must not consume the camera delta: a subsequent genuine
    // move of >100 units must still trigger the normal accumulation update.
    if (!shadowRebuildForced) {
        lastShadowCamX = cp.x; lastShadowCamY = cp.y; lastShadowCamZ = cp.z;
    }
    bool firstFrame = !gos_StaticLightMatrixBuilt();
```

Then, inside the block, after `gos_EndShadowPrePass()` and before the closing `}` (around line ~1203), add:

```cpp
    if (shadowRebuildForced) {
        gos_ClearShadowRebuildPending();
    }
```

- [ ] **Step 5: Confirm `gos_postprocess.h` is included in txmmgr.cpp**

```bash
grep -n "gos_postprocess\|postprocess" \
  "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/mclib/txmmgr.cpp" | head -5
```

If the new `gos_RequestFullShadowRebuild` / `gos_ShadowRebuildPending` / `gos_ClearShadowRebuildPending` symbols are not visible at compile time, add `#include "gos_postprocess.h"` (matching the include style of other GameOS headers already in the file).

- [ ] **Step 6: Build**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build build64 --config RelWithDebInfo 2>&1 | tail -20
```

Expected: zero errors.

- [ ] **Step 7: Deploy exe**

Close the game first.

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

- [ ] **Step 8: In-game validation**

Load a mission that has a cement runway visible from the starting camera position. Check:

1. **Static terrain shadows present from frame 1** — terrain/buildings cast shadows immediately on mission load, before any camera movement. Use RAlt+F2 to cycle the shadow debug overlay and confirm the shadow map is non-empty at mission start.
2. **No visible load hitch** — the forced pass covers only the initial visible tile set (same cost as one normal shadow accumulation frame). Should not produce a visible stall.
3. **Camera-motion accumulation still works** — pan to a new area; shadows fill in as usual.
4. **No shadow map corruption** — check RAlt+F2 overlay for black holes or sampling artifacts after panning.
5. **Forced flag is single-use, with the right cadence** — connect Tracy and confirm `Shadow.StaticAccum` runs **exactly once** during the first 60 frames with a stationary camera. Not zero (would mean the latch never fired or the request never reached the consumer). Not every frame (would mean the flag is never cleared, or `lastShadowCamX` was advanced by the forced pass and the next genuine move is being missed).

### Fallback ladder (if shadow pass cadence is wrong)

**If `Shadow.StaticAccum` runs zero times in the first 60 frames:**
- The `s_terrainShadowPrimed` latch never found a terrain node. Add a temporary `printf("[shadowprime] checked %ld nodes, no terrain\n", nextAvailableVertexNode);` inside the latch loop after the `for` to confirm, and verify which frame terrain first arrives. Move the latch later in `renderLists()` if needed.

**If `Shadow.StaticAccum` runs every frame (camera stationary):**
- `gos_ClearShadowRebuildPending()` is not being reached. Confirm it sits inside the same `if (gos_IsTerrainTessellationActive() && ...)` block as `gos_EndShadowPrePass()`, not outside it.

**If `Shadow.StaticAccum` runs once on init then misses the first genuine 100-unit pan:**
- The forced pass advanced `lastShadowCamX` despite the guard. Re-check that `if (!shadowRebuildForced)` correctly wraps the three assignments.

- [ ] **Step 9: Commit**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add GameOS/gameos/gos_postprocess.h GameOS/gameos/gos_postprocess.cpp \
        mclib/txmmgr.cpp
git commit -m "$(cat <<'EOF'
feat: Phase 4a — force static shadow pass on initial terrain frame

Adds gos_RequestFullShadowRebuild() flag. When set, the Shadow.StaticAccum
block in renderLists() bypasses the camera-motion threshold for one pass.
The flag is latched from inside renderLists() itself on the first frame
that submits real terrain into masterVertexNodes[], guaranteeing the
forced pass has geometry to render.

The forced pass deliberately does NOT advance lastShadowCamX/Y/Z so that
a subsequent genuine >100-unit camera move still triggers a normal
accumulation update.

Camera-motion accumulation continues normally after the forced pass.
Full-map coverage independent of camera travel (Phase 4b) is deferred.

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Update render-contract.md and Roadmap to Reflect Tasks 1 & 2

**Files:**
- Modify: `docs/render-contract.md`
- Modify: `docs/plans/render-contract-and-shadows-roadmap.md`

Documentation-only. Run **after** Tasks 1 and 2 are committed and validated in-game so the "RESOLVED as of <date>" claims match the actual commit dates in git history.

- [ ] **Step 1: Resolve the actual resolution date**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git log --oneline -5 mclib/quad.cpp mclib/txmmgr.cpp
```

Use the commit date of the Task 1 (D1a) commit and the Task 2 (Phase 4a) commit. They may be the same day or different days — use each one accurately in the doc updates below.

- [ ] **Step 2: Update Bucket A3 status in render-contract.md**

Change the A3 entry from "target state, not fully implemented" to:

```
### A3. Terrain overlays and decals

Status:
- active
- complete as of 2026-04-17

Primary files:
- shaders/terrain_overlay.vert
- shaders/terrain_overlay.frag
- shaders/decal.frag
- mclib/quad.cpp (setOverlayWorldCoords, gos_PushTerrainOverlay)
- mclib/crater.cpp (gos_PushDecal)
- GameOS/gameos/gameos_graphics.cpp (WorldOverlayVert batch, overlayProg_, decalProg_)

Authoritative submission space:
- raw MC2 world space

Projection owner:
- GPU via terrain_overlay.vert (terrainMVP + viewport chain, same as TES)

Shadow owner:
- forward shading in terrain_overlay.frag (calcShadow + calcDynamicShadow)
- decal.frag handles craters the same way
- GBuffer1.alpha=1 written by both shaders → shadow_screen.frag skips these pixels
- double-shadow prevention is structurally enforced, not a runtime branch

Notes:
- Overlay.SplitDraw path removed from gameos_graphics.cpp
- IS_OVERLAY bridge in gos_tex_vertex.frag removed
- gos_tex_vertex.frag is now projection-agnostic (water + non-terrain overlays only)
```

- [ ] **Step 3: Update D2 status to RESOLVED**

```
### D2. IS_OVERLAY / `rhw=1.0` / terrainMVP bridge inside `gos_tex_vertex`

Status:
- RESOLVED as of 2026-04-17
- IS_OVERLAY and shadow sampling removed from gos_tex_vertex.frag
- World-space cement overlays and craters now use terrain_overlay.vert/frag and decal.frag
- No terrain-specific semantics remain in the generic textured shader path
```

- [ ] **Step 4: Split D1 into D1a (resolved) and D1b (open)**

Use the **actual** Task 1 commit date from Step 1 in place of `<TASK1_DATE>`:

```
### D1a. Tessellated terrain per-triangle depth gate

Status:
- RESOLVED as of <TASK1_DATE>
- The pz ∈ [0,1) depth acceptance check has been removed from the four
  tessellated DRAWSOLID terrain clusters in mclib/quad.cpp (~1495, ~1660,
  ~1799, ~1961, the pz-based assignments).

Rationale for removal:
- For the tessellated render path the GPU owns depth clipping. The TES
  pipeline clips naturally; no CPU-side depth range acceptance is needed.
- The removal is NOT justified by clipInfo being a clean world-space producer.
  clipInfo is set from onScreen in terrain.cpp, which is a camera-angle/FOV
  coarse test that still depends on projectZ() running immediately beforehand.
  clipInfo is not yet a decoupled world-space bound check.
- The safe claim is only: the GPU clips tessellated geometry; the CPU gate
  was redundant and risks silent triangle drops when projectZ() and the TES
  disagree at oblique angles.

Retained behavior:
- Legacy sw-renderer path (Environment.Renderer == 3) retains the original pz
  gate unchanged, because that path does not use GPU tessellation and relies on
  CPU projection for screen-space correctness.

### D1b. Terrain visibility producer still carries projected/camera debt

Status:
- active
- not in this plan

Primary files:
- mclib/terrain.cpp (onScreen test, projectZ() call, clipInfo assignment)

Problem:
- terrain.cpp:1079 calls eye->projectZ(vertex3D, screenPos) to populate
  pz, pw, px, py on every terrain vertex each frame.
- terrain.cpp:1104 sets clipInfo = onScreen, where onScreen is derived from
  a camera-angle/FOV coarse test that depends on the projected/camera contract.
- clipInfo is the coarse acceptance gate used by quad.cpp upstream of the
  per-triangle gate. Until clipInfo production is a true world-space bound
  check, the visibility contract is still mixed.

Required cleanup (future session):
- Replace the projectZ()-driven onScreen test with a world-space frustum AABB
  check per terrain tile or vertex.
- Stop relying on pz/pw at any load-bearing correctness site in quad.cpp.
- Mark D1b resolved only after clipInfo is produced from world-space geometry
  without calling projectZ().
```

- [ ] **Step 5: Update the Priorities section**

Replace the existing priority list with:

```
## Current Priorities

### Priority 1: D1b — terrain visibility producer cleanup

The per-triangle pz gate is gone (D1a resolved). The remaining D1 debt is the
onScreen / clipInfo producer in terrain.cpp, which still depends on projectZ().
Replacing that with a world-space bound check is the next contract-correctness
task. Until done, D1 is not fully closed.

### Priority 2: Phase 4b — full-map static shadow bake

Phase 4a (initial visible-set forced pass) is shipped. The static shadow map
still depends on camera travel for full coverage. Phase 4b requires building
the shadow pass from all terrain tiles, not just masterVertexNodes[] visible
on the first frame. This is a separate architecture task from Phase 4a.

### Priority 3: Static shadow quality tuning (Phase 5)

Tune after Phase 4a/4b are stable: resolution, bias, PCF filter softness.

### Priority 4: Dynamic shadow reconciliation (Phase 6)

Re-tune dynamic focus region and bias once static shadow is stable.

### Priority 5: Post-shadow bridge reduction (Phase 7)

Inventory shadow_screen.frag consumers. Remove the pass from paths that own
their shadows directly.
```

- [ ] **Step 6: Update the roadmap**

Use the actual Task 1 / Task 2 commit dates from Step 1 in place of `<TASK1_DATE>` / `<TASK2_DATE>`:

```
## Phase 1: Contract Baseline And Cleanup

Status: COMPLETE (<TASK1_DATE>)
- render-contract.md updated with current state
- D2 closure documented
- A3 status updated
- D1 split into D1a (resolved) and D1b (open, future work)
```

```
## Phase 2: Typed Terrain Overlay / Decal Path

Status: COMPLETE (2026-04-17)
- WorldOverlayVert batch, terrain_overlay.vert/frag, decal.frag all shipped
- Alpha cement and bomb craters fully migrated
- IS_OVERLAY bridge removed from gos_tex_vertex.frag
- Double-shadow prevention structurally enforced via GBuffer1.alpha=1
```

```
## Phase 3: Terrain Cull / Visibility Contract Cleanup

Status: PARTIALLY IN PROGRESS
- D1a resolved: per-triangle pz gate removed from tessellated DRAWSOLID path
- D1b open: clipInfo producer (terrain.cpp onScreen / projectZ()) still carries
  projected/camera debt. This is the remaining Phase 3 work.
- Next task: replace onScreen derivation with a world-space frustum bound check
  that does not depend on projectZ().
```

```
## Phase 4: Startup-Built Static Terrain Shadow Map

Status: PHASE 4a SHIPPED (<TASK2_DATE>), PHASE 4b PENDING
- Phase 4a: gos_RequestFullShadowRebuild() flag is latched from inside
  txmmgr.cpp::renderLists() on the first frame that submits real terrain into
  masterVertexNodes[], forcing one static shadow pass at mission start. The
  initial visible tile set is shadowed from frame 1. Camera-motion accumulation
  continues normally afterward.
- Phase 4b: full-map shadow bake from all terrain tiles, independent of
  masterVertexNodes[]. Requires a separate terrain-iteration path for the
  shadow pass. Deferred.
```

- [ ] **Step 7: Commit doc changes**

```bash
cd "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev"
git add docs/render-contract.md docs/plans/render-contract-and-shadows-roadmap.md
git commit -m "$(cat <<'EOF'
docs: update render-contract and roadmap for D1a + Phase 4a closure

D2 (IS_OVERLAY bridge) resolved. A3 typed overlay path complete. D1 split
into D1a (tessellated pz gate, resolved per code commit) and D1b
(clipInfo/visibility producer, still open). Phase 4 split into 4a
(shipped per code commit) and 4b (pending).

Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>
EOF
)"
```

---

## Deferred / Future Tasks (Not In This Plan)

### D1b: Terrain visibility producer cleanup

Decouple `clipInfo` / `onScreen` from `projectZ()` in `terrain.cpp`. Replace the camera-angle/FOV test with a world-space frustum AABB check per terrain tile or vertex. This is the remaining work to fully close D1 and satisfy the Phase 3 contract.

Do not attempt this in the same session as D1a. The D1a change removes the per-triangle gate; D1b changes the upstream producer. They must be validated independently.

### Phase 4b: Full-map static shadow atlas bake

On mission load, iterate over all terrain tiles (not just the camera-visible `masterVertexNodes[]` set) and render them all into the shadow map in one pass. Requires a separate shadow-geometry iteration path in txmmgr or a standalone terrain-for-shadow API, because the current shadow pass is driven by the same `masterVertexNodes[]` loop that serves the rendering pipeline.

### Phase 5: Static shadow quality tuning

Tune after Phase 4a/4b are stable: raise static map resolution, tune PCF tap count and radius in `shaders/include/shadow.hglsl`, tune bias.

### Phase 6: Dynamic shadow reconciliation

Re-tune focus region and bias after static map is stable.

### Phase 7: Post-shadow bridge reduction

Inventory `shadow_screen.frag` consumers. Remove per-path as each gains forward shadow ownership.

### MC2_GPUOVERLAY / Render.NoUnderlayer dead code

`txmmgr.cpp:1417` still sets `gos_State_Overlay=1` for the `MC2_GPUOVERLAY` path. Since `gos_tex_vertex.frag` no longer has IS_OVERLAY logic, this flag has no visible effect. If MC2_GPUOVERLAY is genuinely dead (nothing queues it), remove the entire NoUnderlayer loop. Grep first to confirm no active callers before removing.

### MC2_DEBUG_SHADOW_COLLECT diagnostic cleanup

The worktree CLAUDE.md notes that `MC2_DEBUG_SHADOW_COLLECT` printf diagnostics in `mclib/tgl.cpp` and `mclib/txmmgr.cpp` were removed in a 2026-04-15 session. Verify with `grep -rn "MC2_DEBUG_SHADOW_COLLECT" mclib/` — if any references remain, remove them in a separate cleanup commit (not part of this plan, but a natural follow-up).

---

## Validation Checklist (Every Task)

- [ ] Terrain visible at default zoom (no holes)
- [ ] Terrain visible at Wolfman zoom (altitude ~6000, wide view)
- [ ] Camera pan to map edge — no missing terrain tiles
- [ ] Cement overlays (runway/apron perimeter) still render on terrain
- [ ] Water still renders and animates
- [ ] Mission markers visible, not shadowed by world shadows
- [ ] Static terrain shadows present (RAlt+F2 confirms non-empty shadow map)
- [ ] Dynamic mech/building shadows present
- [ ] No double-shadow on cement tiles
- [ ] No load-time crash or GL error spam in console

---

## Build and Deploy Reference

Always build `RelWithDebInfo` (Release builds crash with GL_INVALID_ENUM):

```bash
"C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" \
  --build "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64" \
  --config RelWithDebInfo 2>&1 | tail -30
```

Deploy exe (close game first — linker cannot write mc2.pdb while the game process holds it):

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/build64/RelWithDebInfo/mc2.exe" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/mc2.exe"
```

Deploy shader only (no exe rebuild needed for shader-only changes):

```bash
cp -f "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/SHADERNAME" \
      "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/SHADERNAME"
diff -q "A:/Games/mc2-opengl-src/.claude/worktrees/nifty-mendeleev/shaders/SHADERNAME" \
        "A:/Games/mc2-opengl/mc2-win64-v0.1.1/shaders/SHADERNAME"
```
