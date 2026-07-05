# STATIC-SCENE-PROXY-SHADOW-1 — Scope / Design Recon (2026-06-20)

**Status:** RECON ONLY. No implementation. Read-only findings + slice definition.

---
## ⚠️ CORRECTION + RUNTIME VALIDATION (BUILDING-SHADOW-GATE-HISTORY-VALIDATE-0, 2026-06-20)

**CORRECTION to this doc:** the earlier claim "registered buildings don't cast shadows by default" is WRONG. With `MC2_STATIC_PROP_BUILDING_SHADOW` OFF (default), `includeBldg = !s_skipBldgInDynamic = true` (`txmmgr.cpp:2603-2606`), so **buildings already cast shadows via the dynamic camera-fit CSM map** (off-screen included via registry walk + lightbox cull). The gate does NOT turn building shadows on/off — it **moves** them from the camera-fit dynamic map to the **world-fixed full-coverage** static map, and excludes them from dynamic to avoid double-shadow (min-combine safeguard `7ea32b83`). So it's a *which-map / coverage-quality* tradeoff.

**Why was the gate off?** Git archaeology: NO "disable due to bug" commit. Introduced default-off as an unvalidated feature (`657d671d`→`8428805e`→`a365e6ad`→`054ca335`→`7639013b`). No known-bad-foundation landmine.

**A/B runtime result (gate ON: `MC2_STATIC_PROP_BUILDING_SHADOW=1` + DYNAMIC_PROP_CASTERS=1 + LIGHTBOX_CULL=1):** mc2_24 PASS 70fps, mc2_10 PASS 70fps, no crash, no meaningful perf delta vs gate-off (~70-75fps). REGFLUSH healthy (drawn==seen). Visual correctness (duplicate shadows / off-screen coverage / artifacts) NOT verified — needs screenshots; `visual-advisory PASS` uses a stale fixed baseline, not gate-on-vs-off. Building-shadow instance count needs `=2` trace (not captured).

**STRATEGIC VERDICT — SHADOW-1 is a SIDE QUEST, does NOT serve the per-frame CPU goal:** Building shadows are GPU draw work (gate-on ADDS a world-fixed map pass); they remove no CPU. The shadow caster gather is already CPU-optimized (`s_dynPropDirtyOnly` default-on, generation-gated, ~120-150µs/frame saved). The real per-frame CPU cost is **Phase 2 `stableLightSkipEligible` (~600µs/frame)** = `STATIC-SCENE-PROXY-SKIP-1`, orthogonal to shadows. **Recommend: PARK SHADOW-1; pivot to SKIP-1.** Everything below documents the shadow design should building-shadow coverage later become a deliberate VISUAL goal.
---
**Branch:** `claude/nifty-mendeleev` @ `39542ed5`. All file:line below are in this worktree.
**Upstream proven this session:** registration GREEN (mc2_24 2671/2672), prewarm baked, registry color-flush draws healthy (`drawn==seen`, 0 stale), `OBJBATCHER gpu_drawn_instances=0` is a benign meter. See `OBJBATCHER-GPU-STATIC-PROP-FALLBACK-RECON-1` (memory handoff).

## FIRST RULE (carried from the charter)
Off-screen static props MUST still be eligible to cast shadows. **Do NOT source casters from the per-frame camera-visible set** (`s_liveRangeIndices` / `liveSize`). Source from the FROZEN registry (`s_recipeRanges`) and cull against the SHADOW frustum, not camera visibility.

---

## HEADLINE FINDING: the proxy machinery already exists (reuse, don't rebuild)

A frozen-sourced, visibility-independent static-prop shadow caster path is **already shipped and default-on for dynamic props**. The charter's "build a shadow proxy that reads the frozen set" is ~80% already implemented. The genuine remaining work is narrow (see SLICE below).

### Existing pieces (all confirmed in code)

| Piece | Where | Default | Notes |
|---|---|---|---|
| Frozen building caster gather | `getBuildingShadowInstances()` `gos_static_prop_registry.cpp:1738-1750` | — | Walks FULL `s_recipeRanges`, visibility-independent, tombstone-skip, `population==Building` filter. Returns world-baked leaves. |
| Frozen dynamic caster gather | `getDynamicPropShadowInstances()` `…:1760-1774` | — | Inverse filter (non-building), skips `noShadow`; `includeBuildings` optional. |
| Dynamic-prop CSM shadow draw | `txmmgr.cpp:2601-2740` gather `:2633`, emit `drawDynamicPropShadows` `:2738` | **ON** (`MC2_SHADOW_DYNAMIC_PROP_CASTERS`, `=0` kill) | Off-screen trees cast TODAY. |
| World-fixed building shadow map | `txmmgr.cpp:2460-2484` gather `:2471`, emit `drawStaticBuildingShadows` `:2477` | **OFF** (`MC2_STATIC_PROP_BUILDING_SHADOW`; `=2` variant) | Registered buildings do NOT cast by default. |
| Shadow-frustum cull | `txmmgr.cpp:2638-2731` (`MC2_SHADOW_CASTER_LIGHTBOX_CULL`) | **ON** | Point-in-NDC via `getDynamicLightSpaceMatrix()` + per-recipe extent radius (`staticPropGetExtentRadius` `:1776`). |
| Shadow shaders | `gameos_graphics.cpp:4729-4761` (prog 91 opaque, 94 alpha foliage) | — | `[SHADOW_STATIC_PROP]`. |

### Why this satisfies the FIRST RULE already
Both gathers read `s_recipeRanges` (frozen, full registry) — **never** `s_liveRangeIndices` and **never** the `getCachedFrame()` stamp gate (`registry.cpp:986`). That stamp gate exists ONLY on the COLOR `flush()` path because color consumes per-frame `lightDataIndex`; shadow is depth-only and correctly bypasses it. The black-tree/stale bug cannot occur on the shadow path as written.

---

## Answers to the 10 scoping questions

1. **Where does the static shadow pass gather casters?** Two sites in `txmmgr.cpp renderLists()`: world-fixed building map (`:2460-2484`) and camera-fit dynamic CSM (`:2601-2740`). Both pull from the registry, not a TG_Shape list.
2. **Already includes registered static props?** YES — registry-sourced is the primary path. Legacy `TG_Shape::RenderShadows` (`tgl.cpp:3772`) / `mechShadowShape` (`mech3d.cpp:3427`) remain only for mechs/vehicles.
3. **Frozen/golden/staticReg/recipeIndex/bounds/lightDataIndex exposure?** `s_recipeRanges` (struct `registry.cpp:223-257`: `first/count/multi/lightDataIndex/extentRadius/shapeName/population/noShadow`) — file-static but exposed via accessors: `staticPropGetModelMatrix` `:1705`, `staticPropGetExtentRadius` `:1776`, `staticPropGetLightDataIndex` `:1782`, plus the two shadow gathers. `staticReg.recipeIndex` in `bdactor.h:250`. **`s_goldenRecipeList` is NOT a shadow source** — it's a gated (`MC2_GPU_CULL_STATIC_FROZEN_RECORDS`) leaf-index scatter list for compute cull; ignore for shadow.
4. **Sufficient data to render a caster?** YES, depth-only: world-baked `modelMatrix` + `typeID` per leaf (struct `gos_static_prop_batcher.h:27-49`); geometry by `typeID` via `batcher_getTypeDrawInfo` (`batcher.h:394`). No lighting needed.
5. **Mesh leaves available without the visible flush?** YES — the gathers bulk-copy `s_recipes[first..first+count)` directly; no `markVisible`, no `flush`, no transform step (geometry is **world-baked**, `batcher.h:28`).
6. **Culling?** Shadow-frustum point-test (lightbox cull, default ON) against `getDynamicLightSpaceMatrix()`; per-recipe `extentRadius` available to widen point→sphere. No generic AABB-vs-frustum helper exists; sphere-box is the idiom.
7. **Destruction/damage/fall invalidation in frozen set?** Via `invalidate(regIdx)` (`registry.cpp:718-766`) → **tombstone** (`count=0`, `multi=null`, leaves zeroed). Both gathers honor it (`:1742/:1765`). Re-querying each shadow build drops dead casters automatically → mc2_10 destruction handled for free. **No per-recipe generation field** — only global `s_registryGeneration` (`:772`) + tombstone.
8. **Avoid the black-tree/stale-cull bug?** Already avoided: shadow path reads frozen `s_recipeRanges`, never the stamp-gated live set. The rule for any new code: same — re-walk frozen, no `getCachedFrame()` compare.
9. **Minimal shadow-only record?** If the proxy **re-walks** via the existing gathers each shadow build, NO new persistent record is needed — the returned `GpuStaticPropInstance` leaves are ready-to-draw. (Persistent-record variant below has gaps.)
10. **Default-off gate + diagnostics?** Convention is inline `static const bool getenv(...)`. Default-OFF example `txmmgr.cpp:2461`; default-ON kill-switch `:2598`. No shadow-draw count diagnostics exist — they must be ADDED for parity proof.

---

## Proposed StaticShadowProxy record — feasibility

The user's proposed record mapped to real sources. **Primary recommendation: do NOT hold a persistent record; re-walk the registry each shadow build** (the existing, hazard-free pattern). If a persistent record is later justified for perf, this table is the build sheet:

| Field | Source | Status |
|---|---|---|
| transform | none — `GpuStaticPropInstance.modelMatrix` world-baked (`batcher.h:28`) | AVAILABLE (no transform step) |
| recipeIndex / rangeId | `staticReg.recipeIndex` (`bdactor.h:250`) | AVAILABLE — **but recycle-aliasing hazard if held across frames** |
| bounds (center) | `staticPropGetModelMatrix` `:1705` → `(-m[3],m[11],m[7])` | AVAILABLE (derive) |
| bounds (radius) | `staticPropGetExtentRadius` `:1776` (RecipeRange.extentRadius) | AVAILABLE |
| mesh/range ref | leaf `typeID` → `batcher_getTypeDrawInfo` `:394` | AVAILABLE |
| caster kind (bldg/tree) | RecipeRange.population `:252` | AVAILABLE |
| noShadow (foliage skip) | RecipeRange.noShadow `:256` | AVAILABLE |
| enabled / tombstone | `count==0 || multi==null` `:712`; `isReady()` `:777` | AVAILABLE |
| invalidationGen (per-caster) | none — global `s_registryGeneration` only | **MISSING** (coarse via global gen) |
| objectId / watchId | static props carry `actorId=0` (`:394`) | **MISSING** (new plumbing if needed) |
| lightDataIndex | `staticPropGetLightDataIndex` `:1782` | NOT NEEDED (depth-only) |

### The one real correctness hazard: RECIPE-SLOT-RECYCLE aliasing
`MC2_STATIC_RECIPE_RECYCLE` (default ON, `registry.cpp:613-665`) lets a tombstoned `regIdx` be reused by a *new* actor. A persistent proxy holding a raw `recipeIndex` across frames can silently point at a different prop after recycle — tombstone-check does NOT catch it (slot is live again). **The existing gathers sidestep this by re-walking the full registry every build (no held indices).** Any persistent-record design MUST add a generation/handle or it is unsafe. → **Re-walk is the recommended design.**

---

## SLICE DEFINITION — what SHADOW-1 actually is

Given the gather/cull/invalidation machinery exists and dynamic-prop (tree) shadows are already default-on and off-screen-capable, **SHADOW-1 is NOT a green-field proxy build.** It is:

> **Enable and validate frozen-sourced static BUILDING shadow casting** — the one default-OFF piece (`MC2_STATIC_PROP_BUILDING_SHADOW`) — as a shadow-only, off-screen-capable path, plus add the parity diagnostics that currently don't exist. Keep default-OFF until parity is proven.

Concrete work items (implementation phase, not now):
1. Add `[STATIC_SHADOW_PROXY v1]` diagnostics to the two shadow gathers + draws (counts below). Without these there is no parity proof.
2. Characterize the existing dynamic-prop shadow path with the new counters (baseline: what casts today).
3. Evaluate enabling building shadows via the existing `MC2_STATIC_PROP_BUILDING_SHADOW` path: correctness (no double-shadow vs any residual legacy caster), perf (extra shadow-map draws), visual parity.
4. Confirm re-walk approach (no persistent record) to dodge recipe-recycle aliasing.
5. If building shadows prove good, decide default-on separately (out of this slice).

### Diagnostics to add (`STATIC_SHADOW_PROXY v1`)
`candidates`, `accepted`, `rejected_invalidated` (tombstone), `rejected_no_bounds`, `rejected_no_recipe`, `shadow_frustum_visible` (post-lightbox-cull), `drawn`, `legacy_static_casters`, `proxy_static_casters`, `overlap_or_duplicate_count`. Emit per-N-frames like `[REGFLUSH_DIAG v1]`.

### Acceptance (later implementation)
- **Gate OFF:** byte-identical / no behavior change.
- **Gate ON (shadow-only):** mc2_24 off-screen/edge static casters included by SHADOW frustum (not camera visibility); mc2_10 destruction removes the caster; no duplicate shadows where a legacy caster is still active; no black/stale tree artifacts; tier1 5/5 PASS.

### Hard boundaries (from charter)
No lighting-model changes. No static-reg/prewarm changes. No dynamic-batcher fixes. No camera-visible live-set dependency. No default-on.

---

## Open question to settle BEFORE implementation
Does the dynamic-prop CSM path (`drawDynamicPropShadows`, default ON) *already* cover BUILDINGS when `includeBuildings` is requested, or only trees? `MC2_STATIC_PROP_BUILDING_SHADOW` is read at BOTH `txmmgr.cpp:2462` (world-fixed map) and `:2584` (dynamic includeBuildings). Determine whether building shadows are missing entirely, or only missing from the world-fixed map while present in CSM. This decides whether SHADOW-1 is "flip a gate + validate" or "the world-fixed building map needs real work." → first runtime task of the implementation phase (visual A/B mc2_24 with the gate on/off, plus the new counters).
